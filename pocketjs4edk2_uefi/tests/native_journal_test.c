// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
static _Alignas(4096) UINT8 arena[16384];
static struct {EFI_MEMORY_DESCRIPTOR d;UINT64 extra;} descriptor;
static int allocations,flushes,denied;
VOID *EFIAPI AllocatePool(UINTN n) {++allocations;return malloc(n);}
VOID EFIAPI FreePool(VOID *p) {--allocations;free(p);}
VOID *EFIAPI ZeroMem(VOID *p,UINTN n) {return memset(p,0,n);}
VOID *EFIAPI CopyMem(VOID *p,CONST VOID *q,UINTN n) {return memcpy(p,q,n);}
VOID *EFIAPI WriteBackDataCacheRange(VOID *p,UINTN n) {
 assert(p==arena+8192 && n==64);++flushes;return p;
}
static EFI_STATUS EFIAPI map(UINTN *n,EFI_MEMORY_DESCRIPTOR *p,UINTN *key,UINTN *step,UINT32 *ver) {
 *step=sizeof(descriptor);*ver=1;*key=9;
 if(!p || *n<sizeof(descriptor)) {*n=sizeof(descriptor);return EFI_BUFFER_TOO_SMALL;}
 memcpy(p,&descriptor,sizeof(descriptor));*n=sizeof(descriptor);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI pages(EFI_ALLOCATE_TYPE type,EFI_MEMORY_TYPE memory,UINTN n,EFI_PHYSICAL_ADDRESS *a) {
 assert(type==AllocateAddress && memory==EfiReservedMemoryType && n==1);
 assert(*a==(UINTN)(arena+8192));return denied?EFI_ACCESS_DENIED:EFI_SUCCESS;
}
static EFI_BOOT_SERVICES services={.GetMemoryMap=map,.AllocatePages=pages};
EFI_BOOT_SERVICES *gBS=&services;
#include "../efi/LinuxBoot/NativeJournal.c"
int main(void) {
 POLARIS_NATIVE_JOURNAL *p,out;UINT64 address=(UINTN)(arena+8192);
 descriptor.d=(EFI_MEMORY_DESCRIPTOR){.Type=EfiConventionalMemory,.PhysicalStart=(UINTN)arena,.NumberOfPages=4,.Attribute=EFI_MEMORY_WB};
 assert(PolarisNativeJournalCreate(&p)==EFI_SUCCESS && p==(VOID *)(UINTN)address);
 assert(p->Magic==POLARIS_NATIVE_JOURNAL_MAGIC && p->Phase==1 && p->Size==64 && flushes==1);
 assert(PolarisNativeJournalRead(address,&out)==EFI_SUCCESS && out.Phase==1);
 assert(PolarisNativeJournalRead(address+1,&out)==EFI_INVALID_PARAMETER);
 assert(PolarisNativeJournalRead((UINTN)(arena+sizeof(arena)),&out)==EFI_NO_MAPPING);
 p->Magic=0;assert(PolarisNativeJournalRead(address,&out)==EFI_NOT_FOUND);
 p->Magic=POLARIS_NATIVE_JOURNAL_MAGIC;descriptor.d.Attribute|=EFI_MEMORY_RP;
 assert(PolarisNativeJournalRead(address,&out)==EFI_NO_MAPPING);
 descriptor.d.Attribute=EFI_MEMORY_WB;descriptor.d.Type=EfiMemoryMappedIO;
 assert(PolarisNativeJournalRead(address,&out)==EFI_NO_MAPPING);
 descriptor.d.Type=EfiConventionalMemory;denied=1;
 assert(PolarisNativeJournalCreate(&p)==EFI_ACCESS_DENIED && !p);
 assert(allocations==0);
 puts("PASS: native journal page selection, RAM permissions, corrupt records and allocation failure");
 return 0;
}
