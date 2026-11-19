// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../efi/LinuxBoot/NativeMemory.h"
static void put(unsigned char *p,unsigned long long v,unsigned n) {
  for(unsigned i=0;i<n;i++) {p[i]=v;v>>=8;}
}
int main(void) {
  unsigned char image[4096]={0};
  UINT64 offset,memory;UINTN count;
  POLARIS_NATIVE_RANGE ranges[POLARIS_NATIVE_MAX_RANGES];
  struct {EFI_MEMORY_DESCRIPTOR d;UINT64 extension;} map[6]={0};
  put(image+8,0x80000,8);put(image+16,0x2000,8);
  put(image+24,0xa,8);put(image+56,0x644d5241,4);
  assert(NativeImage(image,sizeof(image),&offset,&memory));
  assert(offset==0x80000 && memory==0x2000);
  image[24]=0xb;assert(!NativeImage(image,sizeof(image),&offset,&memory));
  image[24]=0x1a;assert(!NativeImage(image,sizeof(image),&offset,&memory));
  image[24]=0xa;put(image+8,0x80001,8);
  assert(!NativeImage(image,sizeof(image),&offset,&memory));
  put(image+8,0x80000,8);put(image+16,0xfff,8);
  assert(!NativeImage(image,sizeof(image),&offset,&memory));
  put(image+16,0x2000,8);image[56]=0;
  assert(!NativeImage(image,sizeof(image),&offset,&memory));
  assert(!NativeImage(image,63,&offset,&memory));
  map[0].d=(EFI_MEMORY_DESCRIPTOR){.Type=EfiLoaderCode,.PhysicalStart=0x100000000ULL,.NumberOfPages=16,.Attribute=EFI_MEMORY_WB};
  map[1].d=(EFI_MEMORY_DESCRIPTOR){.Type=EfiConventionalMemory,.PhysicalStart=0x80000000,.NumberOfPages=16,.Attribute=EFI_MEMORY_WB};
  map[2].d=(EFI_MEMORY_DESCRIPTOR){.Type=EfiBootServicesData,.PhysicalStart=0x80010000,.NumberOfPages=16,.Attribute=EFI_MEMORY_WB};
  map[3].d=(EFI_MEMORY_DESCRIPTOR){.Type=EfiReservedMemoryType,.PhysicalStart=0x80020000,.NumberOfPages=32,.Attribute=EFI_MEMORY_WT};
  map[4].d=(EFI_MEMORY_DESCRIPTOR){.Type=EfiRuntimeServicesData,.PhysicalStart=0x80040000,.NumberOfPages=16,.Attribute=EFI_MEMORY_WB};
  map[5].d=(EFI_MEMORY_DESCRIPTOR){.Type=EfiMemoryMappedIO,.PhysicalStart=0x1000000,.NumberOfPages=16,.Attribute=EFI_MEMORY_WB};
  assert(NativeRanges(map,sizeof(map),sizeof(map[0]),ranges,&count));
  assert(count==2 && ranges[0].Base==0x80000000 && ranges[0].End==0x80020000);
  assert(ranges[1].Base==0x100000000ULL && ranges[1].End==0x100010000ULL);
  assert(NativeCovered(ranges,count,0x8001f000,4096));
  assert(!NativeCovered(ranges,count,0x8001f000,4097));
  assert(!NativeCovered(ranges,count,0x80020000,1));
  assert(!NativeCovered(ranges,count,MAX_UINT64-15,32));
  assert(!NativeRanges(map,sizeof(map)-1,sizeof(map[0]),ranges,&count));
  map[2].d.PhysicalStart=0x8000f000;
  assert(!NativeRanges(map,sizeof(map),sizeof(map[0]),ranges,&count));
  map[2].d.PhysicalStart=MAX_UINT64-4095;map[2].d.NumberOfPages=2;
  assert(!NativeRanges(map,sizeof(map),sizeof(map[0]),ranges,&count));
  puts("PASS: native Image validation, extended/unsorted EFI maps, reserved holes and bounds");
  return 0;
}
