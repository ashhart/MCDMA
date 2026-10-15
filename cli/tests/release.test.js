'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');
const os = require('os');
const crypto = require('crypto');
const { spawnSync } = require('child_process');
const { compareVersions, installDecision } = require('../lib/version');
const { validateMode, validateTrace, validateTestSettings } = require('../lib/verification');
const { ARMS } = require('../lib/testrun');
const { Engine } = require('../lib/engine');
const { Store, DEFAULTS } = require('../lib/store');
const topology = require('../lib/topology');
const checks = require('../lib/checks');
const demo = require('../lib/demo');
const actions = require('../lib/actions');
const exec = require('../lib/exec');

function fixture(version='0.1.18') {
  const info=demo.studio('ready'); info.loaded.version=info.kext.version=version;
  const macs=[{id:'local',kind:'local',info}], sparks=demo.sparks('ready'), settings=structuredClone(DEFAULTS);
  const topo=topology.build({macs,sparks,settings});
  return {info,macs,sparks,settings,topo};
}

test('numeric versions distinguish a downgrade from an update', () => {
  assert.equal(compareVersions('0.1.18','0.1.17'),1);
  assert.equal(compareVersions('0.1.9','0.1.18'),-1);
  assert.equal(compareVersions('0.1.18','0.1.18'),0);
  assert.equal(compareVersions('unknown','0.1.18'),null);
  assert.equal(installDecision('0.1.18','0.1.17').allowed,false);
  assert.equal(installDecision('unknown','0.1.18').allowed,false);
});

test('status does not offer the older package as an update or repair', () => {
  const f=fixture(); f.info.provider.present=false;
  const out=checks.build({studio:f.info,macs:f.macs,sparks:f.sparks,topology:f.topo,pkg:{available:true,version:'0.1.17'}});
  assert.equal(out.steps.find(s=>s.id==='driver').actions.some(a=>a.id==='installDriver'),false);
});

test('direct install refuses downgrade before requesting administrator rights', async () => {
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'mcdma-install-test-'));
  const archive=path.join(dir,'test.tar.gz');fs.writeFileSync(archive,'test');
  const old=exec.Host.prototype.sh; let prompts=0;
  exec.Host.prototype.sh=async()=>({code:0,out:'0.1.18\n'});
  exec.setPrivilegeRunner(async()=>{prompts++;throw new Error('unexpected administrator request');});
  try {
    const result=await actions.installDriver({pkg:{available:true,version:'0.1.17',archive,manifest:{archive_sha256:crypto.createHash('sha256').update('test').digest('hex')}}});
    assert.equal(result.ok,false);assert.match(result.message,/downgrade/);assert.equal(prompts,0);
  } finally {exec.Host.prototype.sh=old;exec.setPrivilegeRunner(null);fs.rmSync(dir,{recursive:true});}
});

test('enable loads the installed newer driver instead of reinstalling an older bundle', async () => {
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'mcdma-enable-test-'));
  const store=new Store(dir), engine=new Engine({store});const f=fixture();f.info.loaded={loaded:false};
  Object.assign(engine.state,{studio:f.info,macs:f.macs,sparks:[],checks:{steps:[{id:'system',status:'ok'},{id:'hardware',status:'ok'}]},pkg:{available:true,version:'0.1.17'}});
  engine.refresh=async()=>{};
  const load=actions.loadDriver,install=actions.installDriver;let loads=0,installs=0;
  actions.loadDriver=async()=>{loads++;f.info.loaded={loaded:true,version:'0.1.18'};return {ok:true,loadKind:'loaded'};};
  actions.installDriver=async()=>{installs++;throw new Error('unexpected downgrade');};
  try {await engine.enableRdma();assert.equal(loads,1);assert.equal(installs,0);}
  finally {actions.loadDriver=load;actions.installDriver=install;fs.rmSync(dir,{recursive:true});}
});

test('driver UUID, provider and test mode invalidate historical passes', () => {
  const f=fixture();const link=f.topo.links[0];
  const saved={spark:link.spark.spark,iface:link.spark.iface,passed:true,identity:link.identity};
  const build=(value)=>topology.build({macs:f.macs,sparks:f.sparks,settings:f.settings,lastTests:{[link.id]:value}}).links[0].status.lastTest;
  assert.ok(build(saved));assert.equal(build({...saved,identity:undefined}),null);
  f.info.loaded.uuid='00000000-0000-0000-0000-000000000099';assert.equal(build(saved),null);
  f.info.loaded.uuid=demo.studio().loaded.uuid;f.info.provider.sha256='f'.repeat(64);assert.equal(build(saved),null);
});

const cq='MCDMA_CQ_OBSERVER mapped=1 bytes=16384 readonly=1 consume=user';
const post='MCDMA_USER_POST enabled=1 uar_bytes=16384\nMCDMA_USER_POST qp=7 queue_mapped=1 bytes=16384';
const bf='MCDMA_USER_BF mode=64 uar_wc=1\nMCDMA_USER_BF qp=7 bank_bytes=256 bytes=64 store=neon';
test('requested posting modes require unique provider evidence and reject fallback', () => {
  validateMode('MCDMA_CQ_OBSERVER mapped=0 reason=disabled','kernel');
  validateMode(cq+'\n'+post,'direct');validateMode(cq+'\n'+post+'\n'+bf,'bf64');
  for(const text of ['',cq+'\n'+post,cq+'\n'+post+'\n'+bf+'\n'+bf,cq+'\n'+post+'\n'+bf+'\nMCDMA_CQ_CONSUME retired cq=7'])
    assert.throws(()=>validateMode(text,'bf64'));
  assert.deepEqual(ARMS.kernel,{MCDMA_CQ_MAP:'0',MCDMA_USER_POST:'0',MCDMA_USER_BF:'0'});
});

test('raw trace validation requires every sample and matching payload', () => {
  const text='operation,bytes,sample,completion_ns\n'+Array.from({length:2000},(_,i)=>`${i<1000?'write':'read'},4096,${i%1000},${6000+i}`).join('\n')+'\n';
  assert.equal(validateTrace(text,4096).samplesPerOperation,1000);
  assert.throws(()=>validateTrace(text,1024));assert.throws(()=>validateTrace(text.split('\n').slice(0,-2).join('\n'),4096));
  assert.throws(()=>validateTestSettings({payload:4096,mtu:1024,arm:'bf64',iterations:10},true));
});

test('CLI status emits parseable JSON with isolated demo settings', () => {
  const dir=fs.mkdtempSync(path.join(os.tmpdir(),'mcdma-cli-test-'));
  try {
    const result=spawnSync(process.execPath,[path.join(__dirname,'../bin/mcdma.js'),'status','--demo','--json','--settings-dir',dir],{encoding:'utf8'});
    assert.equal(result.status,0,result.stderr);assert.equal(JSON.parse(result.stdout).version,'1.2.0');
  } finally {fs.rmSync(dir,{recursive:true});}
});

test('bandwidth maps the selected link and explicit posting mode into the runner', () => {
  const {bandwidthCommand}=require('../lib/bandwidth');const f=fixture(),link=f.topo.links[0];
  link.mac.kind='ssh';link.mac.host='mac-test';link.sparkHost='peer-test';
  const [program,args]=bandwidthCommand({root:path.resolve(__dirname,'../..'),link,settings:f.settings,output:'/tmp/mcdma-evidence-example',options:{sizes:'4194304'}});
  assert.equal(program,'python3');assert.equal(args[args.indexOf('--mac-host')+1],'mac-test');
  assert.equal(args[args.indexOf('--mac-user-bf')+1],'64');assert.equal(args[args.indexOf('--sizes')+1],'4194304');
});


test('status rejects a package linked for a different macOS build', () => {
  const f=fixture();
  const check=()=>checks.build({studio:f.info,macs:f.macs,sparks:f.sparks,topology:f.topo,pkg:{available:true,version:'0.1.18',manifest:{requires:{macos_build:'26A428'}}}}).steps.find(s=>s.id==='system');
  assert.notEqual(check().status,'fail');
  f.info.os.build='26A999';assert.equal(check().status,'fail');
});

test('status accepts every audited build a package lists, and only those', () => {
  const f=fixture();
  const check=()=>checks.build({studio:f.info,macs:f.macs,sparks:f.sparks,topology:f.topo,pkg:{available:true,version:'0.1.18',requiredBuilds:['26A428','26B5101f'],manifest:{requires:{macos_build:'26A428',macos_builds:['26A428','26B5101f']}}}}).steps.find(s=>s.id==='system');
  for (const build of ['26A428','26B5101f']) { f.info.os.build=build; assert.notEqual(check().status,'fail'); }
  for (const build of ['26B5101','26B5101g','26A999']) { f.info.os.build=build; assert.equal(check().status,'fail'); }
});


test('privileged scripts live in a private unpredictable directory and are removed on failure', async () => {
  let directory;
  exec.setPrivilegeRunner(async(file)=>{
    directory=path.dirname(file);
    assert.equal(fs.statSync(directory).mode & 0o777,0o700);
    assert.equal(fs.statSync(file).mode & 0o777,0o700);
    assert.equal(fs.lstatSync(file).isSymbolicLink(),false);
    throw new Error('owner cancelled');
  });
  try {await assert.rejects(()=>exec.adminRun('true'),/owner cancelled/);assert.equal(fs.existsSync(directory),false);}
  finally {exec.setPrivilegeRunner(null);}
});

test('SSH aliases cannot inject options and multiplex sockets use a private directory', () => {
  for(const alias of ['-oProxyCommand=bad','host\noption','host name','']) assert.throws(()=>new exec.Host('ssh',alias));
  const host=new exec.Host('ssh','peer-test');const [,args]=host.spawnArgs('true');
  const control=args.find(a=>a.startsWith('ControlPath=')).slice('ControlPath='.length);
  assert.ok(Buffer.byteLength(control.replace('%C','a'.repeat(40)))+17<104);
  const dir=path.dirname(control);
  assert.equal(fs.statSync(dir).mode & 0o777,0o700);
  exec.dispose([]);assert.equal(fs.existsSync(dir),false);
});


test('PCI discovery preserves supported identities and rejects other generations', () => {
  const { parsePci } = require('../lib/macinfo');
  const data = { SPPCIDataType: ['0x1015', '0x1019', '0x1017'].map((device, i) => ({
    'sppci_vendor-id': '0x15b3', 'sppci_device-id': device, sppci_slot_name: `test@${i},0,0`
  })) };
  assert.deepEqual(parsePci(JSON.stringify(data)).cards.map(c => [c.deviceId, c.name, c.supported]), [
    ['0x1015', 'ConnectX-4 Lx', true], ['0x1019', 'ConnectX-5 Ex', true], ['0x1017', 'ConnectX-5', false]
  ]);
});
