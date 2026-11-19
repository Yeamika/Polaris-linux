// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../efi/LinuxBoot/Initrd.c"
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiDevicePathProtocolGuid,gEfiLoadFile2ProtocolGuid;
static EFI_STATUS InstallStatus,RemoveStatus;
static BOOLEAN ExistingProvider;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) { return memcpy(D,S,N); }
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,EFI_DEVICE_PATH_PROTOCOL **Path,EFI_HANDLE *H) {
  assert(G==&gEfiLoadFile2ProtocolGuid);*H=(VOID *)1;
  if(ExistingProvider) { *Path=&InitrdPath.End;return EFI_SUCCESS; }
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...) { *H=(VOID *)2;return InstallStatus; }
static EFI_STATUS EFIAPI Remove(EFI_HANDLE H,...) { assert(H==(VOID *)2);return RemoveStatus; }
int main(void) {
  EFI_BOOT_SERVICES Bs={0};UINT8 Data[4]={1,2,3,4},Copy[4];UINTN N=0;
  Bs.LocateDevicePath=Locate;Bs.InstallMultipleProtocolInterfaces=Install;
  Bs.UninstallMultipleProtocolInterfaces=Remove;gBS=&Bs;
  ExistingProvider=TRUE;assert(PolarisInitrdInstall(Data,4)==EFI_ALREADY_STARTED && !InitrdHandle);
  ExistingProvider=FALSE;InstallStatus=EFI_DEVICE_ERROR;
  assert(PolarisInitrdInstall(Data,4)==EFI_DEVICE_ERROR && !InitrdData);
  InstallStatus=EFI_SUCCESS;assert(PolarisInitrdInstall(Data,4)==EFI_SUCCESS);
  assert(LoadInitrd(&InitrdProtocol,&InitrdPath.End,TRUE,&N,NULL)==EFI_UNSUPPORTED);
  assert(LoadInitrd(&InitrdProtocol,&InitrdPath.Vendor.Header,FALSE,&N,NULL)==EFI_NOT_FOUND);
  assert(LoadInitrd(&InitrdProtocol,&InitrdPath.End,FALSE,&N,NULL)==EFI_BUFFER_TOO_SMALL && N==4);
  N=3;assert(LoadInitrd(&InitrdProtocol,&InitrdPath.End,FALSE,&N,Copy)==EFI_BUFFER_TOO_SMALL && N==4);
  assert(LoadInitrd(&InitrdProtocol,&InitrdPath.End,FALSE,&N,Copy)==EFI_SUCCESS && !memcmp(Data,Copy,4));
  RemoveStatus=EFI_DEVICE_ERROR;assert(PolarisInitrdRemove()==EFI_DEVICE_ERROR && InitrdData==Data);
  RemoveStatus=EFI_SUCCESS;assert(PolarisInitrdRemove()==EFI_SUCCESS && !InitrdData && !InitrdHandle);
  puts("PASS: Linux initrd LoadFile2 size negotiation, bytes, conflicts and provider lifetime");
  return 0;
}
