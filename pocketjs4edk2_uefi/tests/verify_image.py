#!/usr/bin/env python3
"""Check delivered identities, boot-wrapper structure and uncompressed build FV applications."""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import struct
import uuid
import zlib
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
import android_dualboot

APPS = {
    'Windows': '99636716-0e77-4799-bd99-6b91c23c426e',
    'Android': '7bcfd833-0bbc-452b-b420-7fbf0da7972c',
    'LinuxBoot': '85a6b469-768b-45aa-90f8-9e22db414ae1',
    'Recovery': 'bbb59c78-c763-47aa-91b1-b746f025182e',
}
DRIVERS = {
    'PolarisRotatorDxe': 'a9bc49db-1706-4b48-b8ad-880acfe93509',
    'KernelErrataPatcher': '964efa94-9929-4ef8-9e68-8f6f6fa62e63',
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]


def pe(data, subsystem=10):
    require(len(data) >= 64 and data[:2] == b'MZ', 'missing PE header')
    offset = u32(data, 60)
    require(offset+112 <= len(data) and data[offset:offset+4] == b'PE\0\0', 'invalid PE offset')
    require(struct.unpack_from('<H', data, offset+4)[0] == 0xaa64, 'not AArch64')
    require(struct.unpack_from('<H', data, offset+24)[0] == 0x20b, 'not PE32+')
    require(struct.unpack_from('<H', data, offset+92)[0] == subsystem, 'unexpected EFI subsystem')


def android_direct(data):
    pe(data)
    require('Android: direct ROM kernel and ramdisk from boot.img'.encode('utf-16le') in data,
            'Android EFI is missing the direct ROM loader')
    require('matching early BootShim is not installed'.encode('utf-16le') not in data and
            'PMIC warm reset'.encode('utf-16le') not in data,
            'Android EFI still uses the firmware-bound warm-reset loader')


def verify(output, fv=None):
    output = output.resolve()
    record = json.loads((output/'firmware-manifest.json').read_text())
    require(record['board'] == 'polaris' and record['stage'] == 'all', 'need complete Polaris build')
    for name, identity in record['artifacts'].items():
        path = (output/name).resolve()
        require(output in path.parents, 'artifact escapes output')
        data = path.read_bytes()
        require(len(data) == identity['bytes'] and hashlib.sha256(data).hexdigest() == identity['sha256'],
                'identity mismatch: '+name)
    for name in APPS:
        pe((output/'ESP/EFI'/name/'BOOTAA64.EFI').read_bytes())
    android_direct((output/'ESP/EFI/Android/BOOTAA64.EFI').read_bytes())
    pe((output/'tools/return-probe.efi').read_bytes())
    fd = (output/'polaris-uefi.fd').read_bytes()
    require(len(fd) == 0x700000, 'unexpected firmware region size')
    boot = (output/'polaris-uefi-recovery.img').read_bytes()
    require(boot[:8] == b'ANDROID!' and len(boot) < 64*1024*1024, 'invalid boot wrapper')
    size, ramdisk, second, page, version = (u32(boot, n) for n in (8, 16, 24, 36, 40))
    require(page in (2048, 4096, 8192, 16384) and version == 1, 'unsupported boot header')
    if record.get('android', {}).get('dualboot'):
        info = android_dualboot.verify(boot, fd)
        expected = record['android']['dualboot']
        for key in ('shim_offset', 'entry_offset', 'image_size', 'boot_size', 'appended_dtbs'):
            require(info[key] == expected[key], 'dualboot manifest mismatch: '+key)
        require(int(info['boot_crc32'],16) == expected['boot_crc32'], 'Android CRC binding mismatch')
    else:
        require(ramdisk == second == u32(boot, 1632) == 0, 'unexpected auxiliary payload')
        require(u32(boot, 1644) == 1648 and len(boot) == page+((size+page-1)//page)*page,
                'invalid boot sizes/alignment')
        kernel = boot[page:page+size]
        digest = hashlib.sha1(kernel+struct.pack('<I', size)+b'\0'*12).digest()
        require(boot[576:596] == digest, 'boot header SHA1 mismatch')
        gzip = zlib.decompressobj(16+zlib.MAX_WBITS)
        payload = gzip.decompress(kernel, 8*1024*1024)
        require(gzip.eof and not gzip.unconsumed_tail and payload.endswith(fd), 'BootShim/FD gzip mismatch')
        shim = payload[:-len(fd)]
        require(64 <= len(shim) <= 4096 and shim[56:60] == b'ARM\x64', 'invalid ARM64 BootShim')
        dtb = gzip.unused_data
        require(len(dtb) >= 40 and dtb[:4] == b'\xd0\x0d\xfe\xed', 'missing compatibility DTB')
        require(struct.unpack_from('>I', dtb, 4)[0] == len(dtb), 'truncated compatibility DTB')
    embedded = []
    if fv:
        data = fv.read_bytes()
        require(data[40:44] == b'_FVH', 'invalid FV')
        # Verify the actually delivered compressed firmware, not only build FV.
        compressed_guid=uuid.UUID('9e21fd93-9c72-4c15-8c4b-e77f1db2d792').bytes_le
        file=fd.find(compressed_guid)
        require(file>=0 and fd.find(compressed_guid,file+1)<0, 'missing/ambiguous compressed FV file')
        section=file+24
        length=int.from_bytes(fd[section:section+3],'little')
        require(fd[section+3]==2 and fd[section+4:section+20]==uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le,
                'expected LZMA guided FV section')
        start=struct.unpack_from('<H',fd,section+20)[0]
        unpacked=lzma.decompress(fd[section+start:section+length],format=lzma.FORMAT_ALONE)
        header=8 if unpacked[:3]==b'\xff'*3 else 4
        require(unpacked[3]==0x17 and unpacked[header:]==data, 'delivered FD contains stale/different FV')
        offset = struct.unpack_from('<H', data, 48)[0]
        wanted = {uuid.UUID(guid).bytes_le: name for name, guid in APPS.items()}
        wanted.update({uuid.UUID(guid).bytes_le: name for name, guid in DRIVERS.items()})
        if record['startup']=='pocketjs':
            wanted[uuid.UUID('edb979d9-310e-4145-950e-ac1691a1010a').bytes_le]='PocketJS'
            pe((output/'ESP/EFI/PocketJS/BOOTAA64.EFI').read_bytes())
        while offset+24 <= len(data):
            offset = (offset+7) & ~7
            if data[offset:offset+24] == b'\xff'*24:
                break
            guid = data[offset:offset+16]
            length = int.from_bytes(data[offset+20:offset+23], 'little')
            require(24 <= length <= len(data)-offset, 'invalid FFS length')
            if guid in wanted:
                driver = wanted[guid] in DRIVERS
                require(data[offset+18] == (7 if driver else 9), 'unexpected embedded FFS type')
                section = offset+24
                found = False
                while section+4 <= offset+length:
                    size = int.from_bytes(data[section:section+3], 'little')
                    require(4 <= size <= offset+length-section, 'invalid section length')
                    if data[section+3] == 0x10:
                        image = data[section+4:section+size]
                        pe(image, 11 if driver else 10)
                        if wanted[guid] == 'Android':
                            android_direct(image)
                        # Platform builds select their own library instances, so
                        # compare with the platform PE, not the standalone DSC.
                        platform_image = (output/'ESP/EFI/PocketJS/BOOTAA64.EFI' if wanted[guid]=='PocketJS'
                                          else fv.parent.parent/'AARCH64'/(wanted[guid]+'.efi'))
                        require(image == platform_image.read_bytes(),
                                'embedded/platform EFI differ: '+wanted[guid])
                        found = True
                    section = (section+size+3) & ~3
                require(found, 'missing embedded PE32 section')
                embedded.append(wanted[guid])
            offset += length
        require(set(embedded) == set(wanted.values()), 'missing built-in EFI applications/drivers')
        if record.get('entry_diagnostics'):
            bds = (fv.parent.parent/'AARCH64/BdsDxe.efi').read_bytes()
            for text in ('POLARIS_EARLY_ENTRY', 'POLARIS_ENTRY_DIAG', 'no automatic Shell fallback', 'polaris-'+record['source_id']):
                require(text.encode('utf-16le') in bds, 'missing BDS diagnostic identity: '+text)
    return {'passed': True, 'boot_bytes': len(boot), 'fd_bytes': len(fd),
            'startup': record['startup'], 'embedded_applications': sorted(set(embedded)-set(DRIVERS)),
            'embedded_drivers': sorted(set(embedded)&set(DRIVERS)), 'hardware_tested': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--fv', type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.output, args.fv), indent=2))


if __name__ == '__main__':
    main()
