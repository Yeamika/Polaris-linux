// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "EarlyBoot.h"
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

// Qualcomm EFI_SPMI_PROTOCOL revision 0x10000. Preserve the complete ABI
// prefix, including unused methods, and reject other revisions.
typedef struct POLARIS_SPMI POLARIS_SPMI;
typedef UINT32 (EFIAPI *POLARIS_SPMI_READ)(POLARIS_SPMI *, UINT32, UINT32, UINT32,
                                         UINT8 *, UINT32, UINT32 *);
typedef UINT32 (EFIAPI *POLARIS_SPMI_WRITE)(POLARIS_SPMI *, UINT32, UINT32, UINT32,
                                          UINT8 *, UINT32);
struct POLARIS_SPMI {
  UINT64 Revision;
  POLARIS_SPMI_READ Read;
  POLARIS_SPMI_WRITE Write;
  VOID *Rmw, *Command;
};
STATIC EFI_GUID SpmiGuid={0xfa5f306b,0xf47d,0x4ac4,{0xa4,0x7d,0x88,0x2f,0x82,0x04,0xec,0x30}};
#define POLARIS_PS_HOLD 0x0c264000

STATIC EFI_STATUS Read(POLARIS_SPMI *P, UINT32 Sid, UINT32 Reg, UINT8 *Value) {
  UINT32 Bytes=0, Result;
  *Value=0;
  Result=P->Read(P,Sid,0,Reg,Value,1,&Bytes);
  return Result || Bytes!=1 ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS Write(POLARIS_SPMI *P, UINT32 Reg, UINT8 Value) {
  UINT8 Back=0;
  if (P->Write(P,0,0,Reg,&Value,1) || EFI_ERROR(Read(P,0,Reg,&Back)) || Back!=Value)
    return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS BoardGuard(POLARIS_SPMI *P, UINT8 ResetType) {
  CONST UINT32 Regs[]={0x104,0x105,0x804,0x805,0x85a,0x85b};
  CONST UINT8 Expected[3][6]={{0x51,0x14,1,4,ResetType,0x80},
                             {0x51,0x15,1,5,4,0}, {0x51,0x18,1,5,4,0}};
  UINTN I,J; UINT8 Value;
  if (!P || P->Revision!=0x10000 || !P->Read || !P->Write) return EFI_UNSUPPORTED;
  for (I=0;I<3;++I) for (J=0;J<6;++J) {
    if (EFI_ERROR(Read(P,(UINT32)I*2,Regs[J],&Value)) || Value!=Expected[I][J])
      return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ReadBinding(POLARIS_ANDROID_BINDING *Binding) {
  EFI_MEMORY_DESCRIPTOR *Map=NULL,*D;
  UINTN Size=0,Key,Stride,Offset,Capacity; UINT32 Version;
  EFI_STATUS Status; BOOLEAN Mapped=FALSE;
  Status=gBS->GetMemoryMap(&Size,NULL,&Key,&Stride,&Version);
  if (Status!=EFI_BUFFER_TOO_SMALL || Stride<sizeof(*Map) || Size>1024*1024 || Stride>4096)
    return EFI_UNSUPPORTED;
  Capacity=Size+2*Stride;
  Map=AllocatePool(Capacity);
  if (!Map) return EFI_OUT_OF_RESOURCES;
  Size=Capacity;
  Status=gBS->GetMemoryMap(&Size,Map,&Key,&Stride,&Version);
  if (!EFI_ERROR(Status) && Size<=Capacity && Stride>=sizeof(*Map)) {
    for (Offset=0;Size-Offset>=Stride;Offset+=Stride) {
      D=(EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map+Offset);
      if (D->PhysicalStart<=POLARIS_ANDROID_BINDING_ADDRESS &&
          D->NumberOfPages<=MAX_UINT64/EFI_PAGE_SIZE &&
          POLARIS_ANDROID_BINDING_ADDRESS-D->PhysicalStart<=D->NumberOfPages*EFI_PAGE_SIZE &&
          D->NumberOfPages*EFI_PAGE_SIZE-(POLARIS_ANDROID_BINDING_ADDRESS-D->PhysicalStart)>=sizeof(*Binding) &&
          D->Type==EfiBootServicesData && (D->Attribute&EFI_MEMORY_WB)) {
        Mapped=TRUE;break;
      }
    }
  }
  FreePool(Map);
  if (!Mapped) return EFI_UNSUPPORTED;
  CopyMem(Binding,(CONST VOID *)(UINTN)POLARIS_ANDROID_BINDING_ADDRESS,sizeof(*Binding));
  return PolarisAndroidBindingValid(Binding) ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC VOID ClearRequest(volatile UINT16 *Flags) {
  UINTN I;
  for (I=0;I<POLARIS_ANDROID_FLAG_COUNT;++I) Flags[I]=0;
  WriteBackInvalidateDataCacheRange((VOID *)Flags,POLARIS_ANDROID_FLAG_COUNT*sizeof(*Flags));
}

STATIC EFI_STATUS ConfigureWarmReset(POLARIS_SPMI *P) {
  EFI_STATUS Status,Restore;
  Status=Write(P,0x85b,0);
  if (!EFI_ERROR(Status)) { gBS->Stall(500);Status=Write(P,0x85a,1); }
  if (!EFI_ERROR(Status)) Status=Write(P,0x85b,0x80);
  if (!EFI_ERROR(Status)) Status=BoardGuard(P,1);
  if (!EFI_ERROR(Status)) return EFI_SUCCESS;
  Restore=Write(P,0x85b,0);gBS->Stall(500);
  if (EFI_ERROR(Write(P,0x85a,7))) Restore=EFI_DEVICE_ERROR;
  if (EFI_ERROR(Write(P,0x85b,0x80))) Restore=EFI_DEVICE_ERROR;
  Print(L"Android: warm reset preparation failed; hard-reset restore %r\n",Restore);
  return Status;
}

EFI_STATUS PolarisAndroidEarlyBoot(CONST VOID *Boot, UINTN Size) {
  POLARIS_ANDROID_BINDING Binding;
  POLARIS_SPMI *P=NULL;
  EFI_PHYSICAL_ADDRESS Address=POLARIS_ANDROID_FLAG_ADDRESS;
  volatile UINT16 *Flags=(volatile UINT16 *)(UINTN)Address;
  UINT32 Crc; UINTN I; EFI_STATUS Status;
  Status=ReadBinding(&Binding);
  if (EFI_ERROR(Status)) {
    Print(L"Android: matching early BootShim is not installed (%r)\n",Status);
    return Status;
  }
  if (!Boot || Size!=Binding.BootSize) return EFI_COMPROMISED_DATA;
  Status=gBS->CalculateCrc32((VOID *)Boot,Size,&Crc);
  if (EFI_ERROR(Status)) return Status;
  if (Crc!=Binding.BootCrc32) {
    Print(L"Android: ESP boot.img differs from the embedded native payload\n");
    return EFI_COMPROMISED_DATA;
  }
  Status=gBS->LocateProtocol(&SpmiGuid,NULL,(VOID **)&P);
  if (EFI_ERROR(Status)) return Status;
  Status=BoardGuard(P,7);
  if (EFI_ERROR(Status)) return Status;
  if (MmioRead32(POLARIS_PS_HOLD)!=1) return EFI_UNSUPPORTED;
  Status=gBS->AllocatePages(AllocateAddress,EfiLoaderData,1,&Address);
  if (EFI_ERROR(Status)) return Status;
  for (I=0;I<POLARIS_ANDROID_FLAG_COUNT;++I) Flags[I]=POLARIS_ANDROID_FLAG_VALUE;
  WriteBackInvalidateDataCacheRange((VOID *)Flags,POLARIS_ANDROID_FLAG_COUNT*sizeof(*Flags));
  for (I=0;I<POLARIS_ANDROID_FLAG_COUNT;++I) if (Flags[I]!=POLARIS_ANDROID_FLAG_VALUE) {
    ClearRequest(Flags);gBS->FreePages(Address,1);return EFI_DEVICE_ERROR;
  }

  // Only the DT-designated system-reset PMIC is enabled on this board.
  // Disable PS_HOLD reset, select warm type, then re-enable, checking every
  // write. Secondary PMIC settings are guarded and never changed.
  Status=ConfigureWarmReset(P);
  if (EFI_ERROR(Status)) {
    ClearRequest(Flags);
    gBS->FreePages(Address,1);
    return Status;
  }
  Print(L"Android: ESP image matched; early BootShim request 128/128; PMIC warm reset\n");
  gBS->Stall(100000);
  MmioWrite32(POLARIS_PS_HOLD,0);
  ArmDataSynchronizationBarrier();
  // The resident hardware watchdog remains the fallback if PS_HOLD fails.
  for (;;) CpuPause();
}
