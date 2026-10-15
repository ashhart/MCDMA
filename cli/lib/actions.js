'use strict';
// Side-effecting setup actions. Anything that changes the Mac runs through the
// macOS administrator dialog (the user types the password there, never here);
// Spark changes go over ssh as root. Each returns { ok, message, ...details }.
const fs = require('fs');
const os = require('os');
const path = require('path');
const { adminRun, run, Host } = require('./exec');
const { KEXT_PATH, PROVIDER_PATH, CONF_PATH, TOOLS_DIR } = require('./macinfo');
const { runTransferTest } = require('./testrun');
const { installDecision } = require('./version');
const { sha256 } = require('./driverpkg');

const SUPPORT_DIR = '/Library/Application Support/MCDMA';
const q = (s) => "'" + String(s).replace(/'/g, "'\\''") + "'";
const ident = (s) => /^[A-Za-z0-9_.:%-]{1,64}$/.test(String(s));

function classifyLoad(text, code) {
  if (code === 0) return 'loaded';
  if (/system policy|approv|consent|not allowed|blocked/i.test(text)) return 'approval';
  if (/reboot|restart/i.test(text)) return 'restart';
  return 'error';
}

async function installDriver({ pkg }) {
  if (!pkg || !pkg.available) return { ok: false, message: 'no driver package available' };
  const manifest = pkg.manifest || {};
  if (!/^[0-9a-f]{64}$/.test(manifest.archive_sha256 || '') || await sha256(pkg.archive) !== manifest.archive_sha256)
    return { ok: false, message: 'Driver archive checksum does not match its manifest' };
  const probe = await new Host('local').sh(`if [ -d ${q(KEXT_PATH)} ]; then /usr/libexec/PlistBuddy -c 'Print CFBundleVersion' ${q(KEXT_PATH + '/Contents/Info.plist')}; else echo absent; fi`);
  if (probe.code !== 0) return { ok: false, message: 'Cannot read installed driver version' };
  const installed = probe.out.trim() === 'absent' ? null : probe.out.trim();
  const decision = installDecision(installed, pkg.version);
  if (!decision.allowed) return { ok: false, message: decision.reason };
  const builds = pkg.requiredBuilds && pkg.requiredBuilds.length ? pkg.requiredBuilds : ['26A428'];
  const script = `
set -e
# Recheck after administrator authentication, before changing any installed files.
current=absent
if [ -d ${q(KEXT_PATH)} ]; then current=$(/usr/libexec/PlistBuddy -c 'Print CFBundleVersion' ${q(KEXT_PATH + '/Contents/Info.plist')}); fi
[ "$current" = ${q(installed || 'absent')} ] || { echo 'Installed driver changed during approval; run the check again'; exit 3; }
[ "$(/usr/bin/shasum -a 256 ${q(pkg.archive)} | /usr/bin/cut -d ' ' -f1)" = ${q(manifest.archive_sha256)} ] || { echo 'Archive changed during approval'; exit 3; }
case "$(/usr/bin/sw_vers -buildVersion)" in ${builds.join('|')}) ;; *) echo ${q(`This development package requires macOS build ${builds.join(' or ')}`)}; exit 3 ;; esac
log() { echo "[install] $*"; }
stage=$(mktemp -d /tmp/mcdma-install.XXXXXX)
trap 'rm -rf "$stage"' EXIT
log "extracting $(basename ${q(pkg.archive)})"
/bin/cp ${q(pkg.archive)} "$stage/archive.tar.gz"
printf '%s' ${q(JSON.stringify(manifest))} > "$stage/manifest.json"
python3 ${q(path.join(__dirname, '..', 'tools', 'verify_package.py'))} "$stage/archive.tar.gz" "$stage/payload" "$stage/manifest.json"
/usr/bin/codesign --verify --strict "$stage/payload/MCDMACX5Native.kext"
[ "$(/usr/bin/dwarfdump --uuid "$stage/payload/MCDMACX5Native.kext/Contents/MacOS/MCDMACX5Native" | /usr/bin/awk '{print $2}')" = ${q(pkg.uuid)} ] || { echo 'Kernel extension UUID mismatch'; exit 3; }
/bin/mkdir -p ${q(SUPPORT_DIR)}
if [ -d ${q(KEXT_PATH)} ] || [ -f ${q(PROVIDER_PATH)} ]; then
  backup=${q(SUPPORT_DIR)}/backup-$(date +%Y%m%d-%H%M%S)
  /bin/mkdir -p "$backup"
  [ -d ${q(KEXT_PATH)} ] && /usr/bin/ditto ${q(KEXT_PATH)} "$backup/MCDMACX5Native.kext"
  [ -f ${q(PROVIDER_PATH)} ] && /bin/cp -p ${q(PROVIDER_PATH)} "$backup/"
  [ -f ${q(CONF_PATH)} ] && /bin/cp -p ${q(CONF_PATH)} "$backup/"
  log "previous files backed up to $backup"
fi
/bin/rm -rf ${q(KEXT_PATH)}
/usr/bin/ditto "$stage/payload/MCDMACX5Native.kext" ${q(KEXT_PATH)}
/usr/sbin/chown -R root:wheel ${q(KEXT_PATH)}
/bin/chmod -R go-w ${q(KEXT_PATH)}
/usr/bin/codesign --verify --strict ${q(KEXT_PATH)} && log "kernel extension signature verified"
/bin/mkdir -p /usr/local/lib/rdma /etc/libibverbs.d ${q(TOOLS_DIR)}
/usr/bin/install -m 755 -o root -g wheel "$stage/payload/libmcdma-rdmav34.so" ${q(PROVIDER_PATH)}
/usr/bin/install -m 644 -o root -g wheel "$stage/payload/mcdma.driver" ${q(CONF_PATH)}
for t in "$stage/payload"/tools/*; do [ -f "$t" ] && /usr/bin/install -m 755 -o root -g wheel "$t" ${q(TOOLS_DIR)}/; done
if [ -f "$stage/payload/tools/linux-arm64/verbs-peer" ]; then /bin/mkdir -p ${q(TOOLS_DIR)}/linux-arm64; /usr/bin/install -m 755 "$stage/payload/tools/linux-arm64/verbs-peer" ${q(TOOLS_DIR)}/linux-arm64/; fi
log "driver ${pkg.version} files installed"
set +e
/usr/bin/kmutil load --load-style start-and-match --bundle-path ${q(KEXT_PATH)} > "$stage/load.log" 2>&1; code=$?
set -e
{ echo "version=${pkg.version}"; echo "uuid=${pkg.uuid || ''}"; echo "at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "loadExit=$code"; echo "loadLog:"; cat "$stage/load.log"; } > ${q(SUPPORT_DIR)}/install-state.txt
echo "===LOADLOG"; cat "$stage/load.log"; echo "===LOADEXIT $code"
`;
  const r = await adminRun(script, `mcdma wants to install the driver ${pkg.version}. Enter your password to allow it.`);
  if (r.cancelled) return { ok: false, message: r.err && r.err !== 'cancelled' ? r.err : 'installation cancelled', cancelled: true };
  const loadLog = (r.out.split('===LOADLOG')[1] || '').split('===LOADEXIT')[0].trim();
  const code = Number((r.out.match(/===LOADEXIT (-?\d+)/) || [])[1]);
  const ok = r.code === 0;
  if (!ok) return { ok: false, message: `installation failed: ${(r.out + r.err).trim().split('\n').slice(-3).join(' ')}`, log: r.out };
  const kind = classifyLoad(loadLog, code);
  const messages = {
    loaded: `Driver ${pkg.version} files installed; verify the loaded UUID before running transfers.`,
    approval: 'Driver installed. macOS needs your approval: open System Settings → Privacy & Security, click Allow for the MCDMA driver, then restart.',
    restart: 'Driver installed. Restart the Mac to load it.',
    error: `Driver files installed, but loading reported: ${loadLog.split('\n').slice(-2).join(' ') || `exit ${code}`}`
  };
  return { ok: kind !== 'error', message: messages[kind], loadKind: kind, log: r.out };
}

async function loadDriver() {
  const script = `
/bin/mkdir -p ${q(SUPPORT_DIR)}
/usr/bin/kmutil load --load-style start-and-match --bundle-path ${q(KEXT_PATH)} > /tmp/mcdma-load.log 2>&1; code=$?
{ echo "at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "loadExit=$code"; echo "loadLog:"; cat /tmp/mcdma-load.log; } > ${q(SUPPORT_DIR)}/install-state.txt
echo "===LOADLOG"; cat /tmp/mcdma-load.log; echo "===LOADEXIT $code"
`;
  const r = await adminRun(script, 'mcdma wants to load the driver. Enter your password to allow it.');
  if (r.cancelled) return { ok: false, message: r.err || 'cancelled', cancelled: true };
  const loadLog = (r.out.split('===LOADLOG')[1] || '').split('===LOADEXIT')[0].trim();
  const code = Number((r.out.match(/===LOADEXIT (-?\d+)/) || [])[1]);
  const kind = classifyLoad(loadLog, code);
  const messages = {
    loaded: 'Driver loaded.',
    approval: 'macOS needs your approval: open System Settings → Privacy & Security, click Allow for the MCDMA driver, then restart.',
    restart: 'Restart the Mac to finish loading the driver.',
    error: `kmutil reported: ${loadLog.split('\n').slice(-2).join(' ') || `exit ${code}`}`
  };
  return { ok: kind !== 'error', message: messages[kind], loadKind: kind, log: loadLog };
}

async function restart() {
  const r = await adminRun('/sbin/shutdown -r now', 'mcdma wants to restart this Mac.');
  if (r.cancelled) return { ok: false, message: r.err || 'cancelled', cancelled: true };
  return { ok: r.code === 0, message: r.code === 0 ? 'Restarting…' : `restart failed: ${(r.out + r.err).trim()}` };
}

async function openSecurity() {
  const r = await run('/usr/bin/open', ['x-apple.systempreferences:com.apple.settings.PrivacySecurity.extension'], { timeoutMs: 8000 });
  if (r.code !== 0) await run('/usr/bin/open', ['-b', 'com.apple.systempreferences'], { timeoutMs: 8000 });
  return { ok: true, message: 'System Settings opened. Look for the MCDMA driver under Security and click Allow.' };
}

async function bootPolicy() {
  const r = await adminRun('/usr/bin/bputil -d 2>&1', 'mcdma wants to read the boot security policy (bputil -d).');
  if (r.cancelled) return { ok: false, message: r.err || 'cancelled', cancelled: true };
  const g = (re) => ((r.out.match(re) || [])[1] || '').trim() || null;
  const policy = { mode: g(/Security Mode:\s+(\w+)/), kexts: g(/3rd Party Kexts Status:\s+(\w+)/), sip: g(/SIP Status:\s+(\w+)/), raw: r.out.split('\n').filter((l) => /Security Mode|Kexts Status|SIP Status|Signed System Volume/.test(l)).join('\n') };
  const good = /permissive|reduced/i.test(policy.mode || '') && /enabled/i.test(policy.kexts || '');
  return { ok: true, policy, message: good ? `Boot policy: ${policy.mode} security, third-party kernel extensions ${policy.kexts}.` : `Boot policy: ${policy.mode || '?'} security, third-party kernel extensions ${policy.kexts || '?'}. Reduced or Permissive security with kernel extensions enabled is required.` };
}

// Mac side: link-local aliases, static neighbours, and a LaunchDaemon that
// re-applies them after every boot (the address-only interface starts bare).
function studioNetworkScript(entries) {
  const conf = entries.map((e) => `${e.iface} ${e.studioLinkLocal} ${e.sparkLinkLocal} ${e.sparkMac}`).join('\n');
  return `
set -e
/bin/mkdir -p ${q(SUPPORT_DIR)}
/bin/cat > ${q(SUPPORT_DIR)}/neighbours.conf <<'CONF'
${conf}
CONF
/bin/cat > ${q(SUPPORT_DIR)}/restore-neighbours.sh <<'SCRIPT'
#!/bin/bash
# Re-applies the MCDMA link-local addresses and static IPv6 neighbours.
# Format of neighbours.conf: IFACE STUDIO_LINK_LOCAL SPARK_LINK_LOCAL SPARK_MAC
conf="/Library/Application Support/MCDMA/neighbours.conf"
apply() {
  local iface=$1 ll=$2 peer=$3 mac=$4
  /sbin/ifconfig "$iface" >/dev/null 2>&1 || return 1
  /sbin/ifconfig "$iface" inet6 "$ll" prefixlen 64 alias 2>/dev/null || true
  /sbin/ifconfig "$iface" up 2>/dev/null || true
  /usr/sbin/ndp -d "$peer%$iface" >/dev/null 2>&1 || true
  /usr/sbin/ndp -s "$peer%$iface" "$mac" >/dev/null 2>&1
}
deadline=$((SECONDS + 180))
while :; do
  pending=0
  while read -r iface ll peer mac; do
    [ -n "$iface" ] || continue
    apply "$iface" "$ll" "$peer" "$mac" || pending=1
  done < "$conf"
  [ $pending = 0 ] && exit 0
  [ $SECONDS -ge $deadline ] && exit 1
  sleep 2
done
SCRIPT
/bin/chmod 755 ${q(SUPPORT_DIR)}/restore-neighbours.sh
/usr/sbin/chown root:wheel ${q(SUPPORT_DIR)}/restore-neighbours.sh ${q(SUPPORT_DIR)}/neighbours.conf
/bin/cat > /Library/LaunchDaemons/org.mcdma.neighbours.plist <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>org.mcdma.neighbours</string>
  <key>ProgramArguments</key><array><string>/bin/bash</string><string>/Library/Application Support/MCDMA/restore-neighbours.sh</string></array>
  <key>RunAtLoad</key><true/>
  <key>StandardOutPath</key><string>/Library/Application Support/MCDMA/restore-neighbours.log</string>
  <key>StandardErrorPath</key><string>/Library/Application Support/MCDMA/restore-neighbours.log</string>
</dict></plist>
PLIST
/usr/sbin/chown root:wheel /Library/LaunchDaemons/org.mcdma.neighbours.plist
/bin/chmod 644 /Library/LaunchDaemons/org.mcdma.neighbours.plist
/bin/launchctl bootout system/org.mcdma.neighbours >/dev/null 2>&1 || true
/bin/launchctl bootstrap system /Library/LaunchDaemons/org.mcdma.neighbours.plist >/dev/null 2>&1 || true
/bin/bash ${q(SUPPORT_DIR)}/restore-neighbours.sh && echo "applied" || { echo "some interfaces are not present yet"; exit 4; }
/usr/sbin/ndp -an | /usr/bin/grep -E '${entries.map((e) => e.iface).join('|')}' || true
`;
}

async function configureStudio({ links }) {
  const entries = links.map((l) => ({ iface: l.studio.iface, studioLinkLocal: l.expected.studioLinkLocal, sparkLinkLocal: l.expected.sparkLinkLocal, sparkMac: l.spark.mac }));
  if (!entries.length) return { ok: false, message: 'no links to configure' };
  for (const e of entries) if (![e.iface, e.studioLinkLocal, e.sparkLinkLocal, e.sparkMac].every(ident)) return { ok: false, message: `invalid identifier in ${JSON.stringify(e)}` };
  const r = await adminRun(studioNetworkScript(entries), 'mcdma wants to set the RDMA link-local addresses and static neighbours on this Mac.');
  if (r.cancelled) return { ok: false, message: r.err || 'cancelled', cancelled: true };
  return { ok: r.code === 0, message: r.code === 0 ? `Mac addresses and neighbours set for ${entries.length} link${entries.length === 1 ? '' : 's'} and persisted (LaunchDaemon org.mcdma.neighbours).` : `failed: ${(r.out + r.err).trim().split('\n').slice(-2).join(' ')}`, log: r.out };
}

// Spark side: permanent neighbour for the Mac port plus persistence: a
// systemd service at boot, a 30 s timer that re-applies (the entry vanishes on
// every link flap) and a NetworkManager dispatcher hook.
function sparkScript(entries) {
  const conf = entries.map((e) => `${e.iface} ${e.studioLinkLocal} ${e.studioMac}`).join('\n');
  return `
set -e
[ "$(id -u)" = 0 ] || { echo "not root"; exit 5; }
mkdir -p /etc/mcdma /usr/local/sbin
task_neighbours=$(mktemp /etc/mcdma/neighbours.conf.XXXXXX)
trap 'rm -f "$task_neighbours"' EXIT
cat > "$task_neighbours" <<'CONF'
${conf}
CONF
if [ -f /etc/mcdma/neighbours.conf ]; then
  awk 'NR==FNR { if (NF==3) managed[$1 " " $2]=1; next } NF==3 && !(($1 " " $2) in managed) { print }' "$task_neighbours" /etc/mcdma/neighbours.conf >> "$task_neighbours.existing"
  cat "$task_neighbours.existing" >> "$task_neighbours"
  rm -f "$task_neighbours.existing"
fi
chmod 644 "$task_neighbours"
mv "$task_neighbours" /etc/mcdma/neighbours.conf
cat > /usr/local/sbin/mcdma-neighbours <<'SCRIPT'
#!/bin/bash
# Re-applies permanent IPv6 neighbours for managed MCDMA peers.
# Format of /etc/mcdma/neighbours.conf: IFACE STUDIO_LINK_LOCAL STUDIO_MAC
while read -r iface ll mac; do
  [ -n "$iface" ] || continue
  [ -e "/sys/class/net/$iface" ] || continue
  ip -6 neigh replace "$ll" lladdr "$mac" dev "$iface" nud permanent 2>/dev/null || true
done < /etc/mcdma/neighbours.conf
SCRIPT
chmod 755 /usr/local/sbin/mcdma-neighbours
cat > /etc/systemd/system/mcdma-neighbours.service <<'UNIT'
[Unit]
Description=MCDMA permanent IPv6 neighbours for the Mac RDMA ports
After=network.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/mcdma-neighbours
RemainAfterExit=no

[Install]
WantedBy=multi-user.target
UNIT
cat > /etc/systemd/system/mcdma-neighbours.timer <<'UNIT'
[Unit]
Description=Re-apply MCDMA neighbours every 30 s (they are lost on link flaps)

[Timer]
OnBootSec=20s
OnUnitActiveSec=30s
AccuracySec=5s

[Install]
WantedBy=timers.target
UNIT
if [ -d /etc/NetworkManager/dispatcher.d ]; then
cat > /etc/NetworkManager/dispatcher.d/90-mcdma-neighbours <<'HOOK'
#!/bin/bash
case "$2" in up|connectivity-change|dhcp6-change) /usr/local/sbin/mcdma-neighbours ;; esac
exit 0
HOOK
chmod 755 /etc/NetworkManager/dispatcher.d/90-mcdma-neighbours
fi
systemctl daemon-reload
systemctl enable --now mcdma-neighbours.service >/dev/null 2>&1 || true
systemctl enable --now mcdma-neighbours.timer >/dev/null 2>&1 || true
/usr/local/sbin/mcdma-neighbours
for i in ${entries.map((e) => e.iface).join(' ')}; do ip -6 neigh show dev $i | grep -i permanent || true; done
echo "configured"
`;
}

async function configureSpark({ spark, entries }) {
  for (const e of entries) if (![e.iface, e.studioLinkLocal, e.studioMac].every(ident)) return { ok: false, message: `invalid identifier in ${JSON.stringify(e)}` };
  const host = new Host('ssh', spark.host);
  const r = await host.sh(spark.root ? '/bin/bash -s' : 'sudo -n /bin/bash -s', { input: sparkScript(entries), timeoutMs: 40000 });
  const ok = r.code === 0 && /configured/.test(r.out);
  const waitingFor = !ok && /password is required|a terminal is required|not root/.test(r.err + r.out) ? 'sudo' : null;
  return { ok, waitingFor, message: ok ? `${spark.id}: neighbours set and persisted (systemd service + 30 s timer${/NetworkManager/.test(r.out) ? '' : ', NetworkManager hook'})` : waitingFor ? `${spark.id}: authenticate sudo locally or use a root SSH alias, then configure again` : `${spark.id}: ${(r.err || r.out).trim().split('\n').slice(-2).join(' ') || 'failed'}`, log: r.out + r.err, host: host.alias };
}

// Ad-hoc neighbour entries on a Spark without persistence (used by wiring detection).
async function sparkNeighboursTemp(spark, entries) {
  const cmds = entries.filter((e) => [e.iface, e.studioLinkLocal, e.studioMac].every(ident)).map((e) => `ip -6 neigh replace ${e.studioLinkLocal} lladdr ${e.studioMac} dev ${e.iface} nud permanent`);
  if (!cmds.length) return { ok: true };
  const r = await new Host('ssh', spark.host).sh(cmds.join('; ') + '; echo ok', { timeoutMs: 15000 });
  return { ok: /ok/.test(r.out), message: (r.err || '').trim() };
}

// Wiring detection: give every Mac-port × Spark-port pair the neighbours it
// would need, then try a real (short) RDMA transfer on each pair. The pair that
// passes is the cabled one. Failing pairs fail fast (retry exceeded) and leave
// nothing behind except harmless extra static neighbours.
async function detectWiring({ topology, settings, onProgress = () => {}, macHosts }) {
  const ports = topology.ports.filter((p) => p.iface && p.rdmaDevice && p.portActive);
  const cands = topology.candidates.filter((c) => c.link && c.gidIndex != null);
  if (!ports.length) return { ok: false, message: 'no active Mac port with an RDMA device' };
  if (!cands.length) return { ok: false, message: 'no Spark port with a link' };
  const { eui64 } = require('./parse');
  const skipped = [];
  // 1. Mac-side neighbours for every pair (administrator prompt on this Mac; remote Macs keep what they have)
  const byMac = {};
  for (const p of ports) (byMac[p.macId] = byMac[p.macId] || []).push(p);
  for (const [macId, mps] of Object.entries(byMac)) {
    const missing = [];
    for (const p of mps) for (const c of cands) if (!(p.neighbours || []).some((n) => n.addr === c.gid && n.lladdr === c.mac)) missing.push({ iface: p.iface, studioLinkLocal: eui64(p.mac), sparkLinkLocal: c.gid, sparkMac: c.mac, port: p, cand: c });
    if (!missing.length) continue;
    if (mps[0].macKind !== 'local') { for (const m of missing) skipped.push(`${m.port.key} → ${m.cand.key}`); continue; }
    onProgress('Adding candidate neighbours on this Mac (administrator prompt)…');
    const cmds = missing.map((e) => `/sbin/ifconfig ${e.iface} inet6 ${e.studioLinkLocal} prefixlen 64 alias 2>/dev/null; /usr/sbin/ndp -d ${e.sparkLinkLocal}%${e.iface} >/dev/null 2>&1; /usr/sbin/ndp -s ${e.sparkLinkLocal}%${e.iface} ${e.sparkMac}`);
    const r = await adminRun(cmds.join('\n') + '\necho ok', 'mcdma wants to add temporary static neighbours on this Mac to probe the cabling.');
    if (r.cancelled) return { ok: false, message: r.err || 'cancelled', cancelled: true };
    if (!/ok/.test(r.out)) return { ok: false, message: `could not add neighbours: ${(r.out + r.err).trim()}` };
    for (const m of missing) m.port.neighbours = [...(m.port.neighbours || []), { addr: m.sparkLinkLocal, lladdr: m.sparkMac, permanent: true }];
  }
  // 2. Spark-side neighbours for every pair
  for (const c of cands) {
    const entries = ports.map((p) => ({ iface: c.iface, studioLinkLocal: eui64(p.mac), studioMac: p.mac }));
    onProgress(`Adding candidate neighbours on ${c.spark}…`);
    const r = await sparkNeighboursTemp({ host: c.host }, entries);
    if (!r.ok) return { ok: false, message: `${c.spark}: could not add neighbours (${r.message || 'ssh failed'})` };
  }
  // 3. Probe every pair with a short real transfer
  const results = [];
  for (const p of ports) for (const c of cands) {
    if (!(p.neighbours || []).some((n) => n.addr === c.gid && n.lladdr === c.mac)) { results.push({ key: p.key, mac: p.macId, iface: p.iface, spark: c.spark, sparkIface: c.iface, passed: false, skipped: true }); continue; }
    onProgress(`Probing ${p.macLabel} ${p.iface} → ${c.spark} ${c.iface}…`);
    const link = { id: p.key, mac: { id: p.macId, kind: p.macKind, host: p.macHost }, studio: p, spark: c, sparkHost: c.host, sparkName: c.hostname || c.spark, expected: { studioLinkLocal: eui64(p.mac), sparkLinkLocal: c.gid } };
    const t = await runTransferTest({ studioHost: macHosts ? macHosts(p.macId) : null, link, settings, latency: false, onProgress: () => {} });
    results.push({ key: p.key, mac: p.macId, iface: p.iface, spark: c.spark, sparkIface: c.iface, passed: t.passed, error: t.errors[0] || null });
    onProgress(`${p.macLabel} ${p.iface} → ${c.spark} ${c.iface}: ${t.passed ? 'connected' : 'no path'}`);
  }
  const mapping = {}, verified = {}, conflicts = [];
  for (const p of ports) {
    const hits = results.filter((r) => r.key === p.key && r.passed);
    if (hits.length === 1) { mapping[p.key] = { spark: hits[0].spark, iface: hits[0].sparkIface }; verified[p.key] = { ...mapping[p.key], at: Date.now() }; }
    else if (hits.length > 1) conflicts.push(`${p.key} reached ${hits.map((h) => h.spark).join(' and ')}`);
  }
  const found = Object.keys(mapping).length;
  const tested = results.filter((r) => !r.skipped).length;
  return { ok: found > 0, mapping, verified, results, conflicts, skipped,
    message: found ? `Wiring detected: ${Object.entries(mapping).map(([k, v]) => `${k.split(':')[1]} ↔ ${v.spark} ${v.iface}`).join(', ')}${conflicts.length ? ` (ambiguous: ${conflicts.join('; ')})` : ''}${skipped.length ? ` · ${skipped.length} pair${skipped.length === 1 ? '' : 's'} on a remote Mac not probed` : ''}`
      : tested ? `No pair passed a transfer. ${results.filter((r) => !r.skipped).map((r) => `${r.iface}→${r.spark}: ${r.error || 'failed'}`).join(' · ')}` : 'Nothing could be probed: the Macs are managed remotely and lack the candidate neighbours.' };
}

// Puts the `mcdma` command on the PATH: a wrapper that runs bin/mcdma.js from
// this checkout with the Node that ran install-cli.
async function installCli({ repoDir = '', node = process.execPath } = {}) {
  const body = `exec ${q(node)} ${q(path.join(repoDir, 'bin', 'mcdma.js'))} "$@"`;
  const script = `
set -e
/bin/mkdir -p /usr/local/bin
/bin/cat > /usr/local/bin/mcdma <<'WRAP'
#!/bin/bash
# mcdma: MCDMA command-line tool (installed by: mcdma install-cli)
${body}
WRAP
/bin/chmod 755 /usr/local/bin/mcdma
echo installed
`;
  const r = await adminRun(script, 'mcdma wants to install itself in /usr/local/bin.');
  if (r.cancelled) return { ok: false, message: r.err || 'cancelled', cancelled: true };
  return { ok: /installed/.test(r.out), message: /installed/.test(r.out) ? 'mcdma installed in /usr/local/bin (open a new terminal and run: mcdma status)' : `failed: ${(r.out + r.err).trim().split('\n').slice(-2).join(' ')}` };
}

// Bonjour scan for ssh hosts on the local network (DGX Sparks advertise _ssh._tcp).
async function scanSsh(timeoutMs = 4000) {
  const r = await run('/usr/bin/dns-sd', ['-B', '_ssh._tcp', 'local.'], { timeoutMs });
  const names = new Set();
  for (const line of (r.out || '').split('\n')) {
    const m = line.match(/_ssh\._tcp\.\s+(.+?)\s*$/);
    if (m && /Add/.test(line)) names.add(m[1].trim());
  }
  return [...names].map((n) => ({ name: n, host: `${n.replace(/\s+/g, '-')}.local` }));
}

module.exports = { installDriver, loadDriver, restart, openSecurity, bootPolicy, configureStudio, configureSpark, detectWiring, scanSsh, installCli, SUPPORT_DIR };
