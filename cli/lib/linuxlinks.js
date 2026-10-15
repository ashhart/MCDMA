'use strict';
// Stock Linux verbs links. GPU access and CPU-free submission are separate capabilities.
const crypto = require('crypto');
const exec = require('./exec');
const { normMac, normIp6 } = require('./parse');
const { descriptor, parseLatency } = require('./testrun');
const { readTrace } = require('./verification');

const q = (value) => "'" + String(value).replace(/'/g, "'\\''") + "'";
const safeId = (value) => typeof value === 'string' && /^[A-Za-z0-9_.-]{1,80}$/.test(value);
const usableStates = new Set(['PERMANENT', 'REACHABLE', 'STALE', 'DELAY', 'PROBE']);
const knownStates = new Set([...usableStates, 'FAILED', 'INCOMPLETE', 'NOARP', 'NONE']);
const isLinkLocal = (value) => {
  const ip = normIp6(value);
  return !!ip && (parseInt(ip.split(':')[0], 16) & 0xffc0) === 0xfe80;
};

function usableNeighbour(end, peer) {
  if (!peer.gid || !peer.mac) return null;
  const matches = end.neighbours.filter((item) => normIp6(item.addr) === peer.gid && normMac(item.lladdr) === peer.mac)
    .map((item) => {
      const states = String(item.state || (item.permanent ? 'PERMANENT' : '')).toUpperCase().split(/\s+/).filter((state) => knownStates.has(state));
      if (states.length !== 1 || !usableStates.has(states[0])) return null;
      const state = states[0];
      return { addr: peer.gid, lladdr: peer.mac, state, permanent: state === 'PERMANENT', mode: state === 'PERMANENT' ? 'permanent' : 'dynamic' };
    }).filter(Boolean);
  return matches.length === 1 ? matches[0] : null;
}

function observedNeighbour(text, end, peer) {
  const neighbours = [];
  for (const line of String(text || '').trim().split(/\r?\n/)) {
    const words = line.trim().split(/\s+/), device = words.indexOf('dev'), address = words.indexOf('lladdr');
    if (device >= 0 && words[device + 1] !== end.iface) continue;
    if (address < 0) continue;
    const states = words.filter((word) => knownStates.has(word));
    neighbours.push({ addr: words[0], lladdr: words[address + 1], state: states.join(' ') });
  }
  return usableNeighbour({ neighbours }, peer);
}

async function resolveNeighbour(host, end, peer) {
  const cached = usableNeighbour(end, peer);
  if (cached) return { ...cached, source: 'discovery' };
  const ping = await host.sh(`ping -6 -c 1 -W 2 -I ${q(end.iface)} ${q(peer.gid)}`, { timeoutMs: 10000 });
  if (ping.timedOut || ![0, 1].includes(ping.code)) throw new Error(`${end.node}: IPv6 neighbour discovery probe unavailable or failed`);
  const query = await host.sh(`ip -6 neigh show dev ${q(end.iface)} to ${q(peer.gid)}`, { timeoutMs: 10000 });
  if (query.code !== 0 || query.timedOut) throw new Error(`${end.node}: cannot inspect IPv6 neighbour discovery`);
  const resolved = observedNeighbour(query.out, end, peer);
  if (!resolved) throw new Error(`${end.node}: IPv6 neighbour did not resolve to the selected peer MAC in a usable state`);
  return { ...resolved, source: 'kernel-ndp', pingExitCode: ping.code };
}

function endpoint(peer, iface) {
  const port = (peer.ports || []).find((item) => item.iface === iface) || {};
  const portNumber = port.rdmaPort ?? (peer.rdmaLinks || []).find((item) => item.netdev === iface && item.device === port.rdmaDevice)?.port;
  const gids = (port.gids || []).filter((gid) => Number.isInteger(gid.index) && gid.index >= 0 &&
    gid.index <= 255 && /RoCE v2/.test(gid.type || '') && isLinkLocal(gid.addr) && (!gid.ndev || gid.ndev === iface) &&
    (!Array.isArray(port.addrs) || port.addrs.some((addr) => normIp6(addr) === normIp6(gid.addr))));
  const selectedEntries = gids.filter((gid) => gid.index === port.gidIndex && normIp6(gid.addr) === normIp6(port.gid));
  const selected = selectedEntries.length === 1 ? selectedEntries[0] : null;
  return {
    node: peer.id, host: peer.host, iface, rdmaDevice: port.rdmaDevice || null,
    rdmaPort: Number.isInteger(portNumber) && portNumber > 0 && portNumber <= 255 ? portNumber : null,
    gidIndex: selected ? selected.index : null, gid: selected ? normIp6(selected.addr) : null,
    mac: normMac(port.mac), arch: peer.arch || null, bootId: peer.bootId || null, peerTools: peer.peerTools || [],
    peerToolHashes: peer.peerToolHashes || {}, link: !!peer.reachable && !!port.link,
    neighbours: port.neighbours || []
  };
}

function linuxLinkIdentity(a, b, test = {}) {
  if (![a, b].every((end) => end.host && end.rdmaDevice && end.rdmaPort && end.gid && end.gidIndex !== null && end.mac)) return null;
  const identity = (end) => ({ node: end.node, host: end.host, iface: end.iface,
    rdmaDevice: end.rdmaDevice, rdmaPort: end.rdmaPort, gidIndex: end.gidIndex, gid: end.gid,
    mac: end.mac, arch: end.arch, bootId: end.bootId, peerTools: [...end.peerTools].sort(),
    peerToolHashes: Object.fromEntries(Object.entries(end.peerToolHashes).sort(([x], [y]) => x.localeCompare(y))) });
  return crypto.createHash('sha256').update(JSON.stringify({ schema: 2, backend: 'stock-linux-verbs', a: identity(a), b: identity(b),
    test: { payload: test.payload ?? 4096, mtu: test.mtu ?? 1024, iterations: test.iterations ?? 1000 } })).digest('hex');
}

function buildLinuxLinks({ peers, savedLinks = [], lastTests = {}, test = {} }) {
  const byNode = new Map((peers || []).map((peer) => [peer.id, peer]));
  const links = [], used = new Set(), ids = new Set();
  function add(record, reason) {
    if (!safeId(record.id) || !record.a || !record.b || record.a.node === record.b.node ||
        !byNode.has(record.a.node) || !byNode.has(record.b.node) ||
        !safeId(record.a.iface) || !safeId(record.b.iface)) throw new Error('Invalid saved Linux link');
    const keyA = `${record.a.node}/${record.a.iface}`, keyB = `${record.b.node}/${record.b.iface}`;
    if (ids.has(record.id) || used.has(keyA) || used.has(keyB)) throw new Error('Linux link IDs and ports must be unique');
    ids.add(record.id); used.add(keyA); used.add(keyB);
    const a = endpoint(byNode.get(record.a.node), record.a.iface), b = endpoint(byNode.get(record.b.node), record.b.iface);
    const hasGid = (end) => !!end.gid && end.gidIndex !== null && !!end.rdmaDevice && !!end.rdmaPort && !!end.mac;
    const aNeighbour = usableNeighbour(a, b), bNeighbour = usableNeighbour(b, a);
    const persisted = (end, peer) => {
      const persist = byNode.get(end.node).persist || {};
      return !!persist.service && String(persist.conf || '').split(/\r?\n/).some((line) => {
        const fields = line.trim().split(/\s+/);
        return fields[0] === end.iface && normIp6(fields[1]) === peer.gid && normMac(fields[2]) === peer.mac;
      });
    };
    const identity = linuxLinkIdentity(a, b, test), lastTest = lastTests[record.id];
    const status = { aGid: hasGid(a), bGid: hasGid(b), aNeighbour: !!aNeighbour, bNeighbour: !!bNeighbour,
      aNeighbourMode: aNeighbour && aNeighbour.mode, bNeighbourMode: bNeighbour && bNeighbour.mode,
      aPersisted: persisted(a, b), bPersisted: persisted(b, a),
      portsActive: a.link && b.link, lastTest: identity && lastTest && lastTest.identity === identity ? lastTest : null };
    status.configured = status.aGid && status.bGid && status.aNeighbour && status.bNeighbour;
    status.ready = status.configured && status.portsActive;
    links.push({ id: record.id, type: 'linux', reason, a, b, status, identity });
  }
  for (const record of savedLinks) add(record, 'saved');
  const cables = new Map();
  for (const peer of peers || []) for (const port of peer.ports || []) {
    if (port.primary === false || !port.cable || !port.cable.sn || used.has(`${peer.id}/${port.iface}`)) continue;
    const ends = cables.get(port.cable.sn) || [];
    ends.push({ node: peer.id, iface: port.iface }); cables.set(port.cable.sn, ends);
  }
  for (const ends of cables.values()) {
    if (ends.length !== 2 || ends[0].node === ends[1].node ||
        !ends.some((end) => byNode.get(end.node).kind === 'linux')) continue;
    ends.sort((x, y) => `${x.node}/${x.iface}`.localeCompare(`${y.node}/${y.iface}`));
    const key = ends.map((end) => `${end.node}/${end.iface}`).join('|');
    add({ id: 'linux-' + crypto.createHash('sha256').update(key).digest('hex').slice(0, 16), a: ends[0], b: ends[1] }, 'cable');
  }
  return links;
}

function linuxNeighbourEntries(links) {
  const grouped = {};
  for (const link of links) for (const [end, peer] of [[link.a, link.b], [link.b, link.a]]) {
    if (!isLinkLocal(peer.gid) || !normMac(peer.mac)) continue;
    const entries = grouped[end.node] || (grouped[end.node] = []);
    if (!entries.some((item) => item.iface === end.iface && item.studioLinkLocal === peer.gid))
      entries.push({ iface: end.iface, studioLinkLocal: peer.gid, studioMac: peer.mac });
  }
  return grouped;
}

async function findPeerTool(host, end, settings) {
  const explicit = settings.tools && settings.tools.linuxPeerPaths && settings.tools.linuxPeerPaths[end.node];
  const paths = explicit ? [explicit] : [...new Set(['/usr/local/libexec/mcdma/verbs-peer', ...end.peerTools])];
  for (const pathname of paths) {
    if (typeof pathname !== 'string' || !pathname.startsWith('/') || /[\r\n\x00]/.test(pathname)) throw new Error('Linux peer tool must have an absolute path');
    const check = await host.sh(`test -x ${q(pathname)} && sha256sum -- ${q(pathname)} && ${q(pathname)} --version`, { timeoutMs: 10000 });
    const hash = check.code === 0 && /^([a-f0-9]{64})\s/.exec(check.out || '');
    if (hash) {
      if ((check.out || '').trim().split(/\r?\n/).at(-1) !== 'MCDMA_VERBS_PEER abi=1 platform=linux stock_initiator=1 stock_responder=1')
        throw new Error(`${end.node}: incompatible Linux verbs-peer tool; install the matching native build`);
      if (end.peerToolHashes[pathname] && end.peerToolHashes[pathname] !== hash[1]) throw new Error(`${end.node}: Linux peer tool changed since discovery; refresh before testing`);
      return { path: pathname, sha256: hash[1] };
    }
  }
  throw new Error(`${end.node}: no installed Linux verbs-peer tool; build/install it natively for ${end.arch || 'this host'}`);
}

function checkedDescriptor(line, end) {
  const desc = descriptor(line);
  if (!desc || !/^[0-9]+$/.test(desc.addr) || !/^[0-9]+$/.test(desc.len) ||
      !/^[0-9]+$/.test(desc.rkey) || !/^[0-9]+$/.test(desc.qpn) || !/^[0-9]+$/.test(desc.psn) ||
      Number(desc.qpn) < 1 || Number(desc.qpn) > 0xffffff || Number(desc.psn) > 0xffffff ||
      Number(desc.rkey) < 1 || Number(desc.rkey) > 0xffffffff || Number(desc.len) < 16384 ||
      BigInt(desc.addr) + BigInt(desc.len) > 0xffffffffffffffffn || desc.gid !== end.gid)
    throw new Error(`${end.node}: invalid endpoint descriptor or unexpected GID`);
  return desc;
}

function checkedLatency(line, payload) {
  const parsed = parseLatency(line);
  if (!parsed || parsed.bytes !== payload || parsed.samples !== 1000 || !/ samples=1000 warmup=100 qd=1 /.test(line) ||
      !['min', 'median', 'p95', 'p99', 'max', 'mean'].every((key) => Number.isFinite(parsed[key]) && parsed[key] >= 0))
    throw new Error('Linux latency summary does not match the requested test');
  return { ...parsed, warmup: 100, queueDepth: 1 };
}

async function runLinuxTransferTest({ link, settings, onProgress = () => {}, latency = true }) {
  const endpoints = [], errors = [], log = [];
  let cleanupSucceeded = true;
  const note = (message) => { log.push(message); onProgress(message); };
  const result = { at: Date.now(), link: link && link.id, type: 'linux', identity: link && link.identity,
    passed: false, errors, log, latency: null, boundary: 'cpu-post-to-cq-observed', hardwareGPU: false,
    cpuSubmission: true, submission: 'cpu', gpuNICMMIO: false, payload: settings.test && settings.test.payload, mtu: settings.test && settings.test.mtu };
  try {
    if (!link || link.type !== 'linux' || !link.status.aGid || !link.status.bGid || !link.status.portsActive || !link.identity)
      throw new Error('Select a Linux RDMA link with active ports and observed link-local RoCE v2 GIDs');
    const config = settings.test || {};
    const soft = config.soft === true;
    result.softTransport = soft;
    if (![1024, 4096].includes(config.payload) || ![1024, 4096].includes(config.mtu) ||
        (latency && config.iterations !== undefined && config.iterations !== 1000)) throw new Error('Linux tests require 1 KiB/4 KiB payload, MTU 1024/4096 and exactly 1000 latency samples');
    if (linuxLinkIdentity(link.a, link.b, config) !== link.identity) throw new Error('Linux test settings changed since discovery; refresh before testing');
    const hosts = { a: new exec.Host('ssh', link.a.host), b: new exec.Host('ssh', link.b.host) };
    result.tools = {};
    for (const side of ['a', 'b']) result.tools[side] = await findPeerTool(hosts[side], link[side], settings);
    result.resolvedNeighbours = {};
    for (const side of ['a', 'b']) {
      const peer = link[side === 'a' ? 'b' : 'a'];
      result.resolvedNeighbours[side] = await resolveNeighbour(hosts[side], link[side], peer);
    }
    const command = (side) => {
      const end = link[side];
      return `exec env -u IBV_DRIVERS -u MCDMA_CQ_MAP -u MCDMA_USER_POST -u MCDMA_USER_BF MCDMA_PAYLOAD_BYTES=${config.payload} MCDMA_PATH_MTU=${config.mtu} MCDMA_RDMA_PORT=${end.rdmaPort} MCDMA_LATENCY_PROFILE=0${soft ? ' MCDMA_SOFT_TRANSPORT=1' : ''} ${q(result.tools[side].path)} ${q(end.rdmaDevice)} ${end.gidIndex} ${side === 'a' ? 'stock-initiator' : 'stock-responder'}`;
    };
    for (const side of ['a', 'b']) { note(`Starting stock Linux verbs on ${link[side].node}`); endpoints.push(new exec.Endpoint(hosts[side], command(side))); }
    const [a, b] = endpoints;
    const local = checkedDescriptor(await a.line(25000), link.a), remote = checkedDescriptor(await b.line(25000), link.b);
    a.send(`${remote.qpn} ${remote.psn} ${remote.gid}`); b.send(`${local.qpn} ${local.psn} ${local.gid}`);
    if (await a.line(20000) !== 'READY' || await b.line(20000) !== 'READY') throw new Error('Linux QP did not reach RTS');
    const p = config.payload, measured = { a: {}, b: {} };
    a.send(`${latency ? 'INITIATEBENCH' : 'INITIATE'} ${remote.rkey} ${remote.addr} ${remote.len}`);
    if (await a.line(60000) !== `NATIVE_FORWARD write=1 read=1 verified=${p}`) throw new Error('First Linux initiator failed WRITE/READ byte verification');
    if (latency) {
      for (let i = 0; i < 2; i++) { const row = checkedLatency(await a.line(60000), p); if (measured.a[row.op]) throw new Error('Duplicate latency operation'); measured.a[row.op] = row; }
      result.traces = { a: await readTrace(hosts.a, await a.line(10000), p) };
    }
    b.send(`${latency ? 'ROUNDTRIPBENCH' : 'ROUNDTRIP'} ${local.rkey} ${local.addr} ${local.len}`);
    if (await b.line(60000) !== `PEER_RESULT forward=${p} write=1 read=1 reverse=${p}`) throw new Error('Second Linux initiator failed WRITE/READ byte verification');
    if (latency) {
      for (let i = 0; i < 2; i++) { const row = checkedLatency(await b.line(60000), p); if (measured.b[row.op]) throw new Error('Duplicate latency operation'); measured.b[row.op] = row; }
      result.traces.b = await readTrace(hosts.b, await b.line(10000), p);
    }
    a.send('CHECKREVERSE');
    if (await a.line(20000) !== `NATIVE_REVERSE verified=${p}`) throw new Error('First Linux endpoint did not verify the reverse payload');
    for (const [side, process, role] of [['a', a, 'stock-initiator'], ['b', b, 'stock-responder']]) {
      const markers = (process.stderr || '').split(/\r?\n/).filter((line) => line.startsWith('LINUX_PEER_CONFIG '));
      // A soft run requires the peer's soft_transport evidence with its real vendor id; a
      // normal run still requires Mellanox hardware. An unlabelled marker never passes as soft.
      const marker = new RegExp(`^LINUX_PEER_CONFIG backend=stock-libibverbs${soft ? ' soft_transport=1 vendor_id=(0x[0-9a-f]+)' : ' vendor_id=(0x2c9|0x15b3)'} rdma_port=${link[side].rdmaPort} role=${role}$`);
      const matched = markers.length === 1 ? marker.exec(markers[0]) : null;
      if (!matched) throw new Error(`${side}: Linux peer did not confirm the ${role}${soft ? ' software-transport' : ''} mode`);
      if (soft) result.softVendorId = parseInt(matched[1], 16);
    }
    if (latency && !['a', 'b'].every((side) => measured[side].write && measured[side].read)) throw new Error('Incomplete Linux latency summaries');
    result.latency = latency ? measured : null;
    result.passed = true;
  } catch (error) { errors.push(error.message || String(error)); note(`Failed: ${error.message || error}`); }
  finally {
    for (let i = 0; i < endpoints.length; i++) {
      try { const code = await endpoints[i].stop(); if (code !== 0) { cleanupSucceeded = false; errors.push(`${i === 0 ? 'a' : 'b'}: endpoint cleanup exit ${code}`); } }
      catch (error) { cleanupSucceeded = false; errors.push(`Endpoint cleanup failed: ${error.message || error}`); }
    }
  }
  result.passed = result.passed && errors.length === 0;
  result.cleanupVerified = endpoints.length === 2 && cleanupSucceeded;
  if (result.passed) note('Both Linux initiators verified READ and WRITE byte for byte');
  return result;
}

module.exports = { buildLinuxLinks, linuxNeighbourEntries, runLinuxTransferTest, linuxLinkIdentity };
