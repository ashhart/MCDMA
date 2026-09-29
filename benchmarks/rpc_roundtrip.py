#!/usr/bin/env python3
"""Round trips through one mcdma-rpcd link, on hardware.

`serve NAME` runs on the listen daemon's host: it registers as the link's
service (integrations/vllm/mcdma_kv/mailbox.py) and answers every request
with a reply of the size the request asks for, filled with a pattern seeded
by the request, its first 8 bytes replaced by the call's sequence number so
a reply left over from an earlier call cannot pass. `call NAME` runs on a
Linux connect host: it drives the client side of Protocol 1
(docs/link-daemon.md) directly on the connect daemon's shared-memory mailbox
(`$MCDMA_RPC_BOX_DIR`, default /dev/shm), checks every reply byte, and prints
one JSON line per size with latency percentiles and reply throughput
(`gbit_reply` from the mean latency). Protocol 1 allows one caller per link:
run nothing else on the link meanwhile.

A call is: request payload -> request word; wait for the done word (reply
+64) to carry the same sequence; read the reply. A change of link
generation (request +72) while waiting fails the call, as the protocol says.
"""
import argparse
import ctypes
import json
import mmap
import os
from pathlib import Path
import struct
import sys
import time
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'integrations/vllm'))
from mcdma_kv.mailbox import CTRL, SIZES, ServiceMailbox, load_helper  # noqa: E402

REQUEST_WORD, LINK_UP, GENERATION, DONE_WORD = 0, 64, 72, 64
HEAD = struct.Struct('<IQ')   # reply bytes wanted, seed
SEQ = 0xFFFFFFFF


def stamped(seq, body):
    """The reply the service sends for sequence `seq`: `body` with its first
    8 bytes (or all of a shorter reply) replaced by the sequence number."""
    tag = (seq & SEQ).to_bytes(8, 'little')[:len(body)]
    return tag + bytes(body[len(tag):])


def pattern(seed, n):
    """Deterministic bytes: cheap to make, and every position depends on the seed."""
    block = (seed.to_bytes(8, 'little') * 512)
    out = bytearray(block * (n // len(block) + 1))
    return bytes(out[:n])


def serve(name, helper=None, socket_path=None, mailbox_path=None, stop_after=None):
    box = ServiceMailbox(name, socket_path=socket_path, mailbox_path=mailbox_path, helper=helper)
    served = 0
    # The last reply is kept: with --same-seed calls the service only copies,
    # so large-reply timings measure the link, not Python building 4 MiB of
    # pattern inside every round trip (the first hardware run, 9.5 Gb/s).
    cached = (None, None, b'')
    try:
        while box.alive and (stop_after is None or served < stop_after):
            got = box.next_request(1.0)
            if not got:
                continue
            seq, payload = got
            try:
                want, seed = HEAD.unpack_from(payload)
            except struct.error:          # a malformed request gets an empty reply, not a dead service
                box.publish(seq, 0)
                continue
            want = min(want, box.max_reply)
            if cached[:2] != (seed, want):
                cached = (seed, want, pattern(seed, want))
            area = box.reply_area()
            area[:want] = cached[2]
            tag = (seq & SEQ).to_bytes(8, 'little')[:want]
            area[:len(tag)] = tag
            box.publish(seq, want)
            served += 1
    finally:
        box.close()
    return served


class Client:
    def __init__(self, name, helper=None, mailbox_path=None):
        self.helper = helper or load_helper()
        path = mailbox_path or os.path.join(os.environ.get('MCDMA_RPC_BOX_DIR', '/dev/shm'), f'mcdma-rpc.{name}')
        fd = os.open(path, os.O_RDWR)
        try:
            self.map = mmap.mmap(fd, os.fstat(fd).st_size)
        finally:
            os.close(fd)
        self.buf = memoryview(self.map)
        self.base = ctypes.addressof(ctypes.c_char.from_buffer(self.map))
        self.req, self.rep = struct.unpack_from('<QQ', self.buf, SIZES)
        if not self.req or self.req + self.rep > len(self.buf):
            raise SystemExit(f'{path}: bad half sizes {self.req}+{self.rep}')
        self.seq = (self._load(REQUEST_WORD) >> 32) & SEQ

    def _load(self, off):
        return int.from_bytes(self.buf[off:off + 8], 'little')

    def up(self):
        return self._load(LINK_UP) == 1

    def raw_call(self, payload, timeout_s=5.0):
        """Send `payload` as one request; returns (seconds, reply length) once
        the done word carries this call's sequence. The reply's bytes are then
        at reply_view(length)."""
        gen = self._load(GENERATION)
        self.seq = (self.seq % SEQ) + 1
        self.buf[CTRL:CTRL + len(payload)] = payload
        t0 = time.perf_counter()
        self.helper.mcdma_rpc_store_word(self.base + REQUEST_WORD, self.seq << 32 | len(payload))
        done = self.base + self.req + DONE_WORD
        deadline = t0 + timeout_s
        while True:
            word = self.helper.mcdma_rpc_wait_word(done, self.seq, 1, 100_000, 50_000_000)
            if word and (word >> 32) == self.seq:
                return time.perf_counter() - t0, word & SEQ
            if self._load(GENERATION) != gen:
                raise RuntimeError('link generation changed during the call')
            if time.perf_counter() > deadline:
                raise TimeoutError(f'no reply to sequence {self.seq}')

    def reply_view(self, n):
        start = self.req + CTRL
        return self.buf[start:start + n]

    def call(self, want, seed, timeout_s=5.0):
        elapsed, n = self.raw_call(HEAD.pack(want, seed), timeout_s)
        return elapsed, n, bytes(self.reply_view(n))

    def close(self):
        self.buf.release()
        self.map.close()


def percentile(values, q):
    s = sorted(values)
    return s[min(len(s) - 1, int(round(q * (len(s) - 1))))]


def run_calls(client, sizes, calls, warmup, same_seed=False):
    results = []
    for size in sizes:
        lat, bad = [], 0
        expect = {}
        for i in range(warmup + calls):
            seed = zlib.crc32(struct.pack('<QQ', size, 0 if same_seed else i))
            secs, n, got = client.call(size, seed)
            if seed not in expect:
                expect = {seed: pattern(seed, size)}
            if n != size or got != stamped(client.seq, expect[seed]):
                bad += 1
            if i >= warmup:
                lat.append(secs)
        results.append({'reply_bytes': size, 'calls': calls, 'mismatches': bad,
                        'p50_us': round(percentile(lat, 0.5) * 1e6, 1),
                        'p99_us': round(percentile(lat, 0.99) * 1e6, 1),
                        'max_us': round(max(lat) * 1e6, 1),
                        'gbit_reply': round(size * 8 * len(lat) / sum(lat) / 1e9, 3),
                        'same_seed': same_seed})
    return results


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='mode', required=True)
    s = sub.add_parser('serve', help='echo service on the listen host')
    s.add_argument('name')
    c = sub.add_parser('call', help='client on the connect host')
    c.add_argument('name')
    c.add_argument('--sizes', default='64,4096,65536,1048576,4194304', help='reply sizes in bytes')
    c.add_argument('--calls', type=int, default=1000)
    c.add_argument('--warmup', type=int, default=20)
    c.add_argument('--same-seed', action='store_true',
                   help='one reply pattern per size, so the service only copies (transport-bound timing)')
    args = p.parse_args(argv)
    if args.mode == 'serve':
        print(json.dumps({'served': serve(args.name)}))
        return 0
    client = Client(args.name)
    try:
        if not client.up():
            raise SystemExit('link is down: check STATUS on the connect daemon')
        sizes = [int(x) for x in args.sizes.split(',')]
        too_big = [x for x in sizes if x > client.rep - CTRL]
        if too_big:
            raise SystemExit(f'sizes {too_big} exceed the reply half ({client.rep - CTRL} bytes)')
        bad = 0
        for row in run_calls(client, sizes, args.calls, args.warmup, args.same_seed):
            bad += row['mismatches']
            print(json.dumps(row), flush=True)
    finally:
        client.close()
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
