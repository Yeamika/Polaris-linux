#!/usr/bin/env python3
"""Compile and run native tests against the real recovery sources and EDK II headers."""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--edk2', type=Path, required=True)
    parser.add_argument('--platform', type=Path, help='Initialized Qualcomm sources for framebuffer console tests')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    test = Path(__file__).resolve().parent
    core = test.parent/'efi/Recovery'
    args.output.mkdir(parents=True, exist_ok=True)
    flags = ['-std=c11', '-Wall', '-Werror', '-g']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined']
    headers = args.edk2.resolve()/'MdePkg/Include'
    binary = args.output/'touch_rmi'
    subprocess.run([args.cc, *flags, str(test/'touch_rmi_test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)
    binary = args.output/'boot_output_layout'
    subprocess.run([args.cc, *flags, '-I', str(test.parent.parent/'components/pj4edk2-linux-efi/include'),
                    str(test/'boot_output_layout_test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)
    binary = args.output/'simple_fb_damage'
    subprocess.run([args.cc, *flags, '-fshort-wchar', '-I', str(headers), '-I', str(headers/'X64'),
                    str(test/'simple_fb_damage_test.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)
    for name in ('fastboot_core', 'recovery_transport', 'boot_entry', 'platform_services', 'backend_lifecycle', 'android_image', 'android_warm', 'linux_initrd', 'linux_diagnostics', 'linux_exit_trace', 'native_memory', 'native_journal'):
        extra = ['-I', str(core), '-I', str(test.parent.parent/'components/pj4edk2/pocketjsforedk2/include')]
        if name != 'fastboot_core':
            # PMIC tests also parse ArmLib declarations; unused target reset
            # code is discarded while the test itself runs on the build host.
            architecture = 'AArch64' if name == 'android_warm' else 'X64'
            extra += ['-Wno-unused-variable', '-fshort-wchar', '-ffunction-sections',
                      '-fdata-sections', '-Wl,--gc-sections', '-I', str(headers), '-I', str(headers/architecture),
                      '-I', str(args.edk2.resolve()/'ArmPkg/Include')]
        binary = args.output/name
        subprocess.run([args.cc, *flags, *extra, str(test/(name+'_test.c')),
                        str(core/'FastbootCore.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary.resolve())], check=True)

    if args.platform:
        for name in ('windows_errata', 'windows_errata_driver'):
            binary = args.output/name
            subprocess.run([args.cc, *flags, '-fshort-wchar', '-I', str(headers), '-I', str(headers/'X64'),
                '-I', str(args.platform.resolve()/'Platform/RenegadePkg/Drivers/KernelErrataPatcher'),
                str(test/(name+'_test.c')), '-o', str(binary)], check=True)
            subprocess.run([str(binary.resolve())], check=True)
        binary = args.output/'windows_acpi'
        subprocess.run([args.cc, *flags, '-fshort-wchar', '-I', str(headers), '-I', str(headers/'X64'),
            str(test/'windows_acpi_test.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary.resolve()), str(args.platform.resolve()/
            'Platform/Xiaomi/sdm845/AcpiTables/polaris/DSDT.aml')], check=True)
        binary = args.output/'windows_chain'
        subprocess.run([args.cc, *flags, '-fshort-wchar', '-I', str(headers), '-I', str(headers/'X64'),
            '-I', str(args.edk2.resolve()/'EmbeddedPkg/Include'),
            '-I', str(args.platform.resolve()/'Silicon/Qualcomm/QcomPkg/Include'),
            '-I', str(test.parent.parent/'components/pj4edk2/pocketjsforedk2/include'),
            str(test/'windows_chain_test.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary.resolve())], check=True)
        for name in ('polaris_touch', 'touch_gpio_i2c'):
            binary = args.output/name
            subprocess.run([args.cc, *flags, '-fshort-wchar', '-ffunction-sections', '-fdata-sections',
                '-Wl,--gc-sections', '-I', str(headers), '-I', str(headers/'X64'),
                '-I', str(args.platform.resolve()/'Silicon/Qualcomm/QcomPkg/Include'),
                str(test/(name+'_test.c')), '-o', str(binary)], check=True)
            subprocess.run([str(binary.resolve())], check=True)
        binary = args.output/'boot_console'
        subprocess.run([args.cc, *flags, '-fshort-wchar', '-I', str(headers), '-I', str(headers/'AArch64'),
            '-I', str(args.edk2.resolve()/'ArmPkg/Include'),
            '-I', str(args.platform.resolve()/'Silicon/Qualcomm/QcomPkg/Include'),
            str(test/'boot_console_test.c'), '-o', str(binary)], check=True)
        subprocess.run([str(binary.resolve())], check=True)

    fdt = args.edk2.resolve()/'EmbeddedPkg/Library/FdtLib'
    binary = args.output/'android_dtb'
    subprocess.run([args.cc, *flags, '-D_POSIX_C_SOURCE=200809L', '-Wno-unused-variable', '-fshort-wchar',
        '-I', str(test/'fdt-host'), '-I', str(headers), '-I', str(headers/'X64'),
        '-I', str(args.edk2.resolve()/'EmbeddedPkg/Include'),
        str(test/'android_dtb_test.c'),
        *[str(fdt/(name+'.c')) for name in ('fdt','fdt_ro','fdt_rw','fdt_sw','fdt_wip','fdt_empty_tree')],
        '-o', str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)

    binary = args.output/'linux_handoff'
    subprocess.run([args.cc, *flags, '-D_POSIX_C_SOURCE=200809L', '-Wno-unused-variable', '-fshort-wchar',
        '-I', str(test/'fdt-host'), '-I', str(headers), '-I', str(headers/'X64'),
        '-I', str(args.edk2.resolve()/'EmbeddedPkg/Include'),
        '-I', str(test.parent.parent/'components/pj4edk2/pocketjsforedk2/include'),
        '-I', str(test.parent.parent/'components/pj4edk2-linux-efi/include'),
        str(test/'linux_handoff_test.c'),
        *[str(fdt/(name+'.c')) for name in ('fdt','fdt_ro','fdt_rw','fdt_sw','fdt_wip','fdt_empty_tree','fdt_addresses')],
        '-o', str(binary)], check=True)
    subprocess.run([str(binary.resolve())], check=True)

    subprocess.run([sys.executable, '-B', str(test/'debug_client_test.py')], check=True)
    subprocess.run([sys.executable, '-B', str(test/'android_dualboot_test.py')], check=True)


if __name__ == '__main__':
    main()
