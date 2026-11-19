// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../efi/Android/WarmReset.c"
EFI_BOOT_SERVICES *gBS;
static EFI_BOOT_SERVICES Bs;
static UINT8 Registers[5][4096];
static unsigned Writes,FailWrite,BadRead,Stalls;
static UINT32 EFIAPI MockRead(POLARIS_SPMI *P,UINT32 Sid,UINT32 Priority,UINT32 Reg,
                              UINT8 *Value,UINT32 Size,UINT32 *Bytes) {
  (void)P;assert(Sid<=4 && !(Sid&1) && !Priority && Reg<4096 && Size==1);
  *Value=Registers[Sid][Reg];*Bytes=BadRead?0:1;return 0;
}
static UINT32 EFIAPI MockWrite(POLARIS_SPMI *P,UINT32 Sid,UINT32 Priority,UINT32 Reg,
                               UINT8 *Value,UINT32 Size) {
  (void)P;assert(!Sid && !Priority && (Reg==0x85a || Reg==0x85b) && Size==1);
  ++Writes;
  if (Writes==FailWrite) return 1;
  if (Reg==0x85a) assert(!Registers[0][0x85b]);
  Registers[Sid][Reg]=*Value;return 0;
}
static EFI_STATUS EFIAPI Stall(UINTN Microseconds) { assert(Microseconds==500);++Stalls;return EFI_SUCCESS; }
UINTN EFIAPI Print(CONST CHAR16 *Format,...) { (void)Format;return 0; }
static POLARIS_SPMI P={0x10000,MockRead,MockWrite,NULL,NULL};
static void Init(void) {
  unsigned Sid;memset(Registers,0,sizeof(Registers));
  for(Sid=0;Sid<=4;Sid+=2) {
    Registers[Sid][0x104]=0x51;
    Registers[Sid][0x105]=Sid==0?0x14:Sid==2?0x15:0x18;
    Registers[Sid][0x804]=1;Registers[Sid][0x805]=Sid?5:4;
    Registers[Sid][0x85a]=Sid?4:7;Registers[Sid][0x85b]=Sid?0:0x80;
  }
  P.Revision=0x10000;Writes=FailWrite=BadRead=Stalls=0;Bs.Stall=Stall;gBS=&Bs;
}
int main(void) {
  POLARIS_ANDROID_BINDING B={0x3152444e414c4f50ULL,1,64,67108864,0x12345678,
    0x5241,128,0x17fe00000,0x100,0xa01000,0};
  assert(PolarisAndroidBindingValid(&B));
  B.FlagCount=127;assert(!PolarisAndroidBindingValid(&B));B.FlagCount=128;
  B.FlagAddress+=2;assert(!PolarisAndroidBindingValid(&B));B.FlagAddress-=2;
  B.KernelEntry=B.ImageSize;assert(!PolarisAndroidBindingValid(&B));B.KernelEntry=0x100;
  B.Reserved=1;assert(!PolarisAndroidBindingValid(&B));B.Reserved=0;
  Init();assert(BoardGuard(&P,7)==EFI_SUCCESS && !Writes);
  assert(ConfigureWarmReset(&P)==EFI_SUCCESS && Writes==3 && Stalls==1);
  assert(Registers[0][0x85a]==1 && Registers[0][0x85b]==0x80);
  assert(BoardGuard(&P,1)==EFI_SUCCESS && BoardGuard(&P,7)==EFI_UNSUPPORTED);
  Init();P.Revision++;assert(BoardGuard(&P,7)==EFI_UNSUPPORTED && !Writes);
  Init();Registers[2][0x85b]=0x80;assert(BoardGuard(&P,7)==EFI_UNSUPPORTED && !Writes);
  Init();Registers[4][0x105]=0;assert(BoardGuard(&P,7)==EFI_UNSUPPORTED && !Writes);
  Init();BadRead=1;assert(BoardGuard(&P,7)==EFI_UNSUPPORTED && !Writes);
  for(unsigned Failure=1;Failure<=3;++Failure) {
    Init();FailWrite=Failure;
    assert(EFI_ERROR(ConfigureWarmReset(&P)));
    assert(Registers[0][0x85a]==7 && Registers[0][0x85b]==0x80);
    assert(Registers[2][0x85a]==4 && !Registers[2][0x85b]);
    assert(Registers[4][0x85a]==4 && !Registers[4][0x85b]);
  }
  puts("PASS: Android binding bounds, PMIC identity/secondary guards, warm sequence and write-failure rollback");
  return 0;
}
