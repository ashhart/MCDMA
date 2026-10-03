"""Offline checks for the bandwidth sweep runner and summariser; no SSH or hardware."""
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import tempfile
import unittest
from unittest.mock import Mock, patch

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

    def run_interrupted(self, signum):
        endpoints = []

        class Endpoint:
            def __init__(self, host, _command, _log):
                self.host = host
                self.process = Mock()
                self.process.stdout.fileno.return_value = 1
                endpoints.append(self)

            def stop(self):
                self.stopped = True
                return 0

        def remote(command, **_kwargs):
            target = command[-1]
            stdout = ''
            if 'gid_attrs/types/' in target:
                stdout = 'RoCE v2\n'
            elif 'gid_attrs/ndevs/' in target:
                stdout = 'enp9s0\n'
            elif 'sha256sum ' in target or 'shasum -a 256 ' in target:
                stdout = 'a' * 64 + '  fixture\n'
            return Mock(returncode=0, stdout=stdout, stderr='')

        def interrupt(_relay):
            signal.raise_signal(signum)

        with tempfile.TemporaryDirectory() as directory, \
                patch.object(run_bw.cross, 'Endpoint', Endpoint), \
                patch.object(run_bw.Relay, 'run', interrupt), \
                patch.object(run_bw.subprocess, 'run', remote), \
                patch('builtins.print'):
            code = run_bw.main(BASE_ARGS + ['--ops', 'write', '--sizes', '4096', '--depths', '1',
                                            '--qps', '1', '--initiators', 'mac', '--repeats', '1',
                                            '--output', str(Path(directory) / 'sweep')])
            manifest = json.loads((Path(directory) / 'sweep' / 'manifest.json').read_text())
        self.assertEqual(code, 1)
        self.assertEqual(len(endpoints), 2)
        self.assertTrue(all(endpoint.stopped for endpoint in endpoints))
        self.assertIn(signal.Signals(signum).name, manifest['errors'][-1])

    def test_sigterm_routes_through_endpoint_cleanup(self):
        self.run_interrupted(signal.SIGTERM)

    def test_sighup_routes_through_endpoint_cleanup(self):
        self.run_interrupted(signal.SIGHUP)

    def test_surviving_endpoint_makes_run_nonzero(self):
        endpoints = []

        class Endpoint:
            def __init__(self, host, _command, _log):
                self.host = host
                self.process = Mock()
                self.process.stdout.fileno.return_value = 1
                endpoints.append(self)

            def stop(self):
                raise run_bw.cross.EndpointCleanupError(
                    f'{self.host}: endpoint pid 4321 survived stdin close, 15s wait, SIGTERM, and 5s wait')

        def remote(command, **_kwargs):
            target = command[-1]
            stdout = ''
            if 'gid_attrs/types/' in target:
                stdout = 'RoCE v2\n'
            elif 'gid_attrs/ndevs/' in target:
                stdout = 'enp9s0\n'
            elif 'sha256sum ' in target or 'shasum -a 256 ' in target:
                stdout = 'a' * 64 + '  fixture\n'
            return Mock(returncode=0, stdout=stdout, stderr='')

        with tempfile.TemporaryDirectory() as directory, \
                patch.object(run_bw.cross, 'Endpoint', Endpoint), \
                patch.object(run_bw.Relay, 'run', side_effect=RuntimeError('offline stop')), \
                patch.object(run_bw.subprocess, 'run', remote), \
                patch('builtins.print'):
            output = Path(directory) / 'sweep'
            code = run_bw.main(BASE_ARGS + ['--ops', 'write', '--sizes', '4096', '--depths', '1',
                                            '--qps', '1', '--initiators', 'mac', '--repeats', '1',
                                            '--output', str(output)])
            manifest = json.loads((output / 'manifest.json').read_text())
        self.assertEqual(code, 1)
        self.assertEqual(len(endpoints), 2)
        self.assertIn('cleanup failed', ' '.join(manifest['runs'][0]['errors']))
        self.assertIn('pid 4321 survived', ' '.join(manifest['runs'][0]['errors']))

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
