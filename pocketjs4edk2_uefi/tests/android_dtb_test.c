// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>
#undef NULL
#include "../efi/Android/BootRequest.h"
#include "../efi/Android/NativeFdt.h"
#include "../efi/Android/AndroidMemory.h"
static UINT8 Dtb[16384],Image[8192];

static EFI_MEMORY_DESCRIPTOR Map[] = {
  {.Type=EfiBootServicesData,.PhysicalStart=0xc0000000,.NumberOfPages=0x4000,.Attribute=EFI_MEMORY_WB},
  {.Type=EfiConventionalMemory,.PhysicalStart=0xab000000,.NumberOfPages=0x7600,.Attribute=EFI_MEMORY_WB},
};
static UINTN PoolCount,PageAttempts;
static UINT64 RefuseAddress;

VOID *EFIAPI AllocatePool(UINTN Size) { ++PoolCount; return malloc(Size); }
VOID *EFIAPI AllocateZeroPool(UINTN Size) { ++PoolCount; return calloc(1,Size); }
VOID EFIAPI FreePool(VOID *Data) { --PoolCount; free(Data); }

static EFI_STATUS EFIAPI MemoryMap(UINTN *Size,EFI_MEMORY_DESCRIPTOR *Data,UINTN *Key,
    UINTN *Stride,UINT32 *Version) {
  *Key=1;*Stride=sizeof(Map[0])+8;*Version=1;
  UINTN Required=*Stride*(sizeof(Map)/sizeof(Map[0]));
  if (!Data || *Size<Required) { *Size=Required; return EFI_BUFFER_TOO_SMALL; }
  *Size=Required;
  for (UINTN I=0;I<sizeof(Map)/sizeof(Map[0]);++I)
    memcpy((UINT8 *)Data+I*(*Stride),&Map[I],sizeof(Map[I]));
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI Allocate(EFI_ALLOCATE_TYPE Type,EFI_MEMORY_TYPE Memory,UINTN Pages,
    EFI_PHYSICAL_ADDRESS *Address) {
  assert(Type==AllocateAddress && Memory==EfiBootServicesData);
  assert((*Address&0x1fffff)==0);
  assert(PolarisAndroidDramRange(Dtb,*Address,EFI_PAGES_TO_SIZE(Pages)));
  ++PageAttempts;
  return *Address==RefuseAddress?EFI_NOT_FOUND:EFI_SUCCESS;
}
static EFI_BOOT_SERVICES Services={.GetMemoryMap=MemoryMap,.AllocatePages=Allocate};
EFI_BOOT_SERVICES *gBS=&Services;

static void Build(void) {
  assert(fdt_create_empty_tree(Dtb,sizeof(Dtb))==0);
  assert(fdt_setprop_string(Dtb,0,"model","Xiaomi Technologies, Inc. Polaris MP v2.1")==0);
  fdt32_t Id[]={cpu_to_fdt32(0x2c),0};
  assert(fdt_setprop(Dtb,0,"qcom,board-id",Id,sizeof(Id))==0);
  Id[0]=cpu_to_fdt32(0x141);Id[1]=cpu_to_fdt32(0x20001);
  assert(fdt_setprop(Dtb,0,"qcom,msm-id",Id,sizeof(Id))==0);
  assert(fdt_setprop_u32(Dtb,0,"#address-cells",2)==0);
  assert(fdt_setprop_u32(Dtb,0,"#size-cells",2)==0);
  int N=fdt_add_subnode(Dtb,0,"chosen");assert(N>=0);
  assert(fdt_setprop_string(Dtb,N,"bootargs","androidboot.hwlevel=MP")==0);
  assert(fdt_setprop_u64(Dtb,N,"linux,initrd-start",0x84280000)==0);
  assert(fdt_setprop_u64(Dtb,N,"kaslr-seed",1)==0);
  N=fdt_add_subnode(Dtb,0,"memory");assert(N>=0);
  fdt64_t Reg[]={cpu_to_fdt64(0x80000000ULL),cpu_to_fdt64(0x100000000ULL)};
  assert(fdt_setprop(Dtb,N,"reg",Reg,sizeof(Reg))==0);
  N=fdt_add_subnode(Dtb,0,"reserved-memory");assert(N>=0);
  N=fdt_add_subnode(Dtb,N,"firmware@90000000");assert(N>=0);
  Reg[0]=cpu_to_fdt64(0x90000000ULL);Reg[1]=cpu_to_fdt64(0x200000ULL);
  assert(fdt_setprop(Dtb,N,"reg",Reg,sizeof(Reg))==0);
}

static void TestPlacement(void) {
  Build();
  int Parent=fdt_path_offset(Dtb,"/reserved-memory");
  int Node=fdt_add_subnode(Dtb,Parent,"ramdump@af000000");
  assert(Node>=0);
  fdt64_t Range[]={cpu_to_fdt64(0xaf000000),cpu_to_fdt64(0x01000000)};
  assert(fdt_setprop(Dtb,Node,"reg",Range,sizeof(Range))==0);
  assert(fdt_add_mem_rsv(Dtb,0xaffff001,0x7ff)==0); // Overlaps the same page.
  // Naive placement in the EFI free range would start at 0xae600000.
  assert((UINTN)PolarisAndroidAllocatePages(Dtb,0x4000,0x200000,0xcffff000)==0xab000000);
  assert(PageAttempts==1 && PoolCount==0);
  RefuseAddress=0xab000000;
  Map[1].PhysicalStart=0xaa000000;
  Map[1].NumberOfPages+=0x1000;
  assert((UINTN)PolarisAndroidAllocatePages(Dtb,0x4000,0x200000,0xcffff000)==0xaae00000);
  assert(PageAttempts==3 && PoolCount==0);
  Map[1].PhysicalStart=0xae600000;
  Map[1].NumberOfPages=0x4000;
  assert(PolarisAndroidAllocatePages(Dtb,0x4000,0x200000,0xcffff000)==NULL);
  assert(PageAttempts==3 && PoolCount==0);
}

int main(int argc,char **argv) {
  // Exercise other header helpers too; tests share the exact launcher contract.
  POLARIS_ANDROID_REQUEST R={0};assert(!PolarisAndroidRequestValid(&R,sizeof(R)));
  assert(!PolarisAndroidImageValid(Image,sizeof(Image)));
  Build();assert(PolarisAndroidDtbValid(Dtb,sizeof(Dtb)));
  for (UINTN N=0;N<40;++N) assert(!PolarisAndroidDtbValid(Dtb,N));
  assert(!PolarisAndroidDtbValid(Dtb,sizeof(Dtb)-1));
  assert(PolarisAndroidDramRange(Dtb,0xa0000000,0x4000000));
  assert(!PolarisAndroidDramRange(Dtb,0x8ff00000,0x200000));
  assert(!PolarisAndroidDramRange(Dtb,0x90080000,0x1000));
  assert(!PolarisAndroidDramRange(Dtb,MAX_UINT64-1,4));
  assert(!PolarisAndroidDramRange(Dtb,0x7fffffff,1));
  assert(fdt_add_mem_rsv(Dtb,0xa0000000,0x1000)==0);
  assert(!PolarisAndroidDramRange(Dtb,0xa0000000,0x1000));
  assert(PolarisAndroidDtbSanitize(Dtb));
  int N=fdt_path_offset(Dtb,"/chosen"),Len;
  assert(!fdt_getprop(Dtb,N,"linux,initrd-start",&Len));
  assert(!fdt_getprop(Dtb,N,"kaslr-seed",&Len));
  assert(fdt_getprop(Dtb,N,"bootargs",&Len));
  assert(PolarisAndroidDtbValid(Dtb,sizeof(Dtb)));
  assert(fdt_setprop_u32(Dtb,0,"#address-cells",1)==0);
  assert(!PolarisAndroidDtbValid(Dtb,sizeof(Dtb)));
  Build();fdt32_t Wrong[]={cpu_to_fdt32(0x2a),0};
  assert(fdt_setprop(Dtb,0,"qcom,board-id",Wrong,sizeof(Wrong))==0);
  assert(!PolarisAndroidDtbValid(Dtb,sizeof(Dtb)));
  Build();assert(fdt_setprop_string(Dtb,0,"model","Xiaomi Technologies, Inc. Polaris P2 v2.1")==0);
  assert(!PolarisAndroidDtbValid(Dtb,sizeof(Dtb)));
  TestPlacement();
  if (argc==2) {
    static UINT8 Native[POLARIS_ANDROID_DTB_LIMIT];FILE *F=fopen(argv[1],"rb");assert(F);
    size_t Size=fread(Native,1,sizeof(Native),F);assert(!ferror(F));fclose(F);
    assert(PolarisAndroidDtbValid(Native,Size));
    assert(PolarisAndroidDtbSanitize(Native));
  }
  puts("PASS: native MP FDT identity, DRAM reservations, aligned page placement and stale handoff properties");
  return 0;
}
