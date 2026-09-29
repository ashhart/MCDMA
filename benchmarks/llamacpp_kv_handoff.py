#!/usr/bin/env python3
"""Prefill on one llama.cpp host, decode on another, move the KV between them.

The producer's llama-server prefills a prompt and saves its slot (the KV
cache) to a file. The file crosses the link twice per round, once through
mcdma-bw's resident payload mode (RDMA WRITE, CRC64-verified) and once over a
single TCP stream on the same link, in alternating order. The consumer's
llama-server restores the slot and decodes. The run records:

- producer prefill time (llama-server's own prompt_ms) and slot save time;
- per transport: wire seconds (RDMA: the mcdma-bw data loop summed over
  chunks; TCP: first byte to EOF on the receiver) and the wall clock of the
  whole transfer step, which for RDMA includes per-chunk SSH and QP setup;
- consumer restore time, how many prompt tokens it still had to evaluate
  (1 when the handoff worked), time to first token and decode speed;
- a consumer-only baseline that prefills the whole prompt itself, and
  whether the handed-off text matches the producer's own continuation.

Both servers must run the same GGUF with the same context size, one slot
(-np 1) and --slot-save-path; the slot directories are passed here. Every
host, path and address is an argument; SSH carries control only.

mcdma-bw moves at most 16 MiB x 64 slots per payload run, so larger slot
files go in chunks, each a separate run with its own setup. The `link` arm
(--arms ...,link) instead pulls the file over a running mcdma-rpcd link with
benchmarks/rpc_file.py: no per-transfer setup (see link-daemon.md).
"""
import argparse
import csv
import hashlib
import ipaddress
import json
import math
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
MAX_REQUEST = 16 << 20
MAX_DEPTH = 64
GUARD = 16384
CORPUS = ('Record {n}: the relay on line {m} reported pressure {p} kPa at step {s}, '
          'and the operator logged it without comment. ')


def chunk_plan(size, request=MAX_REQUEST, depth=MAX_DEPTH, max_region=1 << 30):
    """Byte ranges (offset, length) that each fit one resident payload run:
    request * depth slots, less the guard, and within max_region."""
    if size <= 0:
        raise ValueError('empty slot file')
    fit = min(depth, (max_region - GUARD) // request)
    if fit < 1:
        raise ValueError('max_region smaller than one request')
    per = request * fit
    return [(off, min(per, size - off)) for off in range(0, size, per)]


def request_for(length, request=MAX_REQUEST):
    """A short last chunk uses a smaller request (4 KiB multiple), since
    run_bw.py refuses a total below the request size."""
    return min(request, max(4096, -(-length // 4096) * 4096))


def depth_for(length, request=MAX_REQUEST):
    return max(1, math.ceil(length / request))


def token_sha256(tokens):
    return hashlib.sha256(struct.pack(f'<{len(tokens)}I', *tokens)).hexdigest()


def corpus(n_chars):
    out, i = [], 0
    while sum(map(len, out)) < n_chars:
        out.append(CORPUS.format(n=i, m=(i * 7) % 97, p=100 + (i * 13) % 900, s=(i * 31) % 1000))
        i += 1
    return ''.join(out)


def payload_seconds(csv_text):
    """Initiator data-loop seconds from one resident-payload run's CSV."""
    rows = [r for r in csv.DictReader(csv_text.splitlines())
            if r.get('side') == 'initiator' and r.get('warmup') == '0']
    if len(rows) != 1:
        raise ValueError(f'expected one initiator row, got {len(rows)}')
    r = rows[0]
    if r.get('errors') != '0' or r.get('mismatches') != '0' or r.get('guard_ok') != '1':
        raise ValueError(f'unclean payload row: {r}')
    return float(r['seconds'])


class Host:
    def __init__(self, ssh, port, slot_dir, dry):
        self.ssh, self.port, self.slot_dir, self.dry = ssh, port, slot_dir.rstrip('/'), dry

    def sh(self, command, stdin=None, timeout=900):
        argv = ['ssh', '-o', 'BatchMode=yes', self.ssh, command]
        if self.dry:
            print('DRY', shlex.join(argv))
            return ''
        r = subprocess.run(argv, input=stdin, capture_output=True, text=True, timeout=timeout)
        if r.returncode:
            raise RuntimeError(f'{self.ssh}: {command[:80]}: exit {r.returncode}: {r.stderr.strip()[-400:]}')
        return r.stdout

    def api(self, method, path, body=None, timeout=900):
        cmd = (f'curl -sS --fail-with-body -X {method} -H "Content-Type: application/json" '
               f'--data-binary @- {shlex.quote(f"http://127.0.0.1:{self.port}{path}")}')
        out = self.sh(cmd, stdin=json.dumps(body or {}), timeout=timeout)
        return json.loads(out) if out else {}

    def path(self, name):
        return f'{self.slot_dir}/{name}'

    def sha256(self, name):
        out = self.sh(f'sha256sum {shlex.quote(self.path(name))}')
        return out.split()[0] if out else ''

    def size(self, name):
        out = self.sh(f'stat -c %s {shlex.quote(self.path(name))}')
        return int(out) if out else 0


def complete(host, tokens, n_predict):
    r = host.api('POST', '/completion', {'prompt': tokens, 'n_predict': n_predict, 'temperature': 0,
                                          'seed': 0, 'cache_prompt': True, 'id_slot': 0})
    t = r.get('timings', {})
    return {'prompt_n': t.get('prompt_n'), 'prompt_ms': t.get('prompt_ms'),
            'predicted_n': t.get('predicted_n'), 'predicted_ms': t.get('predicted_ms'),
            'predicted_per_second': t.get('predicted_per_second'), 'content': r.get('content', '')}


def tcp_transfer(src, dst, name, dst_ip, port):
    """One TCP stream src -> dst on the link; seconds from first byte to EOF,
    measured on the receiver."""
    recv = ('python3 -c ' + shlex.quote(
        'import socket,sys,time\n'
        's=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)\n'
        f's.bind(("0.0.0.0",{port}));s.listen(1);print("READY",flush=True)\n'
        'c,_=s.accept();f=open(sys.argv[1],"wb");n=0;t0=None\n'
        'while True:\n'
        ' b=c.recv(1<<22)\n'
        ' if not b: break\n'
        ' t0=t0 or time.perf_counter();n+=len(b);f.write(b)\n'
        'f.close();print(n,(time.perf_counter()-t0) if t0 else 0.0)') + ' ' + shlex.quote(dst.path(name)))
    send = ('python3 -c ' + shlex.quote(
        'import socket,sys\n'
        f'c=socket.create_connection(("{dst_ip}",{port}));f=open(sys.argv[1],"rb")\n'
        'c.sendfile(f);c.shutdown(socket.SHUT_WR);c.recv(1)') + ' ' + shlex.quote(src.path(name)))
    if src.dry:
        print('DRY', dst.ssh, recv)
        print('DRY', src.ssh, send)
        return 0, 0.0
    rx = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', dst.ssh, recv], stdout=subprocess.PIPE, text=True)
    if rx.stdout.readline().strip() != 'READY':
        raise RuntimeError('TCP receiver did not start')
    src.sh(send)
    out, _ = rx.communicate(timeout=900)
    n, secs = out.split()
    return int(n), float(secs)


def rdma_transfer(args, src, dst, name, size, outdir):
    """Chunked resident-payload WRITE runs; returns (wire seconds, chunks)."""
    plan = chunk_plan(size, args.request)
    total = 0.0
    part = f'{name}.part'
    for i, (off, length) in enumerate(plan):
        if len(plan) > 1:
            src.sh(f'dd if={shlex.quote(src.path(name))} of={shlex.quote(src.path(part))} bs=1M '
                   f'iflag=skip_bytes,count_bytes skip={off} count={length} status=none')
        dst.sh(f'rm -f {shlex.quote(dst.path(part))}')
        out = outdir / f'chunk{i}'
        req = request_for(length, args.request)
        argv = [sys.executable, str(ROOT / 'run_bw.py'), '--mac-platform', 'linux',
                '--mac-host', src.ssh, '--peer-host', dst.ssh, '--mac-bw', args.src_bw, '--peer-bw', args.dst_bw,
                '--mac-interface', args.src_interface, '--peer-interface', args.dst_interface,
                '--mac-device', args.src_device, '--peer-device', args.dst_device,
                '--mac-gid-index', str(args.src_gid_index), '--peer-gid-index', str(args.dst_gid_index),
                '--ops', 'write', '--sizes', str(req), '--depths', str(depth_for(length, req)),
                '--qps', '1', '--initiators', 'mac', '--repeats', '1', '--warmup', '0', '--mtu', str(args.mtu),
                '--total', str(max(length, req)), '--timeout', '120',
                '--payload', src.path(part if len(plan) > 1 else name), '--dump', dst.path(part),
                '--output', str(out)]
        if args.dry_run:
            print('DRY', shlex.join(argv))
            continue
        r = subprocess.run(argv, capture_output=True, text=True, timeout=900)
        if r.returncode:
            raise RuntimeError(f'run_bw chunk {i}: {r.stdout[-600:]}{r.stderr[-600:]}')
        (csv_path,) = out.glob('*.csv')
        total += payload_seconds(csv_path.read_text())
        dst.sh(f'cat {shlex.quote(dst.path(part))} >> {shlex.quote(dst.path(name))} && rm -f {shlex.quote(dst.path(part))}')
    if len(plan) > 1:
        src.sh(f'rm -f {shlex.quote(src.path(part))}')
    return total, len(plan)


def link_transfer(args, dst, name):
    """Pull the slot file over a running mcdma-rpcd link (rpc_file.py serve on
    the producer, connect daemon on the consumer). Wire seconds are the
    consumer's pull loop; the wall clock adds one SSH round trip."""
    cmd = (f'MCDMA_RPC_LIBRARY={shlex.quote(args.dst_rpc_lib)} python3 '
           f'{shlex.quote(args.dst_repo)}/benchmarks/rpc_file.py pull {shlex.quote(args.link_name)} '
           f'{shlex.quote(name)} {shlex.quote(dst.path(name))}')
    out = dst.sh(cmd)
    if dst.dry:
        return 0.0
    return float(json.loads(out.strip().splitlines()[-1])['seconds'])


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for side in ('src', 'dst'):
        p.add_argument(f'--{side}-host', required=True, help='SSH destination')
        p.add_argument(f'--{side}-port', type=int, default=8080, help='llama-server port on that host (127.0.0.1)')
        p.add_argument(f'--{side}-slot-dir', required=True, help="that server's --slot-save-path")
        p.add_argument(f'--{side}-bw', required=True, help='mcdma-bw binary on that host')
        p.add_argument(f'--{side}-interface', required=True)
        p.add_argument(f'--{side}-device', required=True)
        p.add_argument(f'--{side}-gid-index', type=int, required=True)
    p.add_argument('--dst-ip', required=True, help="consumer's address on the RDMA link, for the TCP arm")
    p.add_argument('--tcp-port', type=int, default=18777)
    p.add_argument('--prompts', default='2048,8192', help='prompt lengths in tokens')
    p.add_argument('--rounds', type=int, default=3)
    p.add_argument('--n-predict', type=int, default=64)
    p.add_argument('--request', type=int, default=MAX_REQUEST, help='RDMA request bytes (<= 16 MiB)')
    p.add_argument('--mtu', type=int, choices=[1024, 2048, 4096], default=1024)
    p.add_argument('--arms', default='rdma,tcp', help='any of rdma (mcdma-bw payload runs), tcp, link (mcdma-rpcd)')
    p.add_argument('--link-name', help='mcdma-rpcd link for the link arm; the producer runs rpc_file.py serve on it')
    p.add_argument('--dst-repo', help="this repository's checkout on the consumer, for the link arm")
    p.add_argument('--dst-rpc-lib', help='libmcdma-rpc.so on the consumer, for the link arm')
    p.add_argument('--output', type=Path, required=True, help='new directory for results.json and chunk logs')
    p.add_argument('--dry-run', action='store_true')
    args = p.parse_args(argv)
    if not 4096 <= args.request <= MAX_REQUEST:
        p.error('--request must be 4 KiB to 16 MiB')
    try:
        ipaddress.ip_address(args.dst_ip)
    except ValueError:
        p.error('--dst-ip must be an IP address')
    if args.src_host.startswith('-') or args.dst_host.startswith('-'):
        p.error('SSH destinations may not start with "-"')
    arms = args.arms.split(',')
    if not arms or set(arms) - {'rdma', 'tcp', 'link'}:
        p.error('--arms takes rdma, tcp and link')
    if 'link' in arms and not (args.link_name and args.dst_repo and args.dst_rpc_lib):
        p.error('the link arm needs --link-name, --dst-repo and --dst-rpc-lib')
    src = Host(args.src_host, args.src_port, args.src_slot_dir, args.dry_run)
    dst = Host(args.dst_host, args.dst_port, args.dst_slot_dir, args.dry_run)
    if not args.dry_run:
        args.output.mkdir(parents=True, exist_ok=False)
    results = {'started': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'args': vars(args) | {'output': str(args.output)},
               'runs': []}
    if not args.dry_run:
        props = [h.api('GET', '/props') for h in (src, dst)]
        models = [Path(x.get('model_path', '')).name for x in props]
        ctxs = [x.get('default_generation_settings', {}).get('n_ctx') for x in props]
        if models[0] != models[1] or not models[0]:
            raise SystemExit(f'model mismatch: {models}')
        results['model'], results['n_ctx'] = models[0], ctxs
    for n in [int(x) for x in args.prompts.split(',')]:
        tokens = list(range(n)) if args.dry_run else \
            src.api('POST', '/tokenize', {'content': corpus(n * 8)})['tokens'][:n]
        if len(tokens) < n:
            raise SystemExit(f'corpus gave only {len(tokens)} tokens')
        name = f'kvh-{n}.bin'
        base = {}
        if not args.dry_run:
            src.api('POST', '/slots/0?action=erase')
            base['producer'] = complete(src, tokens, args.n_predict)
            dst.api('POST', '/slots/0?action=erase')
            base['consumer'] = complete(dst, tokens, args.n_predict)
        for r in range(args.rounds):
            run = {'prompt_tokens': n, 'round': r, 'token_sha256': token_sha256(tokens)}
            src.api('POST', '/slots/0?action=erase')
            pre = complete(src, tokens[:-1], 1)
            run['producer_prefill_ms'], run['producer_prompt_n'] = pre['prompt_ms'], pre['prompt_n']
            save = src.api('POST', '/slots/0?action=save', {'filename': name})
            run['save_ms'] = save.get('timings', {}).get('save_ms')
            run['slot_bytes'] = size = src.size(name) if not args.dry_run else 3 << 30
            want = src.sha256(name)
            order = arms[r % len(arms):] + arms[:r % len(arms)]   # rotate so no arm always goes first
            for arm in order:
                dst.sh(f'rm -f {shlex.quote(dst.path(name))}')
                t0 = time.perf_counter()
                if arm == 'rdma':
                    wire, chunks = rdma_transfer(args, src, dst, name, size, args.output / f'n{n}-r{r}-rdma')
                elif arm == 'link':
                    wire, chunks = link_transfer(args, dst, name), 1
                else:
                    moved, wire = tcp_transfer(src, dst, name, args.dst_ip, args.tcp_port)
                    chunks = 1
                    if not args.dry_run and moved != size:
                        raise RuntimeError(f'TCP moved {moved} of {size} bytes')
                wall = time.perf_counter() - t0
                ok = args.dry_run or dst.sha256(name) == want
                if not ok:
                    raise RuntimeError(f'{arm}: consumer slot file differs from the producer')
                dst.api('POST', '/slots/0?action=erase')
                rest = dst.api('POST', '/slots/0?action=restore', {'filename': name})
                dec = complete(dst, tokens, args.n_predict)
                run[arm] = {'wire_s': wire, 'wall_s': wall, 'chunks': chunks,
                            'gbit_wire': (size * 8 / wire / 1e9) if wire else None, 'sha256_ok': ok,
                            'restore_ms': rest.get('timings', {}).get('restore_ms'),
                            'consumer_prompt_n': dec['prompt_n'], 'ttft_ms': dec['prompt_ms'],
                            'decode_tps': dec['predicted_per_second'],
                            'matches_producer': dec['content'] == base.get('producer', {}).get('content'),
                            'matches_consumer_alone': dec['content'] == base.get('consumer', {}).get('content')}
                print(json.dumps({'n': n, 'round': r, 'arm': arm} | {k: v for k, v in run[arm].items()}), flush=True)
            results['runs'].append(run)
        results.setdefault('baselines', {})[str(n)] = {
            k: {kk: vv for kk, vv in v.items() if kk != 'content'} for k, v in base.items()}
    if not args.dry_run:
        (args.output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
