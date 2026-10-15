"""Offline tests of the vLLM KV handoff producer: protocol, cache geometry, frames, serving and the mailbox."""
import json
import mmap
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
import zlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'integrations' / 'vllm'))

from mcdma_kv import wire  # noqa: E402
from mcdma_kv.export import Export, LayerPages, geometry, plan_frames  # noqa: E402
from mcdma_kv.mailbox import CTRL, ServiceMailbox, load_helper  # noqa: E402
from mcdma_kv.responder import ExportTable, Responder  # noqa: E402

HANDOFF = bytes(range(16))


def flash_shape(blocks, tokens, heads, size, cache_dtype='auto'):
    """vLLM FlashAttention's logical cache shape: K and V packed along the content dimension."""
    if tokens % 16:
        raise ValueError('Block size must be a multiple of 16.')
    return (blocks, heads, tokens, 2 * size)


def kv_first_shape(blocks, tokens, heads, size):
    return (2, blocks, tokens, heads, size)


def mla_shape(blocks, tokens, heads, size):
    return (blocks, tokens, size)


class WireTests(unittest.TestCase):
    def test_headers_round_trip(self):
        header = wire.Header(wire.DATA, HANDOFF, 3, 9, 2, wire.CHECKED, 16, 4, 8, 77)
        self.assertEqual(wire.unpack(wire.pack(header) + b'\0' * 8), header)

    def test_malformed_headers_are_refused(self):
        for payload in (b'\0' * 10, b'XXXX' + b'\0' * 124, wire.pack(wire.Header(wire.DATA, HANDOFF, nbytes=8))):
            with self.assertRaises(wire.WireError):
                wire.unpack(payload)

    def test_the_prompt_digest_is_over_little_endian_uint32_ids(self):
        import hashlib
        expected = hashlib.sha256((1).to_bytes(4, 'little') + (70000).to_bytes(4, 'little')).hexdigest()
        self.assertEqual(wire.token_sha256([1, 70000]), expected)


class GeometryTests(unittest.TestCase):
    def test_packed_flash_attention_pages_are_labelled_by_the_backend(self):
        labels, ratio = geometry(flash_shape(1000, 16, 8, 128), num_blocks=1000, block_size=16, heads=8,
                                 head_size=128, mla=False, probe=flash_shape)
        self.assertEqual((labels, ratio), (['block', 'head', 'token', 'kv_head_dim'], 1))

    def test_the_backend_resolves_sizes_that_look_alike(self):
        # Sixteen heads in sixteen-token blocks cannot be told apart by size alone.
        labels, _ = geometry(kv_first_shape(64, 16, 16, 64), num_blocks=64, block_size=16, heads=16,
                             head_size=64, mla=False, probe=kv_first_shape)
        self.assertEqual(labels, ['kv', 'block', 'token', 'head', 'head_dim'])
        with self.assertRaises(ValueError):
            geometry(kv_first_shape(64, 16, 16, 64), num_blocks=64, block_size=16, heads=16, head_size=64, mla=False)

    def test_kernel_block_splitting_is_found(self):
        # A 1024-token scheduler block kept as four 256-token kernel blocks.
        labels, ratio = geometry(flash_shape(400, 256, 4, 128), num_blocks=100, block_size=1024, heads=4,
                                 head_size=128, mla=False, probe=flash_shape)
        self.assertEqual((labels[0], ratio), ('block', 4))

    def test_mla_pages_hold_one_latent_row_per_token(self):
        labels, _ = geometry(mla_shape(500, 64, 1, 576), num_blocks=500, block_size=64, heads=1, head_size=576,
                             mla=True, probe=mla_shape)
        self.assertEqual(labels, ['block', 'token', 'latent'])

    def test_sizes_alone_label_an_unambiguous_layout(self):
        labels, _ = geometry((300, 32, 4, 256), num_blocks=300, block_size=32, heads=4, head_size=128, mla=False)
        self.assertEqual(labels, ['block', 'token', 'head', 'kv_head_dim'])


class FrameTests(unittest.TestCase):
    def test_frames_split_layers_to_fit_the_reply_half(self):
        self.assertEqual(plan_frames([(0, 5, 100), (1, 2, 100)], 250),
                         [(0, 0, 2), (0, 2, 2), (0, 4, 1), (1, 0, 2)])
        with self.assertRaises(ValueError):
            plan_frames([(0, 1, 300)], 250)


class _Tensor:
    def __init__(self, shape, itemsize=2):
        self.shape = shape
        self._itemsize = itemsize

    def element_size(self):
        return self._itemsize


class _Mailbox:
    """Holds replies in memory the way the reply half would."""

    def __init__(self, reply_bytes=CTRL + 4096):
        self.max_reply = reply_bytes - CTRL
        self.area = bytearray(self.max_reply)
        self.replies = []
        self.alive = True

    def reply(self, seq, parts):
        data = b''.join(bytes(part) for part in parts)
        self.replies.append((seq, data))

    def reply_area(self):
        return memoryview(self.area)

    def publish(self, seq, length):
        self.replies.append((seq, bytes(self.area[:length])))


def _export(tokens=40):
    layer = LayerPages(index=0, tensor=_Tensor((10, 2, 16, 16)), block_dim=0, rows=[4, 5, 6], dims=[
        'block', 'head', 'token', 'kv_head_dim'], dtype='bfloat16', heads=2, total_heads=2, head_size=8)
    return Export('req-1', HANDOFF, list(range(tokens)), 0, 16, [layer])


def _request(kind, frame=0, payload=b''):
    return memoryview(wire.pack(wire.Header(kind, HANDOFF, frame=frame, nbytes=len(payload))) + payload)


def _pattern(export, frame, out):
    _, start, rows = export.frames[frame]
    size = rows * export.layers[0].row_bytes
    out[:size] = bytes((start + index) % 251 for index in range(size))
    return size


class ResponderTests(unittest.TestCase):
    def setUp(self):
        self.table = ExportTable(ttl_s=60, clock=lambda: 0.0)
        self.mailbox = _Mailbox()
        self.responder = Responder(self.mailbox, self.table, _pattern, model='org/model', tp_rank=1, tp_size=2)

    def answer(self, request):
        self.responder.handle(7, request)
        seq, data = self.mailbox.replies[-1]
        return wire.unpack(data), data

    def test_an_unknown_handoff_is_asked_for_again(self):
        header, _ = self.answer(_request(wire.OPEN, payload=b'{"checksum": true}'))
        self.assertEqual(header.kind, wire.WAIT)

    def test_a_handoff_is_served_frame_by_frame_then_released(self):
        self.table.add(HANDOFF, 'req-1', _export())
        header, data = self.answer(_request(wire.OPEN, payload=b'{"checksum": true}'))
        self.assertEqual(header.kind, wire.MANIFEST)
        manifest = json.loads(bytes(wire.body(data, header)))
        # Each 1 KiB row fits the 4 KiB reply half three to a frame after the header.
        self.assertEqual((manifest['frames'], header.frames), (1, 1))
        self.assertEqual((manifest['tp_rank'], manifest['tp_size'], manifest['block_size']), (1, 2, 16))
        self.assertEqual(manifest['token_sha256'], wire.token_sha256(list(range(40))))
        self.assertEqual(manifest['layers'][0]['shape'], [3, 2, 16, 16])
        header, data = self.answer(_request(wire.PULL, 0))
        payload = wire.body(data, header)
        self.assertEqual((header.kind, header.rows, header.nbytes), (wire.DATA, 3, 3 * 1024))
        self.assertEqual(header.crc, zlib.crc32(payload))
        header, _ = self.answer(_request(wire.CLOSE))
        self.assertEqual(header.kind, wire.ACK)
        self.assertEqual(self.table.take_finished(), {'req-1'})

    def test_checksums_are_skipped_when_the_decoder_asks(self):
        self.table.add(HANDOFF, 'req-1', _export())
        self.answer(_request(wire.OPEN, payload=b'{"checksum": false}'))
        header, _ = self.answer(_request(wire.PULL, 0))
        self.assertEqual((header.flags, header.crc), (0, 0))

    def test_a_handoff_that_could_not_be_exported_says_why(self):
        self.table.add(HANDOFF, 'req-1', 'float8_e4m3fn KV caches cannot be exported')
        header, data = self.answer(_request(wire.OPEN, payload=b'{}'))
        self.assertEqual(header.kind, wire.ERROR)
        self.assertIn(b'cannot be exported', bytes(wire.body(data, header)))

    def test_a_frame_outside_the_handoff_is_refused(self):
        self.table.add(HANDOFF, 'req-1', _export())
        self.answer(_request(wire.OPEN, payload=b'{}'))
        header, _ = self.answer(_request(wire.PULL, 9))
        self.assertEqual(header.kind, wire.ERROR)

    def test_unclosed_handoffs_expire_and_free_their_blocks(self):
        now = [0.0]
        table = ExportTable(ttl_s=10, clock=lambda: now[0])
        table.add(HANDOFF, 'req-9', _export())
        now[0] = 11.0
        table.expire()
        self.assertIsNone(table.get(HANDOFF))
        self.assertEqual(table.take_finished(), {'req-9'})


class ChecksumTests(unittest.TestCase):
    """The responder prefers python-isal's CRC-32 and falls back to zlib's; both must match the wire format."""

    def _reloaded_responder(self, block_isal):
        import importlib
        from mcdma_kv import responder
        saved = {name: sys.modules.get(name) for name in ('isal', 'isal.isal_zlib')}
        if block_isal:
            sys.modules['isal'] = None
            sys.modules['isal.isal_zlib'] = None
        try:
            return importlib.reload(responder)
        finally:
            for name, module in saved.items():
                if module is None:
                    sys.modules.pop(name, None)
                else:
                    sys.modules[name] = module

    def tearDown(self):
        import importlib
        from mcdma_kv import responder
        importlib.reload(responder)

    def _serve_one_frame(self, module):
        table = module.ExportTable(ttl_s=60, clock=lambda: 0.0)
        mailbox = _Mailbox()
        responder = module.Responder(mailbox, table, _pattern, model='org/model', tp_rank=1, tp_size=2)
        table.add(HANDOFF, 'req-1', _export())
        responder.handle(7, _request(wire.OPEN, payload=b'{"checksum": true}'))
        responder.handle(7, _request(wire.PULL, 0))
        _, data = mailbox.replies[-1]
        header = wire.unpack(data)
        return header, wire.body(data, header)

    def test_without_isal_the_responder_uses_zlib(self):
        module = self._reloaded_responder(block_isal=True)
        self.assertIs(module._crc32, zlib.crc32)
        header, payload = self._serve_one_frame(module)
        self.assertEqual((header.flags & wire.CHECKED, header.crc), (wire.CHECKED, zlib.crc32(payload)))

    def test_with_isal_the_frame_crc_still_matches_zlib(self):
        try:
            from isal import isal_zlib
        except ImportError:
            self.skipTest('python-isal is not installed')
        module = self._reloaded_responder(block_isal=False)
        self.assertIs(module._crc32, isal_zlib.crc32)
        header, payload = self._serve_one_frame(module)
        self.assertEqual(header.crc, zlib.crc32(payload))
        data = bytearray(os.urandom(1 << 20))
        view = memoryview(data)
        for sample in (b'', b'123456789', bytes(data), data, view, view[128:], view[128:].toreadonly()):
            self.assertEqual(module._crc32(sample), zlib.crc32(sample))
        self.assertEqual(module._crc32(b'123456789'), 0xcbf43926)


@unittest.skipUnless(shutil.which('cc'), 'C compiler required')
class MailboxTests(unittest.TestCase):
    """The real mailbox class over a file mailbox, with a stand-in daemon socket."""

    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp(prefix='kvh-', dir='/tmp')
        suffix = '.dylib' if sys.platform == 'darwin' else '.so'
        cls.library = os.path.join(cls.work, 'libmcdma-rpc' + suffix)
        subprocess.run(['cc', '-std=c11', '-O2', '-shared', '-fPIC', 'rpc/libmcdma_rpc.c', '-o', cls.library],
                       cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def setUp(self):
        self.box = os.path.join(self.work, 'box')
        with open(self.box, 'wb') as stream:
            stream.truncate(2 * (1 << 20))
        with open(self.box, 'r+b') as stream, mmap.mmap(stream.fileno(), 0) as view:
            view[256:264] = (1 << 20).to_bytes(8, 'little')
            view[264:272] = (1 << 20).to_bytes(8, 'little')
        self.sock = os.path.join(self.work, 'd.sock')
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(self.sock)
        self.listener.listen(1)
        self.lines = []
        threading.Thread(target=self._daemon, daemon=True).start()

    def tearDown(self):
        if hasattr(self, 'connection'):
            self.connection.close()
        self.listener.close()
        os.unlink(self.sock)

    def _daemon(self):
        connection, _ = self.listener.accept()
        self.lines.append(connection.recv(64))
        connection.sendall(b'OK\n')
        self.connection = connection

    def test_a_request_lands_and_its_reply_is_staged(self):
        mailbox = ServiceMailbox('t', socket_path=self.sock, mailbox_path=self.box, helper=load_helper(self.library))
        try:
            self.assertEqual(self.lines, [b'MODE poll\n'])
            self.assertIsNone(mailbox.next_request(0.01))
            mailbox.buffer[CTRL:CTRL + 5] = b'hello'
            mailbox.buffer[0:8] = ((9 << 32) | 5).to_bytes(8, 'little')
            seq, payload = mailbox.next_request(1.0)
            self.assertEqual((seq, bytes(payload)), (9, b'hello'))
            payload.release()
            mailbox.reply(seq, (b'world',))
            staged = int.from_bytes(mailbox.buffer[(1 << 20) + 128:(1 << 20) + 136], 'little')
            self.assertEqual(staged, (9 << 32) | 5)
            self.assertEqual(bytes(mailbox.buffer[(1 << 20) + CTRL:(1 << 20) + CTRL + 5]), b'world')
            self.assertTrue(mailbox.alive)
            self.connection.sendall(b'BYE\n')
            import time
            time.sleep(0.05)
            self.assertFalse(mailbox.alive)
        finally:
            mailbox.close()


if __name__ == '__main__':
    unittest.main()
