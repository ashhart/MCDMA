"""Offline checks for benchmarks/rpc_file.py: a thread plays connect daemon
plus the remote file service (rpc_file.handle) on one mapped mailbox, so a
multi-frame pull, error replies and name confinement run without hardware."""
import ctypes
import hashlib
import importlib.util
import mmap
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'benchmarks'))
spec = importlib.util.spec_from_file_location('rf', ROOT / 'benchmarks/rpc_file.py')
rf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rf)
sys.path.insert(0, str(ROOT / 'tests'))
from test_rpc_roundtrip import FakeHelper  # noqa: E402
CTRL = 4096

HALF = 1 << 20


class FileLink(threading.Thread):
    def __init__(self, box_path, root, flip=False):
        super().__init__(daemon=True)
        self.box_path, self.root, self.flip = box_path, root, flip
        self.stop = threading.Event()

    def run(self):
        fd = os.open(self.box_path, os.O_RDWR)
        m = mmap.mmap(fd, os.fstat(fd).st_size)
        os.close(fd)
        view = memoryview(m)
        base = ctypes.addressof(ctypes.c_char.from_buffer(m))
        last = 0
        while not self.stop.is_set():
            w = ctypes.c_uint64.from_address(base).value
            if not w or (w >> 32) == last:
                continue
            last = w >> 32
            payload = bytes(view[CTRL:CTRL + (w & 0xFFFFFFFF)])
            area = view[HALF + CTRL:2 * HALF]
            n = rf.handle(self.root, payload, area, len(area))
            if self.flip and n > 8:
                area[n - 1] ^= 1
            ctypes.c_uint64.from_address(base + HALF + 64).value = last << 32 | n


class Pull(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.box = self.root / 'box'
        with open(self.box, 'wb') as f:
            f.truncate(2 * HALF)
        with open(self.box, 'r+b') as f:
            f.seek(256)
            f.write(struct.pack('<QQ', HALF, HALF))
            f.seek(64)
            f.write(struct.pack('<Q', 1))
        self.served = self.root / 'served'
        self.served.mkdir()
        self.data = os.urandom(3 * HALF + 12345)   # four frames, the last one short
        (self.served / 'kvh-8192.bin').write_bytes(self.data)
        (self.root / 'secret').write_bytes(b'nope')

    def _client(self, **kw):
        link = FileLink(str(self.box), self.served.resolve(), **kw)
        link.start()
        self.addCleanup(link.stop.set)
        c = rf.Client('x', helper=FakeHelper(), mailbox_path=str(self.box))
        self.addCleanup(c.close)
        return c

    def test_multi_frame_pull_is_exact(self):
        dest = self.root / 'out.bin'
        r = rf.pull(self._client(), 'kvh-8192.bin', dest)
        self.assertEqual(r['bytes'], len(self.data))
        self.assertEqual(hashlib.sha256(dest.read_bytes()).digest(), hashlib.sha256(self.data).digest())

    def test_two_links_pull_ranges_concurrently_and_exactly(self):
        box2 = self.root / 'box2'
        box2.write_bytes(self.box.read_bytes())
        clients = [self._client()]
        link = FileLink(str(box2), self.served.resolve())
        link.start()
        self.addCleanup(link.stop.set)
        c2 = rf.Client('y', helper=FakeHelper(), mailbox_path=str(box2))
        self.addCleanup(c2.close)
        clients.append(c2)
        dest = self.root / 'out2.bin'
        r = rf.pull(clients, 'kvh-8192.bin', dest)
        self.assertEqual(r['links'], 2)
        self.assertEqual(dest.read_bytes(), self.data)

    def test_failed_range_is_reported(self):
        c = self._client()
        (self.served / 'kvh-8192.bin').write_bytes(self.data[:100])   # shrinks after STAT would see it
        with self.assertRaises(SystemExit):
            errors = []
            rf._pull_range(c, 'kvh-8192.bin', os.open(self.root / 'o3', os.O_WRONLY | os.O_CREAT), 100, 5000, errors)
            if errors:
                raise SystemExit(str(errors[0]))

    def test_file_ending_in_ff_bytes_is_not_an_error(self):
        # The old in-band marker was four 0xff bytes; a file may end in them.
        data = os.urandom(HALF) + b'\xff\xff\xff\xff'
        (self.served / 'ff.bin').write_bytes(data)
        dest = self.root / 'ff.out'
        rf.pull(self._client(), 'ff.bin', dest)
        self.assertEqual(dest.read_bytes(), data)

    def test_symlink_and_fifo_are_refused(self):
        os.symlink(self.root / 'secret', self.served / 'link.bin')
        os.mkfifo(self.served / 'pipe.bin')
        for name in ('link.bin', 'pipe.bin'):
            with self.assertRaises(SystemExit, msg=name):
                rf.pull(self._client(), name, self.root / 'o')

    def test_duplicate_link_names_refused(self):
        with self.assertRaisesRegex(SystemExit, 'once'):
            rf.main(['pull', 'rt0,rt0', 'x.bin', str(self.root / 'o')])

    def test_corruption_shows_in_the_hash(self):
        dest = self.root / 'out.bin'
        rf.pull(self._client(flip=True), 'kvh-8192.bin', dest)
        self.assertNotEqual(dest.read_bytes(), self.data)

    def test_missing_file_refused(self):
        with self.assertRaises(SystemExit):
            rf.pull(self._client(), 'absent.bin', self.root / 'o')

    def test_names_cannot_leave_the_directory(self):
        for name in ('../secret', '/etc/passwd', '.hidden', '..'):
            self.assertFalse(rf._name_ok(name), name)
        with self.assertRaises(SystemExit):
            rf.pull(self._client(), '../secret', self.root / 'o')


if __name__ == '__main__':
    unittest.main()
