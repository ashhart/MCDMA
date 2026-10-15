#!/usr/bin/env python3 -I
"""Compare the Apple interfaces MCDMA depends on between two macOS builds.

Reads two kernelcaches (a local IMG4 file, or a Restore IPSW URL from which only the
kernelcache member is fetched with HTTP range requests), extracts IORDMAFamily, IOPCIFamily
and the kernel, and reports:

  * every symbol the built kext imports, resolved against each build's exports;
  * the exported method set of every IOKit/libkern class the kext names;
  * every IORDMAFamily call through a device-op pointer, as (load offset, PAC discriminator);
  * the sizeof(ib_device) check in _ib_alloc_device;
  * IORDMAFamily functions whose instructions differ once relocated addresses are masked
    (one .diff per function in --out).

With --userspace it also checks, on the running Mac, that /usr/lib/rdma/libibverbs.dylib
exports every private symbol the provider calls and uses the offsets the provider's
static assertions encode.

Nothing is installed or loaded. Needs Xcode's llvm-objdump, llvm-nm and llvm-cxxfilt.

usage: compare-apple-build.py --old OLD --new NEW --kext build/MCDMACX5Native.kext --out DIR [--userspace]
"""
import argparse, collections, ctypes, difflib, os, re, struct, subprocess, sys, urllib.request, zlib

ENTRIES = ('com.apple.kernel', 'com.apple.iokit.IORDMAFamily', 'com.apple.iokit.IOPCIFamily')
MEMBER = 'kernelcache.release.mac15j'


def xcrun(*args):
    return subprocess.run(['xcrun', *args], check=True, capture_output=True, text=True).stdout


def ipsw_member(url, suffix):
    def get(start, end):
        req = urllib.request.Request(url, headers={'Range': f'bytes={start}-{end}'})
        with urllib.request.urlopen(req, timeout=120) as r:
            if r.status != 206:
                sys.exit(f'{url}: server ignored the range request')
            return r.read()
    with urllib.request.urlopen(urllib.request.Request(url, method='HEAD'), timeout=60) as r:
        size = int(r.headers['Content-Length'])
    tail = get(size - 65536, size - 1)
    cd_size, cd_off = struct.unpack_from('<II', tail, tail.rfind(b'PK\x05\x06') + 12)
    loc = tail.rfind(b'PK\x06\x07')
    if loc >= 0:
        z64, = struct.unpack_from('<Q', tail, loc + 8)
        cd_size, cd_off = struct.unpack_from('<QQ', get(z64, z64 + 55), 40)
    cd, p = get(cd_off, cd_off + cd_size - 1), 0
    while cd[p:p + 4] == b'PK\x01\x02':
        method, = struct.unpack_from('<H', cd, p + 10)
        csize, usize = struct.unpack_from('<II', cd, p + 20)
        nlen, elen, clen = struct.unpack_from('<HHH', cd, p + 28)
        lho, = struct.unpack_from('<I', cd, p + 42)
        name = cd[p + 46:p + 46 + nlen].decode()
        extra, q = cd[p + 46 + nlen:p + 46 + nlen + elen], 0
        while q + 4 <= len(extra):
            tag, ln = struct.unpack_from('<HH', extra, q)
            if tag == 1:
                vals, r = [], q + 4
                for field in (usize, csize, lho):
                    if field == 0xFFFFFFFF:
                        vals.append(struct.unpack_from('<Q', extra, r)[0]); r += 8
                    else:
                        vals.append(field)
                usize, csize, lho = vals
            q += 4 + ln
        if name.endswith(suffix):
            head = get(lho, lho + 29)
            n2, e2 = struct.unpack_from('<HH', head, 26)
            data = get(lho + 30 + n2 + e2, lho + 30 + n2 + e2 + csize - 1)
            return zlib.decompress(data, -15) if method == 8 else data
        p += 46 + nlen + elen + clen
    sys.exit(f'{url}: no member ending in {suffix}')


def decode_kernelcache(blob):
    start = blob.find(b'bvx2')
    end = blob.find(b'bvx$', start)
    if start < 0 or end < 0:
        sys.exit('kernelcache has no LZFSE payload')
    lib = ctypes.CDLL('/usr/lib/libcompression.dylib')
    lib.compression_decode_buffer.restype = ctypes.c_size_t
    lib.compression_decode_buffer.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                              ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
    cap = 512 << 20
    out = ctypes.create_string_buffer(cap)
    payload = blob[start:end + 4]
    n = lib.compression_decode_buffer(out, cap, payload, len(payload), None, 0x801)  # LZFSE
    if not n:
        sys.exit('LZFSE decode failed')
    return bytearray(out.raw[:n])


def write_entries(kc, prefix):
    """Fileset entries keep collection-relative file offsets, so each output is the whole
    collection with that entry's header and load commands copied over offset 0."""
    _, _, _, _, ncmds, _, _, _ = struct.unpack_from('<IiiIIIII', kc, 0)
    entries, first, off = {}, len(kc), 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', kc, off)
        if cmd == 0x80000035:  # LC_FILESET_ENTRY
            _, fileoff, name_off = struct.unpack_from('<QQI', kc, off + 8)
            entries[kc[off + name_off:kc.index(b'\0', off + name_off)].decode()] = fileoff
            first = min(first, fileoff)
        off += size
    paths = {}
    for name in ENTRIES:
        start = entries[name]
        sizeofcmds, = struct.unpack_from('<I', kc, start + 20)
        header = bytes(kc[start:start + 32 + sizeofcmds])
        if len(header) > first:
            sys.exit(f'{name}: header does not fit before the first entry')
        image = bytearray(kc)
        image[0:len(header)] = header
        paths[name] = f'{prefix}-{name}.macho'
        open(paths[name], 'wb').write(image)
    return paths


def load_build(spec, out, label):
    blob = ipsw_member(spec, MEMBER) if re.match(r'https://', spec) else open(spec, 'rb').read()
    return write_entries(decode_kernelcache(blob), os.path.join(out, label))


def exports(paths):
    names = set()
    for p in paths.values():
        names.update(l.split()[-1] for l in xcrun('llvm-nm', '--defined-only', '--extern-only', p).splitlines() if l.strip())
    return names


def normalize(path):
    """Per-function instruction text with relocated addresses and global low-12 offsets masked."""
    funcs, name, body, pages = {}, None, [], set()
    for line in xcrun('llvm-objdump', '--macho', '-d', '--no-show-raw-insn', '--print-imm-hex', path).splitlines():
        if re.match(r'^[^\s0-9a-f/][^\t]*:$', line):
            if name:
                funcs[name] = body
            name, body, pages = line[:-1], [], set()
            continue
        parts = line.split('\t')
        if name is None or len(parts) < 2:
            continue
        mnem, args = parts[1], (parts[2] if len(parts) > 2 else '')
        args = re.sub(r'\s*;.*$', '', args)
        args = re.sub(r'0x[0-9a-f]{9,}', 'ADDR', args)
        dest = args.split(',')[0].strip()
        if mnem in ('adrp', 'adr'):
            text = f'{mnem} {dest}, PAGE'
            pages.add(dest)
        else:
            for reg in list(pages):
                args = re.sub(rf'\[{reg}, #-?0x[0-9a-f]+\]', f'[{reg}, LO12]', args)
                if mnem == 'add' and re.search(rf',\s*{reg},\s*#', args):
                    args = re.sub(r'#-?0x[0-9a-f]+$', 'LO12', args)
            text = f'{mnem} {args}'.strip()
            if mnem.startswith('bl'):
                pages = {r for r in pages if re.fullmatch(r'x(19|2[0-8])', r)}
            elif dest in pages and not (mnem == 'add' and f'{dest}, {dest},' in args) \
                    and not mnem.startswith(('st', 'cmp', 'cb', 'tb', 'b', 'ret')):
                pages.discard(dest)
        body.append(text)
    if name:
        funcs[name] = body
    return funcs


def callbacks(funcs):
    """(load offset, discriminator) for every blraa through x17 after a pointer load."""
    seen = collections.Counter()
    for body in funcs.values():
        loads, disc = {}, None
        for ins in body:
            m = re.match(r'ldr (x\d+), \[x\d+, (#0x[0-9a-f]+)\]$', ins)
            if m:
                loads[m.group(1)] = m.group(2)
            m = re.match(r'mov x17, (#0x[0-9a-f]+)$', ins)
            if m:
                disc = m.group(1)
            m = re.match(r'blraa (x\d+), x17$', ins)
            if m and m.group(1) in loads:
                seen[(loads[m.group(1)], disc)] += 1
    return seen


def ib_device_size(funcs):
    body = funcs.get('__ib_alloc_device', [])
    return next((m.group(1) for ins in body for m in [re.match(r'cmp x0, #(0x[0-9a-f]+)$', ins)] if m), None)


def userspace():
    lib = '/usr/lib/rdma/libibverbs.dylib'
    names = {l.split()[-1] for l in subprocess.run(['dyld_info', '-exports', lib], capture_output=True, text=True).stdout.splitlines() if l.strip().startswith('0x')}
    needed = ['verbs_register_driver_34', '_verbs_init_and_alloc_context', 'verbs_uninit_context', 'verbs_set_ops',
              'ibv_cmd_get_context', 'ibv_cmd_query_device_any', 'ibv_cmd_query_port', 'ibv_cmd_alloc_pd',
              'ibv_cmd_dealloc_pd', 'ibv_cmd_create_cq', 'ibv_cmd_destroy_cq', 'ibv_cmd_create_qp',
              'ibv_cmd_modify_qp', 'ibv_cmd_destroy_qp', 'ibv_cmd_reg_mr', 'ibv_cmd_dereg_mr', 'ibv_cmd_poll_cq',
              'ibv_cmd_post_send', 'ibv_cmd_post_recv', 'darwin_mmap', 'darwin_munmap']
    missing = [n for n in needed if '_' + n not in names]
    text = subprocess.run(['dyld_info', '-disassemble', lib], capture_output=True, text=True).stdout
    def fn(name):
        m = re.search(rf'^{re.escape(name)}:\n(.*?)(?=^\S[^\n]*:\n)', text, re.M | re.S)
        return m.group(1) if m else ''
    # Each provider static assertion in native/user_provider.c, and where libibverbs uses it.
    checks = [('match kind 0x14', '_try_driver', r'ldrb\s+w\d+, \[x\d+, #0x14\]'),
              ('ops->alloc_device 0x38', '_try_driver', r'ldr\s+x\d+, \[x\d+, #0x38\]'),
              ('device->ops 0x298', '_try_driver', r'str\s+x\d+, \[x\d+, #0x298\]'),
              ('device->sysfs 0x2b8', '_try_driver', r'str\s+x\d+, \[x\d+, #0x2b8\]'),
              ('ops->alloc_context 0x28', '_verbs_open_device', r'ldr\s+x\d+, \[x\d+, #0x298\]\s*\n\S+\s+ldr\s+x\d+, \[x\d+, #0x28\]'),
              ('verbs_context.context 0x140', '_verbs_open_device', r'add\s+x\d+, x\d+, #0x140'),
              ('verbs_mr.mr_type 0x30', '_ibv_cmd_reg_mr', r'stp\s+wzr, w\d+, \[x\d+, #0x30\]')]
    rows = [(label, bool(re.search(pattern, fn(func)))) for label, func, pattern in checks]
    return missing, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--old', required=True, help='validated build: kernelcache path or Restore IPSW URL')
    ap.add_argument('--new', required=True, help='candidate build: kernelcache path or Restore IPSW URL')
    ap.add_argument('--kext', required=True, help='built MCDMACX5Native.kext')
    ap.add_argument('--out', required=True)
    ap.add_argument('--userspace', action='store_true', help='also check libibverbs on this Mac')
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out, 'function-diffs'), exist_ok=True)
    old, new = load_build(a.old, a.out, 'old'), load_build(a.new, a.out, 'new')
    problems = []

    binary = os.path.join(a.kext, 'Contents', 'MacOS', 'MCDMACX5Native')
    imports = {l.split()[-1] for l in xcrun('llvm-nm', '-u', binary).splitlines() if l.strip()}
    eo, en = exports(old), exports(new)
    for label, ex in (('old', eo), ('new', en)):
        missing = sorted(imports - ex)
        print(f'kext imports unresolved on {label}: {len(missing)} of {len(imports)}')
        problems += [f'unresolved on {label}: {s}' for s in missing]
    print(f'kernel/family exports: removed {len(eo - en)}, added {len(en - eo)}')
    problems += [f'export removed: {s}' for s in sorted(eo - en)]

    classes = sorted({m.group(1) for s in imports for m in [re.match(r'__ZNK?\d+([A-Za-z]+)', s)] if m})
    for c in classes:
        pat = re.compile(rf'^__ZNK?{len(c)}{c}\d')
        mo, mn = {s for s in eo if pat.match(s)}, {s for s in en if pat.match(s)}
        if mo != mn:
            problems.append(f'{c}: methods differ ({len(mo - mn)} removed, {len(mn - mo)} added)')
    print(f'IOKit/libkern classes compared: {len(classes)}')

    fo = normalize(old['com.apple.iokit.IORDMAFamily'])
    fn = normalize(new['com.apple.iokit.IORDMAFamily'])
    so, sn = ib_device_size(fo), ib_device_size(fn)
    print(f'_ib_alloc_device requires size > {so} (old), > {sn} (new)')
    if so != sn or so is None:
        problems.append(f'sizeof(ib_device) check changed: {so} -> {sn}')
    co, cn = callbacks(fo), callbacks(fn)
    print(f'device-op call sites: {sum(co.values())} old, {sum(cn.values())} new')
    for key in sorted(set(co) | set(cn), key=str):
        if co[key] != cn[key]:
            print(f'  call site count differs at {key[0]} disc {key[1]}: {co[key]} -> {cn[key]}')

    changed = []
    for name in sorted(set(fo) & set(fn)):
        if fo[name] == fn[name]:
            continue
        diff = list(difflib.unified_diff(fo[name], fn[name], 'old', 'new', lineterm='', n=3))
        body = [l for l in diff if l[:1] in '+-' and not l.startswith(('+++', '---'))]
        lines_only = all(re.match(r'[-+]mov w\d+, #0x[0-9a-f]+$', l) for l in body)
        safe = re.sub(r'[^A-Za-z0-9_.]', '_', name)[:150]
        open(os.path.join(a.out, 'function-diffs', safe + '.diff'), 'w').write('\n'.join(diff) + '\n')
        changed.append((len(body), name, lines_only))
    print(f'IORDMAFamily functions: {len(set(fo) & set(fn))} common, {len(changed)} differ, '
          f'{sum(1 for c in changed if c[2])} of those only in line-number constants')
    print(f'  only old: {sorted(set(fo) - set(fn))}\n  only new: {sorted(set(fn) - set(fo))}')
    for n, name, lines_only in sorted(changed):
        if not lines_only:
            print(f'  review: {name} ({n} changed instructions)')

    if a.userspace:
        missing, rows = userspace()
        print(f'libibverbs private exports missing: {missing or "none"}')
        for label, ok in rows:
            print(f'  {"ok  " if ok else "FAIL"} {label}')
            if not ok:
                problems.append(f'libibverbs offset not found: {label}')
        problems += [f'libibverbs export missing: {m}' for m in missing]

    print('\nRESULT:', 'no mechanical incompatibility found; review the listed functions' if not problems else 'PROBLEMS')
    for p in problems:
        print('  ' + p)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
