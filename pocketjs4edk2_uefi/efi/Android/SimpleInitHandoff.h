// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included only in the prepared SimpleInit main application.
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include "Kernel.h"
#include "Trace.h"
extern VOID EFIAPI PolarisAndroidEnter(UINT64 Kernel,UINT64 Dtb);
STATIC EFI_STATUS PolarisAndroidHandoff(linux_boot *Boot,EFI_HANDLE Image,BOOLEAN PrepareOnly) {
  EFI_MEMORY_DESCRIPTOR *Map=NULL;EFI_STATUS Status;UINTN Capacity=0,Size,Key,Stride=0,Attempt;
  UINT32 Version;UINT64 Footprint,El,Features;
  UINT64 Kernel=(UINT64)(UINTN)Boot->kernel.address,Dtb=(UINT64)(UINTN)Boot->dtb.address;
  UINTN DtbSize=Boot->dtb.address?fdt_totalsize(Boot->dtb.address):0;
  UINT64 Initrd=(UINT64)(UINTN)Boot->initrd.address;
  CONST CHAR16 *Failed=NULL;
  if (!Boot->kernel.allocated || Boot->kernel.mem_size<Boot->kernel.offset ||
      !PolarisAndroidKernelValid(Boot->kernel.address,Boot->kernel.size,Kernel,
        Boot->kernel.mem_size-Boot->kernel.offset,&Footprint))
    Failed=L"kernel header or allocation";
  else if (!PolarisAndroidDtbValid(Boot->dtb.address,DtbSize))
    Failed=L"final DTB";
  else if (!PolarisAndroidDramRange(Boot->dtb.address,Kernel,Footprint))
    Failed=L"kernel DRAM range";
  else if (!PolarisAndroidDramRange(Boot->dtb.address,Dtb,DtbSize))
    Failed=L"DTB DRAM range";
  else if (!PolarisAndroidDramRange(Boot->dtb.address,Initrd,Boot->initrd.size))
    Failed=L"initrd DRAM range";
  else if (!PolarisAndroidDramRange(Boot->dtb.address,POLARIS_ANDROID_TRACE_ADDRESS,4096))
    Failed=L"trace DRAM range";
  if (Failed) {
    Print(L"ANDROID_PREFLIGHT_REJECT: %s\n",Failed);
    Print(L"kernel=%lx bytes=%lx alloc=%lx offset=%lx\n",
      Kernel,(UINT64)Boot->kernel.size,(UINT64)Boot->kernel.mem_size,(UINT64)Boot->kernel.offset);
    Print(L"dtb=%lx/%lx initrd=%lx/%lx\n",
      Dtb,(UINT64)DtbSize,Initrd,(UINT64)Boot->initrd.size);
    return EFI_COMPROMISED_DATA;
  }
  __asm__ volatile("mrs %0, CurrentEL\n\tmrs %1, id_aa64mmfr2_el1":"=r"(El),"=r"(Features));
  // SDM845 uses the original CCSIDR layout. Refuse EL3 and extended CCIDX.
  if ((El!=4 && El!=8) || ((Features>>20)&15)) return EFI_UNSUPPORTED;
  if (fdt_add_mem_rsv(Boot->dtb.address,POLARIS_ANDROID_TRACE_ADDRESS,4096)) return EFI_OUT_OF_RESOURCES;
  volatile UINT64 *Trace=PolarisAndroidTrace();
  Trace[0]=POLARIS_ANDROID_TRACE_MAGIC;Trace[2]=Kernel;Trace[3]=Dtb;
  Trace[4]=(UINT64)(UINTN)Boot->initrd.address;Trace[5]=El;Trace[7]=0;
  PolarisAndroidTraceStage(0x10,0);
  Print(L"ANDROID_NATIVE_PREFLIGHT_OK EL%u kernel=%lx footprint=%lx dtb=%lx initrd=%lx\n",
    (UINT32)(El>>2),Kernel,Footprint,Dtb,(UINT64)(UINTN)Boot->initrd.address);
  if (PrepareOnly) return EFI_SUCCESS;
  PolarisAndroidTraceStage(0x20,0);
  EfiSignalEventReadyToBoot();
  PolarisAndroidTraceStage(0x21,0);
  Print(L"ANDROID_NATIVE_EXIT_BOOT_SERVICES: stackless ARM64 entry\n");
  Status=gBS->GetMemoryMap(&Capacity,Map,&Key,&Stride,&Version);
  if (Status!=EFI_BUFFER_TOO_SMALL || Stride<sizeof(*Map) || Capacity>MAX_UINTN-8*Stride)
    return EFI_DEVICE_ERROR;
  Capacity+=8*Stride;
  Map=AllocatePool(Capacity);if (!Map) return EFI_OUT_OF_RESOURCES;
  PolarisAndroidTraceStage(0x30,0);
  // No allocation, logging or other firmware calls between the final map and EBS.
  for (Attempt=0;Attempt<3;++Attempt) {
    Size=Capacity;Status=gBS->GetMemoryMap(&Size,Map,&Key,&Stride,&Version);
    if (EFI_ERROR(Status)) break;
    PolarisAndroidTraceStage(0x31,Key);
    Status=gBS->ExitBootServices(Image,Key);
    if (!EFI_ERROR(Status)) {
      PolarisAndroidTraceStage(0x40,0);
      PolarisAndroidEnter(Kernel,Dtb);
      for (;;) __asm__ volatile("wfe");
    }
    PolarisAndroidTraceStage(0x3e,Status);
    if (Status!=EFI_INVALID_PARAMETER) break;
  }
  // EBS may have signalled shutdown events. Do not try to restart a partial UI.
  PolarisAndroidTraceStage(0x3f,Status);
  gRT->ResetSystem(EfiResetCold,Status,0,NULL);
  for (;;) __asm__ volatile("wfe");
}
