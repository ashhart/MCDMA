'use strict';
// The MCDMA engine: discovery, topology, checklist, actions, tests, the
// Enable flow and the monitor loop. bin/mcdma.js is a thin front-end over it.
const EventEmitter = require('events');
const { Host, dispose } = require('./exec');
const macinfo = require('./macinfo');
const sparksProbe = require('./sparks');
const topology = require('./topology');
const checks = require('./checks');
const actions = require('./actions');
const testrun = require('./testrun');
const driverpkg = require('./driverpkg');
const demo = require('./demo');
const { sshConfigHosts } = require('./store');
const { FabricCollector } = require('./collect');
const linuxlinks = require('./linuxlinks');

const { compareVersions } = require('./version');

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const EMPTY_ENABLE = { running: false, stage: null, message: null, waitingFor: null, done: false, error: null };

class Engine extends EventEmitter {
  // store: lib/store Store; packagePaths: { override?, resourcesPath?, appPath? }
  constructor({ store, packagePaths = {}, demo: demoMode = false, demoStage = 'ready', version = '0.0.0' }) {
    super();
    this.store = store;
    this.packagePaths = packagePaths;
    this.version = version;
    this.keepalive = new testrun.Keepalive();
    this.collector = null; this.monitorTimer = null; this.monitorInFlight = false;
    this.state = { ready: false, busy: { refresh: false, action: null, test: null }, studio: null, macs: [], sparks: [], topology: null, checks: null, pkg: null,
      lastRefresh: 0, demo: !!demoMode || !!store.get().demo, demoStage, monitorRunning: false, log: [], enable: { ...EMPTY_ENABLE }, lastBootPolicy: null };
  }

  // Persist a settings patch, except in demo mode where it stays in memory.
  persist(patch) { if (this.state.demo) this.store.override(patch); else this.store.set(patch); }

  /* ---------- logging & events ---------- */
  log(line) { const entry = `${new Date().toISOString().slice(11, 19)} ${line}`; this.state.log.push(entry); if (this.state.log.length > 800) this.state.log.shift(); this.emit('log', entry); }
  toast(kind, message) { this.emit('toast', { kind, message }); }
  progress(scope, message, extra = {}) { this.log(`[${scope}] ${message}`); this.emit('progress', { scope, message, ...extra }); }
  publish() { this.emit('state', this.snapshot()); }

  /* ---------- hosts ---------- */
  primaryHost() { const s = this.store.get().studio; return s.mode === 'ssh' ? new Host('ssh', s.host || 'studio') : new Host('local'); }
  macHost(id) { const m = this.state.macs.find((x) => x.id === id); if (!m || m.kind === 'local') return new Host('local'); return new Host('ssh', m.host); }
  sparkHosts() { return this.store.get().sparks.map((s) => ({ ...s, hostObj: new Host('ssh', s.host) })); }
  macList() {
    const s = this.store.get();
    const primary = { id: 'local', kind: s.studio.mode === 'ssh' ? 'ssh' : 'local', host: s.studio.mode === 'ssh' ? (s.studio.host || 'studio') : null };
    return [primary, ...(s.macs || []).map((m) => ({ id: m.id, kind: 'ssh', host: m.host }))];
  }
  get local() { return this.store.get().studio.mode !== 'ssh'; }

  snapshot() {
    const settings = this.store.get();
    const st = this.state;
    return { ready: st.ready, busy: st.busy, studio: st.studio, macs: st.macs.map((m) => ({ id: m.id, kind: m.kind, host: m.host, label: m.label, info: m.info })), sparks: st.sparks,
      topology: st.topology, checks: st.checks, pkg: st.pkg, settings, keepalive: this.keepalive.status(), lastRefresh: st.lastRefresh, demo: st.demo, demoStage: st.demoStage,
      monitorRunning: st.monitorRunning, enable: st.enable, appVersion: this.version, platform: { local: this.local } };
  }

  recompute() {
    const settings = this.store.get();
    this.state.topology = topology.build({ macs: this.state.macs, sparks: this.state.sparks, settings, lastTests: settings.lastTests || {} });
    this.state.checks = checks.build({ studio: this.state.studio, macs: this.state.macs, sparks: this.state.sparks, topology: this.state.topology, pkg: this.state.pkg, enable: this.state.enable });
    this.publish();
    return this.state.checks;
  }

  locatePackage() {
    const settings = this.store.get();
    this.state.pkg = driverpkg.locate({ override: settings.driverPackage || this.packagePaths.override || null, resourcesPath: this.packagePaths.resourcesPath || null, appPath: this.packagePaths.appPath || null });
    return this.state.pkg;
  }

  /* ---------- discovery ---------- */
  async refresh(what = 'all') {
    if (this.state.busy.refresh) return this.state.checks;
    this.state.busy.refresh = true; this.publish();
    try {
      this.locatePackage();
      const settings = this.store.get();
      if (this.state.demo) {
        const s = demo.studio(this.state.demoStage);
        this.state.macs = [{ id: 'local', kind: 'local', host: null, label: s.chip.hostname, info: s }];
        this.state.studio = s;
        this.state.sparks = demo.sparks(this.state.demoStage);
      } else {
        const jobs = [];
        if (what === 'all' || what === 'studio' || what === 'macs') {
          jobs.push(Promise.all(this.macList().map(async (m) => {
            const info = await macinfo.probe(m.kind === 'local' ? new Host('local') : new Host('ssh', m.host));
            if (m.id === 'local' && this.state.lastBootPolicy && info.ok) info.bootPolicy = this.state.lastBootPolicy;
            return { ...m, label: info.ok ? (info.chip.hostname || m.id) : (m.host || 'This Mac'), info };
          })).then((r) => { this.state.macs = r; this.state.studio = r[0].info; }));
        }
        if (what === 'all' || what === 'sparks') jobs.push(Promise.all(this.sparkHosts().map(async (s) => ({ ...(await sparksProbe.probe(s.hostObj)), id: s.id, host: s.host, kind: s.kind || 'spark' }))).then((r) => { this.state.sparks = r; }));
        await Promise.all(jobs);
      }
      this.state.lastRefresh = Date.now(); this.state.ready = true;
      if (!settings.firstRunDone && !this.state.sparks.length && !this.state.demo) await this.autoAddSparks();
      this.log(`refresh ${what}: ${this.state.macs.filter((m) => m.info && m.info.ok).length}/${this.state.macs.length} macs, ${this.state.sparks.filter((s) => s.reachable).length}/${this.state.sparks.length} sparks`);
    } catch (e) { this.log(`refresh failed: ${e.message}`); this.toast('error', `Refresh failed: ${e.message}`); }
    finally { this.state.busy.refresh = false; this.recompute(); }
    return this.state.checks;
  }

  // First run: adopt ssh hosts that look like Sparks so the fabric appears without typing.
  async autoAddSparks() {
    const hosts = sshConfigHosts().filter((h) => /spark|dgx/i.test(h.host));
    const seen = new Set(); const candidates = [];
    for (const h of hosts) if (!seen.has(h.host)) { seen.add(h.host); candidates.push({ id: h.host, host: h.host }); }
    const probed = await Promise.all(candidates.map(async (s) => ({ ...(await sparksProbe.probe(new Host('ssh', s.host))), id: s.id, host: s.host })));
    // keep the ones that answer and actually have RDMA ports; the rest stay as suggestions in the add dialog
    const keep = probed.filter((s) => s.reachable && (s.ports || []).some((p) => p.primary));
    this.store.set({ sparks: keep.map((s) => ({ id: s.id, host: s.host })), firstRunDone: true });
    this.state.sparks = keep;
    if (keep.length) this.log(`first run: adopted ${keep.map((s) => s.host).join(', ')} from ~/.ssh/config`);
  }

  /* ---------- settings & inventory ---------- */
  setSettings(patch = {}) {
    const before = this.store.get();
    this.store.set(patch);
    const after = this.store.get();
    if (patch.demo !== undefined) this.state.demo = !!patch.demo;
    if (patch.demoStage) this.state.demoStage = patch.demoStage;
    const reprobe = patch.studio || patch.macs || patch.sparks || patch.demo !== undefined || patch.demoStage || patch.driverPackage !== undefined;
    if (reprobe) this.refresh('all'); else this.recompute();
    if (this.state.monitorRunning && (patch.monitor || JSON.stringify(before.mapping) !== JSON.stringify(after.mapping))) this.startMonitor();
    return after;
  }
  setMapping(key, target) {
    const s = this.store.get();
    const mapping = { ...s.mapping, [key]: target }; // null = leave unassigned
    const wiringVerified = { ...s.wiringVerified };
    if (!target || !wiringVerified[key] || wiringVerified[key].spark !== target.spark || wiringVerified[key].iface !== target.iface) delete wiringVerified[key];
    this.store.set({ mapping, wiringVerified });
    this.recompute();
    if (this.state.monitorRunning) this.startMonitor();
    return this.store.get();
  }
  async addSpark({ host, id, kind = 'spark' }) {
    host = String(host || '').trim(); id = String(id || host).trim();
    if (!/^[A-Za-z0-9_.@-]{1,80}$/.test(host) || !/^[A-Za-z0-9_.-]{1,40}$/.test(id)) return { ok: false, message: 'invalid host or name' };
    const t = await sparksProbe.testSsh(new Host('ssh', host));
    if (kind === 'linux' && (!t.ok || t.system !== 'Linux')) return { ok: false, message: t.ok ? `${host} is not Linux (${t.system})` : `ssh ${host} failed: ${t.error}` };
    const s = this.store.get();
    if (s.sparks.some((x) => x.id === id)) return { ok: false, message: `${id} is already added` };
    this.store.set({ sparks: [...s.sparks, { id, host, kind }], firstRunDone: true });
    await this.refresh('sparks');
    return { ok: true, message: t.ok ? `${id} added (${t.hostname}${t.root ? ', root' : ''})` : `${id} added but ssh failed: ${t.error}` };
  }
  removeSpark(id) {
    const s = this.store.get();
    const mapping = { ...s.mapping };
    for (const [k, v] of Object.entries(mapping)) if (v && v.spark === id) delete mapping[k];
    this.store.set({ sparks: s.sparks.filter((x) => x.id !== id), mapping, linuxLinks: (s.linuxLinks || []).filter((l) => l.a.node !== id && l.b.node !== id) });
    this.state.sparks = this.state.sparks.filter((x) => x.id !== id);
    this.recompute();
    return this.store.get();
  }
  async addMac({ host, id }) {
    host = String(host || '').trim(); id = String(id || host).trim();
    if (!/^[A-Za-z0-9_.@-]{1,80}$/.test(host) || !/^[A-Za-z0-9_.-]{1,40}$/.test(id) || id === 'local') return { ok: false, message: 'invalid host or name' };
    const t = await sparksProbe.testSsh(new Host('ssh', host));
    if (!t.ok) return { ok: false, message: `ssh ${host} failed: ${t.error}` };
    if (t.system !== 'Darwin') return { ok: false, message: `${host} is not a Mac (${t.system})` };
    const s = this.store.get();
    if ((s.macs || []).some((x) => x.id === id)) return { ok: false, message: `${id} is already added` };
    this.store.set({ macs: [...(s.macs || []), { id, host }] });
    await this.refresh('macs');
    return { ok: true, message: `${id} added (${t.hostname})` };
  }
  removeMac(id) {
    const s = this.store.get();
    const mapping = { ...s.mapping };
    for (const k of Object.keys(mapping)) if (k.startsWith(`${id}:`)) delete mapping[k];
    this.store.set({ macs: (s.macs || []).filter((x) => x.id !== id), mapping });
    this.state.macs = this.state.macs.filter((x) => x.id !== id);
    this.recompute();
    return this.store.get();
  }
  testSsh(host) { return sparksProbe.testSsh(new Host('ssh', host)); }
  sshHosts() { return sshConfigHosts(); }
  scanSsh() { return actions.scanSsh(); }

  setLinuxLink({ id, a, b }) {
    const settings = this.store.get();
    const records = [...(settings.linuxLinks || []).filter((l) => l.id !== id), { id, a, b }];
    linuxlinks.buildLinuxLinks({ peers: this.state.sparks, savedLinks: records, lastTests: settings.lastLinuxTests || {} });
    this.persist({ linuxLinks: records });
    this.recompute();
    return this.state.topology.linuxLinks.find((l) => l.id === id);
  }

  async runLinuxTest({ id, latency = true, soft = false }) {
    if (this.state.busy.test) return { passed: false, errors: ['a test is already running'] };
    const link = (this.state.topology && this.state.topology.linuxLinks || []).find((l) => l.id === id);
    if (!link) return { passed: false, errors: ['no such Linux RDMA link'] };
    if (this.state.demo) return { passed: false, errors: ['Linux transfers require real endpoints'] };
    this.state.busy.test = id; this.publish();
    let result;
    try {
      const store = this.store.get();
      result = await linuxlinks.runLinuxTransferTest({ link, settings: soft ? { ...store, test: { ...store.test, soft: true } } : store, latency, onProgress: (m) => this.progress('linux-test', m) });
      result.identity = link.identity;
      this.persist({ lastLinuxTests: { ...this.store.get().lastLinuxTests, [id]: result } });
    } catch (e) { result = { passed: false, errors: [e.message] }; }
    finally { this.state.busy.test = null; this.recompute(); }
    if (result && result.passed) await this.refresh('sparks');
    return result;
  }

  async configureLinuxNetwork(ids = null) {
    const links = (this.state.topology && this.state.topology.linuxLinks || []).filter((l) => !ids || ids.includes(l.id));
    if (!links.length) return { ok: false, message: 'no Linux links selected' };
    const entries = linuxlinks.linuxNeighbourEntries(links);
    const results = [];
    for (const [id, neighbours] of Object.entries(entries)) {
      const peer = this.state.sparks.find((p) => p.id === id);
      if (!peer || !peer.reachable) return { ok: false, message: `${id}: Linux endpoint is unavailable` };
      results.push(await actions.configureSpark({ spark: peer, entries: neighbours }));
    }
    return { ok: results.length > 0 && results.every((r) => r.ok), waitingFor: results.find((r) => r.waitingFor)?.waitingFor || null, message: results.map((r) => r.message).join(' ') };
  }

  /* ---------- actions ---------- */
  linkEntriesBySpark(links) {
    const by = {};
    for (const l of links) (by[l.spark.spark] = by[l.spark.spark] || []).push({ iface: l.spark.iface, studioLinkLocal: l.expected.studioLinkLocal, studioMac: l.studio.mac });
    return by;
  }

  async configureNetwork({ onProgress = () => {} } = {}) {
    const topo = this.state.topology || { links: [], linuxLinks: [] };
    const links = topo.links;
    if (!links.length && !(topo.linuxLinks || []).length) return { ok: false, message: 'no links to configure' };
    const allEntries = this.linkEntriesBySpark(links);
    for (const [node, entries] of Object.entries(linuxlinks.linuxNeighbourEntries(topo.linuxLinks || []))) allEntries[node] = [...(allEntries[node] || []), ...entries];
    const rs = [];
    const localLinks = links.filter((l) => l.mac.kind === 'local');
    if (localLinks.length) { onProgress('Configuring this Mac (administrator rights)…'); rs.push(await actions.configureStudio({ links: localLinks })); }
    const remote = links.filter((l) => l.mac.kind !== 'local');
    if (remote.length) rs.push({ ok: true, message: `${remote.length} link${remote.length === 1 ? '' : 's'} on ${[...new Set(remote.map((l) => l.mac.label))].join(', ')} must be configured on that Mac.` });
    for (const [sparkId, entries] of Object.entries(allEntries)) {
      const sp = this.state.topology.sparks.find((s) => s.id === sparkId);
      if (!sp) continue;
      onProgress(`Configuring ${sp.hostname || sp.id}…`);
      rs.push(await actions.configureSpark({ spark: sp, entries }));
    }
    return { ok: rs.every((x) => x.ok), waitingFor: rs.find((x) => x.waitingFor)?.waitingFor || null, message: rs.map((x) => x.message).join(' ') };
  }

  async runAction(id, args = {}) {
    if (this.state.busy.action) return { ok: false, message: `busy with ${this.state.busy.action}` };
    this.state.busy.action = id; this.publish();
    const settings = this.store.get();
    const local = this.local;
    let r;
    try {
      switch (id) {
        case 'recheck': await this.refresh('all'); r = { ok: true, message: 'Re-checked.' }; break;
        case 'installDriver': r = local ? await actions.installDriver({ pkg: this.state.pkg }) : { ok: false, message: 'install on the Mac that has the card' }; break;
        case 'loadDriver': r = local ? await actions.loadDriver() : { ok: false, message: 'load the driver on the Mac that has the card' }; break;
        case 'restart': r = await actions.restart(); break;
        case 'openSecurity': r = await actions.openSecurity(); break;
        case 'bootPolicy': r = await actions.bootPolicy(); if (r.ok && r.policy) { this.state.lastBootPolicy = r.policy; if (this.state.studio) this.state.studio.bootPolicy = r.policy; } break;
        case 'configureNetwork': case 'configureSparks': r = await this.configureNetwork({ onProgress: (m) => this.progress('action', m) }); break;
        case 'detectWiring': {
          if (this.state.demo) { r = { ok: true, message: 'Demo: wiring confirmed as configured.' }; break; }
          r = await actions.detectWiring({ topology: this.state.topology, settings, macHosts: (mid) => this.macHost(mid), onProgress: (m) => this.progress('action', m) });
          if (r.ok) { const s = this.store.get(); this.persist({ mapping: { ...s.mapping, ...r.mapping }, wiringVerified: { ...s.wiringVerified, ...r.verified } }); }
          break;
        }
        case 'installSparkPeer': {
          const sp = this.state.sparks.find((s) => s.id === args.spark);
          if (sp && sp.kind === 'linux') {
            r = await require('./linuxinstall').installLinuxPeer({ peer: sp });
            if (r.ok) this.persist({ tools: { linuxPeerPaths: { ...this.store.get().tools.linuxPeerPaths, [sp.id]: r.binary } } });
          } else r = sp && this.state.pkg && this.state.pkg.available ? await testrun.installSparkPeer({ archive: this.state.pkg.archive, sparkHostAlias: sp.host }) : { ok: false, message: 'Spark or driver package not available' };
          break;
        }
        case 'installCli': r = await actions.installCli(args); break;
        case 'enable': r = await this.enableRdma(); break;
        case 'cancelEnable': this.store.set({ enableInProgress: false }); this.state.enable = { ...EMPTY_ENABLE }; r = { ok: true, message: 'Setup flow cancelled.' }; break;
        default: r = { ok: false, message: `unknown action ${id}` };
      }
    } catch (e) { r = { ok: false, message: e.message || String(e) }; }
    this.log(`action ${id}: ${r.ok ? 'ok' : 'failed'} - ${r.message}`);
    this.state.busy.action = null;
    if (id !== 'enable') this.toast(r.ok ? 'ok' : r.cancelled ? 'warn' : 'error', r.message);
    this.publish();
    if (!['openSecurity', 'bootPolicy', 'enable', 'cancelEnable', 'installCli'].includes(id)) await this.refresh('all'); else this.recompute();
    return r;
  }

  async runTest({ iface, latency = true }) {
    if (this.state.busy.test) return { passed: false, errors: ['a test is already running'] };
    const link = (this.state.topology || { links: [] }).links.find((l) => l.id === iface);
    if (!link) return { passed: false, errors: ['no such link'] };
    this.state.busy.test = iface; this.publish();
    let result;
    try {
      if (this.state.demo) {
        await sleep(1500);
        result = { at: Date.now(), spark: link.spark.spark, iface: link.spark.iface, studioIface: link.studio.iface, passed: true, errors: [], payload: 4096, mtu: 1024, arm: 'bf64', log: ['demo'],
          latency: { mac: { write: { median: 7.3, min: 6.4, p95: 8.9, p99: 15.9, max: 40.1, mean: 7.6 }, read: { median: 5.9, min: 5.4, p95: 6.8, p99: 9.5, max: 20.3, mean: 6.0 } }, spark: { write: { median: 3.4, min: 3.1, p95: 4.0, p99: 6.1, max: 12.2, mean: 3.5 }, read: { median: 5.3, min: 5.0, p95: 6.0, p99: 8.0, max: 14.0, mean: 5.4 } } } };
      } else {
        result = await testrun.runTransferTest({ studioHost: this.macHost(link.mac.id), link, settings: this.store.get(), latency, onProgress: (m) => this.progress('test', m, { iface }) });
      }
      result.identity = link.identity;
      result.conditions = { gpuKeepalive: this.keepalive.running ? 'managed helper active' : 'uncontrolled', iterations: latency ? 1000 : 0 };
      const s = this.store.get();
      this.persist({ lastTests: { ...s.lastTests, [iface]: { ...result, log: (result.log || []).slice(-60) } } });
      this.toast(result.passed ? 'ok' : 'error', result.passed ? `${link.studio.iface} ↔ ${link.sparkName}: transfer test passed` : `${link.studio.iface} ↔ ${link.sparkName}: ${result.errors[0] || 'failed'}`);
    } catch (e) { result = { passed: false, errors: [e.message] }; this.toast('error', e.message); }
    this.state.busy.test = null;
    this.recompute();
    return result;
  }

  /* ---------- one-click flow ---------- */
  setEnable(patch) { this.state.enable = { ...this.state.enable, ...patch }; this.publish(); }

  // Runs every remaining step in order. Stops (and remembers) when macOS needs
  // the user: approval in System Settings, a restart. Resumes on the next launch.
  async enableRdma() {
    if (this.state.enable.running) return { ok: false, message: 'already running' };
    this.setEnable({ running: true, done: false, error: null, waitingFor: null, stage: 'checks', message: 'Checking this Mac and the Sparks…' });
    this.persist({ enableInProgress: true });
    const st = this.state;
    const say = (stage, message) => { this.setEnable({ stage, message }); this.progress('enable', message); };
    const fail = (message, waitingFor = null) => { this.setEnable({ running: false, error: waitingFor ? null : message, waitingFor, message }); return { ok: !!waitingFor, waitingFor, message }; };
    try {
      await this.refresh('all');
      const c = st.checks;
      const local = this.local;
      const sys = c.steps.find((s) => s.id === 'system'), hw = c.steps.find((s) => s.id === 'hardware');
      if (sys.status === 'fail') return fail(sys.items.find((i) => i.status === 'fail').hint || sys.summary);
      if (hw.status === 'fail') return fail('No ConnectX card found. Connect the enclosure, then run mcdma enable again.');
      if (!st.studio.loaded.loaded) {
        if (!local) return fail('The Mac with the card is managed over ssh: install the driver on that Mac.');
        if (!st.pkg || !st.pkg.available) return fail('No driver package found. Build one with npm run package or set settings.driverPackage.');
        const order = compareVersions(st.pkg.version, st.studio.kext.version);
        if (st.studio.kext.installed && order === null) return fail('Cannot compare installed and package versions.');
        const needsInstall = !st.studio.kext.installed || order === 1;
        if (st.demo) { say('driver', 'Installing the driver (demo)…'); await sleep(1200); this.persist({ enableInProgress: false }); this.setEnable({ running: false, waitingFor: 'approval', message: 'Driver installed. Allow it in System Settings → Privacy & Security, then restart.' }); return { ok: true, waitingFor: 'approval', message: st.enable.message }; }
        say('driver', needsInstall ? `Installing driver ${st.pkg.version} (administrator rights)…` : 'Loading the driver (administrator rights)…');
        const r = needsInstall ? await actions.installDriver({ pkg: st.pkg }) : await actions.loadDriver();
        if (r.cancelled) return fail('Cancelled at the administrator prompt.');
        if (!r.ok) return fail(r.message);
        if (r.loadKind === 'approval') { await this.refresh('studio'); return fail('Driver installed. Now allow it in System Settings → Privacy & Security, then restart and run mcdma enable again.', 'approval'); }
        if (r.loadKind === 'restart') { await this.refresh('studio'); return fail('Driver installed. Restart to load it, then run mcdma enable again.', 'restart'); }
        await this.refresh('studio');
        if (!st.studio.loaded.loaded) return fail('The driver did not load. See mcdma status for details.');
      }
      if (st.studio.registry.some((r) => r.quarantined || r.startError)) return fail('The driver reports a start error or quarantine on a port. A restart usually clears it.');
      if (!st.sparks.length) return fail('Add at least one Spark (mcdma sparks add HOST), then run mcdma enable again.');
      if (!st.sparks.some((s) => s.reachable)) return fail(`No Spark is reachable over ssh: ${st.sparks.map((s) => `${s.id}: ${s.error || 'failed'}`).join('; ')}`);
      for (const sp of st.sparks) {
        if (sp.reachable && !sp.peerTools.length && st.pkg && st.pkg.available && !st.demo) {
          say('sparks', `Installing the test tool on ${sp.hostname || sp.id}…`);
          const r = await testrun.installSparkPeer({ archive: st.pkg.archive, sparkHostAlias: sp.host });
          if (!r.ok) this.log(`installSparkPeer ${sp.id}: ${r.message}`);
        }
      }
      await this.refresh('sparks');
      let topo = st.topology;
      if (!topo.links.length) return fail(topo.candidates.length ? 'No Mac port is assigned to a Spark port. Run mcdma detect or mcdma map.' : 'No Spark port faces a Mac: check the QSFP cables.');
      if (topo.links.some((l) => l.reason === 'guess') && !st.demo) {
        say('topology', 'Working out which Spark each port is cabled to…');
        const r = await actions.detectWiring({ topology: topo, settings: this.store.get(), macHosts: (mid) => this.macHost(mid), onProgress: (m) => say('topology', m) });
        if (r.cancelled) return fail('Cancelled at the administrator prompt.');
        if (r.ok) { const s = this.store.get(); this.persist({ mapping: { ...s.mapping, ...r.mapping }, wiringVerified: { ...s.wiringVerified, ...r.verified } }); }
        else this.log(`detectWiring: ${r.message}`);
        await this.refresh('all'); topo = st.topology;
      }
      if (topo.links.some((l) => !l.status.configured || !l.status.sparkPersisted || (l.mac.kind === 'local' && !l.status.studioPersisted))) {
        say('network', 'Setting addresses and neighbours…');
        if (!st.demo) {
          const r = await this.configureNetwork({ onProgress: (m) => say('network', m) });
          if (!r.ok) return fail(r.message);
        } else await sleep(900);
        await this.refresh('all'); topo = st.topology;
      }
      const notReady = topo.links.filter((l) => !l.status.ready);
      if (notReady.length) return fail(`${notReady.map((l) => `${l.studio.iface} ↔ ${l.sparkName}`).join(', ')}: ${notReady[0].status.portsActive ? 'still not configured' : 'no link on the port'}. See mcdma status.`);
      for (const l of topo.links) {
        say('test', `Testing ${l.studio.iface} ↔ ${l.sparkName} with real RDMA transfers…`);
        const t = await this.runTest({ iface: l.id, latency: true });
        if (!t.passed) return fail(`${l.studio.iface} ↔ ${l.sparkName}: ${(t.errors || [])[0] || 'transfer test failed'}`);
      }
      this.persist({ enableInProgress: false });
      this.setEnable({ running: false, done: true, stage: 'done', message: 'RDMA is enabled and verified on every link.' });
      this.toast('ok', 'RDMA is enabled and verified.');
      this.recompute();
      return { ok: true, message: st.enable.message };
    } catch (e) {
      this.log(`enable failed: ${e.stack || e}`);
      return fail(e.message || String(e));
    }
  }

  // After a restart the Enable flow asked for, pick up where it stopped.
  async resumeIfNeeded() {
    const s = this.store.get();
    if (!s.enableInProgress || this.state.demo || !this.state.studio || !this.state.studio.ok) return false;
    if (this.state.studio.loaded.loaded) { this.log('resuming the Enable RDMA flow after restart'); await this.runAction('enable'); return true; }
    if (this.state.studio.kext.installed) {
      const approval = this.state.studio.installState && /approv|policy|consent/i.test(this.state.studio.installState.loadLog || '');
      this.setEnable({ waitingFor: approval ? 'approval' : 'restart', message: 'The driver is installed but not loaded yet. Allow it in System Settings → Privacy & Security if asked, then restart and run mcdma enable again.' });
    }
    return false;
  }

  /* ---------- keep-alive ---------- */
  async keepaliveOp(op) {
    const r = op === 'start' ? await this.keepalive.start(this.primaryHost(), this.store.get()) : op === 'stop' ? await this.keepalive.stop() : { ok: true, message: this.keepalive.running ? 'running' : 'off' };
    if (op !== 'status') { this.toast(r.ok ? 'ok' : 'error', r.message); this.log(`keepalive ${op}: ${r.message}`); }
    this.publish();
    return { ...r, status: this.keepalive.status() };
  }

  /* ---------- monitor ---------- */
  monitorConfig() {
    const settings = this.store.get();
    const topo = this.state.topology || { links: [], sparkLinks: [], sparks: [], macs: [], ports: [] };
    const hosts = {};
    const nodes = [];
    for (const m of topo.macs) {
      const card = m.cards && m.cards[0];
      nodes.push({ id: m.id, label: m.label || 'Mac', sub: card ? `${card.name}${card.enclosure ? ` · ${card.enclosure.name}` : ''}` : 'Mac', ip: topo.ports.filter((p) => p.macId === m.id && p.iface).map((p) => p.iface).join(' · '), kind: 'studio' });
      if (m.kind === 'ssh') hosts[m.id] = { ssh: m.host, os: 'mac', pings: [] };
    }
    for (const s of topo.sparks) { hosts[s.id] = { ssh: s.host, pings: [] }; nodes.push({ id: s.id, label: s.hostname || s.id, sub: s.gpus[0] ? s.gpus[0].replace(/^GPU \d+: /, '') : 'Spark', ip: s.ports.filter((p) => p.primary).map((p) => p.iface).join(' · '), kind: 'spark' }); }
    const links = [];
    const safe = (x) => x.replace(/[^A-Za-z0-9_-]/g, '-');
    for (const l of topo.links) links.push({ id: `cx-${safe(l.id)}`, from: l.mac.id, to: l.spark.spark, label: `${l.studio.iface} ↔ ${l.spark.iface}`, kind: 'cx', speedGbps: l.spark.speedGbps || 100, swap: true, source: { type: 'ssh', host: l.spark.spark, iface: l.spark.iface } });
    for (const s of topo.sparkLinks) links.push({ id: `roce-${safe(s.id)}`, from: s.a.spark, to: s.b.spark, label: `${s.a.iface} ↔ ${s.b.iface}`, kind: 'roce', speedGbps: s.speedGbps || 200, source: { type: 'ssh', host: s.a.spark, iface: s.a.iface, fallback: { host: s.b.spark, iface: s.b.iface } } });
    for (const x of settings.monitor.extraLinks || []) links.push(x);
    return { pollMs: settings.monitor.pollMs || 1000, heavyEveryTicks: settings.monitor.heavyEveryTicks || 5, hosts, nodes, links };
  }
  startMonitor() {
    this.stopMonitor();
    const cfg = this.monitorConfig();
    this.state.monitorRunning = true;
    if (this.state.demo) {
      let phase = 0;
      this.monitorTimer = setInterval(() => { phase += 0.35; this.emit('tick', demo.demoTick(cfg, phase)); }, cfg.pollMs);
      return cfg;
    }
    this.collector = new FabricCollector(cfg);
    this.monitorTimer = setInterval(async () => {
      if (this.monitorInFlight) return;
      this.monitorInFlight = true;
      try { this.emit('tick', await this.collector.tick()); } catch (e) { this.log(`monitor tick failed: ${e.message}`); }
      this.monitorInFlight = false;
    }, cfg.pollMs);
    return cfg;
  }
  stopMonitor() { if (this.monitorTimer) clearInterval(this.monitorTimer); this.monitorTimer = null; if (this.collector) this.collector.dispose(); this.collector = null; this.state.monitorRunning = false; }

  dispose() { this.stopMonitor(); this.keepalive.stop(); dispose([this.primaryHost(), ...this.state.macs.filter((m) => m.kind === 'ssh').map((m) => new Host('ssh', m.host)), ...this.sparkHosts().map((s) => s.hostObj)]); }
}

module.exports = { Engine };
