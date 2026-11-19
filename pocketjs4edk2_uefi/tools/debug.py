#!/usr/bin/env python3
"""Polaris fastboot debugger: checked RAM reads, maps, logs and returning EFI probes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import zlib


def decode_packets(text, address, size):
    begin=re.findall(r'PJ4EDK2 BEGIN ([0-9a-fA-F]{16}) ([0-9a-fA-F]{8})',text)
    if [(int(a,16),int(n,16)) for a,n in begin]!=[(address,size)]:
        raise ValueError('missing or mismatched RAM read header')
    rows=re.findall(r'PJ4EDK2 (?:M|MEM) ([0-9a-fA-F]{16}) ([0-9a-fA-F]{32})',text)
    if len(rows)!=size//16 or any(int(a,16)!=address+i*16 for i,(a,_) in enumerate(rows)):
        raise ValueError('missing, duplicated or out-of-order RAM data')
    data=b''.join(bytes.fromhex(row) for _,row in rows)
    crc=re.findall(r'PJ4EDK2 CRC ([0-9a-fA-F]{8})',text)
    if len(crc)!=1 or int(crc[0],16)!=zlib.crc32(data):
        raise ValueError('RAM read checksum mismatch')
    return data


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fastboot',default='fastboot')
    p.add_argument('--serial',default='polaris-uefi')
    commands=p.add_subparsers(dest='command',required=True)
    commands.add_parser('info');commands.add_parser('map');commands.add_parser('log')
    read=commands.add_parser('read')
    read.add_argument('--address',type=lambda x:int(x,0),required=True)
    read.add_argument('--size',type=lambda x:int(x,0),required=True)
    read.add_argument('--output',type=Path,required=True)
    probe=commands.add_parser('probe')
    probe.add_argument('--efi',type=Path,required=True)
    probe.add_argument('--cycles',type=int,default=3)
    hang=commands.add_parser('watchdog-test',help='deliberately hang a RAM EFI; expect reset')
    hang.add_argument('--efi',type=Path,required=True)
    a=p.parse_args()
    def run(*args,timeout=30):
        result=subprocess.run([a.fastboot,'-s',a.serial,*map(str,args)],capture_output=True,text=True,timeout=timeout)
        text=result.stdout+result.stderr
        if result.returncode: raise RuntimeError(text)
        return text
    def wait(seconds):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            try:
                output=run('getvar','polaris-boot-format',timeout=3)
                if 'efi-aa64' in output: return
            except (RuntimeError,subprocess.TimeoutExpired): pass
            time.sleep(.5)
        raise TimeoutError('Polaris UEFI fastboot did not return')
    if a.command=='info':
        for name in ('product','version-bootloader','polaris-debugger','polaris-flash-format','max-download-size'):
            print(run('getvar',name),end='')
    elif a.command in ('map','log'):
        print(run('oem','memory-map' if a.command=='map' else 'log'),end='')
    elif a.command=='read':
        if a.address<0 or a.size<=0 or (a.address|a.size)&15 or a.address+a.size>1<<64:
            p.error('choose a positive 16-byte-aligned RAM range')
        if a.output.exists(): p.error('output already exists')
        # Bound host memory; firmware independently validates each RAM window.
        if a.size>16*1024*1024:p.error('read at most 16 MiB per invocation')
        data=bytearray();records=[]
        for offset in range(0,a.size,65536):
            size=min(65536,a.size-offset);address=a.address+offset
            text=run('oem','read-ram',f'{address:x}',f'{size:x}',timeout=60)
            data.extend(decode_packets(text,address,size));records.append(text)
        a.output.parent.mkdir(parents=True,exist_ok=True)
        a.output.write_bytes(data)
        a.output.with_suffix(a.output.suffix+'.json').write_text(json.dumps({
            'address':hex(a.address),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
            'crc32':f'{zlib.crc32(data):08x}','chunks_verified':len(records)},indent=2)+'\n')
        a.output.with_suffix(a.output.suffix+'.log').write_text('\n'.join(records))
        print(f'{len(data)} bytes; every chunk CRC verified')
    elif a.command=='probe':
        if not 1<=a.cycles<=20:p.error('choose 1..20 cycles')
        for index in range(a.cycles):
            print(run('stage',a.efi,'oem','boot-efi'),end='');wait(20)
            print(f'EFI return cycle {index+1} passed')
    elif a.command=='watchdog-test':
        started=time.monotonic()
        print(run('stage',a.efi,'oem','boot-efi'),end='')
        # A fastboot OKAY only acknowledges launch; absence and return must follow.
        time.sleep(2)
        try: run('getvar','product',timeout=2)
        except (RuntimeError,subprocess.TimeoutExpired): pass
        else: raise RuntimeError('hang probe did not suspend service')
        wait(60)
        elapsed=time.monotonic()-started
        if not 20<=elapsed<=65:raise RuntimeError(f'unexpected watchdog interval {elapsed:.1f}s')
        print(f'recovery returned after {elapsed:.1f}s; check firmware log for reset evidence')


if __name__=='__main__':main()
