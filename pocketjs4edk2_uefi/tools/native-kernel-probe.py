#!/usr/bin/env python3
"""Bind a diagnostic EFI to an exact kernel checkpoint; never alter input files."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
p=argparse.ArgumentParser(description=__doc__)
p.add_argument("--efi",type=Path,required=True)
p.add_argument("--image",type=Path,required=True)
p.add_argument("--symbols",type=Path,required=True)
p.add_argument("--stage",choices=("header","primary_entry","__cpu_setup","__primary_switch",
    "__primary_switched","start_kernel","setup_arch","console_init","sched_init","smp_init",
    "kernel_init","kernel_init_freeable","run_init_process","panic","rest_init",
    "parse_early_param","setup_log_buf","paging_init","arm64_memblock_init",
    "do_el0_svc","__arm64_sys_mount","__arm64_sys_reboot"),required=True)
p.add_argument("--output",type=Path,required=True)
p.add_argument("--image-output",type=Path)
a=p.parse_args()
raw=a.image.read_bytes();pe=a.efi.read_bytes()
assert raw[56:60]==b"ARMd" and pe[:2]==b"MZ"
symbols={}
for line in a.symbols.read_text().splitlines():
    fields=line.split()
    if len(fields)==3:
        symbols[fields[2]]=int(fields[0],16)
offset=0 if a.stage=="header" else symbols[a.stage]-symbols["_text"]
assert offset%4==0 and 0<=offset<=len(raw)-32
magic=b"POLARIS-PROBE01\0"
pos=pe.find(magic)
assert pos>=0 and pe.find(magic,pos+1)<0
layout=struct.Struct("<16sIIQQQ32s32s32s")
old=layout.unpack_from(pe,pos)
assert old[1:3]==(1,144)
reset=old[7]
assert struct.unpack("<8I",reset)==(0x52800120,0x72b08000,0xaa1f03e1,0xaa1f03e2,
    0xaa1f03e3,0xd4000003,0xd503207f,0x17ffffff)
memory=struct.unpack_from("<Q",raw,16)[0]
config=layout.pack(magic,1,144,len(raw),memory,offset,raw[offset:offset+32],
    reset,a.stage.encode().ljust(32,b"\0"))
output=pe[:pos]+config+pe[pos+layout.size:]
assert not a.output.exists()
a.output.parent.mkdir(parents=True,exist_ok=True)
a.output.write_bytes(output)
record={"stage":a.stage,"offset":hex(offset),"original":raw[offset:offset+32].hex(),
    "image_sha256":hashlib.sha256(raw).hexdigest(),
    "base_efi_sha256":hashlib.sha256(pe).hexdigest(),
    "efi_sha256":hashlib.sha256(output).hexdigest(),"intentional_reset":True}
if a.image_output:
    assert not a.image_output.exists()
    patched=raw[:offset]+reset+raw[offset+32:]
    a.image_output.write_bytes(patched)
    record["probe_image_sha256"]=hashlib.sha256(patched).hexdigest()
a.output.with_suffix(a.output.suffix+".json").write_text(json.dumps(record,indent=2)+"\n")
print(json.dumps(record,indent=2))
