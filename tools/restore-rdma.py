#!/usr/bin/env python3
"""Restore a chosen MCDMA interface's temporary GID and static peer neighbor."""
import argparse
import ipaddress
import os
from pathlib import Path
import platform
import re
import shlex
import subprocess


def mac_bytes(value):
    if not re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', value):
        raise ValueError('Expected a six-byte colon-separated MAC address')
    result = bytes.fromhex(value.replace(':', ''))
    if result[0] & 1 or result == bytes(6):
        raise ValueError('Expected a nonzero unicast MAC address')
    return result


def link_local(mac):
    b = mac_bytes(mac)
    eui = bytes([b[0] ^ 2]) + b[1:3] + b'\xff\xfe' + b[3:]
    return str(ipaddress.IPv6Address(bytes.fromhex('fe80000000000000') + eui))


def plan(interface, expected_mac, peer_gid, peer_mac):
    if not re.fullmatch(r'mcrdma[0-9]{1,3}', interface):
        raise ValueError('Select an MCDMA mcrdma interface, not a management interface')
    local = link_local(expected_mac)
    # The current transport requires MAC-derived link-local GIDs.
    peer = ipaddress.IPv6Address(peer_gid)
    if '%' in peer_gid or str(peer) != link_local(peer_mac):
        raise ValueError('Peer GID must be its MAC-derived link-local address without a zone')
    return [
        ['/sbin/ifconfig', interface, 'inet6', local, 'prefixlen', '64', 'alias'],
        ['/sbin/ifconfig', interface, 'up'],
        ['/usr/sbin/ndp', '-s', str(peer) + '%' + interface, peer_mac.lower()],
    ]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('interface', 'expected-mac', 'peer-gid', 'peer-mac'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--checker', type=Path, default=Path(__file__).resolve().parents[1] / 'build/cx5-native-check')
    parser.add_argument('--dry-run', action='store_true', help='Print the validated plan; do not inspect or change the host')
    args = parser.parse_args(argv)
    try:
        commands = plan(args.interface, args.expected_mac, args.peer_gid, args.peer_mac)
    except ValueError as error:
        parser.error(str(error))
    provider = Path('/usr/local/lib/rdma/libmcdma-rdmav34.so')
    commands.append([str(args.checker.resolve()), '--provider', str(provider), '--require-gid'])
    for command in commands:
        print(shlex.join(command), flush=True)
    if args.dry_run:
        return 0
    if platform.system() != 'Darwin' or os.geteuid() != 0:
        parser.error('Run on the target Mac with sudo, or use --dry-run')
    build = subprocess.check_output(['/usr/bin/sw_vers', '-buildVersion'], text=True).strip()
    if build not in ('26A428', '26B5101f'):  # keep in step with include/apple_build_ids.h
        parser.error('This setup procedure is validated only for builds 26A428 and 26B5101f')
    loaded = subprocess.check_output(['/usr/bin/kmutil', 'showloaded', '--list-only', '--variant-suffix', 'release'], text=True)
    if not re.search(r'org\.mcdma\.cx5\.native\s+\(0\.1\.18\)', loaded):
        parser.error('Expected the 0.1.18 native driver to be loaded')
    current = subprocess.check_output(['/sbin/ifconfig', args.interface], text=True)
    actual = re.search(r'\bether\s+([0-9a-fA-F:]{17})\b', current)
    if not actual or actual.group(1).lower() != args.expected_mac.lower():
        parser.error('Interface MAC does not match; inspect enumeration after the reboot')
    if not args.checker.is_file() or not os.access(args.checker, os.X_OK) or not provider.is_file():
        parser.error('Build the checker and install the provider first')
    for command in commands:
        subprocess.run(command, check=True)
    print('Discovery/GID check passed; run the separate byte-verifying transfer check next.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
