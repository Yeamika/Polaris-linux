// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "ExitTrace.h"
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_EXIT_BOOT_SERVICES OriginalExit;
STATIC volatile POLARIS_LINUX_EXIT_TRACE *Journal=(VOID *)(UINTN)POLARIS_LINUX_TRACE_ADDRESS;

STATIC VOID Commit(VOID) {
  // Keep the reset-surviving journal in DRAM even if an inherited mapping
  // behaves as WB. A barrier alone does not clean dirty cache lines.
  WriteBackDataCacheRange((VOID *)Journal,sizeof(*Journal));
#if defined(__aarch64__)
  __asm__ volatile("dsb sy" ::: "memory");
#else
  __asm__ volatile("" ::: "memory");
#endif
}

STATIC EFI_STATUS ReservedPage(VOID) {
  EFI_MEMORY_DESCRIPTOR *Map=NULL,*D;
  UINTN Size=0,Key,Step=0,I;
  UINT32 Version;
  EFI_STATUS Status=gBS->GetMemoryMap(&Size,NULL,&Key,&Step,&Version);
  if (Status!=EFI_BUFFER_TOO_SMALL || Step<sizeof(*D) || Step>4096 || Size>1024*1024)
    return EFI_NO_MAPPING;
  Size+=Step*8;Map=AllocatePool(Size);
  if (!Map) return EFI_OUT_OF_RESOURCES;
  Status=gBS->GetMemoryMap(&Size,Map,&Key,&Step,&Version);
  if (!EFI_ERROR(Status) && Step>=sizeof(*D) && Size%Step==0) {
    Status=EFI_NO_MAPPING;
    for (I=0;I<Size;I+=Step) {
      D=(VOID *)((UINT8 *)Map+I);
      if (D->Type==EfiReservedMemoryType && (D->Attribute&EFI_MEMORY_WT) &&
          !(D->Attribute&(EFI_MEMORY_RP|EFI_MEMORY_RO)) && D->PhysicalStart<=POLARIS_LINUX_TRACE_ADDRESS &&
          D->NumberOfPages<=(MAX_UINT64-D->PhysicalStart)/EFI_PAGE_SIZE &&
          D->PhysicalStart+D->NumberOfPages*EFI_PAGE_SIZE>=POLARIS_LINUX_TRACE_ADDRESS+EFI_PAGE_SIZE) {
        Status=EFI_SUCCESS;break;
      }
    }
  } else if (!EFI_ERROR(Status)) Status=EFI_NO_MAPPING;
  FreePool(Map);return Status;
}

STATIC EFI_STATUS EFIAPI TraceExit(EFI_HANDLE Image,UINTN Key) {
  EFI_STATUS Status;
  Journal->Phase=2;++Journal->Calls;Commit();
  Status=OriginalExit(Image,Key);
  // No allocation, file I/O, protocol calls, or table edits after EBS. The
  // reserved WT journal survives a watchdog reset without altering the map.
  Journal->ExitStatus=Status;Journal->Phase=EFI_ERROR(Status)?4:3;Commit();
  return Status;
}

EFI_STATUS PolarisLinuxExitTraceRead(POLARIS_LINUX_EXIT_TRACE *Out) {
  EFI_STATUS Status;
  if (!Out) return EFI_INVALID_PARAMETER;
  Status=ReservedPage();if (EFI_ERROR(Status)) return Status;
  CopyMem(Out,(CONST VOID *)Journal,sizeof(*Out));
  if (Out->Magic!=POLARIS_LINUX_TRACE_MAGIC || Out->Size!=sizeof(*Out) || Out->Phase>5)
    return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}

EFI_STATUS PolarisLinuxExitTraceBegin(VOID) {
  POLARIS_LINUX_EXIT_TRACE Previous;
  UINT64 Sequence=1;
  UINT32 Crc;
  EFI_STATUS Status;
  if (OriginalExit) return EFI_ALREADY_STARTED;
  Status=PolarisLinuxExitTraceRead(&Previous);
  if (!EFI_ERROR(Status)) Sequence=Previous.Sequence+1;
  else if (Status!=EFI_NOT_FOUND) return Status;
  ZeroMem((VOID *)Journal,sizeof(*Journal));
  Journal->Magic=POLARIS_LINUX_TRACE_MAGIC;Journal->Size=sizeof(*Journal);
  Journal->Sequence=Sequence;Journal->Phase=1;Commit();
  Crc=gBS->Hdr.CRC32;OriginalExit=gBS->ExitBootServices;
  gBS->ExitBootServices=TraceExit;gBS->Hdr.CRC32=0;
  Status=gBS->CalculateCrc32(gBS,gBS->Hdr.HeaderSize,&gBS->Hdr.CRC32);
  if (EFI_ERROR(Status)) {
    gBS->ExitBootServices=OriginalExit;OriginalExit=NULL;gBS->Hdr.CRC32=Crc;
  }
  return Status;
}

VOID PolarisLinuxExitTraceEnd(EFI_STATUS Status) {
  if (!OriginalExit) return;
  gBS->ExitBootServices=OriginalExit;OriginalExit=NULL;
  gBS->Hdr.CRC32=0;gBS->CalculateCrc32(gBS,gBS->Hdr.HeaderSize,&gBS->Hdr.CRC32);
  Journal->ReturnStatus=Status;Journal->Phase=5;Commit();
}
