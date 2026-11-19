// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table) {
  EFI_MEMORY_DESCRIPTOR *Map=NULL;UINTN Size=0,Capacity=0,Key,Stride;
  UINT32 Version;EFI_STATUS Status;UINTN Attempt;
  (VOID)Table;
  Print(L"POLARIS_EBS_WATCHDOG_HANG: exit boot services then stall\n");
  for (Attempt=0;Attempt<8;++Attempt) {
    Size=Capacity;
    Status=gBS->GetMemoryMap(&Size,Map,&Key,&Stride,&Version);
    if (Status==EFI_BUFFER_TOO_SMALL) {
      // Allocate before the first EBS attempt. A map with generous slack avoids
      // allocations on a retry after exit notifications have already run.
      if (Attempt && Map) return EFI_OUT_OF_RESOURCES;
      Capacity=Size+EFI_PAGE_SIZE*16;
      Map=AllocatePool(Capacity);
      if (!Map) return EFI_OUT_OF_RESOURCES;
      continue;
    }
    if (EFI_ERROR(Status)) return Status;
    Status=gBS->ExitBootServices(Image,Key);
    if (!EFI_ERROR(Status)) {
      // No console, allocator, timer or other boot-service call is legal here.
      for (;;) { __asm__ volatile("yield"); }
    }
    if (Status!=EFI_INVALID_PARAMETER) return Status;
  }
  return EFI_ABORTED;
}
