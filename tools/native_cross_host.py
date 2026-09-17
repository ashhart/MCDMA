#!/usr/bin/env python3
"""Native Apple verbs <-> Linux verbs, with SSH for descriptors/results only.

Requires a loaded native CX5 provider and configured link-local addresses and
static neighbours on the two spare QSFP interfaces. Changes no network settings.
"""
import argparse
import csv
import hashlib
import io
import ipaddress
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import subprocess
import time
import threading


class Endpoint:
    STDERR_LIMIT = 1024 * 1024

    def __init__(self, host, command, log):
        self.host, self.log, self.pending = host, log, b''
        self.process = subprocess.Popen(
            ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', host, shlex.join(command)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

        self.stderr_data = bytearray()
        self.stderr_overflow = False
        self.stderr_error = None
        self.stderr_stop = threading.Event()
        os.set_blocking(self.process.stderr.fileno(), False)
        self.stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self.stderr_thread.start()

    def _drain_stderr(self):
        # Drain throughout descriptor exchange and peer work, not just shutdown:
        # a full diagnostic pipe must never stop an endpoint before its reply.
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(self.process.stderr, selectors.EVENT_READ)
                while True:
                    events = selector.select(0.1)
                    if not events:
                        if self.stderr_stop.is_set():
                            return
                        continue
                    try:
                        data = os.read(self.process.stderr.fileno(), 4096)
                    except BlockingIOError:
                        continue
                    if not data:
                        return
                    available = self.STDERR_LIMIT - len(self.stderr_data)
                    self.stderr_data.extend(data[:available])
                    self.stderr_overflow |= len(data) > available
        except (OSError, ValueError) as error:
            self.stderr_error = str(error)

    def line(self, timeout=30):
        deadline = time.monotonic() + timeout
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            while b'\n' not in self.pending:
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not selector.select(remaining):
                    raise RuntimeError(f'{self.host}: response timeout')
                data = os.read(self.process.stdout.fileno(), 4096)
                if not data:
                    raise RuntimeError(f'{self.host}: endpoint exited before a complete response')
                self.pending += data
                if len(self.pending) > 65536:
                    raise RuntimeError('Endpoint line exceeds protocol limit')
        line, self.pending = self.pending.split(b'\n', 1)
        text = line.decode().strip()
        self.log.append({'host': self.host, 'response': text})
        print(f'{self.host}: {text}', flush=True)
        return text

    def send(self, text):
        self.process.stdin.write((text+'\n').encode())
        self.process.stdin.flush()

    def stop(self):
        try:
            self.process.stdin.close()
        except BrokenPipeError:
            pass
        try:
            code = self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
            code = -1
        # Preserve CQ/posting diagnostics even when the endpoint returned a
        # well-formed failure response before exiting.
        self.stderr_stop.set()
        self.stderr_thread.join(timeout=2)
        if self.stderr_thread.is_alive() or self.stderr_error or self.stderr_overflow:
            self.log.append({'host': self.host, 'stderr_capture_error':
                             self.stderr_error or ('limit exceeded' if self.stderr_overflow else 'reader did not stop')})
            code = -1
        stderr = bytes(self.stderr_data).decode(errors='replace')
        if not self.stderr_thread.is_alive():
            self.process.stderr.close()
        self.process.stdout.close()
        if stderr:
            self.log.append({'host': self.host, 'endpoint_stderr': stderr})
            print(f'{self.host}: {stderr.strip()}', flush=True)
        return code


OBSERVER_MARKERS = {
    '0': 'MCDMA_CQ_OBSERVER mapped=0 reason=disabled',
    '1': 'MCDMA_CQ_OBSERVER mapped=1 bytes=16384 readonly=1',
    '2': 'MCDMA_CQ_OBSERVER mapped=1 bytes=16384 readonly=1 consume=user',
}


def observer_marker(log, host, requested):
    # This protocol creates exactly one Mac CQ. Reject fallback, a missing or
    # duplicate marker, and contradictory modes instead of relabelling a run.
    # In consume mode the provider must also never have retired the CQ back to
    # kernel polling, or the run measured a different mechanism than claimed.
    lines = [line for entry in log if entry.get('host') == host
             for line in entry.get('endpoint_stderr', '').splitlines()]
    markers = [line for line in lines if line.startswith('MCDMA_CQ_OBSERVER ')]
    if requested not in OBSERVER_MARKERS or markers != [OBSERVER_MARKERS[requested]]:
        raise ValueError('CQ observer mode was not uniquely confirmed by the provider: '
                         + repr(markers))
    retired = [line for line in lines if line.startswith('MCDMA_CQ_CONSUME retired')]
    if retired:
        raise ValueError('userspace completion reporting was retired during the run: ' + repr(retired))
    return requested in ('1', '2')


def user_post_marker(log, host, requested):
    lines = [line for item in log if item.get('host') == host
             for line in item.get('endpoint_stderr', '').splitlines()
             if 'MCDMA_USER_POST' in line]
    if requested == '0':
        if lines:
            raise ValueError('Unexpected userspace posting in the kernel-posting arm')
        return False
    enabled = 'MCDMA_USER_POST enabled=1 uar_bytes=16384'
    queues = [line for line in lines if re.fullmatch(
        r'MCDMA_USER_POST qp=[1-9][0-9]* queue_mapped=1 bytes=16384', line)]
    # This runner opens one context and one QP. Fallback, duplicate markers,
    # or success reported by the other host cannot certify this test arm.
    if lines.count(enabled) != 1 or len(queues) != 1 or len(lines) != 2:
        raise ValueError('Userspace posting was requested but not uniquely confirmed')
    return True


BF_ARMS = {'64': (64, 'neon'), '128': (128, 'scalar'), '64s': (64, 'scalar'), 'db': (0, 'doorbell')}


def user_bf_marker(log, host, requested):
    lines = [line for item in log if item.get('host') == host
             for line in item.get('endpoint_stderr', '').splitlines()
             if 'MCDMA_USER_BF' in line]
    if requested == '0':
        if lines:
            raise ValueError('Unexpected userspace BlueFlame in an arm without it')
        return False
    if requested not in BF_ARMS:
        raise ValueError('Unknown userspace BlueFlame arm')
    bytes_, store = BF_ARMS[requested]
    context = f'MCDMA_USER_BF mode={requested} uar_wc=1'
    queues = []
    for line in lines:
        match = re.fullmatch(r'MCDMA_USER_BF qp=([1-9][0-9]*) bank_bytes=([0-9]+) bytes=([0-9]+) store=(neon|scalar|doorbell)', line)
        if not match:
            continue
        bank = int(match.group(2))
        # The kernel publishes a bank only for a write-combined page; a push
        # arm needs a bank at least as large as its write, a doorbell arm none.
        if (bank not in (128, 256, 512, 1024) or int(match.group(3)) != bytes_ or match.group(4) != store or
                (store != 'doorbell' and bank < bytes_)):
            continue
        queues.append(line)
    # One context, one QP: the write-combined page and the exact arm must be
    # confirmed exactly once each, with no fallback or foreign line.
    if lines.count(context) != 1 or len(queues) != 1 or len(lines) != 2:
        raise ValueError('Userspace BlueFlame arm was requested but not uniquely confirmed: ' + repr(lines))
    return True


def descriptor(line):
    words = line.split()
    if len(words) != 7 or words[0] != 'ENDPOINT':
        raise ValueError('Missing native verbs endpoint')
    qpn, psn, rkey, address, length = map(int, words[1:6])
    gid = ipaddress.IPv6Address(words[6])
    if not (0 < qpn <= 0xffffff and 0 <= psn <= 0xffffff and 0 < rkey <= 0xffffffff
            and 0 < address <= (1 << 64)-length and length == 16384 and gid.is_link_local):
        raise ValueError('Invalid native verbs endpoint bounds')
    # Our initial native implementation uses the physical MAC-derived EUI-64.
    raw = gid.packed
    if raw[11:13] != b'\xff\xfe':
        raise ValueError('Expected a MAC-derived link-local GID')
    mac = ':'.join(f'{x:02x}' for x in bytes([raw[8]^2])+raw[9:11]+raw[13:16])
    return words[1:], mac


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['mac-host', 'peer-host', 'mac-client', 'peer-client', 'mac-provider',
                 'mac-checker', 'mac-interface', 'peer-interface', 'mac-device', 'peer-device']:
        parser.add_argument('--'+name, required=True)
    parser.add_argument('--peer-gid-index', type=int, default=1)
    parser.add_argument('--reverse-only', action='store_true',
                        help='Isolate Linux peer WRITE/READ while the native Mac application waits idle')
    parser.add_argument('--mac-latency', action='store_true',
                        help='After Mac WRITE/READ pass, time 1000 Mac WRITEs and READs each')
    parser.add_argument('--peer-latency', action='store_true',
                        help='After both directions pass, time 1000 Linux peer WRITEs and READs each')
    parser.add_argument('--mac-qos', choices=['inherit', 'initiated', 'interactive'], default='inherit',
                        help='Request a QoS class only for the Mac benchmark thread')
    parser.add_argument('--mac-profile', action='store_true',
                        help='Add post/wait durations and poll counts to the Mac CSV; adds one timestamp per sample')
    parser.add_argument('--payload-bytes', type=int, choices=[1024,4096], default=4096)
    parser.add_argument('--mac-cq-map', choices=['0','1','2'], default='0',
                        help='Enable candidate CQ observation; requires an explicit successful mapping marker')
    parser.add_argument('--mac-user-post', choices=['0','1'], default='0',
                        help='Require direct userspace posting; mode 1 requires --mac-cq-map 2')
    parser.add_argument('--mac-user-bf', choices=['0','64','128','64s','db'], default='0',
                        help='Require a userspace BlueFlame arm (write-combined UAR page); requires --mac-user-post 1')
    parser.add_argument('--path-mtu', type=int, choices=[1024,4096], default=1024)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.mac_user_post == '1' and args.mac_cq_map != '2':
        parser.error('--mac-user-post 1 requires --mac-cq-map 2')
    if args.mac_user_bf != '0' and args.mac_user_post != '1':
        parser.error('--mac-user-bf requires --mac-user-post 1')
    if (args.peer_latency or args.mac_latency) and args.reverse_only:
        parser.error('Latency measurement requires the full bidirectional test')
    if args.mac_profile and not args.mac_latency:
        parser.error('Mac profiling requires --mac-latency')
    for value in [args.mac_interface, args.peer_interface, args.mac_device, args.peer_device]:
        if not re.fullmatch(r'[A-Za-z0-9_.:-]{1,64}', value):
            parser.error('Invalid interface/device identifier')
    if not args.mac_provider.startswith('/') or not args.mac_provider.endswith('-rdmav34.so'):
        parser.error('Use the absolute path to libmcdma-rdmav34.so')
    if not 0 <= args.peer_gid_index <= 255:
        parser.error('Invalid peer GID index')
    log, endpoints, errors = [], [], []
    passed = False

    def run(host, command):
        result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', host,
                                 shlex.join(command)], capture_output=True, text=True, timeout=30)
        log.append({'host': host, 'command': command, 'returncode': result.returncode,
                    'stdout': result.stdout, 'stderr': result.stderr})
        if result.returncode:
            raise RuntimeError(f'{host}: {command[0]} failed: {result.stdout}{result.stderr}')
        return result.stdout.strip()

    def capture_latency(endpoint, host, label):
        for operation in ['write', 'read']:
            if not endpoint.line().startswith('LATENCY op='+operation+' '):
                raise RuntimeError('Missing peer latency summary')
        trace = endpoint.line()
        if not re.fullmatch(r'LATENCY_TRACE /tmp/mcdma-cx5-latency-[A-Za-z0-9]+', trace):
            raise RuntimeError('Invalid peer latency trace path')
        contents = run(host, ['cat', trace.split()[1]])+'\n'
        rows = list(csv.DictReader(io.StringIO(contents)))
        if len(rows) != 2000:
            raise RuntimeError('Incomplete peer latency trace')
        for index, row in enumerate(rows):
            operation = 'write' if index < 1000 else 'read'
            if (row.get('operation') != operation or row.get('bytes') != str(args.payload_bytes) or
                    row.get('sample') != str(index % 1000) or
                    not row.get('completion_ns', '').isdigit() or int(row['completion_ns']) <= 0):
                raise RuntimeError('Malformed peer latency sample')
            if label == 'mac' and args.mac_profile:
                if (not all(row.get(k, '').isdigit() for k in ['post_ns', 'completion_wait_ns', 'poll_calls']) or
                        int(row['post_ns']) + int(row['completion_wait_ns']) != int(row['completion_ns']) or
                        int(row['poll_calls']) < 1):
                    raise RuntimeError('Malformed Mac timing decomposition')
        destination = args.output.with_suffix('.'+label+'-latency.csv')
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(contents)
        log.append({'latency_trace': str(destination),
                    'sha256': hashlib.sha256(contents.encode()).hexdigest(),
                    'measurement': label+' submission-to-local-CQ completion; qd=1; 100 warmup per operation; not one-way time'})

    try:
        # A physical ACTIVE port can still have no usable Apple GID cache entry.
        run(args.mac_host, [args.mac_checker, '--provider', args.mac_provider, '--require-gid'])
        base = f'/sys/class/infiniband/{args.peer_device}/ports/1/gid_attrs'
        if run(args.peer_host, ['cat', f'{base}/types/{args.peer_gid_index}']) != 'RoCE v2' or \
           run(args.peer_host, ['cat', f'{base}/ndevs/{args.peer_gid_index}']) != args.peer_interface:
            raise RuntimeError('Peer GID is not RoCE v2 on the selected spare interface')
        mac = Endpoint(args.mac_host, ['env', 'IBV_DRIVERS='+args.mac_provider[:-len('-rdmav34.so')],
                                      'MCDMA_PAYLOAD_BYTES='+str(args.payload_bytes),
                                      'MCDMA_PATH_MTU='+str(args.path_mtu),
                                      'MCDMA_QOS='+args.mac_qos,
                                      'MCDMA_CQ_MAP='+args.mac_cq_map,
                                      'MCDMA_USER_POST='+args.mac_user_post,
                                      'MCDMA_USER_BF='+args.mac_user_bf,
                                      'MCDMA_LATENCY_PROFILE='+('1' if args.mac_profile else '0'),
                                      args.mac_client, args.mac_device, '0',
                                      'responder' if args.reverse_only else 'initiator'], log)
        endpoints.append(mac)
        spark = Endpoint(args.peer_host, ['env', 'MCDMA_PAYLOAD_BYTES='+str(args.payload_bytes),
                                         'MCDMA_PATH_MTU='+str(args.path_mtu),
                                         args.peer_client, args.peer_device, str(args.peer_gid_index)], log)
        endpoints.append(spark)
        local, mac_address = descriptor(mac.line())
        remote, spark_address = descriptor(spark.line())
        neighbour = run(args.mac_host, ['ndp', '-n', remote[5]+'%'+args.mac_interface]).lower()
        # macOS ndp prints MAC octets without leading zeros (30:c5:99:3f:14:d); normalise both sides.
        _norm = lambda m: ':'.join(o.lstrip('0') or '0' for o in m.split(':'))
        if _norm(spark_address) not in ' '.join(_norm(t) if t.count(':')==5 else t for t in neighbour.split()):
            raise RuntimeError('Mac needs the peer static IPv6 neighbour before QP connection')
        neighbour = run(args.peer_host, ['ip', '-6', 'neigh', 'show', 'to', local[5], 'dev', args.peer_interface]).lower()
        if 'lladdr '+mac_address not in neighbour:
            raise RuntimeError('Linux peer needs the Mac static IPv6 neighbour before QP connection')
        mac.send(' '.join([remote[0], remote[1], remote[5]]))
        spark.send(' '.join([local[0], local[1], local[5]]))
        if mac.line() != 'READY' or spark.line() != 'READY':
            raise RuntimeError('Both native QPs must reach RTS')
        if not args.reverse_only:
            mac.send(('INITIATEBENCH ' if args.mac_latency else 'INITIATE ')+' '.join(remote[2:5]))
            if mac.line() != f'NATIVE_FORWARD write=1 read=1 verified={args.payload_bytes}':
                # Diagnose actual delivery independently of a Mac CQ failure;
                # a matching remote buffer never turns this into a pass.
                spark.send('VERIFY')
                spark.line()
                raise RuntimeError('Native Mac-initiated WRITE/READ failed')
            if args.mac_latency:
                capture_latency(mac, args.mac_host, 'mac')
        mode = 'REVERSE' if args.reverse_only else 'ROUNDTRIPBENCH' if args.peer_latency else 'ROUNDTRIP'
        spark.send(mode+' '+' '.join(local[2:5]))
        expected_forward = 0 if args.reverse_only else args.payload_bytes
        if spark.line() != f'PEER_RESULT forward={expected_forward} write=1 read=1 reverse={args.payload_bytes}':
            raise RuntimeError('Linux peer-initiated WRITE/READ or payload verification failed')
        if args.peer_latency:
            capture_latency(spark, args.peer_host, 'spark')
        mac.send('CHECKREVERSE')
        if mac.line() != f'NATIVE_REVERSE verified={args.payload_bytes}':
            raise RuntimeError('Mac did not observe the Linux peer payload')
        passed = True
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        errors.append(str(error))
    finally:
        for endpoint in endpoints:
            try:
                code = endpoint.stop()
                if code:
                    errors.append(f'{endpoint.host}: endpoint cleanup exit {code}')
            except (OSError, subprocess.SubprocessError) as error:
                errors.append(f'{endpoint.host}: cleanup failed: {error}')
        observed_mapping = False
        confirmed_marker = None
        confirmed_user_post = False
        confirmed_user_bf = False
        try:
            observed_mapping = observer_marker(log, args.mac_host, args.mac_cq_map)
            confirmed_marker = OBSERVER_MARKERS[args.mac_cq_map]
            confirmed_user_post = user_post_marker(log, args.mac_host, args.mac_user_post)
            confirmed_user_bf = user_bf_marker(log, args.mac_host, args.mac_user_bf)
        except ValueError as error:
            errors.append(str(error))
        result = {'native_verbs': True, 'payload_bytes_each_operation': args.payload_bytes,
                  'path_mtu_bytes': args.path_mtu, 'mac_cq_map_requested': args.mac_cq_map,
                  'mac_cq_mapping_observed': observed_mapping,
                  'mac_cq_observer_marker': confirmed_marker,
                  'mac_user_post_requested': args.mac_user_post,
                  'mac_user_post_confirmed': confirmed_user_post,
                  'mac_user_bf_requested': args.mac_user_bf,
                  'mac_user_bf_confirmed': confirmed_user_bf,
                  'mac_qos_request': args.mac_qos, 'mac_latency_profile': args.mac_profile,
                  'operations': (['spark_write', 'spark_read'] if args.reverse_only else
                                 ['mac_write', 'mac_read', 'spark_write', 'spark_read']),
                  'test_passed': passed and not errors, 'errors': errors, 'log': log}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps({k: v for k, v in result.items() if k != 'log'}))
    return 0 if result['test_passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
