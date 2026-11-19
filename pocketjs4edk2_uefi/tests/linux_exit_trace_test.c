// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char Storage[4096] __attribute__((aligned(4096)));
#define POLARIS_LINUX_TRACE_ADDRESS ((UINTN)Storage)
#undef NULL
#include "../efi/LinuxBoot/ExitTrace.c"

static EFI_MEMORY_DESCRIPTOR Descriptor;
static EFI_STATUS ExitResult,CrcResult;
static UINTN Allocations,ExitCalls;
static BOOLEAN BadStride;
VOID *EFIAPI WriteBackDataCacheRange(VOID *Buffer,UINTN Size) {
  assert(Buffer==Storage && Size==sizeof(POLARIS_LINUX_EXIT_TRACE));return Buffer;
}
VOID *EFIAPI AllocatePool(UINTN Size) { ++Allocations;return malloc(Size); }
VOID EFIAPI FreePool(VOID *Buffer) { free(Buffer); }
VOID *EFIAPI CopyMem(VOID *Dest,CONST VOID *Src,UINTN Size) { return memcpy(Dest,Src,Size); }
VOID *EFIAPI ZeroMem(VOID *Buffer,UINTN Size) { return memset(Buffer,0,Size); }
static EFI_STATUS EFIAPI GetMap(UINTN *Size,EFI_MEMORY_DESCRIPTOR *Out,UINTN *Key,UINTN *Step,UINT32 *Version) {
  *Key=42;*Step=sizeof(Descriptor);*Version=1;
  if(!Out) {*Size=sizeof(Descriptor);return EFI_BUFFER_TOO_SMALL;}
  *Size=sizeof(Descriptor);*Out=Descriptor;
  if(BadStride) *Step=sizeof(Descriptor)-1;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Exit(EFI_HANDLE Image,UINTN Key) {
  assert(Image==(VOID *)1 && Key==42 && Journal->Phase==2);
  ++ExitCalls;return ExitResult;
}
static EFI_STATUS EFIAPI Crc(VOID *Data,UINTN Size,UINT32 *Value) {
  assert(Data==gBS && Size==sizeof(*gBS));*Value=0x12345678;return CrcResult;
}
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
static void Setup(void) {
  memset(Storage,0,sizeof(Storage));
  Services=(EFI_BOOT_SERVICES){.Hdr={.HeaderSize=sizeof(Services),.CRC32=17},
    .GetMemoryMap=GetMap,.ExitBootServices=Exit,.CalculateCrc32=Crc};
  Descriptor=(EFI_MEMORY_DESCRIPTOR){.Type=EfiReservedMemoryType,
    .PhysicalStart=(UINTN)Storage,.NumberOfPages=1,.Attribute=EFI_MEMORY_WT};
  ExitResult=CrcResult=EFI_SUCCESS;OriginalExit=NULL;BadStride=FALSE;ExitCalls=Allocations=0;
}
int main(void) {
  POLARIS_LINUX_EXIT_TRACE Out;
  Setup();assert(PolarisLinuxExitTraceBegin()==EFI_SUCCESS);
  assert(gBS->ExitBootServices!=Exit && Journal->Phase==1 && Journal->Sequence==1);
  assert(PolarisLinuxExitTraceBegin()==EFI_ALREADY_STARTED);
  UINTN Before=Allocations;
  ExitResult=EFI_INVALID_PARAMETER;
  assert(gBS->ExitBootServices((VOID *)1,42)==EFI_INVALID_PARAMETER);
  assert(Journal->Phase==4 && Journal->Calls==1 && Allocations==Before);
  ExitResult=EFI_SUCCESS;
  assert(gBS->ExitBootServices((VOID *)1,42)==EFI_SUCCESS);
  assert(Journal->Phase==3 && Journal->Calls==2 && Allocations==Before && ExitCalls==2);
  assert(PolarisLinuxExitTraceRead(&Out)==EFI_SUCCESS && Out.Phase==3);
  PolarisLinuxExitTraceEnd(EFI_ABORTED);
  assert(gBS->ExitBootServices==Exit && Out.Size==sizeof(Out) && Journal->Phase==5);
  assert(Journal->ReturnStatus==EFI_ABORTED);
  assert(PolarisLinuxExitTraceBegin()==EFI_SUCCESS && Journal->Sequence==2);
  PolarisLinuxExitTraceEnd(EFI_SUCCESS);

  Setup();Descriptor.Type=EfiConventionalMemory;
  assert(PolarisLinuxExitTraceBegin()==EFI_NO_MAPPING && !Journal->Magic && gBS->ExitBootServices==Exit);
  Setup();Descriptor.Attribute|=EFI_MEMORY_RO;
  assert(PolarisLinuxExitTraceBegin()==EFI_NO_MAPPING && !Journal->Magic);
  Setup();BadStride=TRUE;
  assert(PolarisLinuxExitTraceBegin()==EFI_NO_MAPPING && !Journal->Magic);
  Setup();CrcResult=EFI_DEVICE_ERROR;
  assert(PolarisLinuxExitTraceBegin()==EFI_DEVICE_ERROR && !OriginalExit);
  assert(gBS->ExitBootServices==Exit && gBS->Hdr.CRC32==17);
  puts("PASS: reserved-page guards, EBS retries, map-preserving journal and hook restoration");
  return 0;
}
