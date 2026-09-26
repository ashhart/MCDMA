"""Offline checks for the bandwidth sweep runner and summariser; no SSH or hardware."""
import importlib.util
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, relative):
    spec = importlib.util.spec_from_file_location(name, ROOT / relative)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


run_bw = load('run_bw', 'benchmarks/run_bw.py')
bw_summary = load('bw_summary', 'benchmarks/bw_summary.py')

# Synthetic identifiers only: no lab host, path or address appears here.
BASE_ARGS = ['--mac-host', 'mac-test', '--peer-host', 'linux-test',
             '--mac-bw', '/opt/test/mcdma-bw', '--peer-bw', '/opt/test/mcdma-bw-linux',
             '--mac-provider', '/opt/test/libmcdma-rdmav34.so', '--mac-checker', '/opt/test/cx5-native-check',
             '--mac-interface', 'mcrdma9', '--peer-interface', 'enp9s0', '--mac-device', 'rdma_mcrdma9',
             '--peer-device', 'rocep9s0', '--peer-gid-index', '3']

CSV_HEADER = ('side,trial,warmup,op,bytes,depth,depth_effective,qps,cq_per_qp,cqe,mtu,total_bytes,wrs,seconds,gbit,'
              'completions,errors,cpu_pct,responder_seconds,responder_cpu_pct,verified_bytes,mismatches,finish,rd_atomic,'
              'post_retries,guard_ok')


def csv_row(side, trial, warmup, gbit, initiator='mac', op='write', size=65536, depth=16, qps=1, cq=0,
            cpu=50.0, rcpu=10.0, verified=4194304, mismatches=0, errors=0, finish='flag', total=67108864):
    seconds = total * 8 / gbit / 1e9 if gbit else 0
    return (f'{initiator},0,1,{side},{trial},{warmup},{op},{size},{depth},{depth},{qps},{cq},31,1024,{total},'
            f'{total // size},{seconds:.6f},{gbit:.4f},{total // size},{errors},{cpu},{seconds:.6f},{rcpu},'
            f'{verified},{mismatches},{finish},1,0,1')


class SweepTests(unittest.TestCase):
    def test_sweep_covers_both_initiators_and_alternates_order(self):
        plan = run_bw.sweep(['write', 'read'], [4096, 65536], [1, 16], [1], [0], ['mac', 'peer'], 3)
        per_round = 2 * 2 * 2 * 1 * 1 * 2
        self.assertEqual(len(plan), 3 * per_round)
        rounds = [[item for item in plan if item['round'] == r] for r in range(3)]
        strip = lambda items: [{k: v for k, v in item.items() if k != 'round'} for item in items]
        self.assertEqual(strip(rounds[1]), list(reversed(strip(rounds[0]))))
        self.assertEqual(strip(rounds[2]), strip(rounds[0]))
        self.assertEqual({item['initiator'] for item in rounds[0]}, {'mac', 'peer'})
        self.assertEqual(rounds[0][0]['initiator'], 'mac')
        self.assertEqual(rounds[0][1]['initiator'], 'peer')
        self.assertEqual(len({run_bw.config_name(item) for item in rounds[0]}), per_round)

    def test_commands_follow_initiator_role(self):
        args = run_bw.main.__globals__['argparse'].Namespace(
            mac_provider='/opt/test/libmcdma-rdmav34.so', mac_cq_map='2', mac_user_post='1', mac_user_bf='64',
            mac_bw='/opt/test/mcdma-bw', peer_bw='/opt/test/mcdma-bw-linux', mac_device='rdma_mcrdma9',
            peer_device='rocep9s0', mac_gid_index=0, peer_gid_index=3, total=1 << 20, warmup=1, mtu=4096,
            finish='auto', timeout=30, verify_bytes=8192)
        config = dict(initiator='peer', op='send', bytes=4096, depth=8, qps=2, cq_per_qp=1)
        mac, peer = run_bw.build_commands(args, config)
        self.assertEqual(mac[:6], ['env', 'IBV_DRIVERS=/opt/test/libmcdma', 'MCDMA_CQ_MAP=2', 'MCDMA_USER_POST=1',
                                   'MCDMA_USER_BF=64', '/opt/test/mcdma-bw'])
        self.assertIn('responder', mac)
        self.assertEqual(peer[0], '/opt/test/mcdma-bw-linux')
        self.assertIn('initiator', peer)
        for command in (mac, peer):
            self.assertIn('--cq-per-qp', command)
            self.assertEqual(command[command.index('--repeats') + 1], '1')
            self.assertEqual(command[command.index('--mtu') + 1], '4096')
        self.assertEqual(mac[mac.index('--gid-index') + 1], '0')
        self.assertEqual(peer[peer.index('--gid-index') + 1], '3')

    def test_dry_run_prints_commands_and_never_uses_ssh(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'sweep'
            buffer = io.StringIO()
            with patch.object(run_bw.subprocess, 'run') as run, patch.object(run_bw.subprocess, 'Popen') as popen, \
                    patch('sys.stdout', buffer):
                code = run_bw.main(BASE_ARGS + ['--ops', 'write,read', '--sizes', '4096', '--depths', '1,4',
                                                '--qps', '1', '--repeats', '2', '--output', str(output), '--dry-run'])
            self.assertEqual(code, 0)
            run.assert_not_called()
            popen.assert_not_called()
            self.assertFalse(output.exists())
        lines = [line for line in buffer.getvalue().splitlines() if line.startswith('ssh ')]
        self.assertEqual(len(lines), 2 * 2 * 2 * 2 * 2)
        self.assertTrue(all('BatchMode=yes' in line for line in lines))
        self.assertIn('mac-test', lines[0])
        self.assertIn('linux-test', lines[1])
        self.assertIn('/opt/test/mcdma-bw ', lines[0].replace("'", ' '))
        self.assertIn('--role initiator', lines[0])
        self.assertIn('--role responder', lines[1])

    def test_refuses_existing_output_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(run_bw.subprocess, 'run') as run, patch('sys.stderr', io.StringIO()):
                with self.assertRaises(SystemExit):
                    run_bw.main(BASE_ARGS + ['--output', directory])
            run.assert_not_called()

    def test_rejects_invalid_sweep_values(self):
        for extra in (['--sizes', '100'], ['--depths', '0'], ['--qps', '9'], ['--ops', 'atomic'],
                      ['--mac-user-post', '1'], ['--sizes', '4096,4096'], ['--total', '10', '--sizes', '4096']):
            with self.subTest(extra=extra), patch('sys.stderr', io.StringIO()), self.assertRaises(SystemExit):
                run_bw.main(BASE_ARGS + extra + ['--output', '/nonexistent/test-output', '--dry-run'])

    def test_descriptor_validation(self):
        good = ('ENDPOINT v=1 nonce=7 role=initiator op=write bytes=65536 depth=16 qps=2 cqpq=0 cqe=31 total=67108864 '
                'mtu=1024 imm_recv=0 rd=1 psn=6636321 rkey=4660 addr=4294967296 length=2113536 gid=fe80::ff:fe00:1 qpn=17,18')
        fields, mac = run_bw.descriptor(good)
        self.assertEqual(mac, '02:00:00:00:00:01')
        self.assertEqual(fields['qpn'], '17,18')
        for bad in (good.replace('qpn=17,18', 'qpn=17'), good.replace('gid=fe80::ff:fe00:1', 'gid=2001:db8::1'),
                    good.replace('rkey=4660', 'rkey=0'), good.replace('op=write', 'op=atomic'),
                    good.replace(' length=2113536', ''), good.replace('ENDPOINT', 'READY'),
                    good.replace('addr=4294967296', 'addr=18446744073709551615')):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                run_bw.descriptor(bad)

    def test_mac_mode_markers_count_queues(self):
        disabled = 'MCDMA_CQ_OBSERVER mapped=0 reason=disabled\n'
        run_bw.mac_mode_markers(disabled * 2, '0', '0', '0', 2, 2)
        with self.assertRaises(ValueError):
            run_bw.mac_mode_markers(disabled, '0', '0', '0', 2, 2)
        consume = 'MCDMA_CQ_OBSERVER mapped=1 bytes=16384 readonly=1 consume=user\n'
        posting = ('MCDMA_USER_POST enabled=1 uar_bytes=16384\nMCDMA_USER_POST qp=17 queue_mapped=1 bytes=16384\n'
                   'MCDMA_USER_POST qp=18 queue_mapped=1 bytes=16384\n')
        run_bw.mac_mode_markers(consume + posting, '2', '1', '0', 1, 2)
        with self.assertRaises(ValueError):
            run_bw.mac_mode_markers(consume + posting, '2', '1', '0', 1, 3)
        with self.assertRaises(ValueError):
            run_bw.mac_mode_markers(consume + posting + 'MCDMA_CQ_CONSUME retired cq=3 reason=x\n', '2', '1', '0', 1, 2)
        bf = 'MCDMA_USER_BF mode=64 uar_wc=1\nMCDMA_USER_BF qp=17 bank_bytes=256 bytes=64 store=neon\n'
        run_bw.mac_mode_markers(consume + posting.replace('MCDMA_USER_POST qp=18 queue_mapped=1 bytes=16384\n', '') + bf,
                                '2', '1', '64', 1, 1)
        with self.assertRaises(ValueError):
            run_bw.mac_mode_markers(consume + posting + bf, '2', '1', '64', 1, 2)



# Two Linux hosts: the first host runs stock rdma-core, so no provider or checker.
LINUX_ARGS = ['--mac-platform', 'linux', '--mac-host', 'linux-a', '--peer-host', 'linux-b',
              '--mac-bw', '/opt/test/mcdma-bw-a', '--peer-bw', '/opt/test/mcdma-bw-b',
              '--mac-interface', 'enp8s0', '--peer-interface', 'enp9s0', '--mac-device', 'rocep8s0',
              '--peer-device', 'rocep9s0', '--mac-gid-index', '1', '--peer-gid-index', '1']
# Pre-change output of MACOS_ARGV --dry-run, recorded before --mac-platform existed.
MACOS_ARGV = BASE_ARGS + ['--ops', 'write,read', '--sizes', '4096', '--depths', '1,4', '--mac-cq-map', '2',
                          '--mac-user-post', '1', '--mac-user-bf', '64', '--output', '/nonexistent/x', '--dry-run']
MACOS_DRY_RUN_SHA256 = 'cf7333d9e990e8dcabb385672f6d8e41082a6d04f4c076058535448a81afaae0'


def dry_run(argv):
    buffer = io.StringIO()
    with patch.object(run_bw.subprocess, 'run') as run, patch.object(run_bw.subprocess, 'Popen') as popen, \
            patch('sys.stdout', buffer):
        code = run_bw.main(argv)
    run.assert_not_called()
    popen.assert_not_called()
    return code, buffer.getvalue()


class LinuxPlatformTests(unittest.TestCase):
    def test_macos_dry_run_is_unchanged(self):
        code, output = dry_run(MACOS_ARGV)
        self.assertEqual(code, 0)
        self.assertEqual(run_bw.hashlib.sha256(output.encode()).hexdigest(), MACOS_DRY_RUN_SHA256)
        code, explicit = dry_run(['--mac-platform', 'macos'] + MACOS_ARGV)
        self.assertEqual((code, explicit), (0, output))

    def test_linux_dry_run_has_no_env_prefix(self):
        code, output = dry_run(LINUX_ARGS + ['--ops', 'write', '--sizes', '4096', '--depths', '1',
                                             '--output', '/nonexistent/test-output', '--dry-run'])
        self.assertEqual(code, 0)
        lines = [line for line in output.splitlines() if line.startswith('ssh ')]
        self.assertEqual(len(lines), 2 * 2 * 3)
        for line in lines:
            self.assertNotIn('env ', line)
            self.assertNotIn('IBV_DRIVERS', line)
            self.assertNotIn('MCDMA_', line)
        self.assertIn("linux-a '/opt/test/mcdma-bw-a --role initiator --device rocep8s0 --gid-index 1 ", lines[0])
        self.assertIn("linux-b '/opt/test/mcdma-bw-b --role responder --device rocep9s0 --gid-index 1 ", lines[1])

    def test_linux_does_not_require_provider_or_checker(self):
        code, output = dry_run(LINUX_ARGS[:-4] + ['--mac-gid-index', '3', '--peer-gid-index', '5',
                                                  '--output', '/nonexistent/test-output', '--dry-run'])
        self.assertEqual(code, 0)
        self.assertIn('--device rocep8s0 --gid-index 3 ', output)
        self.assertIn('--device rocep9s0 --gid-index 5 ', output)
        for extra in (['--mac-provider', '/opt/test/libmcdma-rdmav34.so'], ['--mac-checker', '/opt/test/check']):
            with self.subTest(extra=extra), patch('sys.stderr', io.StringIO()), self.assertRaises(SystemExit):
                run_bw.main(LINUX_ARGS + extra + ['--output', '/nonexistent/test-output', '--dry-run'])
        macos = [a for a in BASE_ARGS if a not in ('--mac-checker', '/opt/test/cx5-native-check')]
        with patch('sys.stderr', io.StringIO()), self.assertRaises(SystemExit):
            run_bw.main(macos + ['--output', '/nonexistent/test-output', '--dry-run'])

    def test_linux_requires_both_gid_indexes(self):
        bare = LINUX_ARGS[:-4]
        for extra in ([], ['--mac-gid-index', '3'], ['--peer-gid-index', '5']):
            stderr = io.StringIO()
            with self.subTest(extra=extra), patch('sys.stderr', stderr), self.assertRaises(SystemExit):
                run_bw.main(bare + extra + ['--output', '/nonexistent/test-output', '--dry-run'])
            self.assertIn('::ffff:<port IPv4>', stderr.getvalue())

    def test_linux_rejects_mac_provider_modes(self):
        for extra in (['--mac-cq-map', '2'], ['--mac-cq-map', '1'], ['--mac-cq-map', '2', '--mac-user-post', '1'],
                      ['--mac-cq-map', '2', '--mac-user-post', '1', '--mac-user-bf', '64']):
            with self.subTest(extra=extra), patch('sys.stderr', io.StringIO()), self.assertRaises(SystemExit):
                run_bw.main(LINUX_ARGS + extra + ['--output', '/nonexistent/test-output', '--dry-run'])
        code, _ = dry_run(LINUX_ARGS + ['--mac-cq-map', '0', '--mac-user-post', '0', '--mac-user-bf', '0',
                                        '--output', '/nonexistent/test-output', '--dry-run'])
        self.assertEqual(code, 0)

    def test_linux_mode_markers(self):
        run_bw.linux_mode_markers('')
        run_bw.linux_mode_markers('libibverbs: warning\n')
        with self.assertRaises(ValueError):
            run_bw.linux_mode_markers('MCDMA_CQ_OBSERVER mapped=0 reason=disabled\n')

    def test_linux_neighbour_check_runs_ip_on_the_named_host(self):
        seen = []

        def run(host, command):
            seen.append((host, command))
            return 'fe80::ff:fe00:a lladdr 02:00:00:00:00:0A PERMANENT'

        self.assertTrue(run_bw.linux_has_neighbour(run, 'linux-a', 'fe80::ff:fe00:a', 'enp8s0', '02:00:00:00:00:0a'))
        self.assertFalse(run_bw.linux_has_neighbour(run, 'linux-a', 'fe80::ff:fe00:a', 'enp8s0', '02:00:00:00:00:0b'))
        self.assertEqual(seen[0], ('linux-a', ['ip', '-6', 'neigh', 'show', 'to', 'fe80::ff:fe00:a', 'dev', 'enp8s0']))

    def test_linux_preflight_uses_linux_checks_on_both_hosts(self):
        calls = []

        def fake_run(command, **kwargs):
            calls.append(command)
            host, remote = command[-2], command[-1]
            if remote.startswith('cat ') and '/types/' in remote:
                out = 'RoCE v2\n'
            elif remote.startswith('cat ') and '/ndevs/' in remote:
                out = ('enp8s0' if host == 'linux-a' else 'enp9s0') + '\n'
            elif remote.startswith('sha256sum '):
                out = 'a' * 64 + '  ' + remote.split()[-1] + '\n'
            else:
                out = ''
            return run_bw.subprocess.CompletedProcess(command, 0, out, '')

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'sweep'
            with patch.object(run_bw.subprocess, 'run', side_effect=fake_run), \
                    patch.object(run_bw.cross.subprocess, 'Popen', side_effect=OSError('no ssh in tests')), \
                    patch('sys.stdout', io.StringIO()):
                code = run_bw.main(LINUX_ARGS + ['--ops', 'write', '--sizes', '4096', '--depths', '1',
                                                 '--initiators', 'mac', '--repeats', '1', '--output', str(output)])
            manifest = run_bw.json.loads((output / 'manifest.json').read_text())
        self.assertEqual(code, 1)
        self.assertEqual(manifest['arguments']['mac_platform'], 'linux')
        self.assertEqual(set(manifest['binaries']), {'mac', 'peer'})
        remotes = [command[-1] for command in calls]
        self.assertIn('cat /sys/class/infiniband/rocep8s0/ports/1/gid_attrs/types/1', remotes)
        self.assertIn('cat /sys/class/infiniband/rocep9s0/ports/1/gid_attrs/types/1', remotes)
        self.assertIn('sha256sum /opt/test/mcdma-bw-a', remotes)
        self.assertFalse(any(r.startswith(('shasum', 'ndp', '/opt/test/cx5')) for r in remotes))
        self.assertTrue(all(error.startswith('no ssh') for error in manifest['runs'][0]['errors']))

    def test_linux_preflight_rejects_roce_v1_first_host(self):
        def fake_run(command, **kwargs):
            out = 'IB/RoCE v1\n' if command[-2] == 'linux-a' and '/types/' in command[-1] else 'RoCE v2\n'
            return run_bw.subprocess.CompletedProcess(command, 0, out, '')

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'sweep'
            with patch.object(run_bw.subprocess, 'run', side_effect=fake_run), patch('sys.stderr', io.StringIO()):
                code = run_bw.main(LINUX_ARGS + ['--output', str(output)])
            manifest = run_bw.json.loads((output / 'manifest.json').read_text())
        self.assertEqual(code, 1)
        self.assertIn('not RoCE v2', manifest['errors'][0])

FAKE_ENDPOINT = r'''
import sys
role, gid = sys.argv[1], sys.argv[2]
def out(text):
    sys.stdout.write(text + '\n'); sys.stdout.flush()
out('BW_CONFIG role=' + role)
out('BW_MSG ENDPOINT v=1 nonce=7 role=%s op=write bytes=65536 depth=16 qps=1 cqpq=0 cqe=31 total=67108864 mtu=1024 '
    'imm_recv=0 rd=1 psn=1 rkey=4660 addr=4294967296 length=2113536 gid=%s qpn=17' % (role, gid))
peer = sys.stdin.readline().strip()
assert peer.startswith('ENDPOINT ') and ('role=responder' if role == 'initiator' else 'role=initiator') in peer, peer
out('BW_MSG READY rd=1')
assert sys.stdin.readline().strip() == 'READY rd=1'
out('BW_CSV_HEADER side,trial')
out('BW_CSV %s,0' % role)
out('BW_RESULT side=%s trial=0' % role)
out('BW_DONE trials=1 failed=0 cleanup=0')
'''


class RelayTests(unittest.TestCase):
    def fake(self, host, role, gid, log):
        process = run_bw.subprocess.Popen([os.sys.executable, '-u', '-c', FAKE_ENDPOINT, role, gid],
                                          stdin=run_bw.subprocess.PIPE, stdout=run_bw.subprocess.PIPE,
                                          stderr=run_bw.subprocess.PIPE)
        with patch.object(run_bw.cross.subprocess, 'Popen', return_value=process):
            endpoint = run_bw.cross.Endpoint(host, ['unused'], log)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        return endpoint

    def test_relay_holds_endpoints_then_forwards_and_collects_rows(self):
        log, seen = [], {}
        mac = self.fake('mac-test', 'initiator', 'fe80::ff:fe00:1', log)
        peer = self.fake('linux-test', 'responder', 'fe80::ff:fe00:2', log)

        def on_endpoints(held):
            for endpoint, payload in held.items():
                seen[endpoint.host] = run_bw.descriptor(payload)[1]

        relay = run_bw.Relay([mac, peer], log, run_bw.time.monotonic() + 20, on_endpoints)
        with patch('builtins.print'):
            relay.run()
            self.assertEqual(mac.stop(), 0)
            self.assertEqual(peer.stop(), 0)
        self.assertEqual(seen, {'mac-test': '02:00:00:00:00:01', 'linux-test': '02:00:00:00:00:02'})
        self.assertEqual(relay.header, 'side,trial')
        self.assertEqual(relay.rows[mac], ['initiator,0'])
        self.assertEqual(relay.rows[peer], ['responder,0'])
        self.assertEqual(relay.errors, [])
        self.assertEqual(relay.done, {mac, peer})
        self.assertTrue(any(entry.get('response', '').startswith('BW_RESULT side=initiator') for entry in log))

    def test_relay_stops_when_neighbour_check_fails(self):
        log = []
        mac = self.fake('mac-test', 'initiator', 'fe80::ff:fe00:1', log)
        peer = self.fake('linux-test', 'responder', 'fe80::ff:fe00:2', log)

        def refuse(_held):
            raise RuntimeError('Mac needs the peer static IPv6 neighbour before QP connection')

        relay = run_bw.Relay([mac, peer], log, run_bw.time.monotonic() + 20, refuse)
        with patch('builtins.print'), self.assertRaisesRegex(RuntimeError, 'neighbour'):
            relay.run()
        with patch('builtins.print'):
            mac.stop()
            peer.stop()
        self.assertEqual(relay.rows[mac], [])


V4_GIDS = {'linux-a': '::ffff:192.0.2.20', 'linux-b': '::ffff:192.0.2.11',
           'mac-test': '::ffff:192.0.2.20', 'linux-test': '::ffff:192.0.2.11'}
ARP_ROWS = {'192.0.2.11': '192.0.2.11 lladdr 02:00:00:00:00:0b STALE',
            '192.0.2.20': '192.0.2.20 lladdr 02:00:00:00:00:14 REACHABLE'}


class IPv4MappedGidTests(unittest.TestCase):
    GOOD = ('ENDPOINT v=1 nonce=7 role=initiator op=write bytes=65536 depth=16 qps=1 cqpq=0 cqe=31 total=67108864 '
            'mtu=1024 imm_recv=0 rd=1 psn=1 rkey=4660 addr=4294967296 length=2113536 gid=::ffff:192.0.2.11 qpn=17')

    def test_descriptor_accepts_ipv4_mapped_only_when_asked(self):
        fields, mac = run_bw.descriptor(self.GOOD, ipv4_mapped=True)
        self.assertEqual((fields['gid'], mac), ('::ffff:192.0.2.11', None))
        with self.assertRaisesRegex(ValueError, 'MAC-derived link-local'):
            run_bw.descriptor(self.GOOD)
        link_local = self.GOOD.replace('::ffff:192.0.2.11', 'fe80::ff:fe00:1')
        self.assertEqual(run_bw.descriptor(link_local, ipv4_mapped=True)[1], '02:00:00:00:00:01')
        for bad in ('::ffff:224.0.0.1', '::ffff:0.0.0.0', '::ffff:127.0.0.1', '::ffff:255.255.255.255', '2001:db8::1'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                run_bw.descriptor(self.GOOD.replace('::ffff:192.0.2.11', bad), ipv4_mapped=True)

    def test_ipv4_neighbour_needs_a_resolved_row(self):
        seen = []

        def run_with(output):
            def run(host, command):
                seen.append((host, command))
                return output
            return run

        for state in ('REACHABLE', 'STALE', 'DELAY', 'PROBE', 'PERMANENT'):
            with self.subTest(state=state):
                self.assertTrue(run_bw.linux_has_ipv4_neighbour(
                    run_with(f'192.0.2.11 lladdr 02:00:00:00:00:0b {state}'), 'linux-a', '192.0.2.11', 'enp8s0'))
        self.assertEqual(seen[0], ('linux-a', ['ip', '-4', 'neigh', 'show', 'to', '192.0.2.11', 'dev', 'enp8s0']))
        for output in ('', '192.0.2.11 FAILED', '192.0.2.11 INCOMPLETE', '192.0.2.12 lladdr 02:00:00:00:00:0b STALE',
                       '192.0.2.11 lladdr (incomplete) STALE', '192.0.2.11 lladdr 02:00:00:00:00:0b NOARP'):
            with self.subTest(output=output):
                self.assertFalse(run_bw.linux_has_ipv4_neighbour(run_with(output), 'linux-a', '192.0.2.11', 'enp8s0'))

    def sweep_with_fake_hosts(self, argv, gids, arp=ARP_ROWS):
        """Runs main() end to end: fake SSH commands, FAKE_ENDPOINT processes."""
        popen = run_bw.subprocess.Popen
        calls = []

        def fake_run(command, **kwargs):
            calls.append(command)
            remote = command[-1]
            if '/types/' in remote:
                out = 'RoCE v2'
            elif '/ndevs/' in remote:
                out = {'linux-a': 'enp8s0', 'linux-b': 'enp9s0', 'linux-test': 'enp9s0'}.get(command[-2], '')
            elif remote.startswith(('sha256sum ', 'shasum ')):
                out = 'a' * 64 + '  x'
            elif remote.startswith('ip -4 neigh'):
                out = arp.get(remote.split()[5], '')
            else:
                out = ''
            return run_bw.subprocess.CompletedProcess(command, 0, out + '\n', '')

        def fake_popen(command, **kwargs):
            host, remote = command[-2], command[-1]
            role = remote.split('--role ')[1].split()[0]
            return popen([os.sys.executable, '-u', '-c', FAKE_ENDPOINT, role, gids[host]], **kwargs)

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'sweep'
            with patch.object(run_bw.subprocess, 'run', side_effect=fake_run), \
                    patch.object(run_bw.cross.subprocess, 'Popen', side_effect=fake_popen), \
                    patch('sys.stdout', io.StringIO()):
                code = run_bw.main(argv + ['--ops', 'write', '--sizes', '65536', '--depths', '16', '--initiators', 'mac',
                                           '--repeats', '1', '--output', str(output)])
            manifest = run_bw.json.loads((output / 'manifest.json').read_text())
        return code, manifest, [command[-1] for command in calls]

    def test_linux_pair_runs_over_ipv4_mapped_gids(self):
        code, manifest, remotes = self.sweep_with_fake_hosts(LINUX_ARGS, V4_GIDS)
        self.assertEqual(manifest['runs'][0]['errors'], [])
        self.assertEqual(code, 0)
        self.assertIn('ip -4 neigh show to 192.0.2.11 dev enp8s0', remotes)
        self.assertIn('ip -4 neigh show to 192.0.2.20 dev enp9s0', remotes)
        self.assertFalse(any(r.startswith(('ip -6', 'ndp')) for r in remotes))

    def test_linux_pair_refuses_missing_arp_entry(self):
        arp = dict(ARP_ROWS, **{'192.0.2.20': '192.0.2.20 FAILED'})
        code, manifest, _ = self.sweep_with_fake_hosts(LINUX_ARGS, V4_GIDS, arp)
        self.assertEqual(code, 1)
        self.assertTrue(any('no ARP entry for 192.0.2.20' in e for e in manifest['runs'][0]['errors']))

    def test_linux_pair_refuses_mixed_gid_kinds(self):
        code, manifest, _ = self.sweep_with_fake_hosts(LINUX_ARGS, dict(V4_GIDS, **{'linux-a': 'fe80::ff:fe00:14'}))
        self.assertEqual(code, 1)
        self.assertTrue(any('both use IPv4-mapped' in e for e in manifest['runs'][0]['errors']))

    def test_macos_pair_still_rejects_ipv4_mapped_gids(self):
        code, manifest, remotes = self.sweep_with_fake_hosts(BASE_ARGS, V4_GIDS)
        self.assertEqual(code, 1)
        self.assertTrue(any('MAC-derived link-local' in e for e in manifest['runs'][0]['errors']))
        self.assertFalse(any(r.startswith('ip -4') for r in remotes))

class SummaryTests(unittest.TestCase):
    def write(self, directory, name, rows):
        path = Path(directory) / name
        path.write_text('initiator,round,mode_confirmed,' + CSV_HEADER + '\n' + '\n'.join(rows) + '\n')
        return path

    def test_median_min_max_and_efficiency(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write(directory, 'mac-write-b65536-d16-q1-cq1.csv', [
                csv_row('initiator', 0, 1, 99.0),          # warmup: excluded
                csv_row('responder', 0, 1, 99.0),
                csv_row('initiator', 1, 0, 10.0, cpu=40, rcpu=5, verified=4096),
                csv_row('responder', 1, 0, 10.0),
                csv_row('initiator', 1, 0, 30.0, cpu=60, rcpu=15),
                csv_row('initiator', 1, 0, 20.0, cpu=50, rcpu=10, mismatches=8),
                csv_row('initiator', 1, 0, 5.0, initiator='peer', op='read'),
            ])
            rows, sources = bw_summary.load_rows([path])
            self.assertEqual(len(sources), 1)
            self.assertEqual(len(sources[0]['sha256']), 64)
            summary = bw_summary.summarize(rows, ceiling=40.0)
        self.assertEqual([(e['initiator'], e['op']) for e in summary], [('mac', 'write'), ('peer', 'read')])
        mac = summary[0]
        self.assertEqual(mac['trials'], 3)
        self.assertAlmostEqual(mac['median_gbit'], 20.0)
        self.assertAlmostEqual(mac['min_gbit'], 10.0)
        self.assertAlmostEqual(mac['max_gbit'], 30.0)
        self.assertAlmostEqual(mac['efficiency_pct'], 50.0)
        self.assertAlmostEqual(mac['initiator_cpu_pct'], 50.0)
        self.assertAlmostEqual(mac['responder_cpu_pct'], 10.0)
        self.assertEqual(mac['verified_bytes_min'], 4096)
        self.assertEqual(mac['mismatches'], 8)
        self.assertAlmostEqual(summary[1]['efficiency_pct'], 12.5)
        self.assertIsNone(bw_summary.summarize(rows)[0]['efficiency_pct'])

    def test_markdown_only_reports_efficiency_with_ceiling(self):
        with tempfile.TemporaryDirectory() as directory:
            self.write(directory, 'a.csv', [csv_row('initiator', 1, 0, 12.5)])
            rows, _ = bw_summary.load_rows(sorted(Path(directory).glob('*.csv')))
        without = bw_summary.markdown(bw_summary.summarize(rows))
        self.assertIn('| n/a |', without)
        self.assertIn('--ceiling-gbit', without)
        self.assertNotIn('%', without.split('\n')[2])
        with_ceiling = bw_summary.markdown(bw_summary.summarize(rows, 25.0), 25.0)
        self.assertIn('| 50.0% |', with_ceiling)
        self.assertIn('| 12.500 |', with_ceiling)

    def test_missing_columns_and_bad_values_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            broken = Path(directory) / 'broken.csv'
            broken.write_text('side,gbit\ninitiator,1\n')
            with self.assertRaises(ValueError):
                bw_summary.load_rows([broken])
            bad = self.write(directory, 'bad.csv', [csv_row('initiator', 1, 0, 1.0).replace(',1.0000,', ',nan,')])
            rows, _ = bw_summary.load_rows([bad])
            with self.assertRaises(ValueError):
                bw_summary.summarize(rows)

    def test_main_writes_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            self.write(directory, 'a.csv', [csv_row('initiator', 1, 0, 8.0), csv_row('initiator', 1, 0, 16.0)])
            table = Path(directory) / 'out' / 'summary.md'
            data = Path(directory) / 'out' / 'summary.json'
            with patch('sys.stdout', io.StringIO()):
                code = bw_summary.main([directory, '--ceiling-gbit', '48', '--output', str(table), '--json', str(data)])
            self.assertEqual(code, 0)
            self.assertIn('| 25.0% |', table.read_text())
            self.assertIn('"median_gbit": 12.0', data.read_text())


if __name__ == '__main__':
    unittest.main()
