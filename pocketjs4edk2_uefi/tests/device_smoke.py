#!/usr/bin/env python3
"""Exercise query -> download -> RAM EFI -> return, without partition writes."""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fastboot', default='fastboot')
    parser.add_argument('--serial', required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 20:
        parser.error('--rounds must be 1..20')
    if not args.probe.is_file():
        parser.error('--probe must exist')
    report = {'passed': False, 'rounds': args.rounds, 'steps': []}
    def run(*command, expected=None, timeout=15):
        result = subprocess.run([args.fastboot, '-s', args.serial, *map(str, command)],
                                text=True, capture_output=True, timeout=timeout)
        output = result.stdout+result.stderr
        report['steps'].append({'command': list(map(str, command)), 'returncode': result.returncode,
                                'output': output})
        if result.returncode or (expected is not None and expected not in output):
            raise RuntimeError('fastboot check failed: '+' '.join(map(str, command)))
        return output
    try:
        # Confirm the custom recovery before sending any executable image.
        run('getvar', 'product', expected='polaris')
        run('getvar', 'polaris-boot-format', expected='efi-aa64')
        run('getvar', 'max-download-size')
        for index in range(args.rounds):
            run('stage', args.probe.resolve(), 'oem', 'boot-efi', timeout=30)
            # Returning EFI disconnects/restarts USB. Bound the complete reconnect.
            deadline = time.monotonic()+20
            while True:
                try:
                    run('getvar', 'polaris-boot-format', expected='efi-aa64', timeout=3)
                    break
                except (RuntimeError, subprocess.TimeoutExpired):
                    if time.monotonic() >= deadline:
                        raise RuntimeError('recovery USB did not return after EFI probe')
                    time.sleep(0.2)
            print('RAM EFI round', index+1, 'returned to recovery', flush=True)
        report['passed'] = True
    finally:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2)+'\n')
    print('PASS: query/download/RAM EFI/USB return; no partition write commands issued')


if __name__ == '__main__':
    main()
