"""Offline checks for benchmarks/rpc_roundtrip.py's client: a thread plays the
connect daemon plus the remote service on one mapped file, so the client's
Protocol 1 handling (words, sequences, reply area, generation) is exercised
without hardware."""
import ctypes
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('rt', ROOT / 'benchmarks/rpc_roundtrip.py')
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)

HALF = 1 << 20


class FakeHelper:
    """Python stand-in for libmcdma-rpc's ordered word store and wait."""

    @staticmethod
    def mcdma_rpc_store_word(addr, value):
        ctypes.c_uint64.from_address(addr).value = value

    @staticmethod
    def mcdma_rpc_wait_word(addr, last, mode, spin, timeout_ns):
        end = time.perf_counter() + timeout_ns / 1e9
        while time.perf_counter() < end:
            v = ctypes.c_uint64.from_address(addr).value
            if v and (((v >> 32) == last) if mode else ((v >> 32) != last)):
                return v
        return 0


class Link(threading.Thread):
    """Answers each request the way daemon + echo service would; `corrupt`
    flips a reply byte, `bump_generation` simulates a reconnect."""

    def __init__(self, path, corrupt=False, bump_generation=False, silent=False, stale=False):
        super().__init__(daemon=True)
        self.path, self.corrupt, self.bump, self.silent = path, corrupt, bump_generation, silent
        self.stale = stale   # after the first reply, only the done word moves
        self.stop = threading.Event()

    def run(self):
        import mmap
        fd = os.open(self.path, os.O_RDWR)
        m = mmap.mmap(fd, os.fstat(fd).st_size)
        os.close(fd)
        base = ctypes.addressof(ctypes.c_char.from_buffer(m))
        last = 0
        while not self.stop.is_set():
            w = ctypes.c_uint64.from_address(base).value
            if not w or (w >> 32) == last:
                continue
            last = w >> 32
            if self.bump:
                ctypes.c_uint64.from_address(base + rt.GENERATION).value += 1
                continue
            if self.silent:
                continue
            want, seed = rt.HEAD.unpack_from(m, rt.CTRL)
            data = bytearray(rt.stamped(last, rt.pattern(seed, want)))
            if self.corrupt and data:
                data[-1] ^= 1
            if not (self.stale and last > 1):
                m[HALF + rt.CTRL:HALF + rt.CTRL + want] = bytes(data)
            ctypes.c_uint64.from_address(base + HALF + rt.DONE_WORD).value = last << 32 | want


class RoundTrip(unittest.TestCase):
    def setUp(self):
        fd, self.path = tempfile.mkstemp()
        os.ftruncate(fd, 2 * HALF)
        os.pwrite(fd, struct.pack('<QQ', HALF, HALF), rt.SIZES)
        os.pwrite(fd, struct.pack('<Q', 1), rt.LINK_UP)
        os.close(fd)
        self.addCleanup(os.unlink, self.path)

    def _client(self, **link):
        self.link = Link(self.path, **link)
        self.link.start()
        self.addCleanup(self.link.stop.set)
        c = rt.Client('x', helper=FakeHelper(), mailbox_path=self.path)
        self.addCleanup(c.close)
        return c

    def test_clean_link_reports_no_mismatches(self):
        c = self._client()
        self.assertTrue(c.up())
        rows = rt.run_calls(c, [64, 4096, 65536], calls=20, warmup=2)
        self.assertEqual([r['mismatches'] for r in rows], [0, 0, 0])
        self.assertEqual([r['reply_bytes'] for r in rows], [64, 4096, 65536])

    def test_same_seed_still_checks_every_reply(self):
        rows = rt.run_calls(self._client(corrupt=True), [4096], calls=5, warmup=0, same_seed=True)
        self.assertEqual(rows[0]['mismatches'], 5)
        self.assertTrue(rows[0]['same_seed'])

    def test_corrupt_reply_is_counted(self):
        rows = rt.run_calls(self._client(corrupt=True), [4096], calls=5, warmup=0)
        self.assertEqual(rows[0]['mismatches'], 5)

    def test_stale_reply_is_caught_even_with_same_seed(self):
        # Review of 26 Sep: identical bytes per size let a leftover reply pass.
        rows = rt.run_calls(self._client(stale=True), [4096], calls=5, warmup=0, same_seed=True)
        self.assertEqual(rows[0]['mismatches'], 4)   # every call after the first

    def test_reconnect_fails_the_call(self):
        with self.assertRaisesRegex(RuntimeError, 'generation'):
            self._client(bump_generation=True).call(64, 1, timeout_s=2)

    def test_silent_peer_times_out(self):
        with self.assertRaises(TimeoutError):
            self._client(silent=True).call(64, 1, timeout_s=0.3)

    def test_pattern_depends_on_seed(self):
        self.assertNotEqual(rt.pattern(1, 64), rt.pattern(2, 64))
        self.assertEqual(len(rt.pattern(7, 10000)), 10000)


if __name__ == '__main__':
    unittest.main()
