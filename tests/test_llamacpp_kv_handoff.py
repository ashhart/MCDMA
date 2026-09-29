"""Offline checks for benchmarks/llamacpp_kv_handoff.py: chunking that fits
mcdma-bw's resident payload limits, the CSV gate, and a dry run."""
import contextlib
import importlib.util
import io
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('kvh', ROOT / 'benchmarks/llamacpp_kv_handoff.py')
kvh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kvh)

HEADER = ('initiator,round,mode_confirmed,side,trial,warmup,op,bytes,depth,depth_effective,qps,cq_per_qp,cqe,mtu,'
          'total_bytes,wrs,seconds,gbit,completions,errors,cpu_pct,responder_seconds,responder_cpu_pct,'
          'verified_bytes,mismatches,finish,rd_atomic,post_retries,guard_ok,measurement')


def row(side='initiator', errors='0', mismatches='0', guard='1', seconds='0.5'):
    return (f'mac,0,1,{side},0,0,write,16777216,63,63,1,0,64,1024,1056964608,63,{seconds},16.9,63,{errors},'
            f'100.0,0.5,100.0,1056964608,{mismatches},imm,1,0,{guard},resident-payload')


class Chunks(unittest.TestCase):
    def test_two_gib_slot_needs_three_chunks_that_each_fit(self):
        size = 8192 * 256 * 1024 + 12345
        plan = kvh.chunk_plan(size)
        self.assertEqual(sum(n for _, n in plan), size)
        self.assertEqual([o for o, _ in plan], [i * 63 * (16 << 20) for i in range(len(plan))])
        for _, n in plan:
            req = kvh.request_for(n)
            depth = kvh.depth_for(n, req)
            self.assertLessEqual(depth, 64)
            self.assertLessEqual(req * depth + kvh.GUARD, 1 << 30)
            self.assertGreaterEqual(req * depth, n)

    def test_small_file_is_one_chunk_with_small_request(self):
        self.assertEqual(kvh.chunk_plan(5000), [(0, 5000)])
        self.assertEqual(kvh.request_for(5000), 8192)
        self.assertEqual(kvh.depth_for(5000, 8192), 1)

    def test_empty_file_refused(self):
        with self.assertRaises(ValueError):
            kvh.chunk_plan(0)


class PayloadGate(unittest.TestCase):
    def test_clean_initiator_row(self):
        self.assertEqual(kvh.payload_seconds('\n'.join([HEADER, row(), row(side='responder')])), 0.5)

    def test_unclean_rows_refused(self):
        for bad in (row(errors='1'), row(mismatches='3'), row(guard='0')):
            with self.assertRaises(ValueError):
                kvh.payload_seconds('\n'.join([HEADER, bad]))

    def test_missing_initiator_refused(self):
        with self.assertRaises(ValueError):
            kvh.payload_seconds('\n'.join([HEADER, row(side='responder')]))


class Misc(unittest.TestCase):
    def test_token_hash_is_uint32_le(self):
        import hashlib
        self.assertEqual(kvh.token_sha256([1, 2]),
                         hashlib.sha256(b'\x01\x00\x00\x00\x02\x00\x00\x00').hexdigest())

    def test_dry_run_plans_chunks_and_both_arms(self):
        argv = ['--dry-run', '--output', '/nonexistent/x', '--dst-ip', '192.0.2.2', '--prompts', '8192', '--rounds', '1']
        for side in ('src', 'dst'):
            argv += [f'--{side}-host', f'{side}-host', f'--{side}-slot-dir', '/dev/shm/kv', f'--{side}-bw', '/opt/bw',
                     f'--{side}-interface', 'eth9', f'--{side}-device', 'rdma9', f'--{side}-gid-index', '3']
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(kvh.main(argv), 0)
        text = out.getvalue()
        self.assertEqual(text.count('run_bw.py'), 4)   # 3 GiB dry-run slot = 4 chunks
        self.assertIn('--payload /dev/shm/kv/kvh-8192.bin.part', text)
        self.assertIn('create_connection(("192.0.2.2",18777))', text)


class LinkArm(unittest.TestCase):
    def _argv(self, *extra):
        argv = ['--dry-run', '--output', '/nonexistent/x', '--dst-ip', '192.0.2.2', '--prompts', '2048',
                '--rounds', '1', *extra]
        for side in ('src', 'dst'):
            argv += [f'--{side}-host', f'{side}-host', f'--{side}-slot-dir', '/dev/shm/kv', f'--{side}-bw', '/opt/bw',
                     f'--{side}-interface', 'eth9', f'--{side}-device', 'rdma9', f'--{side}-gid-index', '3']
        return argv

    def test_link_arm_pulls_with_rpc_file(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            kvh.main(self._argv('--arms', 'link,tcp', '--link-name', 'kv0', '--dst-repo', '/opt/mcdma',
                                '--dst-rpc-lib', '/opt/lib.so'))
        text = out.getvalue()
        self.assertIn('/opt/mcdma/benchmarks/rpc_file.py pull kv0 kvh-2048.bin /dev/shm/kv/kvh-2048.bin', text)
        self.assertNotIn('run_bw.py', text)

    def test_bad_ip_and_dash_host_refused(self):
        for extra in (['--dst-ip', '1.2.3.4",0);import os;os.system("x'], ):
            argv = self._argv()
            i = argv.index('--dst-ip')
            argv[i + 1] = extra[1]
            with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                kvh.main(argv)
        argv = self._argv()
        argv[argv.index('--src-host') + 1] = '-oProxyCommand=x'
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            kvh.main(argv)

    def test_link_arm_requires_its_settings(self):
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            kvh.main(self._argv('--arms', 'link'))


if __name__ == '__main__':
    unittest.main()
