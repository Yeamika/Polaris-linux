#!/usr/bin/env python3
"""Export a matched Polaris Image, DTB, config and modules for REIMUKS."""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
from build import ROOT, digest, inputs


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--profile', choices=['desktop'], default='desktop')
    p.add_argument('--kernel-out', type=Path, help='kernel output containing build-identity.json')
    p.add_argument('--output', type=Path, help='separate destination for the matched runtime')
    a = p.parse_args()
    build = (a.kernel_out or ROOT/'output'/('kernel-'+a.profile)).resolve()
    output = (a.output or ROOT/'output'/('runtime-'+a.profile)).resolve()
    if build == output or build in output.parents or output in build.parents:
        raise ValueError('Kernel build and exported runtime directories must be separate')
    meta = json.loads((build/'build-identity.json').read_text())
    if meta['board_inputs'] != inputs():
        raise ValueError('Board sources changed after the kernel build')
    paths = {'Image': ('arch/arm64/boot/Image', 'image_sha256'),
             'polaris.dtb': ('arch/arm64/boot/dts/qcom/sdm845-xiaomi-polaris.dtb', 'dtb_sha256'),
             'config': ('.config', 'config_sha256')}
    for path, key in paths.values():
        if digest(build/path) != meta[key]:
            raise ValueError('Kernel build identity mismatch: '+key)
    output.mkdir(parents=True, exist_ok=True)
    modules = output/'modules'/meta['release']
    if modules.exists():
        shutil.rmtree(modules)
    modules.mkdir(parents=True)
    names = []
    for entry in (build/'modules.order').read_text().splitlines():
        name = Path(entry).with_suffix('.ko')
        if name.is_absolute() or '..' in name.parts:
            raise ValueError('Unsafe modules.order entry')
        source = build/name
        vermagic = re.search(rb'vermagic=([^\0]+)', source.read_bytes())
        if not vermagic or vermagic[1].decode().split()[0] != meta['release']:
            raise ValueError('Module ABI mismatch: '+str(name))
        target = modules/'kernel'/name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        subprocess.run(['aarch64-linux-gnu-strip', '--strip-debug', str(target)], check=True)
        names.append('kernel/'+str(name))
    for name in ('modules.builtin', 'modules.builtin.modinfo'):
        shutil.copyfile(build/name, modules/name)
    (modules/'modules.order').write_text(''.join(name+'\n' for name in names))
    for name, (source, _) in paths.items():
        shutil.copyfile(build/source, output/name)
    meta.update(standard_modules=len(names), board_modules=0,
                module_hashes={name: digest(modules/name) for name in names})
    (output/'identity.json').write_text(json.dumps(meta, indent=2)+'\n')
    print(json.dumps({'release': meta['release'], 'standard_modules': len(names), 'output': str(output)}, indent=2))


if __name__ == '__main__':
    main()
