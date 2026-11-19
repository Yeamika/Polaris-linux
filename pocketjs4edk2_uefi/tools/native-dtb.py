#!/usr/bin/env python3
"""Reconstruct an Android /proc/device-tree tar using matching boot DT reservations.

Prefer an original /sys/firmware/fdt capture when readable. The filesystem view
does not export the FDT header's reservation table; --boot-image supplies it
from the unique appended tree with the same model and Qualcomm IDs. The caller
must verify that this reservation table was not changed by the native loader.
No device access or partition writes are performed here.
"""
import argparse
import hashlib
from pathlib import Path, PurePosixPath
import struct
import tarfile
import zlib


def parse(blob):
    if len(blob) < 40 or blob[:4] != b'\xd0\x0d\xfe\xed':
        raise ValueError('invalid FDT header')
    total, off, strings, reserve, version, last, cpu, slen, nlen = struct.unpack_from('>9I', blob, 4)
    if total != len(blob) or version != 17 or last > 17 or strings+slen > total or off+nlen > total:
        raise ValueError('invalid FDT bounds/version')
    props, stack = {}, []
    end = off+nlen
    while off+4 <= end:
        token, = struct.unpack_from('>I', blob, off)
        off += 4
        if token == 1:
            stop = blob.index(0, off, end)
            stack.append(blob[off:stop].decode('ascii'))
            off = (stop+4) & ~3
        elif token == 2:
            stack.pop()
        elif token == 3:
            size, name = struct.unpack_from('>II', blob, off)
            off += 8
            if off+size > end or name >= slen:
                raise ValueError('invalid FDT property bounds')
            stop = blob.index(0, strings+name, strings+slen)
            key = '/'.join(stack[1:]+[blob[strings+name:stop].decode('ascii')])
            props[key] = blob[off:off+size]
            off = (off+size+3) & ~3
        elif token == 9:
            break
        elif token != 4:
            raise ValueError('invalid FDT token')
    else:
        raise ValueError('missing FDT end token')
    reservations = []
    while reserve+16 <= total:
        address, size = struct.unpack_from('>QQ', blob, reserve)
        reserve += 16
        if not address and not size:
            return props, reservations, cpu
        reservations.append((address, size))
    raise ValueError('unterminated reservation table')


def encode(props, reservations, cpu):
    tree = {'props': {}, 'nodes': {}}
    for path, value in sorted(props.items()):
        parts = path.split('/')
        node = tree
        for part in parts[:-1]:
            node = node['nodes'].setdefault(part, {'props': {}, 'nodes': {}})
        node['props'][parts[-1]] = value
    names = sorted(set(path.split('/')[-1] for path in props))
    strings, offsets = bytearray(), {}
    for name in names:
        offsets[name] = len(strings)
        strings += name.encode('ascii')+b'\0'
    structure = bytearray()

    def pad():
        structure.extend(b'\0'*(-len(structure) % 4))

    def emit(name, node):
        structure.extend(struct.pack('>I', 1)+name.encode('ascii')+b'\0')
        pad()
        for key, value in node['props'].items():
            structure.extend(struct.pack('>III', 3, len(value), offsets[key])+value)
            pad()
        for key, child in node['nodes'].items():
            emit(key, child)
        structure.extend(struct.pack('>I', 2))
    emit('', tree)
    structure += struct.pack('>I', 9)
    table = b''.join(struct.pack('>QQ', *item) for item in reservations+[(0, 0)])
    off = 40+len(table)
    strings_at = off+len(structure)
    return struct.pack('>10I', 0xd00dfeed, strings_at+len(strings), off, strings_at,
                       40, 17, 16, cpu, len(strings), len(structure))+table+structure+strings


def reconstruct(archive, boot):
    props = {}
    with tarfile.open(archive) as stream:
        for item in stream:
            if not item.isfile():
                continue
            path = PurePosixPath(item.name)
            if path.is_absolute() or '..' in path.parts or item.size > 1024*1024:
                raise ValueError('invalid device-tree archive member')
            key = path.as_posix().removeprefix('./')
            if key in props:
                raise ValueError('duplicate property')
            props[key] = stream.extractfile(item).read()
    keys = ('model', 'qcom,board-id', 'qcom,msm-id')
    if any(key not in props for key in (*keys, 'memory/reg', 'chosen/bootargs')):
        raise ValueError('incomplete native snapshot')
    data = Path(boot).read_bytes()
    if len(data) < 1648 or data[:8] != b'ANDROID!':
        raise ValueError('not an Android boot image')
    size, = struct.unpack_from('<I', data, 8)
    page, = struct.unpack_from('<I', data, 36)
    if page < 2048 or page & (page-1) or page+size > len(data):
        raise ValueError('invalid boot kernel bounds')
    zipped = zlib.decompressobj(31)
    zipped.decompress(data[page:page+size])
    if not zipped.eof:
        raise ValueError('incomplete compressed kernel')
    appended, matches, pos = zipped.unused_data, [], 0
    while pos < len(appended):
        start = appended.find(b'\xd0\x0d\xfe\xed', pos)
        if start < 0:
            break
        total, = struct.unpack_from('>I', appended, start+4)
        if total < 40 or start+total > len(appended):
            raise ValueError('invalid appended FDT size')
        seed, reservations, cpu = parse(appended[start:start+total])
        if all(seed.get(key) == props[key] for key in keys):
            matches.append((reservations, cpu))
        pos = start+total
    if len(matches) != 1:
        raise ValueError('need exactly one matching boot DT reservation table')
    blob = encode(props, *matches[0])
    if parse(blob)[0] != props:
        raise ValueError('FDT property round trip mismatch')
    return blob, len(props), matches[0][0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--boot-image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    blob, count, reservations = reconstruct(args.archive, args.boot_image)
    args.output.write_bytes(blob)
    print(f'properties={count} reservations={len(reservations)} bytes={len(blob)} sha256={hashlib.sha256(blob).hexdigest()}')


if __name__ == '__main__':
    main()
