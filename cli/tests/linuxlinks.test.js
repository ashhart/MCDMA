'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const exec = require('../lib/exec');
const { buildLinuxLinks, linuxNeighbourEntries, runLinuxTransferTest } = require('../lib/linuxlinks');

function fixtures() {
  const make = (id, kind, arch, ip, mac) => ({ id, kind, host: `peer-${id}`, arch, reachable: true,
    peerTools: ['/opt/example/verbs-peer'], peerToolHashes: { '/opt/example/verbs-peer': 'a'.repeat(64) },
    ports: [{ iface: 'eth1', primary: true, rdmaDevice: 'mlx5_0', rdmaPort: 1, link: true,
      gid: ip, gidIndex: 9, mac, addrs: [ip], gids: [{ index: 9, addr: ip, type: 'RoCE v2', ndev: 'eth1' }],
      cable: { sn: 'example-cable' }, neighbours: [] }] });
  const peers = [make('a', 'linux', 'x86_64', 'fe80::1', '02:00:00:00:00:01'),
    make('b', 'spark', 'aarch64', 'fe80::2', '02:00:00:00:00:02')];
  peers[0].ports[0].neighbours = [{ addr: 'fe80::2', lladdr: peers[1].ports[0].mac, permanent: true }];
  peers[1].ports[0].neighbours = [{ addr: 'fe80::1', lladdr: peers[0].ports[0].mac, permanent: true }];
  return peers;
}
const settings = () => ({ tools: { linuxPeerPaths: { a: '/opt/example/verbs-peer', b: '/opt/example/verbs-peer' } },
  test: { payload: 4096, mtu: 1024, iterations: 1000, arm: 'bf64' } });

test('Linux cable matching includes Strix/Spark and leaves existing Spark pairs alone', () => {
  const peers = fixtures(), link = buildLinuxLinks({ peers })[0];
  assert.equal(link.type, 'linux'); assert.equal(link.reason, 'cable'); assert.equal(link.status.ready, true);
  assert.equal(link.a.gidIndex, 9); assert.equal(link.a.gid, 'fe80::1');
  peers[0].kind = 'spark'; assert.deepEqual(buildLinuxLinks({ peers }), []);
  const savedLinks = [{ id: 'explicit-pair', a: { node: 'a', iface: 'eth1' }, b: { node: 'b', iface: 'eth1' } }];
  assert.equal(buildLinuxLinks({ peers, savedLinks })[0].status.ready, true);
});

test('Ambiguous cable matches are not guessed and ports cannot be assigned twice', () => {
  const peers = fixtures();
  const third = structuredClone(peers[1]); third.id = 'c'; peers.push(third);
  assert.deepEqual(buildLinuxLinks({ peers }), []);
  const saved = { id: 'chosen', a: { node: 'a', iface: 'eth1' }, b: { node: 'b', iface: 'eth1' } };
  assert.throws(() => buildLinuxLinks({ peers, savedLinks: [saved, { ...saved, id: 'duplicate-port' }] }), /unique/);
  assert.throws(() => buildLinuxLinks({ peers, savedLinks: [{ ...saved, b: saved.a }] }), /Invalid/);
});

test('Readiness needs observed link-local RoCE v2 GIDs, physical links and usable exact reciprocal neighbours', () => {
  for (const mutate of [
    (peers) => { peers[0].ports[0].gids = []; },
    (peers) => { peers[0].ports[0].gid = null; },
    (peers) => { peers[0].ports[0].gidIndex = null; },
    (peers) => { peers[0].ports[0].gids.push({ ...peers[0].ports[0].gids[0] }); },
    (peers) => { peers[0].ports[0].gids[0].type = 'RoCE v1'; },
    (peers) => { peers[0].ports[0].gids[0].ndev = 'unrelated0'; },
    (peers) => { peers[0].ports[0].gids[0].addr = '2001:db8::1'; },
    (peers) => { peers[0].ports[0].addrs = []; },
    (peers) => { peers[0].ports[0].rdmaPort = null; },
    (peers) => { peers[0].ports[0].link = false; },
    (peers) => { peers[0].ports[0].neighbours[0].permanent = false; }
  ]) {
    const peers = fixtures(); mutate(peers); assert.equal(buildLinuxLinks({ peers })[0].status.ready, false);
  }
});

test('Explicit links remain visible while a known endpoint is unreachable', () => {
  const peers = fixtures(); peers[0].reachable = false; peers[0].ports = [];
  const link = buildLinuxLinks({ peers, savedLinks: [{ id: 'saved', a: { node: 'a', iface: 'eth1' }, b: { node: 'b', iface: 'eth1' } }] })[0];
  assert.equal(link.status.ready, false); assert.equal(link.identity, null); assert.equal(link.a.iface, 'eth1');
});

test('Linux identities invalidate saved verification when endpoint or tool identity changes', () => {
  const peers = fixtures(), link = buildLinuxLinks({ peers })[0], lastTests = { [link.id]: { identity: link.identity, passed: true } };
  assert.equal(buildLinuxLinks({ peers, lastTests })[0].status.lastTest.passed, true);
  for (const mutate of [
    (copy) => { copy[0].ports[0].gidIndex = copy[0].ports[0].gids[0].index = 10; },
    (copy) => { copy[0].peerToolHashes['/opt/example/verbs-peer'] = 'b'.repeat(64); },
    (copy) => { copy[0].arch = 'aarch64'; },
    (copy) => { copy[0].bootId = 'example-new-boot'; }
  ]) {
    const copy = structuredClone(peers); mutate(copy);
    assert.equal(buildLinuxLinks({ peers: copy, lastTests })[0].status.lastTest, null);
  }
});

test('Linux verification fingerprints include requested payload, MTU and iterations', () => {
  const peers = fixtures(), link = buildLinuxLinks({ peers })[0];
  const lastTests = { [link.id]: { identity: link.identity, passed: true } };
  assert.equal(buildLinuxLinks({ peers, lastTests, test: { payload: 4096, mtu: 1024, iterations: 1000 } })[0].status.lastTest.passed, true);
  for (const config of [{ payload: 1024 }, { mtu: 4096 }, { iterations: 1001 }])
    assert.equal(buildLinuxLinks({ peers, lastTests, test: config })[0].status.lastTest, null);
});

test('Linux neighbour plans preserve both endpoints and exclude unobserved GIDs', () => {
  const links = buildLinuxLinks({ peers: fixtures() });
  assert.deepEqual(linuxNeighbourEntries(links), {
    a: [{ iface: 'eth1', studioLinkLocal: 'fe80::2', studioMac: '02:00:00:00:00:02' }],
    b: [{ iface: 'eth1', studioLinkLocal: 'fe80::1', studioMac: '02:00:00:00:00:01' }]
  });
  links[0].b.gid = null; assert.equal(linuxNeighbourEntries(links).a, undefined);
});

test('Linux readiness accepts confirmed dynamic NDP states while persistence remains optional', () => {
  const identity = buildLinuxLinks({ peers: fixtures() })[0].identity;
  for (const state of ['REACHABLE', 'STALE', 'DELAY', 'PROBE']) {
    const peers = fixtures();
    for (const peer of peers) peer.ports[0].neighbours[0] = { ...peer.ports[0].neighbours[0], permanent: false, state };
    const link = buildLinuxLinks({ peers })[0];
    assert.equal(link.status.ready, true); assert.equal(link.status.aNeighbourMode, 'dynamic');
    assert.equal(link.status.bNeighbourMode, 'dynamic'); assert.equal(link.status.aPersisted, false);
    assert.equal(link.status.bPersisted, false); assert.equal(link.identity, identity);
  }
  for (const state of ['FAILED', 'INCOMPLETE', 'NOARP', 'NONE', 'unknown', 'REACHABLE FAILED']) {
    const peers = fixtures(); peers[0].ports[0].neighbours[0] = { ...peers[0].ports[0].neighbours[0], permanent: false, state };
    assert.equal(buildLinuxLinks({ peers })[0].status.ready, false);
  }
  const peers = fixtures(); peers[0].persist = { service: true, conf: 'eth1 fe80::2 02:00:00:00:00:02\n' };
  const link = buildLinuxLinks({ peers })[0];
  assert.equal(link.status.aNeighbourMode, 'permanent'); assert.equal(link.status.aPersisted, true);
  assert.equal(link.status.bPersisted, false);
});

function trace() {
  return 'operation,bytes,sample,completion_ns\n' + Array.from({ length: 2000 }, (_, i) =>
    `${i < 1000 ? 'write' : 'read'},4096,${i % 1000},${1000 + i}`).join('\n') + '\n';
}
const summary = (op, fault = '') => `LATENCY op=${op} bytes=4096 samples=1000 warmup=${fault === 'warmup' ? 0 : 100} qd=1 min_us=1.000 median_us=2.000 p95_us=3.000 p99_us=4.000 max_us=5.000 mean_us=2.500`;

async function simulatedRun(options = {}) {
  const originalHost = exec.Host, originalEndpoint = exec.Endpoint, commands = [], stopped = [], sent = [], hostCommands = [];
  class FakeHost {
    constructor(kind, alias) { this.alias = alias; }
    async sh(command) {
      hostCommands.push(command);
      if (command.startsWith('test -x')) return options.missingTool ? { code: 1, out: '' } : { code: 0,
        out: `${(options.toolChanged ? 'b' : 'a').repeat(64)}  /opt/example/verbs-peer\nMCDMA_VERBS_PEER abi=1 platform=${options.wrongPlatform ? 'macos stock_initiator=0' : 'linux stock_initiator=1 stock_responder=1'}\n` };
      if (command.startsWith('cat ')) return { code: 0, out: options.badTrace ? trace().split('\n').slice(0, -2).join('\n') : trace() };
      if (command.startsWith('ping ')) return { code: options.ndpUnavailable ? 127 : options.pingNoReply ? 1 : 0, timedOut: !!options.ndpTimedOut, out: '' };
      if (command.startsWith('ip -6 neigh ')) {
        if (options.ndpQueryFailure) return { code: 127, out: '' };
        const peer = this.alias === 'peer-a' ? '2' : '1';
        const address = options.ndpWrongGid ? 'fe80::9' : `fe80::${peer}`;
        const mac = options.ndpWrongMac ? '02:00:00:00:00:09' : `02:00:00:00:00:0${peer}`;
        const entry = `${address}${options.ndpWrongInterface ? ' dev unrelated0' : ''} lladdr ${mac} ${options.ndpFailed ? 'FAILED' : 'REACHABLE'}\n`;
        return { code: 0, out: options.ndpAmbiguous ? entry + entry : entry };
      }
      throw new Error('Unexpected host command');
    }
  }
  class FakeEndpoint {
    constructor(host, command) {
      this.side = host.alias === 'peer-a' ? 'a' : 'b'; commands.push(command);
      const role = this.side === 'a' || options.wrongBRole ? 'stock-initiator' : 'stock-responder';
      const badVendor = options.badVendor || (this.side === 'b' && options.badBVendor);
      const softMarker = options.softMarker === true || (options.soft && options.softMarker !== false);
      this.stderr = options.badMode || (this.side === 'b' && options.badBMode) ? '' : softMarker
        ? `LINUX_PEER_CONFIG backend=stock-libibverbs soft_transport=1 vendor_id=0x0 rdma_port=1 role=${role}\n`
        : `LINUX_PEER_CONFIG backend=stock-libibverbs vendor_id=${badVendor ? '0x1234' : options.legacyVendor ? '0x15b3' : '0x2c9'} rdma_port=1 role=${role}\n`;
      if (options.duplicateMode || (this.side === 'b' && options.duplicateBMode)) this.stderr += this.stderr;
      const measured = options.latency !== false;
      this.lines = this.side === 'a' ? [`ENDPOINT 17 91 1 4096 16384 ${options.badGid ? 'fe80::9' : 'fe80::1'}`, 'READY',
        options.badPayload ? 'NATIVE_FORWARD write=1 read=1 verified=0' : 'NATIVE_FORWARD write=1 read=1 verified=4096',
        ...(measured ? [summary('write', options.summaryFault), summary(options.duplicate ? 'write' : 'read'), 'LATENCY_TRACE /tmp/mcdma-cx5-latency-exampleA'] : []),
        'NATIVE_REVERSE verified=4096'] : ['ENDPOINT 18 92 2 32768 16384 fe80::2', options.timeout ? null : 'READY',
        'PEER_RESULT forward=4096 write=1 read=1 reverse=4096',
        ...(measured ? [summary('write'), summary('read'), 'LATENCY_TRACE /tmp/mcdma-cx5-latency-exampleB'] : [])];
    }
    async line() { return this.lines.shift() ?? null; }
    send(line) { sent.push([this.side, line]); }
    async stop() { stopped.push(this.side); return options.cleanupFailure === this.side ? 2 : 0; }
  }
  exec.Host = FakeHost; exec.Endpoint = FakeEndpoint;
  try {
    const peers = fixtures();
    if (options.missingNeighbours) for (const peer of peers) peer.ports[0].neighbours = [];
    const link = buildLinuxLinks({ peers })[0];
    const config = settings(); if (options.settingsChanged) config.test.payload = 1024;
    if (options.soft) config.test.soft = true;
    const result = await runLinuxTransferTest({ link, settings: config, latency: options.latency !== false });
    return { result, commands, stopped, sent, hostCommands };
  } finally { exec.Host = originalHost; exec.Endpoint = originalEndpoint; }
}

test('Stock Linux test retains complete latency traces and verifies both initiators without Mac posting markers', async () => {
  const { result, commands, stopped, sent } = await simulatedRun();
  assert.equal(result.passed, true, result.errors.join('; ')); assert.equal(result.hardwareGPU, false);
  assert.equal(result.cpuSubmission, true); assert.equal(result.submission, 'cpu'); assert.equal(result.gpuNICMMIO, false);
  assert.equal(result.boundary, 'cpu-post-to-cq-observed'); assert.equal('noNICMMIO' in result, false);
  assert.equal(result.latency.a.write.samples, 1000); assert.equal(result.latency.b.read.warmup, 100);
  assert.equal(result.traces.a.samplesPerOperation, 1000); assert.equal(result.traces.b.sha256.length, 64);
  assert.match(commands[0], /stock-initiator$/); assert.match(commands[1], / 9 stock-responder$/);
  assert.match(commands[0], /-u MCDMA_USER_POST/); assert.deepEqual(stopped, ['a', 'b']);
  assert.ok(sent.some(([side, value]) => side === 'a' && value.startsWith('INITIATEBENCH')));
  assert.ok(sent.some(([side, value]) => side === 'b' && value.startsWith('ROUNDTRIPBENCH')));
  assert.ok(sent.some(([side, value]) => side === 'a' && value === 'CHECKREVERSE'));
});

test('Quick Linux test verifies all transfers without latency samples', async () => {
  const { result } = await simulatedRun({ latency: false }); assert.equal(result.passed, true); assert.equal(result.latency, null);
});

test('Soft transport opt-in sends MCDMA_SOFT_TRANSPORT, requires the soft marker and stamps the result', async () => {
  const { result, commands } = await simulatedRun({ soft: true });
  assert.equal(result.passed, true, result.errors.join('; '));
  assert.equal(result.softTransport, true); assert.equal(result.softVendorId, 0);
  assert.equal((commands.join('\n').match(/MCDMA_SOFT_TRANSPORT=1 /g) || []).length, 2);
  assert.match(commands[0], /MCDMA_LATENCY_PROFILE=0 MCDMA_SOFT_TRANSPORT=1 /);
});

test('A soft run refuses classic-only evidence and a normal run refuses soft-labelled evidence', async () => {
  const classic = await simulatedRun({ soft: true, softMarker: false });
  assert.equal(classic.result.passed, false);
  assert.ok(classic.result.errors.some((error) => error.includes('software-transport')));
  const softLabelled = await simulatedRun({ softMarker: true });
  assert.equal(softLabelled.result.passed, false);
  assert.ok(softLabelled.result.errors.some((error) => error.includes('did not confirm')));
});

test('Missing tool, wrong GID, payload mismatch, timeout, missing trace, wrong warmup and duplicate operation fail closed', async () => {
  for (const options of [{ missingTool: true }, { badGid: true }, { badPayload: true }, { timeout: true },
    { badTrace: true }, { summaryFault: 'warmup' }, { duplicate: true }, { badMode: true }]) {
    const { result, stopped } = await simulatedRun(options);
    assert.equal(result.passed, false, JSON.stringify(options)); assert.ok(result.errors.length);
    assert.deepEqual(stopped, options.missingTool ? [] : ['a', 'b']);
  }
});

test('Either endpoint cleanup failure invalidates an otherwise successful Linux test', async () => {
  for (const side of ['a', 'b']) {
    const { result, stopped } = await simulatedRun({ cleanupFailure: side });
    assert.equal(result.passed, false); assert.equal(result.cleanupVerified, false);
    assert.ok(result.errors.some((error) => error.includes('cleanup'))); assert.deepEqual(stopped, ['a', 'b']);
  }
});

test('Wrong platform or changed tool hash fails before any NIC test process starts', async () => {
  for (const options of [{ wrongPlatform: true }, { toolChanged: true }]) {
    const { result, commands, stopped } = await simulatedRun(options);
    assert.equal(result.passed, false); assert.deepEqual(commands, []); assert.deepEqual(stopped, []);
  }
});

test('Linux test resolves missing neighbours through bounded ordinary kernel NDP before starting RDMA', async () => {
  for (const pingNoReply of [false, true]) {
    const { result, commands, hostCommands } = await simulatedRun({ missingNeighbours: true, pingNoReply });
    assert.equal(result.passed, true, result.errors.join('; ')); assert.equal(commands.length, 2);
    assert.equal(hostCommands.filter((command) => command.startsWith('ping -6 -c 1 -W 2 -I ')).length, 2);
    assert.equal(hostCommands.filter((command) => command.startsWith('ip -6 neigh show dev ')).length, 2);
    assert.equal(hostCommands.some((command) => /sudo|neigh replace|systemctl/.test(command)), false);
    for (const side of ['a', 'b']) {
      assert.equal(result.resolvedNeighbours[side].source, 'kernel-ndp');
      assert.equal(result.resolvedNeighbours[side].state, 'REACHABLE');
      assert.equal(result.resolvedNeighbours[side].mode, 'dynamic');
      assert.equal(result.resolvedNeighbours[side].permanent, false);
      assert.equal(result.resolvedNeighbours[side].pingExitCode, pingNoReply ? 1 : 0);
    }
  }
});

test('Failed or mismatched NDP never starts an RDMA process', async () => {
  for (const failure of ['ndpUnavailable', 'ndpTimedOut', 'ndpQueryFailure', 'ndpFailed', 'ndpWrongGid', 'ndpWrongMac', 'ndpWrongInterface', 'ndpAmbiguous']) {
    const { result, commands, stopped } = await simulatedRun({ missingNeighbours: true, [failure]: true });
    assert.equal(result.passed, false, failure); assert.ok(result.errors.length);
    assert.deepEqual(commands, []); assert.deepEqual(stopped, []);
  }
});

test('Existing exact permanent neighbour observations avoid resolution and report their mode', async () => {
  const { result, hostCommands } = await simulatedRun();
  assert.equal(result.passed, true); assert.equal(result.resolvedNeighbours.a.source, 'discovery');
  assert.equal(result.resolvedNeighbours.a.permanent, true); assert.equal(result.resolvedNeighbours.b.mode, 'permanent');
  assert.equal(hostCommands.some((command) => command.startsWith('ping ')), false);
});

test('Stock mode evidence accepts the actual Mellanox OUI or legacy PCI ID and refuses unknown or duplicate vendor markers', async () => {
  for (const legacyVendor of [false, true]) {
    const { result } = await simulatedRun({ legacyVendor }); assert.equal(result.passed, true, result.errors.join('; '));
  }
  for (const options of [{ badVendor: true }, { duplicateMode: true }]) {
    const { result } = await simulatedRun(options); assert.equal(result.passed, false);
    assert.ok(result.errors.some((error) => error.includes('stock-initiator mode')));
  }
});

test('Both Linux endpoints must provide unique exact hardware and stock-role evidence', async () => {
  for (const options of [{ badBMode: true }, { badBVendor: true }, { wrongBRole: true }, { duplicateBMode: true }]) {
    const { result, stopped } = await simulatedRun(options);
    assert.equal(result.passed, false); assert.deepEqual(stopped, ['a', 'b']);
    assert.ok(result.errors.some((error) => error.includes('stock-responder mode')));
  }
});

test('Settings changed after discovery fail before the test starts', async () => {
  const { result, commands } = await simulatedRun({ settingsChanged: true });
  assert.equal(result.passed, false); assert.deepEqual(commands, []);
  assert.ok(result.errors.some((error) => error.includes('settings changed')));
});
