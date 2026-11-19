// SPDX-License-Identifier: BSD-2-Clause-Patent
// Preserve Renegade's assembly register capture, replacing its unbounded
// legacy pattern scans with checked plans for the current ARM64 loader.
#include "WindowsErrata.h"
#include "WindowsErrataProtocol.h"
#include "ntdef.h"
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/UefiBootServicesTableLib.h>

EFI_STATUS EFIAPI ExitBootServicesWrapper(EFI_HANDLE Image,UINTN Key);
STATIC EFI_EXIT_BOOT_SERVICES OriginalExit;
STATIC POLARIS_WINDOWS_ERRATA Policy;
STATIC BOOLEAN Enabled,Patched;
STATIC UINTN PatchedReturn,PatchedBlock;
STATIC UINT8 MemoryMap[96*1024] __attribute__((aligned(8)));
STATIC UINTN MapSize,DescriptorSize;
typedef struct {UINTN Address;UINT32 Before,After;} PHYSICAL_PATCH;
STATIC PHYSICAL_PATCH Physical[128];
STATIC UINT32 PhysicalCount;

STATIC BOOLEAN Ram(UINTN Address,UINTN Size) {
  UINTN I;EFI_MEMORY_DESCRIPTOR *D;UINT64 Bytes;
  for(I=0;I<MapSize;I+=DescriptorSize) {
    D=(VOID *)(MemoryMap+I);
    switch(D->Type) {
    case EfiLoaderCode:case EfiLoaderData:case EfiBootServicesCode:
    case EfiBootServicesData:case EfiRuntimeServicesCode:case EfiRuntimeServicesData:
    case EfiConventionalMemory:case EfiACPIReclaimMemory:case EfiACPIMemoryNVS:
      break;
    default:continue;
    }
    if(D->NumberOfPages>MAX_UINT64/4096) continue;
    Bytes=D->NumberOfPages*4096;
    if(Address>=D->PhysicalStart && Address-D->PhysicalStart<=Bytes &&
       Size<=Bytes-(Address-D->PhysicalStart)) return TRUE;
  }
  return FALSE;
}

#ifndef POLARIS_WINDOWS_ERRATA_HOST_TEST
STATIC BOOLEAN Translate(UINTN Address,BOOLEAN Write,UINTN *PhysicalAddress) {
  UINT64 Saved,Result;
  // AT reports a translation fault through PAR rather than taking a data
  // abort. Preserve PAR because these are the loader's page tables.
  __asm__ volatile("mrs %0, par_el1":"=r"(Saved));
  if(Write) __asm__ volatile("at s1e1w, %0\nisb"::"r"(Address):"memory");
  else __asm__ volatile("at s1e1r, %0\nisb"::"r"(Address):"memory");
  __asm__ volatile("mrs %0, par_el1\nmsr par_el1, %1":"=&r"(Result):"r"(Saved):"memory");
  if(Result&1) return FALSE;
  *PhysicalAddress=(Result&0x0000fffffffff000ULL)|(Address&4095);
  return TRUE;
}
STATIC BOOLEAN KernelPointer(UINTN P) {return P>=0xffff000000000000ULL && !(P&7);}
STATIC VOID SwitchContext(UINTN Function,UINT32 Context) {
  ((VOID (*)(UINT32))Function)(Context);
}
#endif
STATIC BOOLEAN Range(UINTN Address,UINTN Size) {
  UINTN Page,End,Pa,Bytes;
  if(!Size || Size>64*1024*1024 || Address>MAX_UINTN-Size) return FALSE;
  End=Address+Size;
  for(Page=Address;Page<End;Page+=Bytes) {
    Bytes=4096-(Page&4095);if(Bytes>End-Page) Bytes=End-Page;
    if(!Translate(Page,FALSE,&Pa) || !Ram(Pa,Bytes)) return FALSE;
  }
  return TRUE;
}
STATIC UINT16 Read16(UINTN P) {UINT16 V;CopyMem(&V,(VOID *)P,2);return V;}
STATIC UINT32 Read32(UINTN P) {UINT32 V;CopyMem(&V,(VOID *)P,4);return V;}

STATIC EFI_STATUS Open(UINTN Base,UINTN Limit,WINDOWS_IMAGE *Image) {
  UINT32 Pe,Opt,N,Size,I,Start,Bytes;UINTN Optional,Sections;
  if(!Range(Base,64) || Read16(Base)!=0x5a4d) return EFI_UNSUPPORTED;
  Pe=Read32(Base+60);
  if(Pe>1024*1024 || Base>MAX_UINTN-Pe-24 || !Range(Base+Pe,24)) return EFI_COMPROMISED_DATA;
  Opt=Read16(Base+Pe+20);N=Read16(Base+Pe+6);Optional=Base+Pe+24;
  if(Opt<144 || Opt>4096 || !N || N>96 || !Range(Optional,Opt+N*40)) return EFI_COMPROMISED_DATA;
  Size=Read32(Optional+56);Sections=Optional+Opt;
  if(Size>Limit || Base>MAX_UINTN-Size || EFI_ERROR(WindowsImageOpen((VOID *)Base,Limit,Image)))
    return EFI_COMPROMISED_DATA;
  // Validate declared sections, not SizeOfImage's unallocated alignment gaps.
  for(I=0;I<N;++I) {
    Start=Read32(Sections+I*40+12);Bytes=Read32(Sections+I*40+8);
    if(Bytes && !Range(Base+Start,Bytes)) return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FindLoader(UINTN Return,WINDOWS_IMAGE *Image) {
  UINTN Base,Lowest;EFI_STATUS Status;
  if(Return<4096 || !Range(Return-4,4) || Read32(Return-4)!=0xd63f01e0) return EFI_UNSUPPORTED;
  Base=Return&~(UINTN)4095;Lowest=Base>64*1024*1024?Base-64*1024*1024:4096;
  for(;Base>=Lowest;Base-=4096) {
    if(!Range(Base,64) || Read16(Base)!=0x5a4d) continue;
    Status=Open(Base,64*1024*1024,Image);
    if(EFI_ERROR(Status) || Return-Base>=Image->Size) continue;
    if(WindowsImageCode(Image,(UINT32)(Return-Base-4),4)) return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
STATIC EFI_STATUS FindKernel(UINTN Block,WINDOWS_IMAGE *Image) {
  PLOADER_PARAMETER_BLOCK L=(VOID *)Block;LIST_ENTRY *Head,*Entry,*Previous;
  KLDR_DATA_TABLE_ENTRY *Module;UINT32 I,J;CONST CHAR16 Name[]=L"ntoskrnl.exe";
  if(!KernelPointer(Block) || !Range(Block,sizeof(*L)) || L->OsMajorVersion!=10 ||
     L->OsMinorVersion || L->Size<sizeof(*L) || L->Size>16384 || !Range(Block,L->Size)) return EFI_UNSUPPORTED;
  Head=&L->LoadOrderListHead;Previous=Head;Entry=Head->ForwardLink;
  for(I=0;I<512 && Entry!=Head;++I) {
    if(!KernelPointer((UINTN)Entry) || !Range((UINTN)Entry,sizeof(*Module))) return EFI_COMPROMISED_DATA;
    Module=(VOID *)Entry;
    if(Entry->BackLink!=Previous || !Entry->ForwardLink) return EFI_COMPROMISED_DATA;
    if(Module->BaseDllName.Length==sizeof(Name)-2 && Module->BaseDllName.MaximumLength>=sizeof(Name)-2 &&
       KernelPointer((UINTN)Module->BaseDllName.Buffer) && Range((UINTN)Module->BaseDllName.Buffer,sizeof(Name)-2)) {
      for(J=0;J<ARRAY_SIZE(Name)-1;++J) {
        UINT16 Ch=Module->BaseDllName.Buffer[J];if(Ch>='A' && Ch<='Z') Ch+='a'-'A';
        if(Ch!=Name[J]) break;
      }
      if(J==ARRAY_SIZE(Name)-1) {
        if(!KernelPointer((UINTN)Module->DllBase) || !Module->SizeOfImage || Module->SizeOfImage>64*1024*1024)
          return EFI_COMPROMISED_DATA;
        return Open((UINTN)Module->DllBase,Module->SizeOfImage,Image);
      }
    }
    Previous=Entry;Entry=Entry->ForwardLink;
  }
  return Entry==Head && Head->BackLink==Previous?EFI_NOT_FOUND:EFI_COMPROMISED_DATA;
}
STATIC EFI_STATUS TranslatePlan(CONST WINDOWS_IMAGE *Image,CONST WINDOWS_PATCH_PLAN *Plan) {
  UINT32 I;UINTN Address,Pa;
  if(PhysicalCount+Plan->Count>ARRAY_SIZE(Physical)) return EFI_OUT_OF_RESOURCES;
  for(I=0;I<Plan->Count;++I) {
    Address=(UINTN)Image->Base+Plan->Item[I].Offset;
    if(!Translate(Address,FALSE,&Pa) || !Ram(Pa,4) || (Pa&3)) return EFI_COMPROMISED_DATA;
    Physical[PhysicalCount++]=(PHYSICAL_PATCH){Pa,Plan->Item[I].Before,Plan->Item[I].After};
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Apply(VOID) {
  UINT32 I,J;UINTN Pa;
  for(I=0;I<PhysicalCount;++I) {
    // Firmware must expose a writable identity alias. No Windows page-table
    // permissions are changed to patch kernel text.
    if(!Translate(Physical[I].Address,TRUE,&Pa) || Pa!=Physical[I].Address ||
       Read32(Pa)!=Physical[I].Before) return EFI_WRITE_PROTECTED;
    for(J=0;J<I;++J) if(Physical[J].Address==Pa) return EFI_COMPROMISED_DATA;
  }
  for(I=0;I<PhysicalCount;++I) {
    *(volatile UINT32 *)Physical[I].Address=Physical[I].After;
    WriteBackDataCacheRange((VOID *)Physical[I].Address,4);
  }
  InvalidateInstructionCache();return EFI_SUCCESS;
}

EFI_STATUS EFIAPI KernelErrataPatcherExitBootServices(EFI_HANDLE Image,UINTN Key,
  UINTN Block19,UINTN Block20,UINTN Return) {
  EFI_STATUS Status;UINTN MapKey,Block=0;UINT32 Version,Context;
  WINDOWS_IMAGE Loader,Kernel;WINDOWS_PATCH_PLAN LoaderPlan,KernelPlan;
  UINTN Switch;
  if(!Enabled) return OriginalExit(Image,Key);
  if(Patched) {
    if(Return!=PatchedReturn || (Block19!=PatchedBlock && Block20!=PatchedBlock)) {Status=EFI_ACCESS_DENIED;goto Done;}
    goto Exit;
  }
  Policy.Stage=WindowsErrataLoader;MapSize=sizeof(MemoryMap);PhysicalCount=0;
  Status=gBS->GetMemoryMap(&MapSize,(VOID *)MemoryMap,&MapKey,&DescriptorSize,&Version);
  if(EFI_ERROR(Status)) goto Done;
  if(DescriptorSize<sizeof(EFI_MEMORY_DESCRIPTOR) || MapSize%DescriptorSize) {Status=EFI_COMPROMISED_DATA;goto Done;}
  Status=FindLoader(Return,&Loader);if(EFI_ERROR(Status)) goto Done;
  Policy.Stage=WindowsErrataContext;Context=WindowsFindContext(&Loader);Policy.ContextRva=Context;
  if(!Context) {Status=EFI_INCOMPATIBLE_VERSION;goto Done;}
  Status=WindowsPlanLoader(&Loader,&LoaderPlan);if(EFI_ERROR(Status)) goto Done;
  Status=TranslatePlan(&Loader,&LoaderPlan);if(EFI_ERROR(Status)) goto Done;
  Switch=(UINTN)Loader.Base+Context;
  SwitchContext(Switch,ApplicationContext);
  // Only loader-owned structures and translated RAM are touched in the
  // application context. Never call EFI services here.
  Policy.Stage=WindowsErrataModule;
  Status=FindKernel(Block19,&Kernel);
  if(EFI_ERROR(Status)) {Status=FindKernel(Block20,&Kernel);Block=Block20;}
  else Block=Block19;
  if(!EFI_ERROR(Status)) {
    Policy.Stage=WindowsErrataKernel;Status=WindowsPlanKernel(&Kernel,&KernelPlan);
    if(!EFI_ERROR(Status)) Status=TranslatePlan(&Kernel,&KernelPlan);
  }
  SwitchContext(Switch,FirmwareContext);
  if(EFI_ERROR(Status)) goto Done;
  Policy.Stage=WindowsErrataApply;Status=Apply();if(EFI_ERROR(Status)) goto Done;
  Policy.LoaderReads=LoaderPlan.Reads;Policy.KernelReads=KernelPlan.Reads;
  Policy.KernelWrites=KernelPlan.Writes;Policy.Cache=KernelPlan.Cache;Policy.Psci=KernelPlan.Psci;
  Patched=TRUE;PatchedReturn=Return;PatchedBlock=Block;Policy.Stage=WindowsErrataReady;
Exit:
  // Keep the same assembly wrapper and Boot Services CRC on EBS retries.
  // No pool, DMA or event allocation/free and no file I/O occurs here.
  Status=OriginalExit(Image,Key);Policy.Stage=WindowsErrataEbs;
Done:
  Policy.Status=Status;return Status;
}
STATIC EFI_STATUS EFIAPI Set(POLARIS_WINDOWS_ERRATA *This,BOOLEAN Active) {
  if(This!=&Policy) return EFI_INVALID_PARAMETER;
  if(Active && Enabled) return EFI_ALREADY_STARTED;
  Enabled=Active;
  if(Active) {
    Patched=FALSE;PatchedReturn=PatchedBlock=0;
    ZeroMem(&Policy.Stage,sizeof(Policy)-OFFSET_OF(POLARIS_WINDOWS_ERRATA,Stage));
    Policy.Stage=WindowsErrataArmed;
  }
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI KernelErrataPatcherEntryPoint(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table) {
  EFI_GUID Guid=POLARIS_WINDOWS_ERRATA_GUID;EFI_STATUS Status;UINT32 Crc,SavedCrc;
  (VOID)Table;
  Policy=(POLARIS_WINDOWS_ERRATA){.Revision=POLARIS_WINDOWS_ERRATA_REVISION,.Size=sizeof(Policy),.Set=Set};
  Status=gBS->InstallMultipleProtocolInterfaces(&Image,&Guid,&Policy,NULL);
  if(EFI_ERROR(Status)) return Status;
  OriginalExit=gBS->ExitBootServices;gBS->ExitBootServices=ExitBootServicesWrapper;
  SavedCrc=gBS->Hdr.CRC32;gBS->Hdr.CRC32=0;Status=gBS->CalculateCrc32(gBS,gBS->Hdr.HeaderSize,&Crc);
  if(EFI_ERROR(Status)) {
    gBS->ExitBootServices=OriginalExit;
    gBS->Hdr.CRC32=SavedCrc;
    gBS->UninstallMultipleProtocolInterfaces(Image,&Guid,&Policy,NULL);
    return Status;
  }
  gBS->Hdr.CRC32=Crc;return EFI_SUCCESS;
}
