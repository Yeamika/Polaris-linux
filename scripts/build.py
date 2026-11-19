#!/usr/bin/env python3
"""Build the pinned Polaris kernel and board additions locally, out of tree."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inputs():
    files = [*sorted((ROOT/'Devices/drm/boot-display').glob('*')),
             ROOT/'config/kernel/desktop.config']
    files += sorted((ROOT/'patches/kernel').glob('*.patch'))
    files += sorted((ROOT/'components/boot-display/include').glob('*.h'))
    if not files[-1].name.endswith('.h'):
        raise ValueError('Initialize components/boot-display first')
    return {str(p.relative_to(ROOT)): digest(p) for p in files if p.is_file()}


def prepare_source(work):
    lock = json.loads((ROOT/'config/kernel-source.json').read_text())
    head = subprocess.check_output(['git', '-C', str(ROOT/'kernel'), 'rev-parse', 'HEAD'], text=True).strip()
    if head != lock['commit']:
        raise ValueError('Kernel source differs from pinned commit')
    if subprocess.check_output(['git', '-C', str(ROOT/'kernel'), 'diff', 'HEAD', '--'], text=True):
        raise ValueError('Upstream kernel has local tracked edits; use board additions instead')
    source = work/'source'
    marker = work/'source-identity.json'
    identity = {'kernel_commit': head, 'board_inputs': inputs()}
    if marker.is_file() and json.loads(marker.read_text()) == identity:
        return source, identity
    if source.exists():
        shutil.rmtree(source)
    work.mkdir(parents=True, exist_ok=True)
    # Read-only upstream files can share storage; every patched file is replaced,
    # never truncated in place. All compiler output is in the separate O= tree.
    def link(src, dst):
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy2(src, dst)
    shutil.copytree(ROOT/'kernel', source, copy_function=link, symlinks=True,
                    ignore=shutil.ignore_patterns('.git'))
    for patch in sorted((ROOT/'patches/kernel').glob('*.patch')):
        # Break hardlinks for every patched upstream file before applying it.
        # The pinned checkout must stay byte-identical and unmodified.
        for line in patch.read_text().splitlines():
            if not line.startswith('+++ b/'):
                continue
            relative = Path(line[6:])
            if relative.is_absolute() or '..' in relative.parts:
                raise ValueError('Unsafe kernel patch path: '+str(relative))
            path = source/relative
            if path.is_file():
                mode = path.stat().st_mode & 0o777
                content = path.read_bytes()
                path.unlink()
                path.write_bytes(content)
                path.chmod(mode)
        subprocess.run(['git', 'apply', '--check', str(patch.resolve())], cwd=source, check=True)
        subprocess.run(['git', 'apply', str(patch.resolve())], cwd=source, check=True)
    for name, addition in [
        ('drivers/video/Kconfig', '\nsource "drivers/video/polaris-boot-display/Kconfig"\n'),
        ('drivers/video/Makefile', '\nobj-$(CONFIG_POLARIS_BOOT_DISPLAY) += polaris-boot-display/\n')]:
        path = source/name
        content = path.read_text()
        path.unlink()
        path.write_text(content + addition)
    dest = source/'drivers/video/polaris-boot-display'
    shutil.copytree(ROOT/'Devices/drm/boot-display', dest)
    shutil.copytree(ROOT/'components/boot-display/include', dest/'common/include')
    marker.write_text(json.dumps(identity, indent=2)+'\n')
    return source, identity


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('stage', choices=['config', 'all'], nargs='?', default='all')
    p.add_argument('--profile', choices=['desktop'], default='desktop')
    p.add_argument('--kernel-out', type=Path, default=ROOT/'output/kernel-desktop')
    p.add_argument('--workdir', type=Path, default=Path(os.environ.get('POLARIS_BUILD_WORKDIR', ROOT/'.cache/kernel')))
    p.add_argument('--jobs', type=int, default=4)
    a = p.parse_args()
    if a.jobs < 1:
        p.error('--jobs must be positive')
    out = a.kernel_out.resolve()
    work = a.workdir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source, identity = prepare_source(work)
    make = ['make', '-C', str(source), 'O='+str(out), 'ARCH=arm64',
            'CROSS_COMPILE='+os.environ.get('CROSS_COMPILE', 'aarch64-linux-gnu-')]
    env = dict(os.environ, KBUILD_BUILD_USER='builder', KBUILD_BUILD_HOST='polaris',
               KBUILD_BUILD_TIMESTAMP='2026-11-15T00:00:00Z')
    fragments = ['kernel/arch/arm64/configs/'+name for name in
                 ('defconfig', 'pmos.config', 'misc.config', 'sdm845.config')]
    fragments.append('config/kernel/desktop.config')
    seed = '\n'.join((ROOT/path).read_text() for path in fragments)
    (out/'.config').write_text(seed)
    subprocess.run(make+['olddefconfig'], env=env, check=True)
    config = (out/'.config').read_text()
    required = ['EFI', 'EFI_STUB', 'RD_LZ4', 'DEVTMPFS', 'EXT4_FS', 'SCSI_UFS_QCOM',
                'PHY_QCOM_QMP_UFS', 'USB_CONFIGFS_ACM', 'USB_DWC3', 'POLARIS_BOOT_DISPLAY',
                'APERTURE_HELPERS', 'DRM_PANEL_NOVATEK_NT35596S', 'CPU_IDLE',
                'ARM_PSCI_CPUIDLE', 'ARM_PSCI_CPUIDLE_DOMAIN', 'FB_SYSMEM_HELPERS',
                'FRAMEBUFFER_CONSOLE_ROTATION', 'LOGO_LINUX_CLUT224', 'FONT_TER16x32',
                'THERMAL', 'CPU_THERMAL', 'QCOM_TSENS', 'QCOM_LMH']
    missing = [name for name in required if f'CONFIG_{name}=y\n' not in config]
    if missing or 'CONFIG_DRM_MSM=m\n' not in config:
        raise ValueError('Required built-in configuration missing: '+repr(missing))
    if a.stage == 'config':
        return
    subprocess.run(make+[f'-j{a.jobs}', 'Image', 'modules', 'qcom/sdm845-xiaomi-polaris.dtb'], env=env, check=True)
    release = (out/'include/config/kernel.release').read_text().strip()
    if release != '7.1.0-rc1-polaris-desktop':
        raise ValueError('Unexpected kernel release: '+release)
    identity.update(board='polaris', profile=a.profile, release=release,
                    image_sha256=digest(out/'arch/arm64/boot/Image'),
                    dtb_sha256=digest(out/'arch/arm64/boot/dts/qcom/sdm845-xiaomi-polaris.dtb'),
                    config_sha256=digest(out/'.config'), hardware_tested=False)
    (out/'build-identity.json').write_text(json.dumps(identity, indent=2)+'\n')
    print(json.dumps(identity, indent=2))


if __name__ == '__main__':
    main()
