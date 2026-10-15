#!/usr/bin/env node
'use strict';
// mcdma: command-line tool over lib/engine.js; --json for scripts and other tools.
const path = require('path');
const os = require('os');
const fs = require('fs');
const readline = require('readline');
const { Engine } = require('../lib/engine');
const { Store } = require('../lib/store');
const { setPrivilegeRunner, setSshConfig, sudoRunner } = require('../lib/exec');
const pkgJson = require('../package.json');

const USAGE = `mcdma ${pkgJson.version} — set up, verify and watch Mac, Spark and Linux RDMA links

Usage: mcdma <command> [options]

Commands
  status                    Check everything: the seven steps, hardware, driver, Sparks, topology, links
  enable                    Do whatever is still missing, in order; pauses when macOS needs you
  detect                    Find which Spark port each Mac port is cabled to (short real RDMA transfers)
  configure                 Link-local addresses and static neighbours on the Mac and the Sparks, persisted
  test [LINK]               RDMA transfer test on every link, or one (e.g. mcrdma1 or local:mcrdma1)
  bandwidth LINK --output DIR  Sustained RDMA sweep from a source checkout; saves raw evidence
  driver install|load       Install the bundled driver package / ask macOS to load it
  sparks list|add HOST [--name N]|remove ID
  macs list|add HOST [--name N]|remove ID          Another Mac with a card, managed over ssh
  linux list|add HOST [--name N]|remove ID         Linux RDMA peers, including Strix
  linux link NODE/IFACE NODE/IFACE --name ID       Save a Linux/Linux connection
  linux install NODE                             Build the native stock-verbs test tool
  linux status|configure|test [LINK]              Inspect, configure and verify Linux links
  map MAC-PORT SPARK/IFACE|none                     e.g. map local:mcrdma1 spark1/enp1s0f0np0
  keepalive run|status      Hold the Mac's fast platform state (runs until Ctrl-C)
  monitor [--seconds N]     Stream per-link throughput and Spark inference state
  package                   Rebuild the driver package from a Mac's installed driver
  install-cli               Put mcdma on the PATH (/usr/local/bin); or: npm install -g .
  settings [get|set KEY VALUE]

Options
  --json          Machine-readable result on stdout (progress goes to stderr)
  -y, --yes       Do not ask for confirmation
  -q, --quiet     No progress lines
  --quick         Transfer test without latency sampling
  --soft          Linux test: opt into a software transport (Soft-RoCE, siw); results are labelled
  --studio-host H Manage the Mac with the card over ssh (checks, wiring, Sparks and tests only)
  --ssh-config F  Existing private OpenSSH configuration for all remote peers
  --demo          Synthetic data, no hardware needed
  --output DIR    New bandwidth evidence directory
  --settings-dir D  Separate local CLI settings directory
  --no-color

Exit codes: 0 done · 1 failed · 2 usage · 3 waiting for you (approve the driver or restart)`;

/* ---------- argv ---------- */
const argv = process.argv.slice(2);
const flags = { json: false, yes: false, quiet: false, quick: false, soft: false, demo: false, color: process.stdout.isTTY, studioHost: null, sshConfig: null, name: null, seconds: null, output: null, settingsDir: null, bandwidth: {} };
const words = [];
for (let i = 0; i < argv.length; i++) {
  const a = argv[i];
  if (a === '--json') flags.json = true;
  else if (a === '-y' || a === '--yes') flags.yes = true;
  else if (a === '-q' || a === '--quiet') flags.quiet = true;
  else if (a === '--quick') flags.quick = true;
  else if (a === '--soft') flags.soft = true;
  else if (a === '--demo') flags.demo = true;
  else if (a === '--no-color') flags.color = false;
  else if (a === '--studio-host') flags.studioHost = argv[++i];
  else if (a === '--ssh-config') flags.sshConfig = argv[++i];
  else if (a === '--output') flags.output = argv[++i];
  else if (a === '--settings-dir') flags.settingsDir = argv[++i];
  else if (['--ops', '--sizes', '--depths', '--qps', '--total', '--repeats', '--warmup', '--verify-bytes'].includes(a)) flags.bandwidth[a.slice(2)] = argv[++i];
  else if (a === '--name') flags.name = argv[++i];
  else if (a === '--seconds') flags.seconds = Number(argv[++i]);
  else if (a === '-h' || a === '--help' || a === 'help') { console.log(USAGE); process.exit(0); }
  else if (a.startsWith('-')) { console.error(`unknown option ${a}\n`); console.error(USAGE); process.exit(2); }
  else words.push(a);
}
const [cmd, ...rest] = words;
if (!cmd) { console.log(USAGE); process.exit(2); }

/* ---------- output helpers ---------- */
const C = (code) => (s) => (flags.color ? `\x1b[${code}m${s}\x1b[0m` : String(s));
const green = C(32), yellow = C(33), red = C(31), dim = C(2), bold = C(1), cyan = C(36), magenta = C(35);
const GLYPH = { ok: green('✓'), warn: yellow('!'), fail: red('✕'), todo: dim('○'), unknown: dim('·') };
const err = (s) => process.stderr.write(s + '\n');
const out = (s) => process.stdout.write(s + '\n');
const jsonOut = (o) => out(JSON.stringify(o, null, 2));

async function confirm(question) {
  if (flags.yes) return true;
  if (!process.stdin.isTTY) { err('refusing without --yes when not run from a terminal'); return false; }
  const rl = readline.createInterface({ input: process.stdin, output: process.stderr });
  const answer = await new Promise((r) => rl.question(`${question} [y/N] `, r));
  rl.close();
  return /^y(es)?$/i.test(answer.trim());
}

/* ---------- engine ---------- */
const userData = flags.settingsDir ? path.resolve(flags.settingsDir) : path.join(os.homedir(), 'Library', 'Application Support', 'MCDMA');
const packagePaths = { appPath: path.resolve(__dirname, '..') };
const store = new Store(userData);
if (flags.sshConfig) setSshConfig(flags.sshConfig);
if (flags.studioHost) store.override({ studio: { mode: 'ssh', host: flags.studioHost } });
setPrivilegeRunner(sudoRunner);
const engine = new Engine({ store, packagePaths, demo: flags.demo, version: pkgJson.version });
engine.on('progress', (p) => { if (!flags.quiet) err(dim(`  ${p.message}`)); });
engine.on('toast', (t) => { if (!flags.quiet && !flags.json) err((t.kind === 'error' ? red : t.kind === 'warn' ? yellow : green)(`${t.message}`)); });

/* ---------- renderers ---------- */
function linkLine(l) {
  const t = l.status.lastTest;
  const lat = t && t.passed && t.latency ? ` ${cyan(`${t.latency.mac.write.median}/${t.latency.mac.read.median} µs`)} ${magenta(`${t.latency.spark.write.median}/${t.latency.spark.read.median} µs`)}` : '';
  const flagsTxt = [l.status.configured ? green('configured') : yellow('not configured'), l.status.wiringVerified ? green('verified') : l.reason === 'guess' ? yellow('guessed') : dim(l.reason), t ? (t.passed ? green('test passed') : red('test failed')) : dim('untested')].join(' · ');
  return `  ${bold(l.id)} ↔ ${l.sparkName} ${l.spark.iface}  ${l.spark.speedGbps ? `${l.spark.speedGbps}G` : ''}  ${flagsTxt}${lat}`;
}

function printStatus(S) {
  const c = S.checks;
  const head = c.overall === 'ok' ? green('RDMA on') : c.overall === 'fail' ? red('problem') : yellow(`${c.done} of ${c.total} done`);
  out(`${bold('MCDMA')} ${head}${S.demo ? dim('  (demo data)') : ''}`);
  c.steps.forEach((s, i) => {
    out(`${i + 1} ${GLYPH[s.status] || '·'} ${bold(s.title)}  ${dim('—')} ${s.summary}`);
    for (const it of s.items) {
      out(`     ${GLYPH[it.status] || '·'} ${it.label}: ${it.value}`);
      if (it.hint && it.status !== 'ok') out(dim(`       ${it.hint}`));
    }
  });
  const topo = S.topology;
  if (topo && (topo.links.length || topo.sparkLinks.length)) {
    out(`\n${bold('Links')}  ${dim(topo.summary)}  ${dim('(Mac write/read · Spark write/read, 4 KiB medians)')}`);
    for (const l of topo.links) out(linkLine(l));
    for (const s of topo.sparkLinks) out(`  ${bold(`${s.a.spark} ↔ ${s.b.spark}`)}  ${s.speedGbps ? `${s.speedGbps}G` : ''}  ${s.a.iface} ↔ ${s.b.iface}  ${dim(`cable ${s.cable.sn}`)}`);
    for (const o of topo.orphans || []) out(`  ${yellow('?')} ${o.spark} ${o.iface} has a link but no known Mac uses it`);
  }
  for (const l of topo && topo.linuxLinks || []) out(`  ${bold(l.id)}  ${l.a.node}/${l.a.iface} ↔ ${l.b.node}/${l.b.iface}  ${l.status.ready ? green('ready') : yellow('needs configuration')}`);
  if (c.next) { const n = c.steps.find((s) => s.id === c.next); out(`\n${bold('Next:')} ${n.title} — ${n.summary}${n.actions.length ? dim(`  (${n.actions.map((a) => a.label).join(' / ')})`) : ''}`); }
}

function resultJson(extra = {}) {
  const S = engine.snapshot();
  return { ok: true, version: pkgJson.version, checks: S.checks, topology: S.topology, macs: S.macs.map((m) => ({ id: m.id, kind: m.kind, host: m.host, label: m.label, info: m.info })), sparks: S.sparks, pkg: S.pkg, enable: S.enable, settings: S.settings, ...extra };
}

function findLink(name) {
  const links = engine.state.topology.links;
  return links.find((l) => l.id === name || l.studio.iface === name || l.sparkName === name || l.spark.spark === name) || null;
}

/* ---------- commands ---------- */
const commands = {
  async bandwidth() {
    if (flags.demo) throw new Error('Bandwidth has no synthetic results');
    await engine.refresh('all');
    const link = findLink(rest[0]);
    const { bandwidthCommand } = require('../lib/bandwidth');
    const [program, args] = bandwidthCommand({ root: path.resolve(__dirname, '..', '..'), link, settings: store.get(), output: flags.output, options: flags.bandwidth });
    const { spawn } = require('child_process');
    const code = await new Promise((resolve, reject) => {
      const child = spawn(program, args, { stdio: ['ignore', flags.json ? 2 : 1, 2] });
      child.on('error', reject); child.on('close', (value) => resolve(value === null ? 1 : value));
    });
    if (flags.json) jsonOut({ ok: code === 0, output: path.resolve(flags.output) });
    return code === 0 ? 0 : 1;
  },
  async status() {
    await engine.refresh('all');
    const S = engine.snapshot();
    if (flags.json) jsonOut(resultJson({ ok: S.checks.overall !== 'fail' })); else printStatus(S);
    return S.checks.overall === 'fail' ? 1 : 0;
  },
  async enable() {
    await engine.refresh('all');
    const c = engine.state.checks;
    if (!flags.json) { printStatus(engine.snapshot()); out(''); }
    if (c.overall === 'ok') { if (flags.json) jsonOut(resultJson({ message: 'already enabled' })); else out(green('Already enabled and verified.')); return 0; }
    if (!(await confirm('Run every remaining step now (may ask for your password)?'))) return 1;
    const r = await engine.runAction('enable');
    if (flags.json) jsonOut(resultJson({ ok: !!r.ok, result: r })); else out((r.ok ? (r.waitingFor ? yellow : green) : red)(r.message));
    if (r.waitingFor) { if (!flags.json) out(dim(r.waitingFor === 'approval' ? 'Open System Settings → Privacy & Security, click Allow, restart, then run: mcdma enable' : 'Restart, then run: mcdma enable')); return 3; }
    return r.ok ? 0 : 1;
  },
  async detect() {
    await engine.refresh('all');
    if (!(await confirm('Probe every Mac-port/Spark-port pair with a short RDMA transfer (adds static neighbours; may ask for your password)?'))) return 1;
    const r = await engine.runAction('detectWiring');
    if (flags.json) jsonOut(resultJson({ ok: r.ok, result: r })); else { out((r.ok ? green : red)(r.message)); for (const l of engine.state.topology.links) out(linkLine(l)); }
    return r.ok ? 0 : 1;
  },
  async configure() {
    await engine.refresh('all');
    if (!engine.state.topology.links.length) { err(red('no links to configure; run: mcdma status')); return 1; }
    if (!(await confirm('Set addresses and neighbours on this Mac (password) and the Sparks (ssh), persisted?'))) return 1;
    const r = await engine.runAction('configureNetwork');
    if (flags.json) jsonOut(resultJson({ ok: r.ok, result: r })); else { out((r.ok ? green : red)(r.message)); for (const l of engine.state.topology.links) out(linkLine(l)); }
    return r.ok ? 0 : 1;
  },
  async test() {
    await engine.refresh('all');
    const links = rest[0] ? [findLink(rest[0])].filter(Boolean) : engine.state.topology.links;
    if (!links.length) { err(red(rest[0] ? `no link named ${rest[0]}` : 'no links; run: mcdma status')); return 1; }
    const results = [];
    for (const l of links) {
      if (!l.status.ready) { results.push({ link: l.id, passed: false, errors: ['link not configured'] }); if (!flags.json) out(`${red('✕')} ${l.id}: not configured`); continue; }
      if (!flags.json) out(`${bold(l.id)} ↔ ${l.sparkName} ${l.spark.iface}`);
      const r = await engine.runTest({ iface: l.id, latency: !flags.quick });
      results.push({ link: l.id, ...r, log: undefined });
      if (!flags.json) {
        if (r.passed) { out(`  ${green('passed')}${r.latency ? `  Mac write ${cyan(r.latency.mac.write.median + ' µs')} read ${cyan(r.latency.mac.read.median + ' µs')} · Spark write ${magenta(r.latency.spark.write.median + ' µs')} read ${magenta(r.latency.spark.read.median + ' µs')}  (p95 ${r.latency.mac.write.p95}/${r.latency.mac.read.p95})` : ''}`); }
        else out(`  ${red('failed')}: ${(r.errors || []).join('; ')}`);
      }
    }
    if (flags.json) jsonOut(resultJson({ ok: results.every((r) => r.passed), results }));
    return results.every((r) => r.passed) ? 0 : 1;
  },
  async driver() {
    const sub = rest[0];
    if (!['install', 'load'].includes(sub)) { err(USAGE); return 2; }
    await engine.refresh('studio');
    if (!(await confirm(sub === 'install' ? `Install driver ${engine.state.pkg && engine.state.pkg.version} (password)?` : 'Ask macOS to load the driver (password)?'))) return 1;
    const r = await engine.runAction(sub === 'install' ? 'installDriver' : 'loadDriver');
    if (flags.json) jsonOut(resultJson({ ok: r.ok, result: r })); else out((r.ok ? green : red)(r.message));
    return r.ok ? (r.loadKind && r.loadKind !== 'loaded' ? 3 : 0) : 1;
  },
  async sparks() { return hosts('sparks'); },
  async macs() { return hosts('macs'); },
  async linux() {
    const sub = rest[0] || 'list';
    if (flags.demo) throw new Error('Linux endpoint setup and tests require real hosts');
    if (sub === 'add' && rest[1]) {
      const r = await engine.addSpark({ host: rest[1], id: flags.name || rest[1].replace(/^.*@/, ''), kind: 'linux' });
      if (flags.json) jsonOut(r); else out(r.message);
      return r.ok ? 0 : 1;
    }
    if (sub === 'remove' && rest[1]) { engine.removeSpark(rest[1]); if (flags.json) jsonOut(store.get()); else out(`${rest[1]} removed`); return 0; }
    await engine.refresh('sparks');
    if (sub === 'list') {
      if (flags.json) jsonOut(engine.state.sparks);
      else for (const node of engine.state.sparks) out(`${node.id}  ${node.host}  ${node.arch || 'unknown architecture'}  ${node.reachable ? (node.gpus || []).join(', ') || 'Linux' : node.error || 'unreachable'}`);
      return engine.state.sparks.every((node) => node.reachable) ? 0 : 1;
    }
    if (sub === 'install' && rest[1]) {
      const peer = engine.state.sparks.find((node) => node.id === rest[1]);
      const { installLinuxPeer } = require('../lib/linuxinstall');
      const r = await installLinuxPeer({ peer });
      if (r.ok) engine.persist({ tools: { linuxPeerPaths: { ...store.get().tools.linuxPeerPaths, [peer.id]: r.binary } } });
      if (flags.json) jsonOut(r); else out(r.message);
      return r.ok ? 0 : 1;
    }
    if (sub === 'link') {
      const endpoint = (word) => { const match = /^([^/]+)\/(.+)$/.exec(word || ''); if (!match) throw new Error('Use registered NODE/IFACE endpoints'); return { node: match[1], iface: match[2] }; };
      const a = endpoint(rest[1]), b = endpoint(rest[2]);
      const id = flags.name || `${a.node}-${b.node}`;
      if (!/^[A-Za-z0-9_.-]{1,80}$/.test(id)) throw new Error('Invalid Linux link name');
      const r = engine.setLinuxLink({ id, a, b });
      if (flags.json) jsonOut(r); else out(`${id}: ${a.node}/${a.iface} ↔ ${b.node}/${b.iface}`);
      return 0;
    }
    const links = (engine.state.topology && engine.state.topology.linuxLinks || []).filter((l) => !rest[1] || l.id === rest[1]);
    if (sub === 'status') {
      const result = { peers: engine.state.sparks, links, transport: 'stock-linux-verbs', cpuSubmission: true, gpuInitiated: false };
      if (flags.json) jsonOut(result); else for (const l of links) out(`${l.id}: ${l.a.node}/${l.a.iface} ↔ ${l.b.node}/${l.b.iface} ${l.status.ready ? 'ready' : 'needs configuration'}`);
      return links.length && links.every((l) => l.status.ready) ? 0 : 1;
    }
    if (sub === 'configure') {
      if (!(await confirm('Configure managed Linux neighbours while preserving existing Mac links?'))) return 1;
      const r = await engine.configureLinuxNetwork(rest[1] ? [rest[1]] : null);
      if (flags.json) jsonOut(r); else out(r.message);
      return r.ok ? 0 : r.waitingFor ? 3 : 1;
    }
    if (sub === 'test') {
      if (!links.length) throw new Error('No Linux RDMA link selected');
      const results = [];
      for (const l of links) results.push({ link: l.id, ...await engine.runLinuxTest({ id: l.id, latency: !flags.quick, soft: flags.soft }) });
      if (flags.json) jsonOut({ ok: results.every((r) => r.passed), results });
      else for (const r of results) out(`${r.link}: ${r.passed ? 'passed' : (r.errors || []).join('; ')}${r.latency ? ` · ${r.latency.a.write.median}/${r.latency.a.read.median} µs from A · ${r.latency.b.write.median}/${r.latency.b.read.median} µs from B` : ''}`);
      return results.every((r) => r.passed) ? 0 : 1;
    }
    throw new Error('Use mcdma linux list, add, remove, link, install, status, configure or test');
  },
  async map() {
    const [key, target] = rest;
    if (!key || !target) { err(USAGE); return 2; }
    await engine.refresh('all');
    const port = engine.state.topology.ports.find((p) => p.key === key || p.iface === key);
    if (!port) { err(red(`no Mac port ${key}; ports: ${engine.state.topology.ports.map((p) => p.key).join(', ')}`)); return 1; }
    const t = target === 'none' ? null : { spark: target.split('/')[0], iface: target.split('/').slice(1).join('/') };
    if (t && !engine.state.topology.candidates.some((c) => c.spark === t.spark && c.iface === t.iface)) { err(red(`no Spark port ${target}; candidates: ${engine.state.topology.candidates.map((c) => c.key).join(', ')}`)); return 1; }
    engine.setMapping(port.key, t);
    if (flags.json) jsonOut(resultJson()); else for (const l of engine.state.topology.links) out(linkLine(l));
    return 0;
  },
  async keepalive() {
    const sub = rest[0] || 'status';
    if (sub === 'status') { const s = engine.keepalive.status(); if (flags.json) jsonOut(s); else out(s.running ? 'running' : 'not running in this process'); return 0; }
    if (sub === 'run' || sub === 'start') {
      const r = await engine.keepaliveOp('start');
      if (!r.ok) { err(red(r.message)); return 1; }
      err(dim('keep-alive running; press Ctrl-C to stop'));
      await new Promise((resolve) => { process.on('SIGINT', resolve); process.on('SIGTERM', resolve); });
      await engine.keepaliveOp('stop');
      return 0;
    }
    err(USAGE); return 2;
  },
  async monitor() {
    await engine.refresh('all');
    const cfg = engine.startMonitor();
    if (!cfg.links.length) { err(red('no links to monitor')); engine.stopMonitor(); return 1; }
    const fmt = (b) => b >= 1e9 ? `${(b / 1e9).toFixed(2)} GB/s` : b >= 1e6 ? `${(b / 1e6).toFixed(1)} MB/s` : `${(b / 1e3).toFixed(0)} KB/s`;
    const deadline = flags.seconds ? Date.now() + flags.seconds * 1000 : null;
    await new Promise((resolve) => {
      const stop = () => { engine.stopMonitor(); resolve(); };
      process.on('SIGINT', stop);
      engine.on('tick', (t) => {
        if (flags.json) out(JSON.stringify({ t: t.t, links: t.links, nodes: t.nodes, inference: t.inference }));
        else {
          const line = cfg.links.map((l) => { const d = t.links[l.id] || {}; return `${l.label} ${d.state === 'up' ? `↑${fmt(d.tx || 0)} ↓${fmt(d.rx || 0)}` : dim(d.state || 'down')}`; }).join('  │  ');
          const inf = Object.entries(t.inference || {}).filter(([, v]) => v).map(([k, v]) => `${k}: ${v.model || v.engine}${v.decodeTps != null ? ` ${Math.round(v.decodeTps)} tok/s` : ''}`).join('  ');
          out(`${new Date(t.t).toTimeString().slice(0, 8)}  ${line}${inf ? `  │  ${inf}` : ''}`);
        }
        if (deadline && Date.now() >= deadline) stop();
      });
    });
    return 0;
  },
  async package() {
    const script = path.join(__dirname, '..', 'tools', 'make-driver-package.sh');
    if (!fs.existsSync(script)) { err(red('tools/make-driver-package.sh not found')); return 1; }
    const { runInherit } = require('../lib/exec');
    const r = await runInherit('/bin/bash', [script]);
    out(r.out.trim()); return r.code === 0 ? 0 : 1;
  },
  async 'install-cli'() {
    const r = await engine.runAction('installCli', { repoDir: path.resolve(__dirname, '..'), node: process.execPath });
    if (flags.json) jsonOut(r); else out((r.ok ? green : red)(r.message));
    return r.ok ? 0 : 1;
  },
  async settings() {
    const [sub, key, ...valueParts] = rest;
    if (!sub || sub === 'get') { const s = store.get(); jsonOut(key ? key.split('.').reduce((o, k) => (o == null ? o : o[k]), s) : s); return 0; }
    if (sub === 'set' && key) {
      const raw = valueParts.join(' ');
      let value; try { value = JSON.parse(raw); } catch { value = raw; }
      const patch = {}; let o = patch; const ks = key.split('.');
      ks.slice(0, -1).forEach((k) => { o = o[k] = {}; }); o[ks[ks.length - 1]] = value;
      engine.setSettings(patch);
      jsonOut(store.get()); return 0;
    }
    err(USAGE); return 2;
  }
};

async function hosts(kind) {
  const [sub, host] = rest;
  if (!sub || sub === 'list') {
    await engine.refresh(kind === 'sparks' ? 'sparks' : 'macs');
    if (flags.json) { jsonOut(kind === 'sparks' ? engine.state.sparks : engine.state.macs); return 0; }
    if (kind === 'sparks') for (const s of engine.state.sparks) out(`${s.reachable ? green('✓') : red('✕')} ${bold(s.id)}  ${s.host}  ${s.reachable ? `${s.os || 'Linux'} · ${(s.gpus || []).join(', ') || 'no GPU'} · ${s.ports.filter((p) => p.primary).map((p) => `${p.iface}${p.speedGbps ? ` ${p.speedGbps}G` : ''}${p.link ? '' : ' down'}`).join(', ')}${s.root ? '' : ' · not root'}` : s.error || 'unreachable'}`);
    else for (const m of engine.state.macs) out(`${m.info && m.info.ok ? green('✓') : red('✕')} ${bold(m.id)}  ${m.host || 'this Mac'}  ${m.info && m.info.ok ? `${m.info.chip.brand} · ${m.info.pci.cards.map((c) => c.name).join(', ') || 'no card'} · driver ${m.info.loaded.loaded ? m.info.loaded.version : 'not loaded'}` : (m.info && m.info.error) || 'unreachable'}`);
    return 0;
  }
  if (sub === 'add' && host) {
    const r = kind === 'sparks' ? await engine.addSpark({ host, id: flags.name || host.replace(/^.*@/, '') }) : await engine.addMac({ host, id: flags.name || host.replace(/^.*@/, '') });
    if (flags.json) jsonOut(r); else out((r.ok ? green : red)(r.message));
    return r.ok ? 0 : 1;
  }
  if (sub === 'remove' && host) { if (kind === 'sparks') engine.removeSpark(host); else engine.removeMac(host); if (!flags.json) out(`${host} removed`); else jsonOut(store.get()); return 0; }
  err(USAGE); return 2;
}

(async () => {
  const fn = commands[cmd];
  if (!fn) { err(`unknown command ${cmd}\n`); err(USAGE); process.exit(2); }
  let code = 1;
  try { code = await fn(); }
  catch (e) { if (flags.json) jsonOut({ ok: false, error: e.message }); else err(red(e.stack || e.message)); code = 1; }
  finally { engine.dispose(); }
  // stdout to a pipe is asynchronous on macOS: let it drain before exiting
  await new Promise((r) => process.stdout.write('', r));
  await new Promise((r) => process.stderr.write('', r));
  process.exit(code);
})();
