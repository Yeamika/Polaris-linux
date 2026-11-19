// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../Common/Loader.h"
#include "Handoff.h"
#include "Initrd.h"
#ifdef POLARIS_LINUX_NATIVE
#include "NativeHandoff.h"
#endif
#include "Diagnostics.h"
#ifdef POLARIS_LINUX_TRACE
#include "ExitTrace.h"
#endif
#include "../../platform/BootConsole.h"
#include "pj4edk2_fastboot.h"
#include "pj4edk2_rotate.h"
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Protocol/LoadedImage.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <libfdt.h>

STATIC EFI_STATUS StopHardware(VOID) {
  EFI_GUID UsbGuid=PJ4EDK2_FASTBOOT_GUID,RotateGuid=PJ4EDK2_ROTATE_GUID;
  PJ4EDK2_FASTBOOT_PROTOCOL *Usb;
  PJ4EDK2_ROTATE_PROTOCOL *Rotate;
  EFI_STATUS Status=gBS->LocateProtocol(&UsbGuid,NULL,(VOID **)&Usb);
  if (EFI_ERROR(Status)) return Status;
  if (!Usb || Usb->revision!=PJ4EDK2_FASTBOOT_REVISION ||
      Usb->size<sizeof(*Usb) || !Usb->stop) return EFI_INCOMPATIBLE_VERSION;
  Status=Usb->stop(Usb);
  if (EFI_ERROR(Status)) return Status;
  Status=gBS->LocateProtocol(&RotateGuid,NULL,(VOID **)&Rotate);
  if (Status==EFI_NOT_FOUND) return EFI_SUCCESS;
  if (EFI_ERROR(Status)) return Status;
  if (!Rotate || Rotate->revision!=PJ4EDK2_ROTATE_REVISION ||
      Rotate->size<sizeof(*Rotate) || !Rotate->stop) return EFI_INCOMPATIBLE_VERSION;
  return Rotate->stop(Rotate);
}

STATIC VOID EFIAPI Ready(EFI_EVENT Event,VOID *Context) { (VOID)Event;(VOID)Context; }
STATIC EFI_STATUS ReadyToBoot(VOID) {
  EFI_EVENT Event;
  EFI_STATUS Status,Closed;
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Ready,NULL,&gEfiEventReadyToBootGuid,&Event);
  if (EFI_ERROR(Status)) return Status;
  Status=gBS->SignalEvent(Event);Closed=gBS->CloseEvent(Event);
  return EFI_ERROR(Status)?Status:Closed;
}

#ifndef POLARIS_LINUX_NATIVE
STATIC EFI_STATUS StartLinux(EFI_HANDLE Image,EFI_HANDLE Volume,CHAR16 *Options) {
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_HANDLE Child;
  EFI_STATUS Status;
  UINTN OptionBytes=StrSize(Options);
  if (OptionBytes>MAX_UINT32) return EFI_BAD_BUFFER_SIZE;
  Path=FileDevicePath(Volume,L"\\EFI\\LinuxBoot\\Image.efi");
  if (!Path) return EFI_OUT_OF_RESOURCES;
  Print(L"Linux LoadImage begin\r\n");
  Status=gBS->LoadImage(FALSE,Image,Path,NULL,0,&Child);
  FreePool(Path);
  Print(L"Linux LoadImage result: %r (0x%lx)\r\n",Status,(UINT64)Status);
  if (EFI_ERROR(Status)) return Status;
  Status=gBS->HandleProtocol(Child,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if (EFI_ERROR(Status)) goto Done;
  Loaded->LoadOptions=Options;
  Loaded->LoadOptionsSize=(UINT32)OptionBytes;
  Print(L"Linux loaded base=%016lx size=%lx options=%u\r\n",
        (UINT64)(UINTN)Loaded->ImageBase,Loaded->ImageSize,(UINT32)OptionBytes);
#ifdef POLARIS_LINUX_PREPARE
  Print(L"POLARIS_LINUX_PREPARE_ONLY: kernel loaded, entry not called\r\n");
#else
  gBS->SetWatchdogTimer(0,0,0,NULL);
  Print(L"Linux StartImage begin\r\n");
  Status=gBS->StartImage(Child,NULL,NULL);
  Print(L"Linux StartImage returned: %r (0x%lx)\r\n",Status,(UINT64)Status);
#endif
Done:
  // Exit() can already have unloaded an application. Never dereference Loaded
  // after StartImage; the caller retains the options until this function exits.
  gBS->UnloadImage(Child);
  return Status;
}

#endif

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image, EFI_SYSTEM_TABLE *System) {
  EFI_HANDLE Volume;
  EFI_STATUS Status;
  VOID *Dtb = NULL, *RawDtb = NULL, *Text = NULL, *Previous = NULL,*Initrd=NULL;
  CHAR16 *Options = NULL;
  UINTN Bytes, I,InitrdBytes;
  BOOLEAN Installed = FALSE;
  CONST CHAR16 *Stage=L"stop firmware USB and display workers";
#ifdef POLARIS_LINUX_NATIVE
  UINT64 NativeBootTick,NativeBootNow,NativeBootHz;
  __asm__ volatile("mrs %0, cntpct_el0":"=r"(NativeBootTick));
  __asm__ volatile("mrs %0, cntfrq_el0":"=r"(NativeBootHz));
#endif
  Status=StopHardware();
  if (EFI_ERROR(Status)) goto Done;
  Stage=L"initialize boot-output console";
  Status=PolarisLinuxConsoleBegin(System);
  if (EFI_ERROR(Status)) goto Done;
  Stage=L"locate Linux ESP";
  Status = PolarisFindVolume(Image, L"\\EFI\\LinuxBoot\\Image.efi", &Volume);
  if (EFI_ERROR(Status)) goto Done;
  // Failure to save diagnostics must not make an otherwise bootable ESP fail.
  Status=PolarisLinuxLogBegin(Volume);
  Print(L"Linux ESP log status: %r (0x%lx)\r\n",Status,(UINT64)Status);
  {
    EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
    gBS->RestoreTPL(Tpl);
    Print(L"Linux EFI entry TPL: %u\r\n",(UINT32)Tpl);
  }
  Print(L"POLARIS LINUX BOOT 1\r\nLog records pre-EBS progress, not kernel success.\r\n");
  Stage=L"read device tree";Print(L"Stage: %s\r\n",Stage);
  Status = PolarisReadFile(Volume, L"\\EFI\\LinuxBoot\\polaris.dtb", 2 * 1024 * 1024, &RawDtb, &Bytes);
  if (EFI_ERROR(Status)) goto Done;
  if (Bytes < sizeof(struct fdt_header) || fdt_check_header(RawDtb) ||
      (UINTN)fdt_totalsize(RawDtb) > Bytes ||
      fdt_node_check_compatible(RawDtb, 0, "xiaomi,polaris") != 0) {
    Status = EFI_COMPROMISED_DATA;
    goto Done;
  }
  Dtb = AllocateZeroPool(Bytes + 16384);
  if (Dtb == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  if (fdt_open_into(RawDtb, Dtb, (INT32)(Bytes + 16384))) { Status = EFI_VOLUME_CORRUPTED; goto Done; }
  // A packaged DT must not carry EFI or initrd pointers from an earlier boot.
  {
    CONST CHAR8 *Stale[] = {"linux,initrd-start", "linux,initrd-end", "linux,uefi-system-table",
      "linux,uefi-mmap-start", "linux,uefi-mmap-size", "linux,uefi-mmap-desc-size",
      "linux,uefi-mmap-desc-ver", "kaslr-seed", "rng-seed"};
    INT32 Chosen = fdt_path_offset(Dtb, "/chosen");
    if (Chosen >= 0) for (I=0; I<ARRAY_SIZE(Stale); ++I) {
      INT32 Error = fdt_delprop(Dtb, Chosen, Stale[I]);
      if (Error && Error != -FDT_ERR_NOTFOUND) { Status=EFI_VOLUME_CORRUPTED; goto Done; }
    }
  }
  Stage=L"publish display handoff";Print(L"Stage: %s\r\n",Stage);
  Status = PolarisLinuxDisplayHandoff(Dtb, System);
  if (Status != EFI_NOT_FOUND && EFI_ERROR(Status)) goto Done;
  // Require an explicit kernel command line; do not guess a root partition or reuse old UUIDs.
  Stage=L"read command line";Print(L"Stage: %s\r\n",Stage);
  Status = PolarisReadFile(Volume, L"\\EFI\\LinuxBoot\\cmdline.txt", 4096, &Text, &Bytes);
  if (EFI_ERROR(Status)) goto Done;
  Options = AllocateZeroPool((Bytes + 128) * sizeof(CHAR16));
  if (Options == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  for (I = 0; I < Bytes; ++I) {
    UINT8 Ch = ((UINT8 *)Text)[I];
    if (Ch == '\r' || Ch == '\n' || Ch == '\t') Ch = ' ';
    if (Ch < 0x20 || Ch > 0x7e) { Status = EFI_COMPROMISED_DATA; goto Done; }
    Options[I] = Ch;
  }
#ifdef POLARIS_LINUX_TRACE
  // The stub has its own log level: the kernel's loglevel= does not enable
  // its informational messages. Keep tracing explicit in this separate tool.
  StrCatS(Options,Bytes+128,L" efi=debug earlycon=efifb");
#endif
  Print(L"Command line: ");
  for (I=0;Options[I];) {
    UINTN End=I;
    CHAR16 Saved;
    while (Options[End] && End<I+96) ++End;
    Saved=Options[End];Options[End]=0;
    Print(L"%s",Options+I);
    Options[End]=Saved;I=End;
  }
  Print(L"\r\n");
  Stage=L"publish initramfs LoadFile2";Print(L"Stage: %s\r\n",Stage);
  Status=PolarisReadFile(Volume,L"\\EFI\\LinuxBoot\\initrd.img",128*1024*1024,&Initrd,&InitrdBytes);
  if (EFI_ERROR(Status)) goto Done;
  if (Initrd) {
    Status=PolarisInitrdInstall(Initrd,InitrdBytes);
    if (EFI_ERROR(Status)) goto Done;
  }
  EfiGetSystemConfigurationTable(&gFdtTableGuid, &Previous);
  Status = gBS->InstallConfigurationTable(&gFdtTableGuid, Dtb);
  if (EFI_ERROR(Status)) goto Done;
  Installed = TRUE;
  Stage=L"ReadyToBoot event";Print(L"Stage: %s\r\n",Stage);
  Status=ReadyToBoot();
  if (EFI_ERROR(Status)) goto Done;
#ifdef POLARIS_LINUX_NATIVE
  Stage=L"start native Linux handoff";
#else
  Stage=L"start Linux EFI stub";
#endif
  Print(L"Stage: %s; initramfs bytes: %u\r\n",Stage,InitrdBytes);
#ifdef POLARIS_LINUX_TRACE
  Status=PolarisLinuxExitTraceBegin();
  Print(L"Linux exit trace journal: %r\r\n",Status);
  if (EFI_ERROR(Status)) goto Done;
#endif
#ifdef POLARIS_LINUX_NATIVE
  __asm__ volatile("mrs %0, cntpct_el0":"=r"(NativeBootNow));
  Print(L"Native preparation before raw Image: %lu ms\r\n",
    NativeBootHz?(NativeBootNow-NativeBootTick)*1000/NativeBootHz:0);
  Status=PolarisLinuxNativeStart(Image,Volume,Dtb,Options,Initrd,InitrdBytes,
#ifdef POLARIS_LINUX_NATIVE_PREPARE
    TRUE
#else
    FALSE
#endif
  );
#else
  Status=StartLinux(Image,Volume,Options);
#endif
#ifdef POLARIS_LINUX_TRACE
  PolarisLinuxExitTraceEnd(Status);
#endif
Done:
  Print(L"Polaris LinuxBoot returned at %s: %r (0x%lx)\r\n",Stage,Status,(UINT64)Status);
  PolarisLinuxLogEnd();
  if (Initrd) {
    EFI_STATUS Remove=PolarisInitrdRemove();
    // A published provider must never reference freed memory if uninstall fails.
    if (EFI_ERROR(Remove)) {
      // The provider contains callbacks into this image: it cannot be unloaded.
      gRT->ResetSystem(EfiResetCold,Remove,0,NULL);
      for (;;) gBS->Stall(1000000);
    }
    else FreePool(Initrd);
  }
  // Only reached if loading failed or the child exited. Never leave a dangling DT pointer.
  if (Installed) {
    EFI_STATUS Restore = gBS->InstallConfigurationTable(&gFdtTableGuid, Previous);
    if (EFI_ERROR(Restore)) {
      // Preserve memory still referenced by the firmware table on restoration failure.
      Dtb = NULL;
      Status = Restore;
    }
  }
  if (Options != NULL) FreePool(Options);
  if (Text != NULL) FreePool(Text);
  if (Dtb != NULL) FreePool(Dtb);
  if (RawDtb != NULL) FreePool(RawDtb);
  return Status;
}
