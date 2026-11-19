// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../efi/Android/BootRequest.h"
#include "../efi/Android/Kernel.h"
static UINT8 Image[16384];
static void Put(unsigned Offset,UINT32 Value) {
  for(unsigned I=0;I<4;++I) Image[Offset+I]=(UINT8)(Value>>(8*I));
}
int main(int argc,char **argv) {
  POLARIS_ANDROID_REQUEST R={POLARIS_ANDROID_MAGIC,POLARIS_ANDROID_REVISION,sizeof(R),
    0x90000000,16384,0,0,0x94000000,4096};
  assert(PolarisAndroidRequestValid(&R,sizeof(R)));
  for (UINTN N=0;N<sizeof(R);++N) assert(!PolarisAndroidRequestValid(&R,N));
  R.Revision=2;assert(!PolarisAndroidRequestValid(&R,sizeof(R)));R.Revision=POLARIS_ANDROID_REVISION;
  R.Image=MAX_UINT64-8192;assert(!PolarisAndroidRequestValid(&R,sizeof(R)));R.Image=0x90000000;
  R.Dtb=MAX_UINT64-2048;assert(!PolarisAndroidRequestValid(&R,sizeof(R)));R.Dtb=0x94000000;
  R.Flags=2;assert(!PolarisAndroidRequestValid(&R,sizeof(R)));R.Flags=POLARIS_ANDROID_PREPARE_ONLY;
  assert(PolarisAndroidRequestValid(&R,sizeof(R)));
  R.Reserved=1;assert(!PolarisAndroidRequestValid(&R,sizeof(R)));
  UINT64 Footprint;UINT8 Kernel[64]={0};
  memcpy(Kernel+56,"ARMd",4);Kernel[10]=8;Kernel[18]=0x10;
  assert(PolarisAndroidKernelValid(Kernel,sizeof(Kernel),0x90080000,0x200000,&Footprint));
  assert(Footprint==0x100000);
  assert(!PolarisAndroidKernelValid(Kernel,sizeof(Kernel),0x90080000,0x1000,&Footprint));
  assert(!PolarisAndroidKernelValid(Kernel,sizeof(Kernel),0x90081000,0x200000,&Footprint));
  Kernel[24]=1;assert(!PolarisAndroidKernelValid(Kernel,sizeof(Kernel),0x90080000,0x200000,&Footprint));
  memcpy(Image,"ANDROID!",8);Put(8,4096);Put(16,1024);Put(36,4096);
  assert(PolarisAndroidImageValid(Image,sizeof(Image)));
  assert(!PolarisAndroidImageValid(Image,8192));
  Put(8,0xffffffff);assert(!PolarisAndroidImageValid(Image,sizeof(Image)));Put(8,4096);
  Put(36,4095);assert(!PolarisAndroidImageValid(Image,sizeof(Image)));Put(36,4096);
  Put(40,1);assert(PolarisAndroidImageValid(Image,sizeof(Image)));
  Put(1632,1);assert(!PolarisAndroidImageValid(Image,sizeof(Image)));Put(1632,0);
  Put(40,2);assert(!PolarisAndroidImageValid(Image,sizeof(Image)));Put(40,1);
  Put(24,1);assert(!PolarisAndroidImageValid(Image,sizeof(Image)));Put(24,0);
  Image[0]=0;assert(!PolarisAndroidImageValid(Image,sizeof(Image)));
  if(argc==2) {
    static UINT8 Original[64*1024*1024];FILE *F=fopen(argv[1],"rb");assert(F);
    size_t N=fread(Original,1,sizeof(Original),F);assert(!ferror(F));fclose(F);
    assert(PolarisAndroidImageValid(Original,N));
  }
  puts("PASS: Android request bounds, kernel placement/BSS and legacy boot image validation");
  return 0;
}
