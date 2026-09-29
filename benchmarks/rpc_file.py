#!/usr/bin/env python3
"""Pull a file across one mcdma-rpcd link.

`serve NAME DIR` runs on the listen host next to the file's owner (for a
KV handoff: the producer's llama-server slot directory). It answers two
requests: STAT (file size) and READ (bytes at an offset, straight from the
file into the reply half with readinto). Only plain names inside DIR are
served. `pull NAME FILE DEST` runs on the connect host: it pulls the file in
reply-sized frames straight from the mailbox into DEST and prints one JSON
line with bytes, seconds and Gbit/s. The link stays up between pulls, so
there is no per-transfer setup (unlike mcdma-bw's resident payload runs).
"""
import argparse
import json
import os
from pathlib import Path
import stat
import struct
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rpc_roundtrip import Client, ServiceMailbox  # noqa: E402

REQ = struct.Struct('<BQI')      # op, offset, length; the file name follows
STAT, READ = 1, 2
# An empty reply is the error signal: STAT always answers 8 bytes and the
# client only READs inside the file, so a valid answer is never empty.


def _name_ok(name):
    return name and '/' not in name and name not in ('.', '..') and not name.startswith('.')


def _open_regular(path):
    """Open `path` read-only as a regular file: no symlinks, and no FIFO or
    device that could block or misbehave (anyone who can write the mailbox
    chooses the name)."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            raise ValueError('not a regular file')
    except BaseException:
        os.close(fd)
        raise
    return fd


def handle(root, payload, area, max_reply):
    """Answer one request into `area`; returns the reply length, 0 on error."""
    try:
        op, offset, length = REQ.unpack_from(payload)
        fname = bytes(payload[REQ.size:]).decode()
        if not _name_ok(fname):
            raise ValueError('bad name')
        fd = _open_regular(root / fname)
        try:
            if op == STAT:
                area[:8] = struct.pack('<Q', os.fstat(fd).st_size)
                return 8
            if op == READ:
                length = min(length, max_reply)
                with open(fd, 'rb', buffering=0, closefd=False) as f:
                    f.seek(offset)
                    return f.readinto(area[:length]) or 0
            raise ValueError('bad op')
        finally:
            os.close(fd)
    except (OSError, ValueError, struct.error, UnicodeDecodeError):
        return 0


def serve(name, directory, helper=None, socket_path=None, mailbox_path=None, stop_after=None):
    box = ServiceMailbox(name, socket_path=socket_path, mailbox_path=mailbox_path, helper=helper)
    root = Path(directory).resolve()
    served = 0
    try:
        while box.alive and (stop_after is None or served < stop_after):
            got = box.next_request(1.0)
            if not got:
                continue
            seq, payload = got
            box.publish(seq, handle(root, payload, box.reply_area(), box.max_reply))
            served += 1
    finally:
        box.close()
    return served


def _request(client, op, offset, length, fname, timeout_s=10.0):
    """One call; returns the reply length (the bytes are at client.reply_view)."""
    return client.raw_call(REQ.pack(op, offset, length) + fname.encode(), timeout_s)[1]


def _stat(client, fname):
    if _request(client, STAT, 0, 0, fname) != 8:
        raise SystemExit(f'{fname}: not served')
    return struct.unpack_from('<Q', client.reply_view(8))[0]


def _pull_range(client, fname, fd, first, end, errors):
    frame = len(client.reply_view(client.rep))
    done = first
    try:
        while done < end:
            n = _request(client, READ, done, min(frame, end - done), fname)
            if n == 0:
                raise RuntimeError(f'{fname}: read failed at {done}')
            os.pwrite(fd, client.reply_view(n), done)
            done += n
    except Exception as exc:   # reported by the caller; a thread must not die silently
        errors.append(exc)


def pull(clients, fname, dest):
    """Pull `fname` into `dest`. With several links (one Client each, each
    with its own rpc_file.py service) the file is split into contiguous
    ranges pulled concurrently, so one link's file copies overlap another's
    wire time. Protocol 1 allows one call at a time per link, so a single
    link runs read, wire and write strictly in series (measured 26 Sep:
    12 Gbit/s on a link that echoes at 26)."""
    if not isinstance(clients, (list, tuple)):
        clients = [clients]
    size = _stat(clients[0], fname)
    frame = min(len(c.reply_view(c.rep)) for c in clients)
    per = -(-size // len(clients))
    per = -(-per // frame) * frame or frame      # whole frames per link
    ranges = [(i * per, min(size, (i + 1) * per)) for i in range(len(clients))]
    errors = []
    fd = os.open(dest, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        os.ftruncate(fd, size)
        t0 = time.perf_counter()
        threads = [threading.Thread(target=_pull_range, args=(c, fname, fd, a, b, errors))
                   for c, (a, b) in zip(clients, ranges) if a < b]
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        secs = time.perf_counter() - t0
    finally:
        os.close(fd)
    if errors:
        raise SystemExit(str(errors[0]))
    return {'bytes': size, 'seconds': secs, 'gbit': size * 8 / secs / 1e9 if secs else None,
            'frame_bytes': frame, 'links': len(clients)}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='mode', required=True)
    s = sub.add_parser('serve')
    s.add_argument('name')
    s.add_argument('directory')
    c = sub.add_parser('pull')
    c.add_argument('name', help='link name, or several comma-separated to pull ranges concurrently')
    c.add_argument('file', help='plain name inside the served directory')
    c.add_argument('dest')
    args = p.parse_args(argv)
    if args.mode == 'serve':
        print(json.dumps({'served': serve(args.name, args.directory)}))
        return 0
    names = args.name.split(',')
    if len(set(names)) != len(names):
        # Protocol 1 has one caller per link: two clients on one mailbox would
        # race on its sequence and take each other's replies.
        raise SystemExit(f'each link may appear once: {args.name}')
    clients = [Client(n) for n in names]
    try:
        down = [n for n, c in zip(args.name.split(','), clients) if not c.up()]
        if down:
            raise SystemExit(f'link down: {down}')
        print(json.dumps(pull(clients, args.file, args.dest)))
    finally:
        for c in clients:
            c.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
