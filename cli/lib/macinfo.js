'use strict';
// Probes of the Mac that carries the ConnectX card: OS/security, PCI devices,
// Thunderbolt enclosure, the MCDMA driver (bundle, load state, registry),
// the libibverbs provider, RDMA devices, interfaces and static neighbours.
// Everything here is read-only and needs no administrator rights.
const { sections, jsonSafe, normMac, normIp6 } = require('./parse');

const KEXT_ID = 'org.mcdma.cx5.native';
const KEXT_PATH = '/Library/Extensions/MCDMACX5Native.kext';
const PROVIDER_PATH = '/usr/local/lib/rdma/libmcdma-rdmav34.so';
const CONF_PATH = '/etc/libibverbs.d/mcdma.driver';
const TOOLS_DIR = '/usr/local/libexec/mcdma';

const MLX_DEVICES = {
  '0x1013': 'ConnectX-4', '0x1015': 'ConnectX-4 Lx', '0x1017': 'ConnectX-5', '0x1019': 'ConnectX-5 Ex',
  '0x101b': 'ConnectX-6', '0x101d': 'ConnectX-6 Dx', '0x101f': 'ConnectX-6 Lx', '0x1021': 'ConnectX-7',
  '0x1023': 'ConnectX-8', '0xa2d6': 'BlueField-2', '0xa2dc': 'BlueField-3'
};
const DRIVER_SUPPORTED = new Set(['0x1019', '0x1015']);

const PROBE = `
echo '===SWVERS'; sw_vers 2>/dev/null
echo '===CHIP'; sysctl -n machdep.cpu.brand_string 2>/dev/null; sysctl -n hw.memsize 2>/dev/null; uname -m; hostname -s
echo '===CSR'; csrutil status 2>&1
echo '===CONSENT'; spctl kext-consent status 2>&1
echo '===PCI'; system_profiler SPPCIDataType -json 2>/dev/null; echo
echo '===TB'; system_profiler SPThunderboltDataType -json 2>/dev/null; echo
echo '===REG'; ioreg -a -r -c MCDMACX5Native -d 1 2>/dev/null | plutil -convert json -o - - 2>/dev/null
echo; echo '===PCIREG'; ioreg -r -c IOPCIDevice -d 1 2>/dev/null | grep -E '^\\+-o|"pcidebug"|"Tunnel Endpoint GUID"|"Tunnel Endpoint Device Model ID"|"IOPCITunnelled"'
echo; echo '===LOADED'; kmutil showloaded --list-only --bundle-identifier ${KEXT_ID} 2>&1
echo '===KEXT'; if [ -d ${KEXT_PATH} ]; then plutil -convert json -o - ${KEXT_PATH}/Contents/Info.plist 2>/dev/null; echo; echo '---SIGN'; codesign -dv ${KEXT_PATH} 2>&1; echo '---SHA'; shasum -a 256 ${KEXT_PATH}/Contents/MacOS/MCDMACX5Native 2>/dev/null; fi
echo '===PROVIDER'; shasum -a 256 ${PROVIDER_PATH} 2>/dev/null; echo '---CONF'; cat ${CONF_PATH} 2>/dev/null; echo; echo '---SIGN'; codesign -dv ${PROVIDER_PATH} 2>&1 | grep -E 'Signature|flags'
echo '===IBV'; ibv_devinfo 2>&1
echo '===IFCONFIG'; ifconfig -a 2>/dev/null
echo '===NDP'; ndp -an 2>/dev/null
echo '===TOOLS'; ls -1 ${TOOLS_DIR} 2>/dev/null; echo '---LAUNCHD'; ls -1 /Library/LaunchDaemons/org.mcdma.neighbours.plist 2>/dev/null; echo '---CONF'; cat "/Library/Application Support/MCDMA/neighbours.conf" 2>/dev/null
echo '===PENDING'; cat "/Library/Application Support/MCDMA/install-state.txt" 2>/dev/null; echo
true
`;

function parseSwVers(t) {
  const g = (k) => ((t || '').match(new RegExp(`^${k}:\\s*(.+)$`, 'm')) || [])[1] || null;
  const version = g('ProductVersion');
  return { name: g('ProductName'), version, build: g('BuildVersion'), major: version ? parseInt(version, 10) : null };
}

function parseChip(t) {
  const [brand, mem, arch, host] = (t || '').trim().split('\n');
  return { brand: brand || null, memoryGiB: mem ? Math.round(Number(mem) / 2 ** 30) : null, arch: arch || null, hostname: host || null };
}

function parseCsr(t) {
  const s = (t || '').trim();
  const m = s.match(/status:\s*(\w+)/i);
  return { raw: s, state: m ? m[1].toLowerCase() : 'unknown' }; // enabled | disabled | unknown
}

function parsePci(t) {
  const j = jsonSafe(t) || {};
  const list = j.SPPCIDataType || [];
  const devices = [];
  for (const d of list) {
    const vendor = (d['sppci_vendor-id'] || '').toLowerCase();
    if (vendor !== '0x15b3') continue;
    const deviceId = (d['sppci_device-id'] || '').toLowerCase();
    const slot = d.sppci_slot_name || '';
    const m = slot.match(/@(\d+),(\d+),(\d+)/);
    devices.push({
      name: MLX_DEVICES[deviceId] || `Mellanox device ${deviceId}`,
      vendorId: vendor, deviceId, subsystemId: d['sppci_subsystem-id'] || null, revision: d['sppci_revision-id'] || null,
      slot, pci: m ? `${m[1]}:${m[2]}:${m[3]}` : null, card: m ? `${m[1]}:${m[2]}` : slot,
      tunnelled: /thunderbolt/i.test(slot) || /yes/i.test(d['sppci_tunnel-compatible'] || ''),
      linkWidth: d['sppci_link-width'] || null, linkSpeed: d['sppci_link-speed'] || null,
      linkUp: /up/i.test(d['sppci_link-status'] || ''),
      driverAttached: /affirmative|yes/i.test(d.sppci_driver_installed || ''),
      supported: DRIVER_SUPPORTED.has(deviceId)
    });
  }
  // group functions by card (bus:device)
  const cards = {};
  for (const d of devices) {
    const c = cards[d.card] || (cards[d.card] = { id: d.card, name: d.name, deviceId: d.deviceId, functions: [], tunnelled: d.tunnelled,
      linkWidth: d.linkWidth, linkSpeed: d.linkSpeed, supported: d.supported });
    c.functions.push(d);
  }
  return { devices, cards: Object.values(cards) };
}

function parseTb(t) {
  const j = jsonSafe(t) || {};
  const enclosures = [];
  const walk = (items, bus, receptacle) => {
    for (const it of items || []) {
      if (it.device_name_key && !/^Mac/.test(it.device_name_key || '') && it.vendor_name_key !== 'Apple Inc.') {
        const up = it.receptacle_upstream_ambiguous_tag || {};
        enclosures.push({ name: it.device_name_key, vendor: it.vendor_name_key || null, mode: it.mode_key || null, uid: it.switch_uid_key || it.uid_key || null,
          deviceId: it.device_id_key || null, speed: up.current_speed_key || null, firmware: it.switch_version_key || it.firmware_version_key || null, bus, receptacle });
      }
      if (it._items) walk(it._items, bus, receptacle);
    }
  };
  for (const b of j.SPThunderboltDataType || []) {
    const rec = Object.entries(b).find(([k, v]) => /^receptacle_\d+_tag$/.test(k) && v && v.receptacle_id_key);
    walk(b._items, b._name, rec ? String(rec[1].receptacle_id_key) : null);
  }
  return enclosures;
}

// Tunnelled PCI root bridges carry the Thunderbolt endpoint's GUID, which is the
// enclosure's switch UID (byte-reversed). That is how a card is tied to the exact
// enclosure it sits in, whatever brand it is.
function parsePciReg(t) {
  const tunnels = [];
  let cur = null;
  for (const line of (t || '').split('\n')) {
    if (/^\+-o/.test(line)) { cur = {}; tunnels.push(cur); continue; }
    if (!cur) continue;
    let m;
    if ((m = line.match(/"pcidebug" = "([^"]+)"/))) cur.pcidebug = m[1];
    else if ((m = line.match(/"Tunnel Endpoint GUID" = <([0-9a-f]+)>/i))) cur.guid = m[1].toLowerCase();
    else if ((m = line.match(/"Tunnel Endpoint Device Model ID" = <([0-9a-f]+)>/i))) cur.model = m[1].toLowerCase();
    else if (/"IOPCITunnelled" = Yes/.test(line)) cur.tunnelled = true;
  }
  return tunnels.filter((x) => x.guid && x.pcidebug).map((x) => {
    const r = x.pcidebug.match(/^(\d+):(\d+):(\d+)\((\d+):(\d+)\)/);
    const bytes = x.guid.match(/../g) || [];
    return { pcidebug: x.pcidebug, bus: r ? Number(r[1]) : null, sec: r ? Number(r[4]) : null, sub: r ? Number(r[5]) : null,
      uid: '0x' + bytes.reverse().join('').toUpperCase(), modelId: x.model ? '0x' + (x.model.match(/../g) || []).reverse().join('').replace(/^0+/, '').toUpperCase() : null };
  });
}

function attachEnclosures(pci, tunnels, thunderbolt) {
  for (const c of pci.cards) {
    const bus = Number((c.id || '').split(':')[0]);
    const tun = tunnels.find((t) => t.sec != null && bus >= t.sec && bus <= t.sub) || null;
    const known = tun ? thunderbolt.find((e) => e.uid && e.uid.toLowerCase() === tun.uid.toLowerCase()) || null : null;
    c.enclosure = known ? { ...known, matched: true } : tun ? { name: 'Thunderbolt enclosure', vendor: null, uid: tun.uid, receptacle: null, speed: null, matched: false } : (c.tunnelled ? { name: 'Thunderbolt enclosure', vendor: null, uid: null, receptacle: null, speed: null, matched: false } : null);
    for (const f of c.functions) f.enclosure = c.enclosure;
  }
}

function parseRegistry(t) {
  const j = jsonSafe((t || '').trim());
  if (!Array.isArray(j)) return [];
  return j.map((e) => {
    const out = { iface: e.MCDMAAddressInterface || null, pci: (e.MCDMAPCIePath || '').split(' ')[0] || null, raw: {} };
    for (const [k, v] of Object.entries(e)) if (k.startsWith('MCDMA')) out.raw[k.slice(5)] = v;
    out.portActive = !!e.MCDMAPortActive; out.gidLive = !!e.MCDMAGidLive;
    out.quarantined = !!e.MCDMAQuarantined; out.startError = e.MCDMANativeStartError || null;
    out.userQueues = !!e.MCDMAUserQueues; out.userBlueFlame = !!e.MCDMAUserBlueFlame; out.blueFlame = !!e.MCDMABlueFlameEnabled;
    out.ethernetMtu = e.MCDMAEthernetMTU || null; out.frameMtu = e.MCDMAFrameOperMTU || null;
    out.transport = e.MCDMATransport || null; out.build = e.MCDMANativeBuild || null; out.pciePath = e.MCDMAPCIePath || null;
    out.knobs = { maxReadRequest: e.MCDMAMaxReadRequestBytes, relaxedOrdering: e.MCDMARelaxedOrdering, ackEveryPacket: e.MCDMAAckRequestEveryPacket };
    return out;
  });
}

function parseLoaded(t) {
  const m = (t || '').match(/org\.mcdma\.cx5\.native \(([^)]+)\) ([0-9A-Fa-f-]{36})/);
  return m ? { loaded: true, version: m[1], uuid: m[2].toUpperCase() } : { loaded: false, version: null, uuid: null };
}

function parseKext(t) {
  const s = t || '';
  if (!s.trim()) return { installed: false };
  const [plistPart, rest] = s.split('---SIGN');
  const plist = jsonSafe((plistPart || '').trim()) || {};
  const libs = plist.OSBundleLibraries || {};
  const kpi = Object.entries(libs).find(([k]) => k.startsWith('com.apple.kpi.'));
  const signRaw = (rest || '').split('---SHA')[0] || '';
  const sha = ((rest || '').split('---SHA')[1] || '').trim().split(/\s+/)[0] || null;
  const pers = (plist.IOKitPersonalities || {}).MCDMACX5Native || {};
  return {
    installed: true, version: plist.CFBundleVersion || null, bundleId: plist.CFBundleIdentifier || null,
    requiresMacOSMajor: kpi ? parseInt(kpi[1], 10) : null,
    signature: /Signature=adhoc/.test(signRaw) ? 'ad hoc' : (signRaw.match(/Authority=([^\n]+)/) || [])[1] || (signRaw.match(/code object is not signed/) ? 'unsigned' : 'unknown'),
    sha256: sha, personality: pers, match: pers.IOPCIMatch || null
  };
}

function parseProvider(t) {
  const [shaPart, rest] = (t || '').split('---CONF');
  const sha = (shaPart || '').trim().split(/\s+/)[0] || null;
  const [conf, sign] = (rest || '').split('---SIGN');
  return { present: !!sha, sha256: sha, conf: (conf || '').trim() || null, confPresent: !!(conf || '').trim(),
    signature: /adhoc/.test(sign || '') ? 'ad hoc' : (sign || '').trim() || null };
}

function parseIbv(t) {
  const devices = [];
  let cur = null;
  for (const line of (t || '').split('\n')) {
    const h = line.match(/^hca_id:\s*(\S+)/);
    if (h) { cur = { name: h[1], ports: [] }; devices.push(cur); continue; }
    if (!cur) continue;
    const f = line.trim().match(/^([\w_]+):\s*(.+)$/);
    if (!f) continue;
    const [, k, v] = f;
    if (k === 'transport') cur.transport = v;
    else if (k === 'node_guid') cur.nodeGuid = v;
    else if (k === 'vendor_id') cur.vendorId = v;
    else if (k === 'vendor_part_id') cur.partId = Number(v);
    else if (k === 'port') cur.ports.push({ port: Number(v) });
    else if (cur.ports.length) {
      const p = cur.ports[cur.ports.length - 1];
      if (k === 'state') { p.state = v.split(' ')[0]; p.active = /PORT_ACTIVE/.test(v); }
      else if (k === 'link_layer') p.linkLayer = v;
      else if (k === 'active_mtu') p.activeMtu = parseInt(v, 10);
      else if (k === 'max_mtu') p.maxMtu = parseInt(v, 10);
    }
  }
  return devices;
}

function parseIfconfig(t) {
  const ifaces = {};
  let cur = null;
  for (const line of (t || '').split('\n')) {
    const h = line.match(/^([A-Za-z0-9_.-]+): flags=\d+<([A-Z0-9,]*)>(?: mtu (\d+))?/);
    if (h) { cur = ifaces[h[1]] = { name: h[1], flags: h[2].split(',').filter(Boolean), mtu: h[3] ? Number(h[3]) : null, mac: null, inet6: [], inet: [], status: null }; continue; }
    if (!cur) continue;
    const s = line.trim();
    let m;
    if ((m = s.match(/^ether ([0-9a-f:]+)/i))) cur.mac = normMac(m[1]);
    else if ((m = s.match(/^inet6 ([0-9a-f:]+)(?:%\S+)? prefixlen (\d+)/i))) cur.inet6.push({ addr: normIp6(m[1]), prefix: Number(m[2]) });
    else if ((m = s.match(/^inet (\d+\.\d+\.\d+\.\d+)/))) cur.inet.push(m[1]);
    else if ((m = s.match(/^status: (\w+)/))) cur.status = m[1];
  }
  return ifaces;
}

function parseNdp(t) {
  const entries = [];
  for (const line of (t || '').split('\n')) {
    const m = line.match(/^(\S+)%(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)/);
    if (!m || m[1] === 'Neighbor') continue;
    entries.push({ addr: normIp6(m[1]), iface: m[2], lladdr: normMac(m[3]), expire: m[5], state: m[6], permanent: m[5] === 'permanent' });
  }
  return entries;
}

function parseTools(t) {
  const [list, rest] = (t || '').split('---LAUNCHD');
  const [ld, conf] = (rest || '').split('---CONF');
  return { installed: (list || '').trim().split('\n').filter(Boolean), launchDaemon: !!(ld || '').trim(), neighboursConf: (conf || '').trim() || null };
}

function parseInstallState(t) {
  const text = (t || '').trim();
  if (!text) return null;
  const g = (k) => ((text.match(new RegExp(`^${k}=(.*)$`, 'm')) || [])[1] || '').trim() || null;
  return { version: g('version'), uuid: g('uuid'), at: g('at'), loadExit: g('loadExit') != null ? Number(g('loadExit')) : null, loadLog: (text.split('loadLog:')[1] || '').trim() };
}

function studioPorts(info) {
  // One entry per ConnectX function that the driver drives (or would drive).
  const ports = [];
  for (const d of info.pci.devices) {
    const reg = info.registry.find((r) => r.pci === d.pci) || null;
    const iface = reg && reg.iface ? info.ifaces[reg.iface] || null : null;
    const rdma = reg && reg.iface ? info.rdma.find((x) => x.name === `rdma_${reg.iface}`) || null : null;
    ports.push({
      id: reg && reg.iface ? reg.iface : `pci-${d.pci}`, pci: d.pci, card: d.card, cardName: d.name, deviceId: d.deviceId, enclosure: d.enclosure || null,
      supported: d.supported, driverAttached: d.driverAttached && !!reg,
      iface: reg ? reg.iface : null, mac: iface ? iface.mac : null,
      linkLocal: iface ? (iface.inet6.find((a) => a.addr && a.addr.startsWith('fe80:')) || {}).addr || null : null,
      mtu: iface ? iface.mtu : null, up: iface ? iface.flags.includes('UP') && iface.flags.includes('RUNNING') : false,
      portActive: reg ? reg.portActive : false, gidLive: reg ? reg.gidLive : false, quarantined: reg ? reg.quarantined : false,
      startError: reg ? reg.startError : null,
      rdmaDevice: rdma ? rdma.name : null, rdmaActive: !!(rdma && rdma.ports[0] && rdma.ports[0].active),
      rdmaMtu: rdma && rdma.ports[0] ? rdma.ports[0].activeMtu : null,
      neighbours: info.ndp.filter((n) => reg && n.iface === reg.iface && n.permanent),
      registry: reg
    });
  }
  ports.sort((a, b) => (a.pci || '').localeCompare(b.pci || ''));
  return ports;
}

async function probe(host) {
  const r = await host.sh(PROBE, { timeoutMs: 40000 });
  if (r.code === -1 && !r.out) return { ok: false, error: r.err || 'probe failed', reachable: false };
  const s = sections(r.out);
  const info = {
    ok: true, reachable: true, at: Date.now(), hostKind: host.kind, hostLabel: host.label,
    os: parseSwVers(s.SWVERS), chip: parseChip(s.CHIP), sip: parseCsr(s.CSR),
    kextConsent: /ENABLED/i.test(s.CONSENT || '') ? 'enabled' : /DISABLED/i.test(s.CONSENT || '') ? 'disabled' : 'unknown',
    pci: parsePci(s.PCI), thunderbolt: parseTb(s.TB), tunnels: parsePciReg(s.PCIREG), registry: parseRegistry(s.REG),
    loaded: parseLoaded(s.LOADED), kext: parseKext(s.KEXT), provider: parseProvider(s.PROVIDER),
    rdma: parseIbv(s.IBV), ifaces: parseIfconfig(s.IFCONFIG), ndp: parseNdp(s.NDP), tools: parseTools(s.TOOLS),
    installState: parseInstallState(s.PENDING)
  };
  info.appleSilicon = info.chip.arch === 'arm64';
  attachEnclosures(info.pci, info.tunnels, info.thunderbolt);
  info.ports = studioPorts(info);
  info.mcdmaDevices = info.rdma.filter((d) => /^rdma_mcrdma/.test(d.name));
  return info;
}

module.exports = { probe, parsePciReg, attachEnclosures, KEXT_ID, KEXT_PATH, PROVIDER_PATH, CONF_PATH, TOOLS_DIR, MLX_DEVICES, DRIVER_SUPPORTED,
  parsePci, parseTb, parseRegistry, parseLoaded, parseKext, parseProvider, parseIbv, parseIfconfig, parseNdp, parseCsr };
