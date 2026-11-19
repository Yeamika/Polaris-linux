// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Initrd.h"
#include <Guid/LinuxEfiInitrdMedia.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadFile2.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} InitrdPath={
  {{MEDIA_DEVICE_PATH,MEDIA_VENDOR_DP,{sizeof(VENDOR_DEVICE_PATH),0}},LINUX_EFI_INITRD_MEDIA_GUID},
  {END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{sizeof(EFI_DEVICE_PATH_PROTOCOL),0}}
};
STATIC EFI_HANDLE InitrdHandle;
STATIC VOID *InitrdData;
STATIC UINTN InitrdSize;
STATIC EFI_STATUS EFIAPI LoadInitrd(EFI_LOAD_FILE2_PROTOCOL *This,EFI_DEVICE_PATH_PROTOCOL *Path,
                                  BOOLEAN BootPolicy,UINTN *Size,VOID *Buffer);
STATIC EFI_LOAD_FILE2_PROTOCOL InitrdProtocol={LoadInitrd};
STATIC EFI_STATUS EFIAPI LoadInitrd(EFI_LOAD_FILE2_PROTOCOL *This,EFI_DEVICE_PATH_PROTOCOL *Path,
                                  BOOLEAN BootPolicy,UINTN *Size,VOID *Buffer) {
  if (This!=&InitrdProtocol || !Path || !Size) return EFI_INVALID_PARAMETER;
  if (BootPolicy) return EFI_UNSUPPORTED;
  if (Path->Type!=END_DEVICE_PATH_TYPE || Path->SubType!=END_ENTIRE_DEVICE_PATH_SUBTYPE ||
      Path->Length[0]!=sizeof(*Path) || Path->Length[1] || !InitrdData) return EFI_NOT_FOUND;
  if (!Buffer || *Size<InitrdSize) { *Size=InitrdSize;return EFI_BUFFER_TOO_SMALL; }
  CopyMem(Buffer,InitrdData,InitrdSize);*Size=InitrdSize;return EFI_SUCCESS;
}
EFI_STATUS PolarisInitrdInstall(VOID *Data,UINTN Size) {
  EFI_DEVICE_PATH_PROTOCOL *Path=(VOID *)&InitrdPath;EFI_HANDLE Existing;EFI_STATUS Status;
  if (!Data || !Size) return EFI_INVALID_PARAMETER;
  if (InitrdHandle) return EFI_ALREADY_STARTED;
  Status=gBS->LocateDevicePath(&gEfiLoadFile2ProtocolGuid,&Path,&Existing);
  if (!EFI_ERROR(Status) && Path->Type==END_DEVICE_PATH_TYPE) return EFI_ALREADY_STARTED;
  InitrdData=Data;InitrdSize=Size;
  Status=gBS->InstallMultipleProtocolInterfaces(&InitrdHandle,
    &gEfiDevicePathProtocolGuid,&InitrdPath,&gEfiLoadFile2ProtocolGuid,&InitrdProtocol,NULL);
  if (EFI_ERROR(Status)) { InitrdHandle=NULL;InitrdData=NULL;InitrdSize=0; }
  return Status;
}
EFI_STATUS PolarisInitrdRemove(VOID) {
  EFI_STATUS Status;
  if (!InitrdHandle) return EFI_SUCCESS;
  Status=gBS->UninstallMultipleProtocolInterfaces(InitrdHandle,
    &gEfiDevicePathProtocolGuid,&InitrdPath,&gEfiLoadFile2ProtocolGuid,&InitrdProtocol,NULL);
  if (!EFI_ERROR(Status)) { InitrdHandle=NULL;InitrdData=NULL;InitrdSize=0; }
  return Status;
}
