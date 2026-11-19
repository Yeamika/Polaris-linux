#!/usr/bin/env python3
"""Package a preserved Polaris Android kernel with an early UEFI selector."""
import gzip
import hashlib
import struct
import zlib

LIMIT = 64 * 1024 * 1024
FD_SIZE = 0x700000
SHIM_SIZE = 4096
MANIFEST = struct.Struct('<8sIIQIHHQQQQ')
MARKER = b'polaris.android_entry=early-bootshim'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def align(value, size):
    return (value + size - 1) & -size


def branch_target(instruction):
    require(instruction & 0xfc000000 == 0x14000000, 'Image entry must be an ARM64 B instruction')
    offset = (instruction & 0x3ffffff) << 2
    return offset - (1 << 28) if offset & (1 << 27) else offset


def parse_boot(data):
    require(1648 <= len(data) <= LIMIT and data[:8] == b'ANDROID!', 'invalid Android boot image')
    kernel_size, _, ramdisk_size, _, second, _, _, page, version = struct.unpack_from('<9I', data, 8)
    require(page in (2048, 4096, 8192, 16384) and version == 1, 'need legacy Polaris boot header v1')
    require(kernel_size and ramdisk_size and not second, 'need kernel/ramdisk without second stage')
    require(data[1632:1644] == bytes(12) and struct.unpack_from('<I', data, 1644)[0] in (0, 1648),
            'recovery DTBO or unknown boot header layout')
    ramdisk_start = page + align(kernel_size, page)
    end = ramdisk_start + align(ramdisk_size, page)
    require(end <= len(data), 'truncated Android payload')
    return {'header': data[:page], 'kernel': data[page:page+kernel_size],
            'ramdisk': data[ramdisk_start:ramdisk_start+ramdisk_size], 'page': page,
            # Some original full-partition backups contain opaque bytes after
            # their declared payloads. Keep them; only trim terminal zero fill.
            'trailer': data[end:].rstrip(b'\0')}


def unpack_kernel(kernel):
    decoder = zlib.decompressobj(31)
    raw = decoder.decompress(kernel, LIMIT + 1)
    require(decoder.eof and not decoder.unconsumed_tail and len(raw) <= LIMIT,
            'invalid or oversized gzip Image')
    dtbs = decoder.unused_data
    cursor = count = 0
    while cursor < len(dtbs):
        require(len(dtbs)-cursor >= 40 and dtbs[cursor:cursor+4] == b'\xd0\x0d\xfe\xed',
                'invalid kernel-appended DT stream')
        size = struct.unpack_from('>I', dtbs, cursor+4)[0]
        require(40 <= size <= len(dtbs)-cursor, 'truncated appended DTB')
        cursor += size
        count += 1
        while cursor < len(dtbs) and dtbs[cursor] == 0:
            cursor += 1
    require(count and len(raw) >= 64 and raw[56:60] == b'ARM\x64', 'missing ARM64 Image or DTBs')
    return raw, dtbs, count


def layout(original, fd):
    boot = parse_boot(original)
    raw, _, _ = unpack_kernel(boot['kernel'])
    entry = branch_target(struct.unpack_from('<I', raw)[0])
    footprint, flags = struct.unpack_from('<QQ', raw, 16)
    require(64 <= entry < len(raw) and flags in (2, 10), 'unsupported native Image entry/flags')
    require(len(raw) <= footprint <= LIMIT, 'invalid native Image memory footprint')
    require(len(fd) == FD_SIZE, 'unexpected UEFI FD size')
    shim_offset = align(footprint, 0x200000)
    image_size = shim_offset + SHIM_SIZE + len(fd)
    require(image_size <= LIMIT and shim_offset < 1 << 27, 'dualboot Image exceeds bounds')
    return {'shim_offset': shim_offset, 'entry_offset': entry, 'image_size': image_size,
            'boot_size': len(original), 'boot_crc32': zlib.crc32(original)}


def manifest_bytes(info):
    return MANIFEST.pack(b'POLANDR1', 1, 64, info['boot_size'], info['boot_crc32'],
                         0x5241, 128, 0x17fe00000, info['entry_offset'], info['image_size'], 0)


def pack(original, fd, shim):
    info = layout(original, fd)
    require(len(shim) == SHIM_SIZE and shim.count(manifest_bytes(info)) == 1,
            'BootShim does not match the Android image/layout')
    boot = parse_boot(original)
    raw, dtbs, count = unpack_kernel(boot['kernel'])
    image = bytearray(raw)
    struct.pack_into('<I', image, 0, 0x14000000 | (info['shim_offset'] >> 2))
    struct.pack_into('<Q', image, 16, info['image_size'])
    # Keep the native page-size flag; ask ABL for the lowest suitable address.
    struct.pack_into('<Q', image, 24, struct.unpack_from('<Q', raw, 24)[0] & ~8)
    image.extend(bytes(info['shim_offset'] - len(image)))
    image.extend(shim)
    image.extend(fd)
    kernel = gzip.compress(bytes(image), mtime=0) + dtbs
    header = bytearray(boot['header'])
    command = (header[64:576] + header[608:1632]).split(b'\0', 1)[0]
    require(MARKER not in command, 'input already contains a dualboot marker')
    command += b' ' + MARKER
    require(len(command) < 1536, 'Android command line has no room for provenance marker')
    command = command.ljust(1536, b'\0')
    header[64:576], header[608:1632] = command[:512], command[512:]
    struct.pack_into('<I', header, 8, len(kernel))
    struct.pack_into('<I', header, 1644, 1648)
    ramdisk = boot['ramdisk']
    header[576:608] = hashlib.sha1(kernel + struct.pack('<I', len(kernel)) + ramdisk +
                                  struct.pack('<III', len(ramdisk), 0, 0)).digest() + bytes(12)
    page = boot['page']
    result = bytes(header) + kernel + bytes(-len(kernel) % page)
    result += ramdisk + bytes(-len(ramdisk) % page) + boot['trailer']
    result += bytes(-len(result) % page)
    require(len(result) < LIMIT, 'dualboot boot image exceeds partition size')
    verify(result, fd, original)
    return result, dict(info, original_sha256=hashlib.sha256(original).hexdigest(),
                        native_kernel_sha256=hashlib.sha256(raw).hexdigest(), appended_dtbs=count,
                        original_trailer_bytes=len(boot['trailer']))


def verify(data, fd, original=None):
    boot = parse_boot(data)
    raw, dtbs, count = unpack_kernel(boot['kernel'])
    offset = branch_target(struct.unpack_from('<I', raw)[0])
    require(offset >= 0x200000 and offset % 0x200000 == 0, 'invalid selector offset')
    require(len(fd) == FD_SIZE and len(raw) == offset + SHIM_SIZE + len(fd), 'invalid dualboot layout')
    require(raw[offset+SHIM_SIZE:] == fd, 'dualboot FD mismatch')
    footprint, flags = struct.unpack_from('<QQ', raw, 16)
    require(footprint == len(raw) and flags == 2, 'invalid dualboot Image header')
    shim = raw[offset:offset+SHIM_SIZE]
    require(shim.count(b'POLANDR1') == 1, 'missing/ambiguous Android binding')
    position = shim.index(b'POLANDR1')
    require(position+MANIFEST.size <= len(shim), 'truncated Android binding')
    magic, rev, size, boot_size, crc, value, number, address, entry, total, reserved = MANIFEST.unpack_from(shim, position)
    require(magic == b'POLANDR1' and (rev, size, value, number, address, total, reserved) ==
            (1, 64, 0x5241, 128, 0x17fe00000, len(raw), 0), 'invalid Android binding fields')
    require(1648 <= boot_size <= LIMIT and 64 <= entry < offset, 'invalid original Android bounds')
    require(MARKER in (boot['header'][64:576]+boot['header'][608:1632]).split(b'\0', 1)[0],
            'missing early-boot provenance marker')
    digest = hashlib.sha1(boot['kernel'] + struct.pack('<I', len(boot['kernel'])) + boot['ramdisk'] +
                          struct.pack('<III', len(boot['ramdisk']), 0, 0)).digest()
    require(boot['header'][576:608] == digest+bytes(12), 'dualboot boot header SHA1 mismatch')
    if original is not None:
        require((len(original), zlib.crc32(original)) == (boot_size, crc), 'original Android binding mismatch')
        old = parse_boot(original)
        native, tail, _ = unpack_kernel(old['kernel'])
        expected = bytearray(native)
        struct.pack_into('<I', expected, 0, 0x14000000 | (offset >> 2))
        struct.pack_into('<QQ', expected, 16, len(raw), 2)
        require(raw[:len(native)] == expected and not any(raw[len(native):offset]),
                'native kernel changed beyond entry/size/placement header fields')
        require(dtbs == tail and boot['ramdisk'] == old['ramdisk'] and boot['trailer'] == old['trailer'],
                'native DT stream, ramdisk or opaque trailer changed')
        require(entry == branch_target(struct.unpack_from('<I', native)[0]), 'native branch target changed')
    return {'mode': 'early-android-warm-reset', 'appended_dtbs': count,
            'boot_size': boot_size, 'boot_crc32': f'{crc:08x}', 'entry_offset': entry,
            'shim_offset': offset, 'image_size': len(raw)}
