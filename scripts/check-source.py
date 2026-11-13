#!/usr/bin/env python3
"""Check the pinned kernel identity without fetching objects or building."""
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
lock = json.loads((root/'provenance/kernel.json').read_text())
try:
    kernel = root/'kernel'
    head = subprocess.check_output(['git', '-C', str(kernel), 'rev-parse', 'HEAD'], text=True).strip()
    if head != lock['commit']:
        raise ValueError('kernel HEAD differs from pinned source')
    version = (kernel/'Makefile').read_text()
    for field, value in [('VERSION', '7'), ('PATCHLEVEL', '1'), ('SUBLEVEL', '0'), ('EXTRAVERSION', '-rc1')]:
        if f'{field} = {value}\n' not in version:
            raise ValueError('unexpected kernel version: '+field)
    if not (kernel/lock['dts']).is_file():
        raise ValueError('Polaris DTS is not checked out')
    complete = all((kernel/name).is_file() for name in ('scripts/Makefile.build', 'include/linux/kernel.h', 'arch/arm64/Makefile'))
    print(json.dumps({'version': lock['version'], 'commit': head, 'complete_checkout': complete, 'hardware_tested': False}))
except (OSError, ValueError, subprocess.SubprocessError) as error:
    sys.exit(str(error))
