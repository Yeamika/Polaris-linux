#!/usr/bin/env python3
"""Build Polaris Renegade UEFI and three standalone AArch64 EFI launchers offline."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

PROJECT = Path(__file__).resolve().parent
REPO = PROJECT.parent
APPS = ('Windows', 'Android', 'LinuxBoot', 'Recovery')
sys.path.insert(0, str(PROJECT/'tools'))
import android_dualboot
import simple_init_android_memory


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(args, env, log, cwd=None):
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open('w') as stream:
        result = subprocess.run([str(x) for x in args], env=env, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'Build failed ({result.returncode}); see {log}')
    if any(message in log.read_text(errors='replace') for message in
           ('cannot find entry symbol', 'plugin needed to handle lto object')):
        raise RuntimeError(f'Invalid linker output; see {log}')


def copy_build_sources(origin, destination, excluded):
    """Keep symlinks valid when relocating a private platform build copy."""
    origin = origin.resolve()
    shutil.copytree(origin, destination, symlinks=True,
                    ignore=shutil.ignore_patterns(*excluded))
    for directory, dirs, files in os.walk(destination, followlinks=False):
        for name in dirs + files:
            target = Path(directory)/name
            if not target.is_symlink():
                continue
            original = origin/target.relative_to(destination)
            resolved = original.resolve()
            if resolved.is_relative_to(origin):
                resolved = destination/resolved.relative_to(origin)
            target.unlink()
            target.symlink_to(os.path.relpath(resolved, target.parent))


def edk_build(edk2, packages, workspace, args, environment, log, config=None):
    conf = workspace/'Conf'
    conf.mkdir(parents=True, exist_ok=True)
    if config:
        for name in ('build_rule.txt', 'tools_def.txt'):
            shutil.copy2(config/name, conf/name)
    tools = conf/'tools_def.txt'
    if not tools.is_file():
        shutil.copy2(edk2/'BaseTools/Conf/tools_def.template', tools)
    text = tools.read_text()
    flags = [line for line in text.splitlines()
             if line.startswith('DEFINE GCC_AARCH64_CC_FLAGS ')]
    if len(flags) != 1:
        raise ValueError('Unsupported AArch64 toolchain configuration')
    # Freestanding UEFI has no host stack-protector runtime.
    if '-fno-stack-protector' not in flags[0]:
        tools.write_text(text.replace(flags[0], flags[0]+' -fno-stack-protector', 1))
    env = dict(environment, WORKSPACE=str(workspace), CONF_PATH=str(conf),
               PACKAGES_PATH=':'.join(str(p) for p in packages), PYTHON_COMMAND=sys.executable,
               GCC5_AARCH64_PREFIX=environment.get('GCC5_AARCH64_PREFIX', 'aarch64-linux-gnu-'))
    command(['bash', '-c', 'set -e; edk=$1; shift; args=("$@"); set --; set +u; source "$edk/edksetup.sh"; set -u; build "${args[@]}"',
             'polaris-edk2', edk2, '-a', 'AARCH64', '-t', 'GCC5', '-b', 'RELEASE', *args], env, log, workspace)


def patch_platform(source, entry='menu'):
    # Apply only in the private build copy, never in the caller's source checkout.
    memory = source/'Silicon/Qualcomm/sdm845/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c'
    normalized = ''.join(memory.read_text().split())
    if '{"UEFIFD",0xCE000000,0x02000000,AddMem,SYS_MEM,SYS_MEM_CAP,BsData,WRITE_BACK}' not in normalized:
        raise ValueError('Unsupported SDM845 reserved firmware mapping for Android handoff trace')
    fdf = source/'Platform/Qualcomm/sdm845/sdm845.fdf'
    text = fdf.read_text()
    before = '!if $(ENABLE_LINUX_UTILS) == 1\n  FILE FREEFORM = 4b0364cf-1c5b-47aa-9073-d7b5039ce49b {'
    after = '  FILE FREEFORM = 4b0364cf-1c5b-47aa-9073-d7b5039ce49b {'
    if text.count(before) != 1:
        raise ValueError('Unsupported Renegade static-config FDF layout')
    text = text.replace(before, after, 1)
    app = '  INF Platform/RenegadePkg/Application/Reboot2PayloadApp/Reboot2PayloadApp.inf'
    text = text.replace(app, '!if $(ENABLE_LINUX_UTILS) == 1\n'+app, 1)
    fdf.write_text(text)
    shutil.copy2(PROJECT/'config/simpleinit.static.uefi.cfg', source/'tools/simpleinit.static.uefi.cfg')
    dsc = source/'Platform/Xiaomi/sdm845/polaris.dsc'
    if 'ENABLE_POCKETJS_BOOT' in dsc.read_text():
        raise ValueError('Input must be the clean Renegade platform, not a legacy PocketJS experiment')
    flags = 'GCC:*_*_AARCH64_CC_FLAGS = -DENABLE_SIMPLE_INIT -DENABLE_LINUX_SIMPLE_MASS_STORAGE'
    original = dsc.read_text()
    if flags not in original: raise ValueError('Unsupported Polaris build options')
    dsc.write_text(original.replace(flags, flags+' -I"$(POLARIS_ABI_INCLUDE)" -I"$(POLARIS_LINUX_EFI_INCLUDE)" -I"$(POLARIS_WINDOWS_INCLUDE)"', 1))
    backend = '  pocketjs4edk2_uefi/efi/Recovery/Backend.inf\n'
    touch = '  pocketjs4edk2_uefi/platform/PolarisTouchDxe/PolarisTouchDxe.inf\n'
    rotation = '  pocketjs4edk2_uefi/platform/PolarisRotatorDxe/PolarisRotatorDxe.inf\n'
    entries = touch+rotation+backend+''.join(f'  pocketjs4edk2_uefi/efi/{name}/{name}.inf\n' for name in APPS)
    with dsc.open('a') as f: f.write('\n[Components.common]\n'+entries)
    with (source/'Platform/Xiaomi/sdm845/polaris.fdf.inc').open('a') as f:
        f.write('\n'+''.join('  INF '+line.strip()+'\n' for line in entries.splitlines()))
    patch_boot_console(source)
    patch_simple_fb_damage(source)
    patch_windows_errata(source)


def patch_windows_errata(source):
    module = source/'Platform/RenegadePkg/Drivers/KernelErrataPatcher'
    # Keep the original assembly capture of x19/x20/x30. Only replace the C
    # implementation in the private build copy, retaining the pinned source.
    for name in ('WindowsErrata.c', 'WindowsErrata.h', 'WindowsErrataProtocol.h'):
        shutil.copy2(PROJECT/'platform'/name, module/name)
    shutil.copy2(PROJECT/'platform/WindowsErrataDxe.c', module/'KernelErrataPatcher.c')
    inf = module/'KernelErrataPatcher.inf'
    text = inf.read_text()
    if text.count('  Utilities.c') != 1:
        raise ValueError('Unsupported kernel errata module layout')
    text = text.replace('  Utilities.c', '  WindowsErrata.c', 1)
    inf.write_text(text.replace('[LibraryClasses]', '[LibraryClasses]\n  CacheMaintenanceLib', 1))


def patch_simple_fb_damage(source):
    module = source/'Silicon/Qualcomm/QcomPkg/Drivers/SimpleFbDxe'
    shutil.copy2(PROJECT/'platform/SimpleFbDamage.h', module/'SimpleFbDamage.h')
    path = module/'SimpleFbDxe.c'
    text = path.read_text()
    anchor = '#include <Uefi.h>'
    if text.count(anchor) != 1: raise ValueError('Unsupported SimpleFb includes')
    text = text.replace(anchor, anchor+'\n#include "SimpleFbDamage.h"', 1)
    before = '''  // zhuowei: hack: flush the cache manually since my memory maps are still
  // broken
  WriteBackInvalidateDataCacheRange(
      (void *)mDisplay.Mode->FrameBufferBase, mDisplay.Mode->FrameBufferSize);
  // zhuowei: end hack'''
    if text.count(before) != 1: raise ValueError('Unsupported SimpleFb BLT cache maintenance')
    text = text.replace(before, '''  if (!RETURN_ERROR(Status))
    PolarisSimpleFbFlush(This, BltOperation, DestinationX, DestinationY, Width, Height);''', 1)
    path.write_text(text)


def patch_boot_console(source):
    include = source/'Silicon/Qualcomm/QcomPkg/Include'
    shutil.copy2(PROJECT/'platform/BootConsole.h', include/'BootConsole.h')
    library = source/'Silicon/Qualcomm/QcomPkg/Library/FrameBufferSerialPortLib'
    shutil.copy2(PROJECT/'platform/FrameBufferSerialPortLib.c', library/'FrameBufferSerialPortLib.c')
    inf = library/'FrameBufferSerialPortLib.inf'
    inf.write_text(inf.read_text().replace('[LibraryClasses]', '[LibraryClasses]\n  BaseMemoryLib', 1))
    utils = source/'Silicon/Qualcomm/sdm845/Library/PlatformPrePiLib/PlatformUtils.c'
    text = utils.read_text().replace('#include "PlatformUtils.h"', '#include "PlatformUtils.h"\n#include "BootConsole.h"', 1)
    start = text.index('  INTN* pFbConPosition')
    end = text.index('\n}', start)
    text = text[:start]+'  ZeroMem(PolarisBootConsole(), sizeof(POLARIS_BOOT_CONSOLE));'+text[end:]
    before = '  SerialPortInitialize();\n\n  InitializeSharedUartBuffers();'
    if text.count(before) != 1: raise ValueError('Unsupported PrePi serial initialization')
    utils.write_text(text.replace(before, '  InitializeSharedUartBuffers();\n  SerialPortInitialize();', 1))
    # GOP initialization must preserve the early console's already painted log.
    fb = source/'Silicon/Qualcomm/QcomPkg/Drivers/SimpleFbDxe/SimpleFbDxe.c'
    text = fb.read_text().replace('#include <Uefi.h>', '#include <Uefi.h>\n#include "BootConsole.h"', 1)
    before = '  ZeroMem((void *)FrameBufferAddress, FrameBufferSize);'
    if text.count(before) != 1: raise ValueError('Unsupported SimpleFb initialization')
    fb.write_text(text.replace(before, '  if (!PolarisBootConsoleEarly())\n  '+before, 1))


def overlay_module(edk2, overlay, relative):
    parts = Path(relative).parts
    for depth in range(1, len(parts)):
        origin = edk2.joinpath(*parts[:depth])
        target = overlay.joinpath(*parts[:depth])
        if target.is_symlink(): target.unlink()
        if not target.exists():
            target.mkdir()
            for child in origin.iterdir(): (target/child.name).symlink_to(child)
    target = overlay.joinpath(*parts)
    if target.is_symlink(): target.unlink()
    shutil.copytree(edk2.joinpath(*parts), target)
    return target


def patch_graphics_console(edk2, overlay):
    module = overlay_module(edk2, overlay, 'MdeModulePkg/Universal/Console/GraphicsConsoleDxe')
    for name in ('BootConsole.h', 'BootConsoleText.h'):
        shutil.copy2(PROJECT/'platform'/name, module/name)
    source = module/'GraphicsConsole.c'
    text = source.read_text().replace('#include "GraphicsConsole.h"',
        '#include "GraphicsConsole.h"\n#include "BootConsoleText.h"', 1)
    # During boot, ConOut/StdErr are append-only logs with the same serial
    # renderer/cursor as DEBUG. Normal UEFI text semantics resume for child EFI.
    output = text.index('GraphicsConsoleConOutOutputString (')
    position = text.index('  if (This->Mode->Mode == -1) {', output)
    text = text[:position]+('  if (PolarisBootConsoleEarly())\n'
        '    return PolarisBootConsoleOutput(WString);\n\n')+text[position:]
    clear = text.index('GraphicsConsoleConOutClearScreen (')
    position = text.index('  if (This->Mode->Mode == -1) {', clear)
    text = text[:position]+'  if (PolarisBootConsoleEarly()) return EFI_SUCCESS;\n\n'+text[position:]
    cursor = text.index('\nFlushCursor (')
    position = text.index('  CurrentMode = This->Mode;', cursor)
    text = text[:position]+'  if (PolarisBootConsoleEarly()) return EFI_SUCCESS;\n\n'+text[position:]
    mode = text.index('GraphicsConsoleConOutSetMode (')
    before = ('    } else {\n      //\n'
              '      // The current graphics mode is correct, so simply clear the entire display')
    position = text.index(before, mode)
    text = text[:position]+text[position:].replace(before,
        '    } else if (PolarisBootConsoleEarly()) {\n      Status = EFI_SUCCESS;\n'+before, 1)
    source.write_text(text)
    inf = module/'GraphicsConsoleDxe.inf'
    inf.write_text(inf.read_text().replace('[LibraryClasses]', '[LibraryClasses]\n  SerialPortLib', 1))
    # Exceptions still take over the display after ordinary DEBUG is muted.
    module = overlay_module(edk2, overlay, 'ArmPkg/Library/DefaultExceptionHandlerLib')
    source = module/'AArch64/DefaultExceptionHandler.c'
    text = source.read_text()
    anchor = '#include <Library/SerialPortLib.h>'
    if text.count(anchor) != 1: raise ValueError('Unsupported ARM exception serial handler')
    text = text.replace(anchor, anchor+'\nUINTN EFIAPI SerialPortWriteCritical(UINT8 *Buffer, UINTN Size);', 1)
    if text.count('SerialPortWrite (') != 2: raise ValueError('Unsupported ARM exception dump')
    text = text.replace('SerialPortWrite (', 'SerialPortWriteCritical (')
    for expression, indent in [('Message', '    '), ('Buffer', '  ')]:
        before = indent+'if (gST->ConOut != NULL) {\n'+indent+'  AsciiPrint ('+expression+');\n'+indent+'}\n'
        if text.count(before) != 1: raise ValueError('Unsupported duplicate ARM exception output')
        text = text.replace(before, '', 1)
    source.write_text(text)


def patch_early_bds(edk2, work, entry):
    # Package overlay: all unchanged files remain read-only symlinks to EDK2.
    overlay = work/'edk2-overlay'
    if overlay.exists(): shutil.rmtree(overlay)
    overlay.mkdir(parents=True)
    module = overlay_module(edk2, overlay, 'MdeModulePkg/Universal/BdsDxe')
    bds = module/'BdsEntry.c'
    text = bds.read_text()
    anchor = '  PERF_INMODULE_END ("EfiBootManagerConnectAllDefaultConsoles");'
    if text.count(anchor) != 1: raise ValueError('Unsupported EDK2 BDS console layout')
    helper = (PROJECT/'platform/BootEntry.c').read_text()
    shutil.copy2(PROJECT/'platform/BootConsole.h', module/'BootConsole.h')
    declarations = ('#define POLARIS_USE_POCKETJS 1\n' if entry == 'pocketjs' else '')+'extern EFI_GUID gSimpleInitFileGuid;\nextern EFI_GUID gUefiShellFileGuid;\n'
    text = text.replace('#include "Bds.h"', '#include "Bds.h"\n'+declarations+helper, 1)
    startup = '\n  Print(L"POLARIS_EARLY_ENTRY: console ready\\n");\n  EfiBootManagerConnectAll();\n  PolarisRunEntry('+('TRUE' if entry == 'recovery' else 'FALSE')+');'
    bds.write_text(text.replace(anchor, anchor+startup, 1))
    inf = module/'BdsDxe.inf'
    text = inf.read_text().replace('[LibraryClasses]', '[LibraryClasses]\n  DxeServicesLib', 1)
    text = text.replace('[Pcd]', '[Pcd]\n  gEfiMdeModulePkgTokenSpaceGuid.PcdFirmwareVersionString', 1)
    inf.write_text(text)
    patch_graphics_console(edk2, overlay)
    return overlay


def patch_simple_menu(simple):
    # Set only the in-memory session policy; never save over device config.
    header = ['/* Generated Polaris session menu; no persistent writes. */',
              'static void polaris_session_menu(void) {',
              'confd_delete("boot.current");',
              'confd_set_integer("boot.timeout", -1);',
              'confd_set_string("boot.default", "recovery");',
              'confd_set_string("boot.title", "Polaris UEFI");',
              'confd_set_integer("gui.rotate", 90);']
    import re
    cfg = (PROJECT/'config/simpleinit.static.uefi.cfg').read_text()
    for name, desc, guid in re.findall(r'(recovery|windows|android|linux)\s*\{\s*mode = "efi"\s*desc = "([^"]+)".*?efi_fv_guid = "([^"]+)"', cfg, re.S):
        for key, value in [('mode', 'efi'), ('desc', desc), ('extra.efi_fv_guid', guid)]:
            header.append(f'confd_set_string("boot.configs.{name}.{key}", "{value}");')
        for key in ('show', 'enabled'):
            header.append(f'confd_set_boolean("boot.configs.{name}.{key}", true);')
    if len(header) != 27: raise ValueError('Expected four Polaris menu entries')
    header.append('}')
    (simple/'src/main/polaris-menu.h').write_text('\n'.join(header)+'\n')
    main = simple/'src/main/uefimain.c'
    text = main.read_text()
    if '#include "polaris-menu.h"' not in text:
        text = text.replace('#define TAG "main"', '#include "polaris-menu.h"\n#define TAG "main"', 1)
        text = text.replace('\tconfd_init();', '\tconfd_init();\n\tpolaris_session_menu();', 1)
    main.write_text(text)
    for name in ('BootRequest.h', 'NativeFdt.h', 'Kernel.h', 'Trace.h', 'AndroidMemory.h', 'SimpleInitHandoff.h', 'SimpleInitAndroid.h', 'EnterKernel.S'):
        shutil.copy2(PROJECT/'efi/Android'/name, simple/'src/main'/name)
    simple_init_android_memory.patch_android_allocation(simple)
    inf = simple/'src/main/SimpleInitMain.inf'
    text = inf.read_text()
    if '  EnterKernel.S' not in text:
        text = text.replace('  uefimain.c', '  uefimain.c\n  EnterKernel.S', 1)
        text = text.replace('[LibraryClasses]', '[LibraryClasses]\n  UefiRuntimeServicesTableLib', 1)
    text = text.replace('  ArmLib\n', '')
    inf.write_text(text)
    text = main.read_text()
    if '#include "SimpleInitAndroid.h"' not in text:
        text = text.replace('#define TAG "main"', '#include "SimpleInitAndroid.h"\n#define TAG "main"', 1)
        anchor = '\tREPORT_STATUS_CODE(EFI_PROGRESS_CODE,(EFI_SOFTWARE_DXE_BS_DRIVER|EFI_SW_PC_USER_SETUP));'
        if anchor not in text: raise ValueError('SimpleInit Android dispatch anchor missing')
        text = text.replace(anchor, anchor+'\n\tBOOLEAN Handled;\n\tEFI_STATUS AndroidStatus=polaris_android_dispatch(ih,&Handled);\n\tif(Handled)return AndroidStatus;',1)
    main.write_text(text)


def check_pe(path):
    data = path.read_bytes()
    if data[:2] != b'MZ' or len(data) < 64:
        raise ValueError('Missing PE DOS header: '+path.name)
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    if pe+96 > len(data) or data[pe:pe+4] != b'PE\0\0':
        raise ValueError('Invalid PE header: '+path.name)
    if struct.unpack_from('<H', data, pe+4)[0] != 0xaa64:
        raise ValueError('Not AArch64: '+path.name)
    if struct.unpack_from('<H', data, pe+24)[0] != 0x20b or struct.unpack_from('<H', data, pe+24+68)[0] != 10:
        raise ValueError('Not a PE32+ EFI application: '+path.name)
    entry = struct.unpack_from('<I', data, pe+24+16)[0]
    count, optional = struct.unpack_from('<H', data, pe+6)[0], struct.unpack_from('<H', data, pe+20)[0]
    table = pe+24+optional
    if not entry or table+count*40 > len(data) or not any(
            (lambda s: s[1] <= entry < s[1]+s[0] and s[2] and s[3]+s[2] <= len(data) and s[4] & 0x20000000)(
                (*struct.unpack_from('<4I', data, table+i*40+8), struct.unpack_from('<I', data, table+i*40+36)[0]))
            for i in range(count)):
        raise ValueError('EFI entry point has no executable section: '+path.name)
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'machine': 'AARCH64', 'subsystem': 'EFI_APPLICATION'}


def windows_assets(platform, work):
    if platform is None:
        raise ValueError('Windows EFI requires --platform with the pinned SDM845 sources')
    csrt = (platform/'Silicon/Qualcomm/sdm845/AcpiTables/CSRT.aml').read_bytes()
    if (csrt[:4] != b'CSRT' or len(csrt) < 36 or
            struct.unpack_from('<I', csrt, 4)[0] != len(csrt) or sum(csrt) % 256 or
            hashlib.sha256(csrt).hexdigest() != '3906a2c857e09ba1dab9ab6f8de2b1f8e8ff7a012c8cb1f8da5e7a46ffdf9c72'):
        raise ValueError('Unexpected SDM845 CSRT; review the Windows platform contract before rebuilding')
    generated = work/'generated/windows'
    generated.mkdir(parents=True, exist_ok=True)
    (generated/'WindowsCsrt.h').write_text(
        '// Generated from the pinned upstream SDM845 CSRT; do not edit.\n'
        'STATIC CONST UINT8 WindowsCsrt[]={\n'+
        '\n'.join(','.join(f'0x{b:02x}' for b in csrt[i:i+24])+',' for i in range(0,len(csrt),24))+
        '\n};\n')
    return generated


def build(args):
    platform = args.platform.resolve() if args.platform else None
    edk2 = (args.edk2 or (platform/'Common/edk2' if platform else None))
    if edk2 is None or not (edk2/'edksetup.sh').is_file():
        raise ValueError('Provide an initialized --edk2 or --platform with Common/edk2')
    edk2 = edk2.resolve()
    if not os.environ.get('EDK_TOOLS_PATH'):
        raise ValueError('Set EDK_TOOLS_PATH to prebuilt BaseTools')
    source_id = hashlib.sha256(b''.join(str(p.relative_to(PROJECT)).encode()+p.read_bytes()
        for p in sorted(PROJECT.rglob('*')) if p.is_file() and p.suffix in ('.c', '.h', '.S', '.py', '.inf', '.dsc', '.cfg', '.json')
        and 'tests' not in p.relative_to(PROJECT).parts)).hexdigest()[:12]
    work = args.workdir.resolve()
    output = args.output.resolve()
    if work == output or work in output.parents or output in work.parents:
        raise ValueError('Work and final output directories must be separate')
    for directory in (REPO, edk2, platform):
        if directory is not None and (work == directory or work in directory.parents or output == directory or output in directory.parents):
            raise ValueError('Build directories must not contain source directories')
    if args.stage != 'efi' and (platform is None or not (platform/'Platform/Xiaomi/sdm845/polaris.dsc').is_file()):
        raise ValueError('The UEFI stage requires --platform with the Renegade Polaris sources')
    work.mkdir(parents=True, exist_ok=True)
    logs = work/'logs'
    logs.mkdir(exist_ok=True)
    env = dict(os.environ)
    abi = REPO/'components/pj4edk2/pocketjsforedk2/include'
    linux_efi = REPO/'components/pj4edk2-linux-efi/include'
    if not (abi/'pocketjs_boot_context.h').is_file() or not (linux_efi/'pj4edk2_linux_fdt.h').is_file():
        raise ValueError('Initialize the pj4edk2 and pj4edk2-linux-efi components')
    windows_include = windows_assets(platform, work)
    includes = ['-D', 'POLARIS_ABI_INCLUDE='+str(abi.resolve()),
                '-D', 'POLARIS_LINUX_EFI_INCLUDE='+str(linux_efi.resolve()),
                '-D', 'POLARIS_WINDOWS_INCLUDE='+str(windows_include)]
    efi_work = work/'efi'
    edk_build(edk2, [REPO, platform, edk2], efi_work,
              ['-p', 'pocketjs4edk2_uefi/efi/PolarisEfi.dsc', '-n', str(args.jobs), *includes], env, logs/'efi.log')
    delivered = {}
    for name in APPS:
        file = efi_work/'Build/PolarisEfi/RELEASE_GCC5/AARCH64'/(name+'.efi')
        delivered[name] = check_pe(file)
    probe = efi_work/'Build/PolarisEfi/RELEASE_GCC5/AARCH64/ReturnProbe.efi'
    check_pe(probe)
    # Validate all applications before replacing any delivered application.
    for name in APPS:
        path = output/'ESP/EFI'/name/'BOOTAA64.EFI'
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(efi_work/'Build/PolarisEfi/RELEASE_GCC5/AARCH64'/(name+'.efi'), path)
    (output/'tools').mkdir(exist_ok=True)
    shutil.copy2(probe, output/'tools/return-probe.efi')
    hang = efi_work/'Build/PolarisEfi/RELEASE_GCC5/AARCH64/HangProbe.efi'
    check_pe(hang)
    shutil.copy2(hang, output/'tools/hang-probe.efi')
    ebs_hang = efi_work/'Build/PolarisEfi/RELEASE_GCC5/AARCH64/EbsHangProbe.efi'
    check_pe(ebs_hang)
    shutil.copy2(ebs_hang, output/'tools/ebs-hang-probe.efi')
    android_prepare = efi_work/'Build/PolarisEfi/RELEASE_GCC5/AARCH64/AndroidPrepare.efi'
    check_pe(android_prepare)
    shutil.copy2(android_prepare, output/'tools/android-prepare.efi')
    linux_inspect = efi_work/'Build/PolarisEfi/RELEASE_GCC5/AARCH64/LinuxInspect.efi'
    check_pe(linux_inspect)
    shutil.copy2(linux_inspect, output/'tools/linux-inspect.efi')
    linux_prepare = efi_work/'Build/PolarisEfi/RELEASE_GCC5/AARCH64/LinuxPrepare.efi'
    check_pe(linux_prepare)
    shutil.copy2(linux_prepare, output/'tools/linux-prepare.efi')
    linux_trace = efi_work/'Build/PolarisEfi/RELEASE_GCC5/AARCH64/LinuxTrace.efi'
    check_pe(linux_trace)
    shutil.copy2(linux_trace, output/'tools/linux-trace.efi')
    for source, name in (('LinuxNative', 'linux-native'), ('LinuxNativePrepare', 'linux-native-prepare'), ('LinuxNativeProbe', 'linux-native-probe'), ('LinuxNativePreExitProbe', 'linux-native-pre-exit-probe'), ('LinuxNativeExitProbe', 'linux-native-exit-probe'), ('LinuxNativeKernelProbe', 'linux-native-kernel-probe')):
        native = efi_work/'Build/PolarisEfi/RELEASE_GCC5/AARCH64'/(source+'.efi')
        check_pe(native)
        shutil.copy2(native, output/('tools/'+name+'.efi'))
    shutil.copy2(PROJECT/'tools/debug.py', output/'tools/debug.py')
    if args.entry == 'pocketjs':
        menu = output/'ESP/EFI/BOOT/pj4edk/config.json'
        menu.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(PROJECT/'config/pocketjs.json', menu)
    dualboot = None
    if args.stage != 'efi':
        source = work/'platform-source'
        if source.exists(): shutil.rmtree(source)
        copy_build_sources(platform, source, ('.git', 'workspace', 'build', 'dist', 'output'))
        simple = work/'simple-init'
        if not simple.exists():
            copy_build_sources(platform/'GPLDrivers/Library/SimpleInit', simple, ('.git', 'build', 'workspace'))
        simple_build = simple/'build'
        simple_build.mkdir(exist_ok=True)
        env.update(CC='aarch64-linux-gnu-gcc', HOSTCC='gcc', CROSS_COMPILE='aarch64-linux-gnu-')
        command(['bash', simple/'scripts/gen-rootfs-source.sh', simple, simple_build], env, logs/'rootfs-assets.log')
        if args.entry == 'pocketjs':
            runtime = work/'pj4edk2'
            command([sys.executable, PROJECT/'build-runtime.py', '--edk2', edk2,
                     '--workdir', runtime, '--source-cache', work/'runtime-source-cache',
                     '--jobs', str(args.jobs)], env, logs/'runtime.log')
            pocket = runtime/'edk-work/Build/PocketJsBoot/RELEASE_GCC5/AARCH64/PocketJsBoot.efi'
            check_pe(pocket)
            shutil.copy2(pocket, source/'tools/PocketJsBoot.efi')
            with (source/'Platform/Xiaomi/sdm845/polaris.fdf.inc').open('a') as f:
                f.write('\n  FILE APPLICATION = edb979d9-310e-4145-950e-ac1691a1010a {\n'
                        '    SECTION PE32 = tools/PocketJsBoot.efi\n'
                        '    SECTION UI = "Polaris PocketJS"\n  }\n')
            destination = output/'ESP/EFI/PocketJS/BOOTAA64.EFI'
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(pocket, destination)
        patch_platform(source, args.entry)
        patch_simple_menu(simple)
        overlay = patch_early_bds(edk2, work, args.entry)
        firmware_work = work/'firmware'
        edk_build(edk2, [overlay, REPO, source, source/'GPLDrivers', simple, source/'Common/edk2-platforms', edk2], firmware_work,
                  ['-p', 'Platform/Xiaomi/sdm845/polaris.dsc', '-n', str(args.jobs),
                   '-D', 'FIRMWARE_VER=polaris-'+source_id, '-D', 'USE_UART=0', '-D', 'FIX_CLANG=0',
                   '-D', 'NO_EXCEPTION_DISPLAY=0', '-D', 'FD_BASE=0xCE000000', '-D', 'FD_SIZE=0x00700000',
                   '-D', 'ENABLE_LINUX_UTILS=0', *includes], env, logs/'firmware.log', source/'tools')
        fd = firmware_work/'Build/polaris/RELEASE_GCC5/FV/SDM845_UEFI.fd'
        if fd.stat().st_size != 0x700000: raise ValueError('Unexpected Polaris FD size')
        shim = work/'bootshim'
        shim.mkdir(exist_ok=True)
        image = work/'polaris-uefi-recovery.img'
        if args.android_boot_image:
            original = args.android_boot_image.read_bytes()
            info = android_dualboot.layout(original, fd.read_bytes())
            defines = {'SHIM_OFFSET': info['shim_offset'], 'KERNEL_ENTRY': info['entry_offset'],
                       'ANDROID_BOOT_SIZE': info['boot_size'], 'ANDROID_BOOT_CRC': info['boot_crc32'],
                       'PACKED_IMAGE_SIZE': info['image_size']}
            command(['aarch64-linux-gnu-gcc', '-c', PROJECT/'platform/AndroidBootShim.S',
                     '-o', shim/'BootShim.o', *[f'-D{k}={v}' for k,v in defines.items()]],
                    env, logs/'bootshim-compile.log')
            command(['aarch64-linux-gnu-ld', '-Ttext=0', '--entry=_Start',
                     '-o', shim/'BootShim.elf', shim/'BootShim.o'], env, logs/'bootshim-link.log')
            command(['aarch64-linux-gnu-objcopy', '-O', 'binary', shim/'BootShim.elf', shim/'BootShim.bin'],
                    env, logs/'bootshim-objcopy.log')
            packed, dualboot = android_dualboot.pack(original, fd.read_bytes(), (shim/'BootShim.bin').read_bytes())
            image.write_bytes(packed)
        else:
            command(['aarch64-linux-gnu-gcc', '-c', source/'tools/BootShim/BootShim.S', '-o', shim/'BootShim.elf',
                     '-DUEFI_BASE=0xCE000000', '-DUEFI_SIZE=0x00700000'], env, logs/'bootshim-compile.log')
            command(['aarch64-linux-gnu-objcopy', '-O', 'binary', shim/'BootShim.elf', shim/'BootShim.bin'], env, logs/'bootshim-objcopy.log')
            kernel = work/'uefi-kernel'
            kernel.write_bytes(gzip.compress((shim/'BootShim.bin').read_bytes()+fd.read_bytes(), mtime=0)
                               +(source/'Platform/Xiaomi/sdm845/FdtBlob_compat/polaris.dtb').read_bytes())
            ramdisk = work/'empty-ramdisk'
            ramdisk.write_bytes(b'')
            command([sys.executable, source/'tools/mkbootimg.py', '--kernel', kernel, '--ramdisk', ramdisk,
                     '--kernel_offset', '0', '--ramdisk_offset', '0', '--tags_offset', '0', '--os_version', '9.0.0',
                     '--os_patch_level', '2020-09', '--header_version', '1', '-o', image], env, logs/'packaging.log')
        if image.stat().st_size >= 64*1024*1024: raise ValueError('Recovery image exceeds partition size')
        shutil.copy2(fd, output/'polaris-uefi.fd')
        shutil.copy2(image, output/image.name)
    artifacts = {}
    names = [f'ESP/EFI/{name}/BOOTAA64.EFI' for name in APPS]
    names += ['tools/return-probe.efi', 'tools/hang-probe.efi', 'tools/ebs-hang-probe.efi',
              'tools/android-prepare.efi', 'tools/linux-inspect.efi', 'tools/linux-prepare.efi',
              'tools/linux-trace.efi', 'tools/linux-native.efi', 'tools/linux-native-prepare.efi', 'tools/linux-native-probe.efi', 'tools/linux-native-pre-exit-probe.efi', 'tools/linux-native-exit-probe.efi', 'tools/linux-native-kernel-probe.efi', 'tools/debug.py']
    if args.entry == 'pocketjs' and args.stage != 'efi':
        names += ['ESP/EFI/PocketJS/BOOTAA64.EFI', 'ESP/EFI/BOOT/pj4edk/config.json']
    if args.stage != 'efi': names += ['polaris-uefi.fd', 'polaris-uefi-recovery.img']
    for name in names:
        p = output/name
        artifacts[name] = {'bytes': p.stat().st_size, 'sha256': digest(p)}
    sources = {}
    for path in sorted(PROJECT.rglob('*')):
        if path.is_file() and path.suffix in ('.c', '.h', '.S', '.inf', '.dsc', '.py', '.cfg', '.json'):
            sources[str(path.relative_to(REPO))] = digest(path)
    receipt = {'schema': 1, 'board': 'polaris', 'stage': args.stage, 'artifacts': artifacts,
               'applications': delivered, 'sources': sources, 'hardware_tested': False,
               'ui': 'PJ4EDK2 runtime' if args.entry == 'pocketjs' else 'Renegade SimpleInit fallback'}
    receipt['handoff'] = {'protocol': 'pj4edk2 optional BootDisplay to DT',
        'requires': 'Polaris BGRX GOP in reserved scanout RAM; matching no-map/write-combine Linux receiver',
        'sdm845_display_compatibility': 'board-specific WT/WC/WB reservation validation; inherited console retires through aperture before MSM DRM modesetting',
        'initrd': 'required; EFI LoadFile2 protocol',
        'hardware_tested': False}
    receipt['recovery'] = {
        'complete': False,
        'usb_drivers': 'platform binary drivers; hardware untested',
        'fastboot': 'USBFn query/download service; hardware untested',
        'partition_flashing': 'complete raw GPT partitions, per-chunk flush/readback; hardware untested; sparse unsupported',
        'ram_read': 'mapped normal RAM only, 64 KiB chunks with CRC32; hardware untested',
        'hardware_watchdog': 'KPSS SDM845 30 seconds; feed during service, arm before EFI and rearm after ExitBootServices cleanup; candidate hardware untested',
        'usb_recovery': 'resident PJ4EDK2 backend plus recovery app; reinitialization retry and reset invalidation; hardware untested',
        'temporary_boot': 'downloaded AArch64 EFI application only; hardware untested',
    }
    receipt['android'] = {
        'profile': 'Polaris MP / SDM845 v2.1',
        'inputs': ['EFI/Android/boot.img', 'EFI/Android/board.dtb'],
        'optional_loader': 'EFI/Android/loader.efi',
        'handoff': 'direct ARM64 kernel and ramdisk from the selected ROM; no BootShim or firmware image binding',
        'dualboot': dualboot,
        'diagnostic': 'android-prepare.efi validates and prepares the same payload without handing off',
        'request_revision': 3,
        'handoff_trace': {'address': '0xcffff000', 'bytes': 64, 'last_pre_kernel_stage': '0x54'},
        'hardware_tested': False,
    }
    if args.entry == 'pocketjs' and args.stage != 'efi':
        receipt['runtime'] = json.loads((work/'pj4edk2/polaris-runtime.json').read_text())
    receipt['windows'] = {
        'chain': 'validated SDM845 ACPI/SoC/CSRT, ReadyToBoot, ACPI-only Microsoft boot manager',
        'requires': 'matching Polaris firmware with Windows handoff policy protocol',
        'watchdog': 'firmware heartbeat until EBS, then release APSS timer to Windows CSRT HAL',
        'kernel_errata': 'original assembly register capture; bounded ARM64 PE plans with cache maintenance',
        'handoff_revision': 2,
        'hardware_tested': False,
    }
    receipt['startup'] = args.entry
    receipt['source_id'] = source_id
    receipt['entry_diagnostics'] = 'POLARIS_ENTRY_DIAG; FV/read, load and application status; explicit Shell only'
    receipt['shared_headers'] = {p.name: digest(p) for p in
        [abi/'pocketjs_boot_context.h', *sorted(linux_efi.glob('*.h'))]}
    (output/'firmware-manifest.json').write_text(json.dumps(receipt, indent=2)+'\n')
    (output/'SHA256SUMS').write_text(''.join(v['sha256']+'  '+n+'\n' for n,v in artifacts.items()))
    print(json.dumps({'output': str(output), 'artifacts': artifacts, 'hardware_tested': False}, indent=2))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage', choices=['all', 'uefi', 'efi'], default='all')
    p.add_argument('--platform', type=Path)
    p.add_argument('--edk2', type=Path)
    p.add_argument('--output', type=Path, default=REPO/'output')
    p.add_argument('--workdir', type=Path, default=REPO/'build')
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--android-boot-image', type=Path,
                   help='original Polaris Android boot image to preserve in the early dualboot payload')
    p.add_argument('--entry', choices=['pocketjs', 'menu', 'recovery'], default='pocketjs',
                   help='built-in entry to launch after console and drivers are connected')
    a = p.parse_args()
    if a.jobs < 1: p.error('--jobs must be positive')
    try: build(a)
    except (OSError, ValueError, RuntimeError) as error: p.exit(1, str(error)+'\n')


if __name__ == '__main__':
    main()
