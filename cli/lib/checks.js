'use strict';
const { compareVersions } = require('./version');
// Turns the probe results into the setup checklist. Pure: no I/O.
const STATUS_RANK = { fail: 0, warn: 1, todo: 2, unknown: 3, ok: 4 };
const worst = (list) => list.reduce((w, s) => (STATUS_RANK[s] < STATUS_RANK[w] ? s : w), 'ok');

function item(label, value, status = 'ok', hint = null) { return { label, value, status, hint }; }
function action(id, label, opts = {}) { return { id, label, kind: opts.kind || 'normal', primary: !!opts.primary, hint: opts.hint || null }; }
const plural = (n, w) => `${n} ${w}${n === 1 ? '' : 's'}`;

function systemCheck(studio, pkg) {
  if (!studio || !studio.ok) return { id: 'system', title: 'macOS & security', status: 'unknown', summary: studio && studio.error ? studio.error : 'Not checked yet', items: [], actions: [] };
  const items = [], actions = [];
  const need = (pkg && pkg.available && pkg.requiresMacOSMajor) || (studio.kext && studio.kext.requiresMacOSMajor) || null;
  const requiredBuild = pkg && pkg.available && pkg.manifest && pkg.manifest.requires && pkg.manifest.requires.macos_build;
  const osOk = requiredBuild ? studio.os.build === requiredBuild : need ? studio.os.major >= need : true;
  items.push(item('macOS', `${studio.os.version} (${studio.os.build})`, osOk ? 'ok' : 'fail',
    requiredBuild ? `This package requires macOS build ${requiredBuild}.` : need && !osOk ? `This driver build is linked against the macOS ${need} kernel and cannot load on ${studio.os.version}.` : need ? `Driver build requires macOS ${need} or later.` : null));
  items.push(item('Mac', `${studio.chip.brand || studio.chip.arch}${studio.chip.memoryGiB ? ` · ${studio.chip.memoryGiB} GB` : ''}`, studio.appleSilicon ? 'ok' : 'fail', studio.appleSilicon ? null : 'MCDMA needs an Apple silicon Mac.'));
  const sip = studio.sip.state;
  let polStatus, polHint;
  if (studio.loaded.loaded) { polStatus = 'ok'; polHint = 'The driver is loaded, so the security policy already allows it.'; }
  else if (sip === 'disabled') { polStatus = 'ok'; polHint = 'System Integrity Protection is off (Permissive Security), so an ad-hoc-signed kernel extension can load.'; }
  else if (sip === 'enabled') { polStatus = 'warn'; polHint = 'With SIP on, this ad-hoc-signed driver will not load. Shut down, hold the power button to reach Recovery, open Startup Security Utility, choose Reduced Security and allow user management of kernel extensions; for an ad-hoc build also run "csrutil disable" in the Recovery Terminal, then restart.'; }
  else { polStatus = 'unknown'; polHint = 'Could not read the SIP state.'; }
  items.push(item('Kernel extension policy', sip === 'disabled' ? 'SIP disabled (Permissive Security)' : sip === 'enabled' ? 'SIP enabled' : 'unknown', polStatus, polHint));
  items.push(item('Kernel extension user consent', studio.kextConsent, studio.kextConsent === 'enabled' ? 'ok' : 'warn', 'macOS asks for approval in System Settings the first time a new driver loads.'));
  if (studio.bootPolicy) items.push(item('Boot security policy', `${studio.bootPolicy.mode || '?'} · third-party kexts ${studio.bootPolicy.kexts || '?'}`, /permissive|reduced/i.test(studio.bootPolicy.mode || '') ? 'ok' : 'warn'));
  if (studio.hostKind === 'local') actions.push(action('bootPolicy', 'sudo bputil -d', { kind: 'admin', hint: 'Reports Security Mode and third-party kernel extension status.' }));
  if (studio.thunderbolt.length) items.push(item('Thunderbolt devices', studio.thunderbolt.map((e) => `${e.name}${e.receptacle ? ` on port ${e.receptacle}` : ''}${e.speed ? ` · ${e.speed}` : ''}`).join(', '), 'ok'));
  const status = worst(items.map((i) => i.status));
  const summary = status === 'ok' ? `macOS ${studio.os.version} on ${studio.chip.brand || 'Apple silicon'}; the policy allows the driver` : status === 'fail' ? items.find((i) => i.status === 'fail').hint : 'The security policy needs attention before the driver can load';
  return { id: 'system', title: 'macOS & security', status, summary, items, actions };
}

function hardwareCheck(studio) {
  if (!studio || !studio.ok) return { id: 'hardware', title: 'ConnectX hardware', status: 'unknown', summary: 'Not checked yet', items: [], actions: [] };
  const items = [], actions = [];
  const cards = studio.pci.cards;
  if (!cards.length) {
    items.push(item('Mellanox device', 'none found', 'fail', 'No Mellanox PCI device is visible. Check that the Thunderbolt enclosure is connected and powered, then re-check.'));
    return { id: 'hardware', title: 'ConnectX hardware', status: 'fail', summary: 'No ConnectX card found', items, actions: [action('recheck', 'mcdma status')] };
  }
  for (const c of cards) {
    items.push(item(c.name, `${plural(c.functions.length, 'port')} · PCIe ${c.linkWidth || '?'} ${c.linkSpeed || ''}${c.enclosure ? ` · ${c.enclosure.name}${c.enclosure.receptacle ? ` on Thunderbolt port ${c.enclosure.receptacle}` : ''}` : c.tunnelled ? ' · Thunderbolt' : ' · internal PCIe'}`, c.supported ? 'ok' : 'warn',
      c.supported ? null : `The MCDMA driver matches ConnectX-5 Ex (0x1019) and ConnectX-4 Lx (0x1015) only; this card is ${c.deviceId}.`));
    for (const f of c.functions) {
      const p = studio.ports.find((x) => x.pci === f.pci) || {};
      items.push(item(`Port ${f.pci}`, `${f.linkUp ? 'PCIe link up' : 'PCIe link down'}${p.iface ? ` · ${p.iface}` : ''}${p.mac ? ` · ${p.mac}` : ''}${p.portActive ? ' · QSFP link up' : p.iface ? ' · QSFP link down' : ''}`,
        f.linkUp ? (p.iface && !p.portActive ? 'warn' : 'ok') : 'warn', p.iface && !p.portActive ? 'No link on the QSFP port: check the cable and the Spark port.' : null));
    }
  }
  const status = worst(items.map((i) => i.status));
  const total = cards.reduce((a, c) => a + c.functions.length, 0);
  const enc = cards[0].enclosure;
  return { id: 'hardware', title: 'ConnectX hardware', status, summary: `${cards.length === 1 ? `One ${cards[0].name}` : `${cards.length} cards`} with ${plural(total, 'port')}${enc ? ` in ${enc.matched ? `a ${enc.name}` : 'a Thunderbolt enclosure'}${enc.receptacle ? ` on Thunderbolt port ${enc.receptacle}` : ''}` : ''}`, items, actions };
}

function driverCheck(studio, pkg) {
  if (!studio || !studio.ok) return { id: 'driver', title: 'MCDMA driver', status: 'unknown', summary: 'Not checked yet', items: [], actions: [] };
  const items = [], actions = [];
  const local = studio.hostKind === 'local';
  const k = studio.kext, l = studio.loaded, pr = studio.provider;
  const pkgV = pkg && pkg.available ? pkg.version : null;
  const pending = studio.installState || null;
  items.push(item('Driver package', pkg && pkg.available ? `${pkg.version}${pkg.uuid ? ` · ${pkg.uuid.slice(0, 8)}…` : ''} · ${pkg.source}` : 'none found', pkg && pkg.available ? 'ok' : 'warn',
    pkg && pkg.available ? null : 'No driver package found. Build one with npm run package, or point settings.driverPackage at a folder containing manifest.json and the archive (mcdma settings set driverPackage /path).'));
  if (!k.installed) {
    items.push(item('Installed', 'not installed', 'todo', 'Install copies the kernel extension, the libibverbs provider and the tools, then asks macOS to load the driver.'));
    if (pkg && pkg.available && local) actions.push(action('installDriver', 'mcdma driver install', { kind: 'admin', primary: true }));
  } else {
    const newer = pkgV && compareVersions(pkgV, k.version) === 1;
    items.push(item('Installed', `${k.version} · ${k.signature}${k.sha256 ? ` · ${k.sha256.slice(0, 8)}…` : ''}`, newer ? 'warn' : 'ok', newer ? `The package is ${pkgV}; ${k.version} is installed.` : null));
    if (newer && local && pkg.available) actions.push(action('installDriver', `mcdma driver install (update to ${pkgV})`, { kind: 'admin', primary: true }));
  }
  if (l.loaded) {
    const mism = k.installed && k.version && l.version !== k.version;
    items.push(item('Loaded', `${l.version} · ${l.uuid}`, mism ? 'warn' : 'ok', mism ? `The kernel still runs ${l.version}; restart to activate ${k.version}.` : null));
    if (mism && local) actions.push(action('restart', 'restart the Mac', { kind: 'admin' }));
  } else if (k.installed) {
    const approve = pending && /approv|policy|consent|not allowed|reboot|restart/i.test(pending.loadLog || '');
    items.push(item('Loaded', 'not loaded', 'todo', approve
      ? 'macOS is waiting for you: open System Settings → Privacy & Security, click Allow next to the MCDMA driver, then restart; mcdma enable continues afterwards.'
      : 'Ask macOS to load the driver (mcdma driver load). The first load needs approval in System Settings → Privacy & Security followed by a restart.'));
    if (local) {
      if (approve) actions.push(action('openSecurity', 'allow it in System Settings → Privacy & Security, then restart'));
      else actions.push(action('loadDriver', 'mcdma driver load', { kind: 'admin', primary: true }));
    }
  } else {
    items.push(item('Loaded', 'not loaded', 'todo'));
  }
  items.push(item('libibverbs provider', pr.present ? `installed${pr.confPresent ? ' · registered' : ' · not registered'}${pr.sha256 ? ` · ${pr.sha256.slice(0, 8)}…` : ''}` : 'missing', pr.present && pr.confPresent ? 'ok' : 'todo',
    pr.present && pr.confPresent ? null : 'The userspace provider (libmcdma-rdmav34.so) and its /etc/libibverbs.d entry are installed together with the driver.'));
  if (k.installed && (!pr.present || !pr.confPresent) && local && pkg && pkg.available && [0, 1].includes(compareVersions(pkgV, k.version))) actions.push(action('installDriver', 'mcdma driver install (repair)', { kind: 'admin' }));
  const regs = studio.registry;
  if (l.loaded) {
    if (!regs.length) items.push(item('Driver instances', 'none attached', 'fail', 'The driver is loaded but not attached to any ConnectX function.'));
    for (const r of regs) {
      const bad = r.quarantined || r.startError;
      items.push(item(`Instance ${r.iface || r.pci}`, bad ? (r.quarantined ? 'quarantined' : `start error ${r.startError}`) : `${r.portActive ? 'port active' : 'port down'} · ${r.gidLive ? 'GID live' : 'no GID'} · MTU ${r.ethernetMtu || '?'}${r.userQueues ? ' · user queues' : ''}${r.blueFlame ? ' · BlueFlame' : ''}`,
        bad ? 'fail' : r.portActive && r.gidLive ? 'ok' : 'warn', bad ? 'See the driver log; a restart usually clears a quarantine.' : !r.gidLive ? 'The port needs its link-local address before RDMA can use it (mcdma configure).' : !r.portActive ? 'No QSFP link.' : null));
    }
    const devs = studio.mcdmaDevices;
    items.push(item('RDMA devices (libibverbs)', devs.length ? devs.map((d) => `${d.name} ${d.ports[0] && d.ports[0].active ? 'active' : d.ports[0] ? d.ports[0].state : ''}`).join(', ') : 'none', devs.length ? 'ok' : 'warn',
      devs.length ? null : 'ibv_devinfo does not list the MCDMA devices: the provider is missing or not registered.'));
  }
  const status = worst(items.map((i) => i.status));
  const summary = l.loaded ? (status === 'ok' ? `Driver ${l.version} loaded and attached to ${plural(regs.length, 'port')}` : `Driver ${l.version} loaded, attention needed`)
    : k.installed ? `Driver ${k.version} installed, waiting to load` : 'Driver not installed';
  return { id: 'driver', title: 'MCDMA driver', status, summary, items, actions };
}

function sparksCheck(sparks) {
  const items = [], actions = [action('addSpark', 'mcdma sparks add HOST')];
  if (!sparks.length) {
    items.push(item('Sparks', 'none configured', 'todo', 'Add each DGX Spark by its ssh host name (mcdma sparks add HOST). Key-based ssh as root (or a sudo-capable user) is required.'));
    return { id: 'sparks', title: 'Sparks', status: 'todo', summary: 'No Sparks added yet', items, actions };
  }
  for (const s of sparks) {
    if (!s.reachable) { items.push(item(s.id, `unreachable · ${s.error || 'ssh failed'}`, 'fail', `Check that "ssh ${s.host}" works from a terminal without a password.`)); continue; }
    const rdmaPorts = (s.ports || []).filter((p) => p.primary);
    const up = rdmaPorts.filter((p) => p.link).length;
    const missing = ['ethtool', 'rdma', 'ip'].filter((t) => !(s.tools || {})[t]);
    items.push(item(`${s.id} (${s.hostname || s.host})`, `${s.os || 'Linux'} · ${(s.gpus || []).length ? s.gpus.join(', ') : 'no GPU seen'} · ${plural(rdmaPorts.length, 'RDMA port')}, ${up} up${s.root ? '' : ' · not root'}`,
      missing.length ? 'warn' : s.root ? 'ok' : 'warn', missing.length ? `Missing tools: ${missing.join(', ')}` : s.root ? null : 'Neighbour configuration on the Spark needs root over ssh.'));
    if (!(s.peerTools || []).length) items.push(item(`${s.id} test tool`, 'verbs-peer not found', 'warn', 'The transfer test needs the verbs-peer tool on the Spark; mcdma enable installs it from the driver package.'));
  }
  const status = worst(items.map((i) => i.status));
  const reach = sparks.filter((s) => s.reachable).length;
  return { id: 'sparks', title: 'Sparks', status, summary: `${reach} of ${plural(sparks.length, 'Spark')} reachable`, items, actions };
}

function topologyCheck(topo) {
  const items = [], actions = [];
  if (!topo || !topo.studioPorts.length) return { id: 'topology', title: 'Topology', status: 'unknown', summary: 'Needs the card and at least one Spark', items, actions };
  if (!topo.candidates.length) {
    items.push(item('Spark ports', 'none available', 'todo', 'No Spark port with a free cable was found. Cable a QSFP port of the card in the Mac to a Spark ConnectX port.'));
    return { id: 'topology', title: 'Topology', status: 'todo', summary: 'No Spark port available', items, actions };
  }
  for (const l of topo.links) {
    const guessed = l.reason === 'guess';
    items.push(item(`${topo.macs.length > 1 ? `${l.mac.label} ` : ''}${l.studio.iface} → ${l.sparkName} ${l.spark.iface}`, `${l.spark.speedGbps ? `${l.spark.speedGbps}G` : ''}${l.spark.cable ? ` · ${l.spark.cable.pn || ''} ${l.spark.cable.sn || ''}` : ''} · ${l.status.wiringVerified ? 'wiring verified' : guessed ? 'guessed' : l.reason === 'neighbour' ? 'from the current configuration' : 'chosen'}`,
      l.status.wiringVerified ? 'ok' : guessed ? 'warn' : 'ok', guessed ? 'This pairing is a guess. Run mcdma detect, or choose the port with mcdma map.' : null));
  }
  for (const p of topo.unmapped) items.push(item(`${p.iface || p.pci}`, 'not connected to a Spark', p.portActive ? 'warn' : 'todo', p.portActive ? 'The port has a link but no Spark is assigned to it.' : null));
  for (const s of topo.sparkLinks) items.push(item(`${s.a.spark} ↔ ${s.b.spark}`, `${s.speedGbps ? `${s.speedGbps}G` : ''} · ${s.a.iface} ↔ ${s.b.iface} · cable ${s.cable.sn}`, s.up ? 'ok' : 'warn'));
  actions.push(action('detectWiring', 'mcdma detect', { hint: 'Sends a short real RDMA transfer on every Mac-port/Spark-port pair; the pair that answers is the cabled one.' }));
  for (const o of topo.orphans || []) items.push(item(`${o.hostname || o.spark} ${o.iface}`, `link up${o.speedGbps ? ` at ${o.speedGbps}G` : ''}, not used by a known Mac`, 'warn', 'Probably cabled to another Mac. Add that Mac (mcdma macs add HOST) so its links are managed too, or ignore it.'));
  const status = topo.links.length ? worst(items.map((i) => i.status)) : 'todo';
  return { id: 'topology', title: 'Topology', status, summary: topo.links.length ? `${topo.presetName}: ${topo.summary}` : 'Choose which Spark each Mac port connects to (mcdma detect, or mcdma map)', items, actions };
}

function networkCheck(topo, studio) {
  const items = [], actions = [];
  if (!topo || !topo.links.length) return { id: 'network', title: 'Addresses & neighbours', status: 'unknown', summary: 'Waiting for the topology', items, actions };
  const local = studio && studio.hostKind === 'local';
  for (const l of topo.links) {
    const s = l.status;
    items.push(item(`${l.studio.iface} link-local`, l.expected.studioLinkLocal, s.studioAddress ? 'ok' : 'todo', s.studioAddress ? null : 'The Mac port needs its EUI-64 link-local address; the driver mirrors it into the RDMA GID table.'));
    items.push(item(`${l.studio.iface} neighbour → ${l.sparkName}`, `${l.expected.sparkLinkLocal} = ${l.spark.mac}`, s.studioNeighbour ? 'ok' : 'todo', s.studioNeighbour ? null : 'Static IPv6 neighbour on the Mac (the address-only interface cannot resolve it by itself).'));
    items.push(item(`${l.sparkName} ${l.spark.iface} neighbour → Mac`, `${l.expected.studioLinkLocal} = ${l.studio.mac}`, s.sparkNeighbour ? 'ok' : 'todo', s.sparkNeighbour ? null : 'Permanent neighbour on the Spark; it disappears whenever the link flaps, so it is also persisted.'));
    items.push(item(`${l.sparkName} GID`, l.spark.gidIndex != null ? `index ${l.spark.gidIndex} · ${l.spark.gid}` : 'no RoCE v2 GID', s.sparkGid ? 'ok' : 'fail'));
    items.push(item(`${l.studio.iface} persisted`, `${s.studioPersisted ? 'Mac ✓' : 'Mac –'} · ${s.sparkPersisted ? 'Spark ✓' : 'Spark –'}`, s.studioPersisted && s.sparkPersisted ? 'ok' : 'warn', 'Without persistence the entries are lost at reboot (Mac) or at a link flap (Spark).'));
  }
  const need = topo.links.some((l) => !l.status.configured || !l.status.sparkPersisted || (l.mac.kind === 'local' && !l.status.studioPersisted));
  if (need) {
    if (local) actions.push(action('configureNetwork', 'mcdma configure', { kind: 'admin', primary: true, hint: 'Sets this Mac\'s link-local addresses and static neighbours (sudo), the Sparks\' permanent neighbours over ssh, and persists both.' }));
    else actions.push(action('configureSparks', 'mcdma configure (Spark side only from here)', { primary: true, hint: 'The Mac side must be configured on the Mac itself.' }));
  }
  const status = worst(items.map((i) => i.status));
  const ready = topo.links.filter((l) => l.status.configured).length;
  return { id: 'network', title: 'Addresses & neighbours', status, summary: `${ready} of ${plural(topo.links.length, 'link')} configured`, items, actions };
}

function testCheck(topo) {
  const items = [], actions = [];
  if (!topo || !topo.links.length) return { id: 'test', title: 'Transfer test', status: 'unknown', summary: 'Waiting for a configured link', items, actions };
  for (const l of topo.links) {
    const t = l.status.lastTest;
    if (!t) { items.push(item(`${l.studio.iface} ↔ ${l.sparkName}`, 'not tested', l.status.ready ? 'todo' : 'unknown', l.status.ready ? null : 'Configure the link first (mcdma configure).')); continue; }
    const age = Math.round((Date.now() - t.at) / 60000);
    const lat = t.latency && t.latency.mac ? ` · Mac WRITE ${t.latency.mac.write.median} µs, READ ${t.latency.mac.read.median} µs` : '';
    items.push(item(`${l.studio.iface} ↔ ${l.sparkName}`, `${t.passed ? 'passed' : 'failed'} ${age < 1 ? 'just now' : `${age} min ago`}${lat}`, t.passed ? 'ok' : 'fail', t.passed ? null : (t.errors || []).join('; ')));
  }
  if (topo.links.some((l) => l.status.ready)) actions.push(action('runTests', 'mcdma test', { primary: true }));
  const status = worst(items.map((i) => i.status));
  const passed = topo.links.filter((l) => l.status.lastTest && l.status.lastTest.passed).length;
  return { id: 'test', title: 'Transfer test', status, summary: passed === topo.links.length ? 'All links verified with real RDMA transfers' : `${passed} of ${plural(topo.links.length, 'link')} verified`, items, actions };
}

function extraMacs(step, macs) {
  for (const m of (macs || []).slice(1)) {
    if (!m.info || !m.info.ok) { step.items.push(item(`${m.label || m.id} (over ssh)`, `unreachable · ${(m.info && m.info.error) || 'ssh failed'}`, 'fail')); continue; }
    const cards = m.info.pci.cards;
    step.items.push(item(`${m.label} (over ssh)`, `${cards.length ? `${cards.map((c) => c.name).join(', ')} · ${cards.reduce((a, c) => a + c.functions.length, 0)} ports` : 'no ConnectX card'} · driver ${m.info.loaded.loaded ? `${m.info.loaded.version} loaded` : m.info.kext.installed ? `${m.info.kext.version} not loaded` : 'not installed'}`,
      cards.length && m.info.loaded.loaded ? 'ok' : 'warn', m.info.loaded.loaded ? null : 'Run mcdma driver install on that Mac.'));
  }
  step.status = worst(step.items.map((i) => i.status).concat([step.status]));
  return step;
}

function build({ studio, macs, sparks, topology, pkg }) {
  const steps = [systemCheck(studio, pkg), extraMacs(hardwareCheck(studio), macs), driverCheck(studio, pkg), sparksCheck(sparks), topologyCheck(topology), networkCheck(topology, studio), testCheck(topology)];
  const done = steps.filter((s) => s.status === 'ok').length;
  const overall = steps.some((s) => s.status === 'fail') ? 'fail' : done === steps.length ? 'ok' : steps.some((s) => s.status === 'warn') ? 'warn' : 'todo';
  const next = steps.find((s) => s.status !== 'ok') || null;
  return { steps, done, total: steps.length, overall, next: next ? next.id : null };
}

module.exports = { build, worst };
