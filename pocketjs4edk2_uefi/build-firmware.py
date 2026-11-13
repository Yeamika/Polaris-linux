#!/usr/bin/env python3
"""Reserved Polaris boot adapter; no firmware build is implemented yet."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--stage', choices=['all', 'uefi', 'efi'], default='all')
parser.add_argument('--jobs', type=int, default=4)
parser.add_argument('--kernel-source', type=Path)
parser.add_argument('--source-cache', type=Path)
parser.add_argument('--inputs', type=Path)
parser.parse_args()
parser.exit(1, 'Polaris boot is registered but the SDM845/PocketJS port is not implemented. No output was created.\n')
