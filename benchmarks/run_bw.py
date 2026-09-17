#!/usr/bin/env python3
"""Cross-host sustained-bandwidth sweep for benchmarks/mcdma_bw.c.

SSH carries the control protocol and results only: the two mcdma-bw processes
exchange "BW_MSG" lines through this relay, and the payload moves over the
RDMA link. Requires a loaded native CX5 provider, configured link-local
addresses and static neighbours on both spare QSFP interfaces; changes no
network settings. Every host, path, interface and device is an explicit
argument, so no private lab value is built in.
"""
import argparse
import datetime
import hashlib
import importlib.util
import ipaddress
import itertools
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import subprocess
import sys
import time

_source = Path(__file__).resolve().parents[1] / 'tools/native_cross_host.py'
_spec = importlib.util.spec_from_file_location('native_cross_host', _source)
cross = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(cross)

SSH = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8']
OPS = ('write', 'read', 'send')
INITIATORS = ('mac', 'peer')
CSV_PREFIX = ['initiator', 'round', 'mode_confirmed']


def parse_list(text, kind, allowed=None, low=None, high=None):
    items = [item.strip() for item in text.split(',') if item.strip()]
    if not items:
        raise argparse.ArgumentTypeError(f'empty {kind} list')
    values = []
    for item in items:
        if allowed is not None:
            if item not in allowed:
                raise argparse.ArgumentTypeError(f'{kind} must be one of {",".join(allowed)}')
            values.append(item)
            continue
        if not re.fullmatch(r'[0-9]+', item):
            raise argparse.ArgumentTypeError(f'{kind} values must be decimal integers')
        value = int(item)
        if (low is not None and value < low) or (high is not None and value > high):
            raise argparse.ArgumentTypeError(f'{kind} value {value} outside {low}..{high}')
        values.append(value)
    if len(set(values)) != len(values):
        raise argparse.ArgumentTypeError(f'duplicate {kind} value')
    return values


def sweep(ops, sizes, depths, qps, cq_modes, initiators, rounds):
    """Every configuration for both initiators, once per round. Odd rounds run
    the reversed order so drift affects each configuration from both ends."""
    configs = [dict(initiator=initiator, op=op, bytes=size, depth=depth, qps=q, cq_per_qp=cq)
               for op, size, depth, q, cq, initiator in itertools.product(ops, sizes, depths, qps, cq_modes, initiators)]
    plan = []
    for round_index in range(rounds):
        ordered = configs if round_index % 2 == 0 else list(reversed(configs))
        plan.extend(dict(config, round=round_index) for config in ordered)
    return plan


def config_name(config):
    return (f"{config['initiator']}-{config['op']}-b{config['bytes']}-d{config['depth']}-q{config['qps']}-"
            f"{'cqpq' if config['cq_per_qp'] else 'cq1'}")


def program_arguments(config, role, device, gid_index, args):
    argv = ['--role', role, '--device', device, '--gid-index', str(gid_index),
            '--op', config['op'], '--bytes', str(config['bytes']), '--depth', str(config['depth']),
            '--qps', str(config['qps']), '--total', str(args.total), '--repeats', '1',
            '--warmup', str(args.warmup), '--mtu', str(args.mtu), '--finish', args.finish,
            '--timeout', str(args.timeout), '--verify-bytes', str(args.verify_bytes)]
    if config['cq_per_qp']:
        argv.append('--cq-per-qp')
    if getattr(args,'payload',None) and role=='initiator':
        argv += ['--payload', args.payload]
    if getattr(args,'dump',None) and role=='responder':
        argv += ['--dump', args.dump]
    return argv


def build_commands(args, config):
    """Return the exact (mac_command, peer_command) argument vectors that run
    on each host; the initiator role follows config['initiator']."""
    mac_role = 'initiator' if config['initiator'] == 'mac' else 'responder'
    peer_role = 'responder' if mac_role == 'initiator' else 'initiator'
    mac = ['env', 'IBV_DRIVERS=' + args.mac_provider[:-len('-rdmav34.so')],
           'MCDMA_CQ_MAP=' + args.mac_cq_map, 'MCDMA_USER_POST=' + args.mac_user_post,
           'MCDMA_USER_BF=' + args.mac_user_bf, args.mac_bw]
    mac += program_arguments(config, mac_role, args.mac_device, args.mac_gid_index, args)
    peer = [args.peer_bw] + program_arguments(config, peer_role, args.peer_device, args.peer_gid_index, args)
    return mac, peer


def ssh_line(host, command):
    return shlex.join(SSH + [host, shlex.join(command)])


def descriptor(payload):
    """Validate an ENDPOINT payload; returns (fields, mac_address)."""
    words = payload.split()
    if not words or words[0] != 'ENDPOINT':
        raise ValueError('Missing bandwidth endpoint')
    fields = {}
    for word in words[1:]:
        key, sep, value = word.partition('=')
        if not sep or not re.fullmatch(r'[a-z_]+', key) or key in fields:
            raise ValueError('Malformed endpoint field')
        fields[key] = value
    required = ['v', 'nonce', 'role', 'op', 'bytes', 'depth', 'qps', 'cqpq', 'cqe', 'total', 'mtu',
                'imm_recv', 'rd', 'psn', 'rkey', 'addr', 'length', 'gid', 'qpn']
    if any(key not in fields for key in required):
        raise ValueError('Incomplete endpoint')
    numbers = {key: fields[key] for key in required if key not in ('role', 'op', 'gid', 'qpn')}
    if any(not re.fullmatch(r'[0-9]+', value) for value in numbers.values()):
        raise ValueError('Non-numeric endpoint field')
    numbers = {key: int(value) for key, value in numbers.items()}
    qpns = fields['qpn'].split(',')
    if (numbers['v'] != 1 or fields['role'] not in ('initiator', 'responder') or fields['op'] not in OPS or
            not 0 < numbers['rkey'] <= 0xffffffff or not 0 < numbers['length'] <= 1 << 40 or
            not 0 < numbers['addr'] <= (1 << 64) - numbers['length'] or not 1 <= numbers['qps'] <= 8 or
            len(qpns) != numbers['qps'] or numbers['psn'] > 0xffffff or
            any(not re.fullmatch(r'[0-9]+', q) or not 0 < int(q) <= 0xffffff for q in qpns)):
        raise ValueError('Invalid endpoint bounds')
    gid = ipaddress.IPv6Address(fields['gid'])
    raw = gid.packed
    if not gid.is_link_local or raw[11:13] != b'\xff\xfe':
        raise ValueError('Expected a MAC-derived link-local GID')
    mac = ':'.join(f'{x:02x}' for x in bytes([raw[8] ^ 2]) + raw[9:11] + raw[13:16])
    return fields, mac


def mac_mode_markers(stderr, cq_map, user_post, user_bf, cqs, qps):
    """Confirm the provider ran the requested Mac mode for every CQ and QP.
    Raises ValueError when a mode was requested but not uniquely confirmed."""
    lines = stderr.splitlines()
    observers = [line for line in lines if line.startswith('MCDMA_CQ_OBSERVER ')]
    if cq_map not in cross.OBSERVER_MARKERS or observers != [cross.OBSERVER_MARKERS[cq_map]] * cqs:
        raise ValueError('CQ observer mode was not confirmed for every CQ: ' + repr(observers))
    if any(line.startswith('MCDMA_CQ_CONSUME retired') for line in lines):
        raise ValueError('userspace completion reporting was retired during the run')
    posts = [line for line in lines if 'MCDMA_USER_POST' in line]
    if user_post == '0':
        if posts:
            raise ValueError('Unexpected userspace posting in the kernel-posting arm')
    else:
        queues = [line for line in posts if re.fullmatch(r'MCDMA_USER_POST qp=[1-9][0-9]* queue_mapped=1 bytes=16384', line)]
        if posts.count('MCDMA_USER_POST enabled=1 uar_bytes=16384') != 1 or len(queues) != qps or len(posts) != qps + 1:
            raise ValueError('Userspace posting was requested but not confirmed for every QP')
    bfs = [line for line in lines if 'MCDMA_USER_BF' in line]
    if user_bf == '0':
        if bfs:
            raise ValueError('Unexpected userspace BlueFlame in an arm without it')
        return
    bytes_, store = cross.BF_ARMS[user_bf]
    queues = []
    for line in bfs:
        match = re.fullmatch(r'MCDMA_USER_BF qp=([1-9][0-9]*) bank_bytes=([0-9]+) bytes=([0-9]+) store=(neon|scalar|doorbell)', line)
        if not match:
            continue
        bank = int(match.group(2))
        if (bank not in (128, 256, 512, 1024) or int(match.group(3)) != bytes_ or match.group(4) != store or
                (store != 'doorbell' and bank < bytes_)):
            continue
        queues.append(line)
    if bfs.count(f'MCDMA_USER_BF mode={user_bf} uar_wc=1') != 1 or len(queues) != qps or len(bfs) != qps + 1:
        raise ValueError('Userspace BlueFlame arm was requested but not confirmed for every QP: ' + repr(bfs))


class Relay:
    """Drives one benchmark pair: forwards BW_MSG payloads, collects rows."""

    def __init__(self, endpoints, log, deadline, on_endpoints):
        self.endpoints, self.log, self.deadline, self.on_endpoints = endpoints, log, deadline, on_endpoints
        self.header = None
        self.rows = {endpoint: [] for endpoint in endpoints}
        self.errors = []
        self.done = set()
        self.exited = set()
        self.held = {}
        self.results = []

    def other(self, endpoint):
        return self.endpoints[1] if endpoint is self.endpoints[0] else self.endpoints[0]

    def handle(self, endpoint, text):
        self.log.append({'host': endpoint.host, 'response': text})
        if text.startswith('BW_MSG '):
            payload = text[len('BW_MSG '):]
            if payload.startswith('ENDPOINT '):
                self.held[endpoint] = payload
                if len(self.held) == 2:
                    self.on_endpoints(self.held)
                    for source, held in list(self.held.items()):
                        self.other(source).send(held)
                return
            self.other(endpoint).send(payload)
        elif text.startswith('BW_CSV_HEADER '):
            header = text[len('BW_CSV_HEADER '):]
            if self.header not in (None, header):
                raise RuntimeError('Endpoints disagree on the CSV header')
            self.header = header
        elif text.startswith('BW_CSV '):
            self.rows[endpoint].append(text[len('BW_CSV '):])
        elif text.startswith('BW_RESULT ') or text.startswith('BW_CLAMP ') or text.startswith('BW_FINISH '):
            self.results.append(f'{endpoint.host}: {text}')
            print(f'{endpoint.host}: {text}', flush=True)
        elif text.startswith('BW_ERROR '):
            self.errors.append(f'{endpoint.host}: {text}')
            print(f'{endpoint.host}: {text}', flush=True)
        elif text.startswith('BW_DONE '):
            self.done.add(endpoint)
            print(f'{endpoint.host}: {text}', flush=True)

    def run(self):
        with selectors.DefaultSelector() as selector:
            for endpoint in self.endpoints:
                selector.register(endpoint.process.stdout, selectors.EVENT_READ, endpoint)
            while len(self.done | self.exited) < 2:
                remaining = self.deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError('benchmark pair timeout')
                for key, _ in selector.select(min(remaining, 1.0)):
                    endpoint = key.data
                    data = os.read(endpoint.process.stdout.fileno(), 65536)
                    if not data:
                        selector.unregister(endpoint.process.stdout)
                        self.exited.add(endpoint)
                        continue
                    endpoint.pending += data
                    if len(endpoint.pending) > 1 << 20:
                        raise RuntimeError('Endpoint output exceeds protocol limit')
                    while b'\n' in endpoint.pending:
                        line, endpoint.pending = endpoint.pending.split(b'\n', 1)
                        self.handle(endpoint, line.decode(errors='replace').strip())
                if self.errors and len(self.done | self.exited) < 2:
                    # A failed side cannot recover; let the other see EOF.
                    for endpoint in self.endpoints:
                        if endpoint not in self.exited:
                            try:
                                endpoint.process.stdin.close()
                            except (OSError, ValueError):
                                pass


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def remote_sha256(run, host, path, linux):
    output = run(host, ['sha256sum', path] if linux else ['shasum', '-a', '256', path])
    digest = output.split()[0] if output.split() else ''
    if not re.fullmatch(r'[0-9a-f]{64}', digest):
        raise RuntimeError(f'{host}: cannot hash {path}')
    return digest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ['mac-host', 'peer-host', 'mac-bw', 'peer-bw', 'mac-provider', 'mac-checker',
                 'mac-interface', 'peer-interface', 'mac-device', 'peer-device']:
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--mac-gid-index', type=int, default=0)
    parser.add_argument('--peer-gid-index', type=int, default=1)
    parser.add_argument('--ops', type=lambda t: parse_list(t, 'op', allowed=OPS), default=list(OPS))
    parser.add_argument('--sizes', type=lambda t: parse_list(t, 'size', low=4096, high=16 << 20),
                        default=[4096, 65536, 1 << 20, 16 << 20], help='bytes per request, 4096..16777216')
    parser.add_argument('--depths', type=lambda t: parse_list(t, 'depth', low=1, high=64), default=[1, 16])
    parser.add_argument('--qps', type=lambda t: parse_list(t, 'qps', low=1, high=8), default=[1])
    parser.add_argument('--cq-modes', type=lambda t: parse_list(t, 'cq mode', allowed=('shared', 'per-qp')), default=['shared'])
    parser.add_argument('--initiators', type=lambda t: parse_list(t, 'initiator', allowed=INITIATORS), default=list(INITIATORS))
    parser.add_argument('--total', type=int, default=64 << 20, help='bytes per trial')
    parser.add_argument('--repeats', type=int, default=3, help='rounds; each round runs every configuration once')
    parser.add_argument('--warmup', type=int, default=1, help='unmeasured trials before each measured trial')
    parser.add_argument('--mtu', type=int, choices=[1024, 2048, 4096], default=1024)
    parser.add_argument('--finish', choices=['auto', 'flag', 'imm'], default='auto')
    parser.add_argument('--timeout', type=int, default=30, help='seconds per trial phase inside mcdma-bw')
    parser.add_argument('--verify-bytes', type=int, default=4 << 20)
    parser.add_argument('--mac-cq-map', choices=['0', '1', '2'], default='0')
    parser.add_argument('--mac-user-post', choices=['0', '1'], default='0')
    parser.add_argument('--mac-user-bf', choices=['0', '64', '128', '64s', 'db'], default='0')
    parser.add_argument('--output', type=Path, required=True, help='new directory for CSVs, logs and the manifest')
    parser.add_argument('--dry-run', action='store_true', help='print every command line; no SSH, no output directory')
    args = parser.parse_args(argv)
    if args.mac_user_post == '1' and args.mac_cq_map != '2':
        parser.error('--mac-user-post 1 requires --mac-cq-map 2')
    if args.mac_user_bf != '0' and args.mac_user_post != '1':
        parser.error('--mac-user-bf requires --mac-user-post 1')
    for value in [args.mac_interface, args.peer_interface, args.mac_device, args.peer_device]:
        if not re.fullmatch(r'[A-Za-z0-9_.:-]{1,64}', value):
            parser.error('Invalid interface/device identifier')
    if not args.mac_provider.startswith('/') or not args.mac_provider.endswith('-rdmav34.so'):
        parser.error('Use the absolute path to libmcdma-rdmav34.so')
    if not (0 <= args.mac_gid_index <= 255 and 0 <= args.peer_gid_index <= 255):
        parser.error('Invalid GID index')
    if args.total < max(args.sizes) or args.repeats < 1 or args.warmup < 0 or args.timeout < 1 or args.verify_bytes < 8:
        parser.error('total must cover the largest request; repeats >= 1; warmup >= 0; timeout >= 1')
    if args.output.exists():
        parser.error(f'refusing to overwrite existing output directory {args.output}')
    cq_modes = [1 if mode == 'per-qp' else 0 for mode in args.cq_modes]
    plan = sweep(args.ops, args.sizes, args.depths, args.qps, cq_modes, args.initiators, args.repeats)

    if args.dry_run:
        print(f'# dry run: {len(plan)} benchmark pairs, output {args.output} (not created)')
        for item in plan:
            mac, peer = build_commands(args, item)
            print(f"# round {item['round']} {config_name(item)}")
            print(ssh_line(args.mac_host, mac))
            print(ssh_line(args.peer_host, peer))
        return 0

    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / 'logs').mkdir()
    manifest = {'started': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'arguments': {k: (str(v) if isinstance(v, Path) else v) for k, v in vars(args).items()},
                'plan': [dict(item, name=config_name(item)) for item in plan],
                'runs': [], 'csv_sha256': {}, 'binaries': {}, 'errors': []}
    preflight_log = []

    def run(host, command):
        result = subprocess.run(SSH + [host, shlex.join(command)], capture_output=True, text=True, timeout=30)
        preflight_log.append({'host': host, 'command': command, 'returncode': result.returncode,
                              'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(f'{host}: {command[0]} failed: {result.stdout}{result.stderr}')
        return result.stdout.strip()

    def write_manifest():
        manifest['preflight'] = preflight_log
        manifest['finished'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')

    try:
        run(args.mac_host, [args.mac_checker, '--provider', args.mac_provider, '--require-gid'])
        base = f'/sys/class/infiniband/{args.peer_device}/ports/1/gid_attrs'
        if run(args.peer_host, ['cat', f'{base}/types/{args.peer_gid_index}']) != 'RoCE v2' or \
           run(args.peer_host, ['cat', f'{base}/ndevs/{args.peer_gid_index}']) != args.peer_interface:
            raise RuntimeError('Peer GID is not RoCE v2 on the selected spare interface')
        manifest['binaries'] = {
            'mac': {'path': args.mac_bw, 'sha256': remote_sha256(run, args.mac_host, args.mac_bw, linux=False)},
            'peer': {'path': args.peer_bw, 'sha256': remote_sha256(run, args.peer_host, args.peer_bw, linux=True)},
            'mac_provider': {'path': args.mac_provider, 'sha256': remote_sha256(run, args.mac_host, args.mac_provider, linux=False)}}
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        manifest['errors'].append(f'preflight: {error}')
        write_manifest()
        print(f'preflight failed: {error}', file=sys.stderr)
        return 1

    def check_endpoints(held):
        gids = {}
        for endpoint, payload in held.items():
            fields, mac_address = descriptor(payload)
            gids[endpoint.host] = (fields['gid'], mac_address)
        mac_gid, mac_address = gids[args.mac_host]
        peer_gid, peer_address = gids[args.peer_host]
        neighbour = run(args.mac_host, ['ndp', '-n', peer_gid + '%' + args.mac_interface]).lower()
        if peer_address not in neighbour:
            raise RuntimeError('Mac needs the peer static IPv6 neighbour before QP connection')
        neighbour = run(args.peer_host, ['ip', '-6', 'neigh', 'show', 'to', mac_gid, 'dev', args.peer_interface]).lower()
        if 'lladdr ' + mac_address not in neighbour:
            raise RuntimeError('Linux peer needs the Mac static IPv6 neighbour before QP connection')

    exit_code = 0
    try:
        for item in plan:
            name = config_name(item)
            mac_command, peer_command = build_commands(args, item)
            log, endpoints, errors, rows = [], [], [], {}
            record = {'name': name, 'round': item['round'], 'config': item, 'mac_command': mac_command,
                      'peer_command': peer_command, 'errors': errors, 'mode_confirmed': False}
            deadline = time.monotonic() + (args.warmup + 1) * args.timeout * 3 + 60
            print(f'=== round {item["round"]} {name}', flush=True)
            try:
                mac = cross.Endpoint(args.mac_host, mac_command, log)
                endpoints.append(mac)
                peer = cross.Endpoint(args.peer_host, peer_command, log)
                endpoints.append(peer)
                relay = Relay([mac, peer], log, deadline, check_endpoints)
                relay.run()
                errors.extend(relay.errors)
                rows = {('mac' if endpoint is mac else 'peer'): relay.rows[endpoint] for endpoint in endpoints}
                header = relay.header
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
                errors.append(str(error))
                header = None
            finally:
                for endpoint in endpoints:
                    try:
                        code = endpoint.stop()
                        record[f'{"mac" if endpoint.host == args.mac_host else "peer"}_returncode'] = code
                        if code:
                            errors.append(f'{endpoint.host}: exit {code}')
                    except (OSError, subprocess.SubprocessError) as error:
                        errors.append(f'{endpoint.host}: cleanup failed: {error}')
            stderr = ''.join(entry.get('endpoint_stderr', '') for entry in log if entry.get('host') == args.mac_host)
            cqs = item['qps'] if item['cq_per_qp'] else 1
            try:
                mac_mode_markers(stderr, args.mac_cq_map, args.mac_user_post, args.mac_user_bf, cqs, item['qps'])
                record['mode_confirmed'] = True
            except ValueError as error:
                errors.append(f'mac mode: {error}')
            log_path = args.output / 'logs' / f'{name}-r{item["round"]}.json'
            log_path.write_text(json.dumps({'record': record, 'log': log}, indent=2) + '\n')
            record['log'] = str(log_path)
            if header and any(rows.values()):
                csv_path = args.output / f'{name}.csv'
                new = not csv_path.exists()
                with csv_path.open('a') as handle:
                    if new:
                        handle.write(','.join(CSV_PREFIX) + ',' + header + '\n')
                    for side in ('mac', 'peer'):
                        for row in rows.get(side, []):
                            handle.write(f"{item['initiator']},{item['round']},{int(record['mode_confirmed'])},{row}\n")
                record['csv'] = str(csv_path)
            if errors:
                exit_code = 1
                print(f'--- {name} round {item["round"]} errors: {errors}', flush=True)
            manifest['runs'].append(record)
            if errors:
                break
    except KeyboardInterrupt:
        manifest['errors'].append('interrupted')
        exit_code = 1
    for csv_path in sorted(args.output.glob('*.csv')):
        manifest['csv_sha256'][csv_path.name] = sha256_file(csv_path)
    write_manifest()
    print(json.dumps({'runs': len(manifest['runs']), 'failed': sum(1 for r in manifest['runs'] if r['errors']),
                      'csv_files': len(manifest['csv_sha256']), 'output': str(args.output)}))
    return exit_code


if __name__ == '__main__':
    raise SystemExit(main())
