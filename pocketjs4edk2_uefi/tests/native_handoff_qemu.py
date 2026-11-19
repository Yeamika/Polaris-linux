#!/usr/bin/env python3
"""Run actual AArch64 native entry at EL1 and EL2; QEMU is not an SDM845 model."""
import argparse
from pathlib import Path
import subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--edk2',type=Path,required=True)
p.add_argument('--qemu',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
root=Path(__file__).resolve().parent
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
edk=a.edk2.resolve()
inc=[edk/'ArmPkg/Include',edk/'MdePkg/Include',edk/'MdePkg/Include/AArch64']
objects=[]
for source,name in [(root/'native_handoff_qemu.S','harness'),(root.parent/'efi/LinuxBoot/NativeEnterKernel.S','entry')]:
 obj=out/(name+'.o')
 subprocess.run(['aarch64-linux-gnu-gcc','-DASM_PFX(x)=x','-c','-ffreestanding','-nostdlib',*[arg for d in inc for arg in ['-I',str(d)]],str(source),'-o',str(obj)],check=True)
 objects.append(str(obj))
link=out/'link.ld'
link.write_text('ENTRY(_start)\nSECTIONS { . = 0x40080000; .text : { *(.text*) } .rodata : { *(.rodata*) } .data : { *(.data*) } .bss : { *(.bss*) } /DISCARD/ : { *(.comment*) *(.note*) } }\n')
subprocess.run(['aarch64-linux-gnu-ld','-T',str(link),*objects,'-o',str(out/'handoff.elf')],check=True)
subprocess.run(['aarch64-linux-gnu-objcopy','-O','binary',str(out/'handoff.elf'),str(out/'handoff.bin')],check=True)
for mode in ('off','on'):
 result=subprocess.run([str(a.qemu.resolve()),'-machine','virt,virtualization='+mode,'-cpu','cortex-a57','-m','128M','-nographic','-monitor','none','-nic','none','-no-reboot','-semihosting-config','enable=on,target=native','-kernel',str(out/'handoff.bin')],capture_output=True,text=True,timeout=10)
 log=result.stdout+result.stderr
 (out/('el'+('2' if mode=='on' else '1')+'.log')).write_text(log)
 print('virtualization='+mode,log,flush=True)
 if result.returncode or 'PASS:' not in log or 'FAIL:' in log:
  raise SystemExit('AArch64 handoff validation failed')
