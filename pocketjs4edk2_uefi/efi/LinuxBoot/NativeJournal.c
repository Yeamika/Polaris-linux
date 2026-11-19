// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeJournal.h"
#include "NativeMemory.h"
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_STATUS ReadMap(VOID **Map,UINTN *Bytes,UINTN *Step) {
  UINTN Key;UINT32 Version;EFI_STATUS Status;
  *Bytes=0;*Map=NULL;
  Status=gBS->GetMemoryMap(Bytes,NULL,&Key,Step,&Version);
  if(Status!=EFI_BUFFER_TOO_SMALL || *Step<sizeof(EFI_MEMORY_DESCRIPTOR) || *Step>4096 || *Bytes>1024*1024)
    return EFI_NO_MAPPING;
  *Bytes+=64*(*Step);*Map=AllocatePool(*Bytes);if(!*Map) return EFI_OUT_OF_RESOURCES;
  Status=gBS->GetMemoryMap(Bytes,*Map,&Key,Step,&Version);
  if(!EFI_ERROR(Status) && (*Step<sizeof(EFI_MEMORY_DESCRIPTOR) || *Bytes%(*Step))) Status=EFI_NO_MAPPING;
  if(EFI_ERROR(Status)) {FreePool(*Map);*Map=NULL;}
  return Status;
}
EFI_STATUS PolarisNativeJournalCreate(POLARIS_NATIVE_JOURNAL **Out) {
  VOID *Map;UINTN Bytes,Step,I;EFI_MEMORY_DESCRIPTOR *D;EFI_STATUS Status;
  EFI_PHYSICAL_ADDRESS Address=0;UINT64 MostPages=0;
  if(!Out) return EFI_INVALID_PARAMETER;
  *Out=NULL;Status=ReadMap(&Map,&Bytes,&Step);if(EFI_ERROR(Status)) return Status;
  for(I=0;I<Bytes;I+=Step) {
    D=(VOID *)((UINT8 *)Map+I);
    if(D->Type!=EfiConventionalMemory || !(D->Attribute&EFI_MEMORY_WB) || D->NumberOfPages<2 ||
        D->NumberOfPages>(MAX_UINT64-D->PhysicalStart)/4096) continue;
    if(D->NumberOfPages>MostPages) {
      MostPages=D->NumberOfPages;Address=D->PhysicalStart+(MostPages/2)*4096;
    }
  }
  FreePool(Map);if(!Address || Address&4095) return EFI_NOT_FOUND;
  Status=gBS->AllocatePages(AllocateAddress,EfiReservedMemoryType,1,&Address);
  if(EFI_ERROR(Status)) return Status;
  *Out=(VOID *)(UINTN)Address;ZeroMem(*Out,4096);
  (*Out)->Magic=POLARIS_NATIVE_JOURNAL_MAGIC;(*Out)->Size=sizeof(**Out);(*Out)->Phase=1;
  WriteBackDataCacheRange(*Out,sizeof(**Out));return EFI_SUCCESS;
}
EFI_STATUS PolarisNativeJournalRead(UINT64 Address,POLARIS_NATIVE_JOURNAL *Out) {
  VOID *Map;UINTN Bytes,Step,I;EFI_MEMORY_DESCRIPTOR *D;EFI_STATUS Status;UINT64 Size;
  if(!Out || !Address || Address&4095 || Address>MAX_UINT64-4096) return EFI_INVALID_PARAMETER;
  ZeroMem(Out,sizeof(*Out));Status=ReadMap(&Map,&Bytes,&Step);if(EFI_ERROR(Status)) return Status;
  Status=EFI_NO_MAPPING;
  for(I=0;I<Bytes;I+=Step) {
    D=(VOID *)((UINT8 *)Map+I);
    if((!NativeCacheType(D->Type) && D->Type!=EfiReservedMemoryType) ||
        !(D->Attribute&(EFI_MEMORY_WB|EFI_MEMORY_WT)) || D->Attribute&EFI_MEMORY_RP ||
        D->NumberOfPages>(MAX_UINT64-D->PhysicalStart)/4096) continue;
    Size=D->NumberOfPages*4096;
    if(Address>=D->PhysicalStart && Address-D->PhysicalStart<Size && sizeof(*Out)<=Size-(Address-D->PhysicalStart)) {
      CopyMem(Out,(VOID *)(UINTN)Address,sizeof(*Out));
      Status=(Out->Magic==POLARIS_NATIVE_JOURNAL_MAGIC && Out->Size==sizeof(*Out) && Out->Phase<=4)?EFI_SUCCESS:EFI_NOT_FOUND;
      break;
    }
  }
  FreePool(Map);return Status;
}
