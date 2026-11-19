// SPDX-License-Identifier: BSD-2-Clause-Patent
// Polaris ARM64 entry, shared by the ESP Linux launcher and explicit diagnostics.
#include "NativeHandoff.h"
#include "NativeMemory.h"
#include "NativeJournal.h"
#include "Diagnostics.h"
#include "../Common/Loader.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <libfdt.h>
#ifdef POLARIS_NATIVE_KERNEL_PROBE
#include "NativeKernelProbe.h"
#endif
VOID PolarisNativeEnter(UINT64 Entry,UINT64 Dtb,VOID *Map,UINTN Bytes,UINTN Step,POLARIS_NATIVE_JOURNAL *Journal) __attribute__((noreturn));

#if defined(POLARIS_NATIVE_PRE_EXIT_PROBE) || defined(POLARIS_NATIVE_EXIT_PROBE)
// Diagnostic images only. This is the same SDM845 KPSS register sequence as
// PlatformServices.c, with interrupts masked so firmware cannot keep petting it.
// Compare before/after EBS with identical payload preparation and reboot path.
STATIC VOID NativeProbeArm(UINT32 Seconds) {
  volatile UINT32 *Wdt=(volatile UINT32 *)(UINTN)0x17980000;
  Wdt[2]=0;__asm__ volatile("dsb sy" ::: "memory");
  Wdt[1]=1;__asm__ volatile("dsb sy" ::: "memory");
  Wdt[4]=Seconds*32768;__asm__ volatile("dsb sy" ::: "memory");
  Wdt[5]=Seconds*32768;__asm__ volatile("dsb sy" ::: "memory");
  Wdt[2]=1;__asm__ volatile("dsb sy" ::: "memory");
}
STATIC VOID NativeExitProbeStall(VOID) __attribute__((noreturn));
STATIC VOID NativeExitProbeStall(VOID) {
  __asm__ volatile("msr daifset, #0xf" ::: "memory");
#ifdef POLARIS_NATIVE_EXIT_PROBE
  NativeProbeArm(5);
#endif
  for(;;) __asm__ volatile("yield" ::: "memory");
}
#endif

STATIC EFI_STATUS NativeTree(VOID *Dtb,CONST CHAR16 *Options,VOID *Initrd,UINTN Bytes) {
  CHAR8 Text[4096];UINTN I;INT32 Chosen,Error;fdt64_t Pointer;
  CONST CHAR8 *Drop[]={"linux,uefi-system-table","linux,uefi-mmap-start","linux,uefi-mmap-size",
    "linux,uefi-mmap-desc-size","linux,uefi-mmap-desc-ver","kaslr-seed","rng-seed"};
  for(I=0;Options[I];++I) {if(I>=sizeof(Text)-1 || Options[I]<32 || Options[I]>126) return EFI_BAD_BUFFER_SIZE;Text[I]=(CHAR8)Options[I];}
  Text[I]=0;Chosen=fdt_path_offset(Dtb,"/chosen");if(Chosen<0) return EFI_NOT_FOUND;
  if(fdt_setprop_string(Dtb,Chosen,"bootargs",Text)) return EFI_VOLUME_CORRUPTED;
  for(I=0;I<ARRAY_SIZE(Drop);++I) {
    Error=fdt_delprop(Dtb,Chosen,Drop[I]);if(Error && Error!=-FDT_ERR_NOTFOUND) return EFI_VOLUME_CORRUPTED;
  }
  Pointer=cpu_to_fdt64((UINT64)(UINTN)Initrd);
  if(fdt_setprop(Dtb,Chosen,"linux,initrd-start",&Pointer,8)) return EFI_VOLUME_CORRUPTED;
  Pointer=cpu_to_fdt64((UINT64)(UINTN)Initrd+Bytes);
  if(fdt_setprop(Dtb,Chosen,"linux,initrd-end",&Pointer,8)) return EFI_VOLUME_CORRUPTED;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS NativeMemory(VOID *Dtb,VOID *Map,UINTN Bytes,UINTN Step,
    UINT64 Entry,UINT64 ImageBytes,VOID *Initrd,UINTN InitrdBytes,UINTN *OutCount) {
  POLARIS_NATIVE_RANGE Ranges[POLARIS_NATIVE_MAX_RANGES];
  fdt64_t Reg[POLARIS_NATIVE_MAX_RANGES*2];UINTN I,Count;INT32 Node,Depth=0,Memory=-1;
  CONST CHAR8 *Type;INT32 Len;
  if(!NativeRanges(Map,Bytes,Step,Ranges,&Count) ||
      !NativeCovered(Ranges,Count,Entry,ImageBytes) ||
      !NativeCovered(Ranges,Count,(UINT64)(UINTN)Dtb,fdt_totalsize(Dtb)) ||
      !NativeCovered(Ranges,Count,(UINT64)(UINTN)Initrd,InitrdBytes)) return EFI_NO_MAPPING;
  if(fdt_address_cells(Dtb,0)!=2 || fdt_size_cells(Dtb,0)!=2) return EFI_UNSUPPORTED;
  Node=-1;
  while((Node=fdt_next_node(Dtb,Node,&Depth))>=0) {
    Type=fdt_getprop(Dtb,Node,"device_type",&Len);
    if(Type && Len==7 && !CompareMem(Type,"memory",7)) {
      if(Memory>=0 || fdt_parent_offset(Dtb,Node)!=0) return EFI_UNSUPPORTED;
      Memory=Node;
    }
  }
  if(Node!=-FDT_ERR_NOTFOUND || Memory<0) return EFI_NOT_FOUND;
  for(I=0;I<Count;++I) {Reg[I*2]=cpu_to_fdt64(Ranges[I].Base);Reg[I*2+1]=cpu_to_fdt64(Ranges[I].End-Ranges[I].Base);}
  if(fdt_setprop(Dtb,Memory,"reg",Reg,(INT32)(Count*16))) return EFI_VOLUME_CORRUPTED;
  *OutCount=Count;return EFI_SUCCESS;
}

EFI_STATUS PolarisLinuxNativeStart(EFI_HANDLE Image,EFI_HANDLE Volume,VOID *Dtb,
    CONST CHAR16 *Options,VOID *Initrd,UINTN InitrdBytes,BOOLEAN PrepareOnly) {
  VOID *Raw=NULL,*Allocation=NULL,*Map=NULL;EFI_STATUS Status;
  POLARIS_NATIVE_JOURNAL *Journal=NULL;
  UINT64 Offset,Memory,El,Entry,CacheFormat;UINTN RawBytes,Pages=0,MapBytes=0,Capacity,Key,Step=0,Count,Attempt;
  UINT32 Version;
  UINT64 NativeTick,NativeNow,NativeHz;
  __asm__ volatile("mrs %0, cntpct_el0":"=r"(NativeTick));
  __asm__ volatile("mrs %0, cntfrq_el0":"=r"(NativeHz));
  __asm__ volatile("mrs %0, CurrentEL":"=r"(El));
  if(El!=4 && El!=8) return EFI_UNSUPPORTED;
  __asm__ volatile("mrs %0, ID_AA64MMFR2_EL1":"=r"(CacheFormat));
  // SDM845 uses legacy CCSIDR fields; never interpret an extended format as legacy.
  if((CacheFormat>>20)&15) return EFI_UNSUPPORTED;
  if(!Dtb || !Initrd || !InitrdBytes || (UINT64)(UINTN)Initrd>MAX_UINT64-InitrdBytes) return EFI_INVALID_PARAMETER;
  Print(L"POLARIS NATIVE LINUX: EL%u, firmware stub bypass\r\n",(UINT32)(El>>2));
  Status=PolarisReadFile(Volume,L"\\EFI\\LinuxBoot\\Image.efi",128*1024*1024,&Raw,&RawBytes);
  if(EFI_ERROR(Status)) goto Done;
  if(!NativeImage(Raw,RawBytes,&Offset,&Memory)) {Status=EFI_UNSUPPORTED;goto Done;}
#ifdef POLARIS_NATIVE_KERNEL_PROBE
  Status=NativeKernelProbeApply(Raw,RawBytes,Memory);if(EFI_ERROR(Status)) goto Done;
#endif
#ifdef POLARIS_NATIVE_ENTRY_PROBE
  if(Memory>0x07fffffc || Memory&3) {Status=EFI_UNSUPPORTED;goto Done;}
  Pages=EFI_SIZE_TO_PAGES(Memory+Offset+EFI_PAGE_SIZE);
#else
  Pages=EFI_SIZE_TO_PAGES(Memory+Offset);
#endif
  Allocation=AllocateAlignedPages(Pages,0x200000);
  if(!Allocation) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  Entry=(UINT64)(UINTN)Allocation+Offset;
  ZeroMem((VOID *)(UINTN)Entry,(UINTN)Memory);CopyMem((VOID *)(UINTN)Entry,Raw,RawBytes);
#ifdef POLARIS_NATIVE_ENTRY_PROBE
  {
    extern UINT8 PolarisNativeProbe[],PolarisNativeProbeEnd[];
    UINTN MarkerBytes=(UINTN)PolarisNativeProbeEnd-(UINTN)PolarisNativeProbe;
    if(!MarkerBytes || MarkerBytes>EFI_PAGE_SIZE) {Status=EFI_BAD_BUFFER_SIZE;goto Done;}
    CopyMem((VOID *)(UINTN)(Entry+Memory),PolarisNativeProbe,MarkerBytes);
    *(UINT32 *)(UINTN)Entry=0x14000000U|(UINT32)(Memory>>2);
    Print(L"NATIVE_ENTRY_PROBE: raw header -> 5-second watchdog; Linux body intentionally skipped\r\n");
    Memory+=EFI_PAGE_SIZE;
  }
#endif
  FreePool(Raw);Raw=NULL;
  Status=NativeTree(Dtb,Options,Initrd,InitrdBytes);if(EFI_ERROR(Status)) goto Done;
  Status=PolarisNativeJournalCreate(&Journal);if(EFI_ERROR(Status)) goto Done;
  Journal->Entry=Entry;Journal->Dtb=(UINT64)(UINTN)Dtb;
  Print(L"NATIVE_JOURNAL_ADDRESS=%016lx\r\n",(UINT64)(UINTN)Journal);
  Status=gBS->GetMemoryMap(&MapBytes,NULL,&Key,&Step,&Version);
  if(Status!=EFI_BUFFER_TOO_SMALL || Step<sizeof(EFI_MEMORY_DESCRIPTOR) || Step>4096 || MapBytes>1024*1024) {Status=EFI_NO_MAPPING;goto Done;}
  Capacity=MapBytes+64*Step;Map=AllocatePool(Capacity);
  if(!Map) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  MapBytes=Capacity;Status=gBS->GetMemoryMap(&MapBytes,Map,&Key,&Step,&Version);if(EFI_ERROR(Status)) goto Done;
  Status=NativeMemory(Dtb,Map,MapBytes,Step,Entry,Memory,Initrd,InitrdBytes,&Count);if(EFI_ERROR(Status)) goto Done;
  Print(L"Native Image=%016lx bytes=%lx; DTB=%016lx initrd=%016lx; RAM ranges=%u\r\n",
      Entry,Memory,(UINT64)(UINTN)Dtb,(UINT64)(UINTN)Initrd,(UINT32)Count);
  if(PrepareOnly) {Print(L"POLARIS_NATIVE_PREPARE_ONLY: memory and image checked; entry not called\r\n");Status=EFI_SUCCESS;goto Done;}
#ifdef POLARIS_NATIVE_PRE_EXIT_PROBE
  Print(L"NATIVE_PRE_EXIT_PROBE: stall before EBS, fifteen-second watchdog; no kernel entry\r\n");
#elif defined(POLARIS_NATIVE_EXIT_PROBE)
  Print(L"NATIVE_EXIT_PROBE: stall after successful EBS, five-second watchdog; no kernel entry\r\n");
#endif
  __asm__ volatile("mrs %0, cntpct_el0":"=r"(NativeNow));
  Print(L"Native raw Image preparation: %lu ms\r\n",
    NativeHz?(NativeNow-NativeTick)*1000/NativeHz:0);
#ifdef POLARIS_NATIVE_PRE_EXIT_PROBE
  NativeProbeArm(15);
  {
    volatile UINT32 *Wdt=(volatile UINT32 *)(UINTN)0x17980000;
    Print(L"NATIVE_WATCHDOG_READBACK en=%x bark=%x bite=%x\r\n",Wdt[2],Wdt[4],Wdt[5]);
  }
#endif
  Print(L"Native handoff: closing ESP log, exiting firmware, entering kernel\r\n");
  // Close storage before the final map, retaining output through EBS callbacks.
  PolarisLinuxLogSeal();
#ifdef POLARIS_NATIVE_PRE_EXIT_PROBE
  NativeExitProbeStall();
#endif
  for(Attempt=0;Attempt<3;++Attempt) {
    MapBytes=Capacity;Status=gBS->GetMemoryMap(&MapBytes,Map,&Key,&Step,&Version);
    if(EFI_ERROR(Status)) {if(!Attempt) goto Done;break;}
    Status=NativeMemory(Dtb,Map,MapBytes,Step,Entry,Memory,Initrd,InitrdBytes,&Count);
    if(EFI_ERROR(Status)) {if(!Attempt) goto Done;break;}
    // Only memory edits above; no I/O, allocation, protocol or timer calls.
    Status=gBS->ExitBootServices(Image,Key);
    if(!EFI_ERROR(Status)) {
#ifdef POLARIS_NATIVE_EXIT_PROBE
      NativeExitProbeStall();
#endif
      PolarisNativeEnter(Entry,(UINT64)(UINTN)Dtb,Map,MapBytes,Step,Journal);
    }
    if(Status!=EFI_INVALID_PARAMETER) break;
  }
  // EBS can have partially shut down firmware even on failure. Wait for the
  // existing hardware watchdog; never return to a menu with stale protocols.
  CpuDeadLoop();
Done:
  // Keep a successful preflight record for the read-only RAM inspector.
  if(Journal && (!PrepareOnly || EFI_ERROR(Status))) gBS->FreePages((UINT64)(UINTN)Journal,1);
  if(Map) FreePool(Map);
  if(Raw) FreePool(Raw);
  if(Allocation) FreeAlignedPages(Allocation,Pages);
  return Status;
}
