#!/usr/bin/env python3
"""REIMUKS adapter for an offline, initialized Polaris platform source tree."""
import argparse
import json
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--stage', choices=['all', 'uefi', 'efi'], default='all')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--kernel-source', type=Path)
    parser.add_argument('--source-cache', type=Path)
    parser.add_argument('--inputs', type=Path)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    config = {}
    if args.inputs:
        config = json.loads(args.inputs.read_text())
        if config.get('schema') != 1 or config.get('board') != 'polaris':
            parser.error('boot inputs require schema=1 and board=polaris')
    def source(name):
        value = config.get(name)
        if value is None:
            return None
        path = Path(value)
        return path.resolve() if path.is_absolute() else (args.inputs.resolve().parent/path).resolve()
    platform = source('platform')
    if platform is None and args.source_cache:
        platform = args.source_cache.resolve()/'edk2-sdm845'
    edk2 = source('edk2')
    entry = config.get('entry', 'menu')
    if entry not in ('menu', 'recovery'):
        parser.error('entry must be menu or recovery')
    if platform is None and (args.stage != 'efi' or edk2 is None):
        parser.error('provide initialized platform via --inputs or --source-cache; no sources are downloaded')
    argv = [sys.executable, '-B', str(Path(__file__).with_name('build.py')),
            '--stage', args.stage, '--jobs', str(args.jobs), '--output', str(args.output.resolve()),
            '--workdir', str(Path(__file__).resolve().parents[1]/'build'), '--entry', entry]
    if platform is not None:
        argv += ['--platform', str(platform)]
    if edk2 is not None:
        argv += ['--edk2', str(edk2)]
    subprocess.run(argv, check=True)


if __name__ == '__main__':
    main()
