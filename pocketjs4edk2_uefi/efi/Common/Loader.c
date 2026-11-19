// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Loader.h"
#include <Guid/FileInfo.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_STATUS Open(EFI_HANDLE Volume, CONST CHAR16 *Path, EFI_FILE_PROTOCOL **File) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root;
  EFI_STATUS Status;
  *File = NULL;
  Status = gBS->HandleProtocol(Volume, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR(Status)) return Status;
  Status = Fs->OpenVolume(Fs, &Root);
  if (EFI_ERROR(Status)) return Status;
  Status = Root->Open(Root, File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  Root->Close(Root);
  return Status;
}

EFI_STATUS PolarisFindVolume(EFI_HANDLE Image, CONST CHAR16 *Path, EFI_HANDLE *Volume) {
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_HANDLE *Handles;
  EFI_FILE_PROTOCOL *File;
  EFI_STATUS Status;
  UINTN Count, I;
  *Volume = NULL;
  Status = gBS->HandleProtocol(Image, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (!EFI_ERROR(Status) && !EFI_ERROR(Open(Loaded->DeviceHandle, Path, &File))) {
    File->Close(File);
    *Volume = Loaded->DeviceHandle;
    return EFI_SUCCESS;
  }
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return Status;
  Status = EFI_NOT_FOUND;
  for (I = 0; I < Count; ++I) {
    if (EFI_ERROR(Open(Handles[I], Path, &File))) continue;
    File->Close(File);
    if (*Volume != NULL) {
      *Volume = NULL;
      Status = EFI_NO_MAPPING;
      break;
    }
    *Volume = Handles[I];
    Status = EFI_SUCCESS;
  }
  FreePool(Handles);
  return Status;
}

EFI_STATUS PolarisReadFile(EFI_HANDLE Volume, CONST CHAR16 *Path, UINTN Limit, VOID **Data, UINTN *Size) {
  EFI_FILE_PROTOCOL *File;
  EFI_FILE_INFO *Info = NULL;
  EFI_STATUS Status;
  UINTN InfoSize = 0, Bytes;
  *Data = NULL;
  *Size = 0;
  Status = Open(Volume, Path, &File);
  if (EFI_ERROR(Status)) return Status;
  Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL || InfoSize < SIZE_OF_EFI_FILE_INFO || InfoSize > 65536) {
    Status = EFI_VOLUME_CORRUPTED;
    goto Done;
  }
  Info = AllocatePool(InfoSize);
  if (Info == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, Info);
  if (EFI_ERROR(Status)) goto Done;
  if ((Info->Attribute & EFI_FILE_DIRECTORY) || Info->FileSize == 0 || Info->FileSize > Limit) {
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }
  Bytes = (UINTN)Info->FileSize;
  *Data = AllocateZeroPool(Bytes + 1);
  if (*Data == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  *Size = Bytes;
  Status = File->Read(File, &Bytes, *Data);
  if (!EFI_ERROR(Status) && Bytes != *Size) Status = EFI_DEVICE_ERROR;
Done:
  if (Info != NULL) FreePool(Info);
  File->Close(File);
  if (EFI_ERROR(Status) && *Data != NULL) { FreePool(*Data); *Data = NULL; *Size = 0; }
  return Status;
}

EFI_STATUS PolarisStartImage(EFI_HANDLE Parent, EFI_HANDLE Volume, CONST CHAR16 *Path, CONST CHAR16 *Options) {
  EFI_DEVICE_PATH_PROTOCOL *DevicePath;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_HANDLE Child;
  EFI_STATUS Status;
  VOID *Copy = NULL;
  UINTN Size;
  DevicePath = FileDevicePath(Volume, (CHAR16 *)Path);
  if (DevicePath == NULL) return EFI_OUT_OF_RESOURCES;
  Status = gBS->LoadImage(FALSE, Parent, DevicePath, NULL, 0, &Child);
  FreePool(DevicePath);
  if (EFI_ERROR(Status)) return Status;
  if (Options != NULL) {
    Size = StrSize(Options);
    if (Size > MAX_UINT32) { Status = EFI_BAD_BUFFER_SIZE; goto Done; }
    Copy = AllocateCopyPool(Size, Options);
    if (Copy == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
    Status = gBS->HandleProtocol(Child, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
    if (EFI_ERROR(Status)) goto Done;
    Loaded->LoadOptions = Copy;
    Loaded->LoadOptionsSize = (UINT32)Size;
  }
  gBS->SetWatchdogTimer(0, 0, 0, NULL);
  Status = gBS->StartImage(Child, NULL, NULL);
Done:
  // An application may already have been unloaded by Exit(); do not dereference it here.
  gBS->UnloadImage(Child);
  if (Copy != NULL) FreePool(Copy);
  return Status;
}
