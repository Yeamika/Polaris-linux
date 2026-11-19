#!/usr/bin/env python3
"""Exercise early dualboot packaging with an assembled selector and native payload."""
import gzip
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT/'tools'))
import android_dualboot as dual


class DualbootTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        header = bytearray(4096)
        header[:8] = b'ANDROID!'
        raw = bytearray(bytes(range(256))*16)
        struct.pack_into('<I', raw, 0, 0x14000040)
        struct.pack_into('<QQQ', raw, 8, 0x80000, 0x300000, 10)
        raw[56:60] = b'ARM\x64'
        dtb = b'\xd0\x0d\xfe\xed' + struct.pack('>I', 40) + bytes(32)
        kernel = gzip.compress(raw, mtime=0)+dtb*2
        cls.ramdisk = b'unchanged native ramdisk'
        struct.pack_into('<9I', header, 8, len(kernel), 0x8000, len(cls.ramdisk), 0x1000000,
                         0, 0xf00000, 0x100, 4096, 1)
        header[64:80] = b'native.argument\0'
        cls.original = bytes(header)+kernel+bytes(-len(kernel)%4096)
        cls.original += cls.ramdisk+bytes(-len(cls.ramdisk)%4096)+b'opaque vendor trailer\x01'+bytes(4096)
        cls.fd = bytes(range(256))*(dual.FD_SIZE//256)
        cls.work = tempfile.TemporaryDirectory(prefix='android-dualboot-')
        work = Path(cls.work.name)
        info = dual.layout(cls.original, cls.fd)
        defines = {'SHIM_OFFSET': info['shim_offset'], 'KERNEL_ENTRY': info['entry_offset'],
                   'ANDROID_BOOT_SIZE': info['boot_size'], 'ANDROID_BOOT_CRC': info['boot_crc32'],
                   'PACKED_IMAGE_SIZE': info['image_size']}
        subprocess.run(['aarch64-linux-gnu-gcc', '-c', str(PROJECT/'platform/AndroidBootShim.S'),
                        '-o', str(work/'shim.o'), *[f'-D{k}={v}' for k,v in defines.items()]], check=True)
        subprocess.run(['aarch64-linux-gnu-ld', '-Ttext=0', '--entry=_Start',
                        '-o', str(work/'shim.elf'), str(work/'shim.o')], check=True)
        subprocess.run(['aarch64-linux-gnu-objcopy', '-O', 'binary', str(work/'shim.elf'), str(work/'shim.bin')], check=True)
        relocations = subprocess.check_output(['aarch64-linux-gnu-readelf','-r',str(work/'shim.elf')], text=True)
        assert 'There are no relocations' in relocations
        cls.shim = (work/'shim.bin').read_bytes()
        cls.packed, cls.receipt = dual.pack(cls.original, cls.fd, cls.shim)

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def test_preserve_native_payload(self):
        checked = dual.verify(self.packed, self.fd, self.original)
        self.assertEqual(checked['appended_dtbs'], 2)
        self.assertEqual(dual.parse_boot(self.packed)['ramdisk'], self.ramdisk)
        self.assertEqual(checked['entry_offset'], 256)
        self.assertEqual(checked['shim_offset'], 0x400000)
        self.assertEqual(len(self.shim), 4096)

    def test_mismatched_original(self):
        changed = bytearray(self.original);changed[-1] = 1
        with self.assertRaisesRegex(ValueError, 'binding mismatch'):
            dual.verify(self.packed, self.fd, changed)
        with self.assertRaisesRegex(ValueError, 'BootShim does not match'):
            dual.pack(changed, self.fd, self.shim)

    def test_corrupt_ramdisk(self):
        boot = dual.parse_boot(self.packed)
        offset = boot['page']+dual.align(len(boot['kernel']),boot['page'])
        changed = bytearray(self.packed);changed[offset] ^= 1
        with self.assertRaisesRegex(ValueError, 'SHA1 mismatch'):
            dual.verify(changed, self.fd)

    def test_corrupt_fd(self):
        changed = bytearray(self.fd);changed[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'FD mismatch'):
            dual.verify(self.packed, changed)

    def test_truncated_boot(self):
        with self.assertRaises(ValueError):
            dual.layout(self.original[:4096], self.fd)

    def test_auxiliary_payload_rejected(self):
        changed = bytearray(self.original);struct.pack_into('<I',changed,1632,4096)
        with self.assertRaisesRegex(ValueError, 'DTBO'):
            dual.layout(changed, self.fd)

    def test_branch_range_and_dt_bounds(self):
        with self.assertRaisesRegex(ValueError, 'B instruction'):
            dual.branch_target(0xd503201f)
        self.assertEqual(dual.branch_target(0x17ffffff),-4)
        raw = dual.unpack_kernel(dual.parse_boot(self.original)['kernel'])[0]
        with self.assertRaisesRegex(ValueError, 'truncated appended DTB'):
            dual.unpack_kernel(gzip.compress(raw)+b'\xd0\x0d\xfe\xed'+struct.pack('>I',4096)+bytes(32))


if __name__ == '__main__':
    unittest.main()
