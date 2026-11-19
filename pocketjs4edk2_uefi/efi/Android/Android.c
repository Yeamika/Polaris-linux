// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../Common/Loader.h"
#include "BootRequest.h"
#include <libfdt.h>
#include "NativeFdt.h"
#include <PiDxe.h>
#include <Protocol/LoadedImage.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#define ANDROID_BOOT_LIMIT (64U * 1024U * 1024U)
#define ANDROID_LOADER_LIMIT (16U * 1024U * 1024U)
STATIC CONST CHAR16 BootPath[] = L"\\EFI\\Android\\boot.img";
STATIC CONST CHAR16 DtbPath[] = L"\\EFI\\Android\\board.dtb";
STATIC CONST CHAR16 LoaderPath[] = L"\\EFI\\Android\\loader.efi";
STATIC EFI_GUID SimpleInit = { 0x6d77b2bb, 0x69eb, 0x42ab,
  { 0xbe, 0xcf, 0x4f, 0x40, 0xc8, 0x95, 0x68, 0xc3 } };

STATIC EFI_STATUS StartAndroid(EFI_HANDLE Image, EFI_HANDLE Volume, POLARIS_ANDROID_REQUEST *Request) {
  EFI_HANDLE Child = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  VOID *Loader = NULL;
  UINTN LoaderSize = 0;
  EFI_STATUS Status;

  // A paired loader supplied on the ESP can be updated without replacing the firmware.
  Status = Volume ? PolarisReadFile(Volume, LoaderPath, ANDROID_LOADER_LIMIT, &Loader, &LoaderSize) : EFI_NOT_FOUND;
  if (Status == EFI_NOT_FOUND)
    Status = GetSectionFromAnyFv(&SimpleInit, EFI_SECTION_PE32, 0, &Loader, &LoaderSize);
  if (EFI_ERROR(Status)) {
    Print(L"Android: native kernel loader unavailable (%r)\n", Status);
    return Status;
  }

  Status = gBS->LoadImage(FALSE, Image, NULL, Loader, LoaderSize, &Child);
  FreePool(Loader);
  if (EFI_ERROR(Status)) {
    Print(L"Android: could not load the native kernel loader (%r)\n", Status);
    return Status;
  }
  Status = gBS->HandleProtocol(Child, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR(Status)) {
    gBS->UnloadImage(Child);
    return Status;
  }
  Loaded->LoadOptions = Request;
  Loaded->LoadOptionsSize = sizeof(*Request);
  Print(L"Android: direct ROM kernel and ramdisk from boot.img\n");
  return gBS->StartImage(Child, NULL, NULL);
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image, EFI_SYSTEM_TABLE *System) {
  EFI_HANDLE Volume = NULL;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  VOID *Boot = NULL, *Dtb = NULL;
  UINTN BootSize = 0, DtbSize = 0;
  BOOLEAN FromRam = FALSE;
  POLARIS_ANDROID_REQUEST Request = { POLARIS_ANDROID_MAGIC, POLARIS_ANDROID_REVISION, sizeof(Request) };
  EFI_STATUS Status;
  (VOID)System;

  // An EFI caller may provide the same versioned request from RAM. The Android
  // card normally passes no binary request and uses the files on its own ESP.
  Status = gBS->HandleProtocol(Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (!EFI_ERROR(Status) && Loaded->LoadOptions && Loaded->LoadOptionsSize == sizeof(Request)) {
    POLARIS_ANDROID_REQUEST Input;
    CopyMem(&Input, Loaded->LoadOptions, sizeof(Input));
    if (Input.Magic == POLARIS_ANDROID_MAGIC) {
      if (!PolarisAndroidRequestValid(&Input, sizeof(Input))) return EFI_INVALID_PARAMETER;
      FromRam = TRUE;
      Boot = (VOID *)(UINTN)Input.Image;
      BootSize = (UINTN)Input.ImageSize;
      DtbSize = (UINTN)Input.DtbSize;
      Dtb = AllocateCopyPool(DtbSize, (CONST VOID *)(UINTN)Input.Dtb);
      if (!Dtb) return EFI_OUT_OF_RESOURCES;
      Request.Flags = Input.Flags;
    }
  }
  if (!FromRam) {
    Status = PolarisFindVolume(Image, BootPath, &Volume);
    if (!EFI_ERROR(Status))
      Status = PolarisReadFile(Volume, BootPath, ANDROID_BOOT_LIMIT, &Boot, &BootSize);
    if (EFI_ERROR(Status)) {
      Print(L"Android: cannot read %s (%r)\n", BootPath, Status);
      goto Done;
    }
    Status = PolarisReadFile(Volume, DtbPath, POLARIS_ANDROID_DTB_LIMIT, &Dtb, &DtbSize);
    if (EFI_ERROR(Status)) {
      Print(L"Android: cannot read %s; supply a native device tree for this ROM (%r)\n", DtbPath, Status);
      goto Done;
    }
  }

  Status = EFI_COMPROMISED_DATA;
  if (!PolarisAndroidImageValid(Boot, BootSize)) {
    Print(L"Android: invalid or unsupported boot.img; expected a Polaris legacy v0/v1 image\n");
    goto Done;
  }
  if (!PolarisAndroidDtbValid(Dtb, DtbSize) || !PolarisAndroidDtbSanitize(Dtb)) {
    Print(L"Android: device tree does not describe the supported Polaris board and native boot state\n");
    goto Done;
  }
  Request.Image = (UINTN)Boot;
  Request.ImageSize = BootSize;
  Request.Dtb = (UINTN)Dtb;
  Request.DtbSize = DtbSize;
#ifdef POLARIS_ANDROID_DIAGNOSTIC
  Request.Flags |= POLARIS_ANDROID_PREPARE_ONLY;
#endif
  Status = StartAndroid(Image, Volume, &Request);

Done:
  if (Dtb) FreePool(Dtb);
  if (Boot && !FromRam) FreePool(Boot);
  Print(L"Android loader returned: %r\n", Status);
  return Status;
}
