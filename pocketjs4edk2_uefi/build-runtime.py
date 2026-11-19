#!/usr/bin/env python3
"""Build the real shared PJ4EDK2 runtime as a board-configured FV application."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--edk2',type=Path,required=True)
p.add_argument('--workdir',type=Path,required=True)
p.add_argument('--source-cache',type=Path,required=True)
p.add_argument('--jobs',type=int,default=4)
a=p.parse_args()
project=Path(__file__).resolve().parent
repo=(project.parent/'components/pj4edk2').resolve()
work=a.workdir.resolve();work.mkdir(parents=True,exist_ok=True)
edk=a.edk2.resolve()
spec=importlib.util.spec_from_file_location('firmware_build',project/'build.py')
b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
env=dict(os.environ,RUSTUP_TOOLCHAIN='1.93.0',PJB_DEFAULT_UI=str(repo/'default-ui'),
         CARGO_TARGET_DIR=str(work/'cargo-target'),PJB_BUILD_DIR=str(work),PJB_ARCHIVE=str(work/'Rust.a'))
logs=work/'logs';logs.mkdir(exist_ok=True)
# Reuse pinned upstreams and build caches, but refresh our source snapshot.
if not (work/'source-paths.json').exists():
    b.command([sys.executable,repo/'tools/upstream.py','prepare','--repo',repo,
               '--edk2',edk,'--output',work,'--source-cache',a.source_cache],env,logs/'prepare.log')
paths=json.loads((work/'source-paths.json').read_text())
source=Path(paths['source_tree'])
if source.resolve() != work/'source-tree' or source.resolve() == repo:
    raise ValueError('Runtime source snapshot must be inside the build directory')
for name in ('pocketjsforedk2','default-ui','uefi-runtime','crates','tools'):
    destination=source/name
    if destination.exists(): shutil.rmtree(destination)
    shutil.copytree(repo/name,destination,symlinks=True,
                    ignore=shutil.ignore_patterns('.git','target','__pycache__','*.pyc','.venv-docs'))
shutil.copy2(repo/'rust-toolchain.toml',source/'rust-toolchain.toml')
env['PJB_DEFAULT_UI']=str(source/'default-ui')
env.update(PJB_EDK2_ROOT=str(edk),PJB_EDK2_LIBC=paths['edk2-libc'],
           PJB_QUICKJS=paths['quickjs'],PJB_UPSTREAM_LOCK=str(work/'upstream.lock.json'))
b.command(['bash',source/'uefi-runtime/tools/build-staticlib.sh'],env,logs/'rust.log')
b.command([sys.executable,source/'uefi-runtime/tools/prepare-edk2.py','--quickjs',paths['quickjs'],
           '--archive',work/'Rust.a','--edk2',edk,'--libc',paths['edk2-libc'],
           '--output',work/'packages','--config',project/'config/pocketjs.json'],env,logs/'packages.log')
b.edk_build(edk,[edk,Path(paths['edk2-libc']),work/'packages'],work/'edk-work',
            ['-p','PocketJsPkg/PocketJs.dsc','-n',str(a.jobs)],env,logs/'efi.log')
efi=work/'edk-work/Build/PocketJsBoot/RELEASE_GCC5/AARCH64/PocketJsBoot.efi'
b.check_pe(efi)
snapshot=hashlib.sha256()
for file in sorted(source.rglob('*')):
    if file.is_file() and '.git' not in file.parts:
        snapshot.update(str(file.relative_to(source)).encode());snapshot.update(file.read_bytes())
record={'schema':1,'source_sha256':snapshot.hexdigest(),'efi_sha256':b.digest(efi),'config_sha256':b.digest(project/'config/pocketjs.json'),
        'rust_sha256':b.digest(work/'Rust.a'),'upstream':json.loads((work/'upstream.lock.json').read_text())}
(work/'polaris-runtime.json').write_text(json.dumps(record,indent=2)+'\n')
print(efi)
