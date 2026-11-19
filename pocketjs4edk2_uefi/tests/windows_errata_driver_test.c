// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
static BOOLEAN InApplication,RejectWrite;
static UINTN ExpectedSwitch;
static unsigned Switches,CacheWrites,CacheInvalidations,MapReads,ExitCalls;
static BOOLEAN Translate(UINTN Address,BOOLEAN Write,UINTN *PhysicalAddress) {
 if(Write && RejectWrite)return FALSE;
 *PhysicalAddress=Address;return TRUE;
}
static BOOLEAN KernelPointer(UINTN P) {return P>=4096 && !(P&7);}
static void SwitchContext(UINTN Function,UINT32 Context) {
 assert(Function==ExpectedSwitch && Context<=1);InApplication=Context==0;++Switches;
}
#define POLARIS_WINDOWS_ERRATA_HOST_TEST
#include "../platform/WindowsErrata.c"
#include "../platform/WindowsErrataDxe.c"
#include "windows_errata_fixture.h"
EFI_BOOT_SERVICES *gBS;
static EFI_BOOT_SERVICES Bs;
static EFI_MEMORY_DESCRIPTOR Descriptors[3];
VOID *EFIAPI ZeroMem(VOID *P,UINTN N) {return memset(P,0,N);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N) {return memmove(A,B,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) {return memcmp(A,B,N);}
VOID *EFIAPI WriteBackDataCacheRange(VOID *Address,UINTN Length) {
 assert(!InApplication && Address && Length==4);++CacheWrites;return Address;
}
VOID EFIAPI InvalidateInstructionCache(VOID) {assert(!InApplication);++CacheInvalidations;}
EFI_STATUS EFIAPI ExitBootServicesWrapper(EFI_HANDLE Image,UINTN Key) {(void)Image;(void)Key;abort();}
static EFI_STATUS EFIAPI Exit(EFI_HANDLE Image,UINTN Key) {
 (void)Image;assert(!InApplication && Key==0x456 && Patched);
 assert(gBS->ExitBootServices==ExitBootServicesWrapper && gBS->Hdr.CRC32==0x12345678);
 ++ExitCalls;return ExitCalls==1?EFI_INVALID_PARAMETER:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Map(UINTN *Size,EFI_MEMORY_DESCRIPTOR *Out,UINTN *Key,UINTN *Stride,UINT32 *Version) {
 assert(!InApplication && *Size>=sizeof(Descriptors));++MapReads;
 *Size=sizeof(Descriptors);memcpy(Out,Descriptors,*Size);*Key=0x456;*Stride=sizeof(*Out);*Version=1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *Handle,...) {(void)Handle;assert(!InApplication);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Remove(EFI_HANDLE Handle,...) {(void)Handle;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Crc(VOID *Data,UINTN Size,UINT32 *Result) {
 assert(Data==gBS && Size==sizeof(*gBS) && !gBS->Hdr.CRC32);*Result=0x12345678;return EFI_SUCCESS;
}
int main(void) {
 UINT8 *Loader=aligned_alloc(4096,FIXTURE_BYTES),*Kernel=aligned_alloc(4096,FIXTURE_BYTES),*Heap=aligned_alloc(4096,4096);
 assert(Loader && Kernel && Heap);memset(Heap,0,4096);Fixture(Loader,FALSE);Fixture(Kernel,TRUE);
 for(unsigned I=0;I<3;++I) {
  Descriptors[I]=(EFI_MEMORY_DESCRIPTOR){.Type=EfiLoaderData,.PhysicalStart=(UINTN)(I==0?Loader:I==1?Kernel:Heap),.NumberOfPages=I==2?1:FIXTURE_BYTES/4096};
 }
 Bs.GetMemoryMap=Map;Bs.ExitBootServices=Exit;Bs.InstallMultipleProtocolInterfaces=Install;
 Bs.UninstallMultipleProtocolInterfaces=Remove;Bs.CalculateCrc32=Crc;Bs.Hdr.HeaderSize=sizeof(Bs);gBS=&Bs;
 assert(KernelErrataPatcherEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS && Bs.ExitBootServices==ExitBootServicesWrapper);
 PLOADER_PARAMETER_BLOCK Block=(VOID *)Heap;KLDR_DATA_TABLE_ENTRY *Module=(VOID *)(Heap+0x180);
 CHAR16 Name[]=L"ntoskrnl.exe";memcpy(Heap+0x300,Name,sizeof(Name));
 Block->OsMajorVersion=10;Block->Size=0x168;
 Block->LoadOrderListHead.ForwardLink=Block->LoadOrderListHead.BackLink=&Module->InLoadOrderLinks;
 Module->InLoadOrderLinks.ForwardLink=Module->InLoadOrderLinks.BackLink=&Block->LoadOrderListHead;
 Module->BaseDllName=(UNICODE_STRING){sizeof(Name)-2,sizeof(Name),(VOID *)(Heap+0x300)};
 Module->DllBase=Kernel;Module->SizeOfImage=FIXTURE_BYTES;
 ExpectedSwitch=(UINTN)Loader+0x1100;
 assert(Policy.Set(&Policy,TRUE)==EFI_SUCCESS && Policy.Stage==WindowsErrataArmed);
 assert(KernelErrataPatcherExitBootServices((EFI_HANDLE)1,0x456,1,(UINTN)Block,(UINTN)Loader+0x1050)==EFI_INVALID_PARAMETER);
 assert(!InApplication && Switches==2 && MapReads==1 && ExitCalls==1 && CacheWrites==7 && CacheInvalidations==1);
 assert(Policy.LoaderReads==1 && Policy.KernelReads==2 && Policy.KernelWrites==1 && Policy.Cache==1 && Policy.Psci==1);
 assert(Policy.Stage==WindowsErrataEbs && Policy.Status==EFI_INVALID_PARAMETER);
 assert(Get32(Kernel,0x1900)==0xd503201f && Get32(Loader,0x1600)==0xd2800008);
 // The failed EBS map-key attempt reuses the complete patch and original
 // entry/CRC, without allocation, reapplying bytes or switching context again.
 assert(KernelErrataPatcherExitBootServices((EFI_HANDLE)1,0x456,1,(UINTN)Block,(UINTN)Loader+0x1050)==EFI_SUCCESS);
 assert(MapReads==1 && Switches==2 && ExitCalls==2 && CacheWrites==7);
 assert(KernelErrataPatcherExitBootServices((EFI_HANDLE)1,0x456,1,(UINTN)Block,(UINTN)Loader+0x1060)==EFI_ACCESS_DENIED);
 assert(Policy.Set(&Policy,FALSE)==EFI_SUCCESS);
 // Missing kernel fixes refuse the entire handoff and leave winload intact.
 Fixture(Loader,FALSE);Fixture(Kernel,TRUE);Put32(Kernel,0x1b80,0);
 assert(Policy.Set(&Policy,TRUE)==EFI_SUCCESS);
 assert(KernelErrataPatcherExitBootServices((EFI_HANDLE)1,0x456,1,(UINTN)Block,(UINTN)Loader+0x1050)==EFI_UNSUPPORTED);
 assert(!InApplication && Get32(Loader,0x1600)==0xd5381028 && Get32(Kernel,0x1900)==0xd5181029 && ExitCalls==2);
 assert(Policy.Stage==WindowsErrataKernel && CacheWrites==7);
 Policy.Set(&Policy,FALSE);Fixture(Kernel,TRUE);RejectWrite=TRUE;Policy.Set(&Policy,TRUE);
 assert(KernelErrataPatcherExitBootServices((EFI_HANDLE)1,0x456,1,(UINTN)Block,(UINTN)Loader+0x1050)==EFI_WRITE_PROTECTED);
 assert(!InApplication && Get32(Loader,0x1600)==0xd5381028 && ExitCalls==2);
 Policy.Set(&Policy,FALSE);RejectWrite=FALSE;Module->InLoadOrderLinks.BackLink=NULL;Policy.Set(&Policy,TRUE);
 assert(KernelErrataPatcherExitBootServices((EFI_HANDLE)1,0x456,1,(UINTN)Block,(UINTN)Loader+0x1050)==EFI_COMPROMISED_DATA && !InApplication && ExitCalls==2);
 free(Loader);free(Kernel);free(Heap);
 puts("PASS: actual EBS driver plans, x20 fallback, context restoration, physical cache flush, retries, bad modules and all-or-none refusal");
 return 0;
}
