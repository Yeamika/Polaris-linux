#!/usr/bin/env python3
import runpy
from pathlib import Path
import unittest
import zlib
api=runpy.run_path(str(Path(__file__).resolve().parents[1]/'tools/debug.py'))
decode=api['decode_packets']

class DebugClient(unittest.TestCase):
    def test_checked_stream(self):
        data=bytes(range(32));address=0x100000000
        text=f'(bootloader) PJ4EDK2 BEGIN {address:016x} 00000020\n'
        for offset in (0,16):text+=f'(bootloader) PJ4EDK2 MEM {address+offset:016x} {data[offset:offset+16].hex()}\n'
        text+=f'(bootloader) PJ4EDK2 CRC {zlib.crc32(data):08x}\n'
        self.assertEqual(decode(text,address,32),data)
        compact=text.replace('PJ4EDK2 MEM ','PJ4EDK2 M ')
        self.assertEqual(decode(compact,address,32),data)
        with self.assertRaises(ValueError):decode(compact.replace(data[:16].hex(),data[:16].hex()[:-1]),address,32)
        for invalid in (text.replace('00000020','00000010'), text.replace('CRC','bad'),
                        text.replace(f'{address+16:016x}',f'{address:016x}'),text+text,
                        text.replace(data[:16].hex(),'ff'*16)):
            with self.assertRaises(ValueError):decode(invalid,address,32)

if __name__=='__main__':unittest.main()
