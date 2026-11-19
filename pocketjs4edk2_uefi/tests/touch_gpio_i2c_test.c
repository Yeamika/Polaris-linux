/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFII2C.h>
#include <Protocol/EFITlmm.h>
#include "../platform/PolarisTouchDxe/TouchGpioI2c.h"

EFI_BOOT_SERVICES *gBS;
static EFI_BOOT_SERVICES Bs;
static EFI_QCOM_TLMM_PROTOCOL Tlmm;
static UINT64 Now;
static unsigned Low[2], Latch[2], Function[2], Samples[256], SampleCount, SampleIndex;
static unsigned Clocks[4096], ClockCount, ConfigCalls, FailConfig, StuckClock;
static unsigned PinIndex(UINT32 C) {
  unsigned Pin=(C>>4)&0x3ff;
  assert(Pin==33 || Pin==34);
  return Pin-33;
}
static EFI_STATUS EFIAPI Config(UINT32 C,UINT32 Enable) {
  unsigned P=PinIndex(C), NextLow=((C>>14)&1);
  assert(Enable==TLMM_GPIO_ENABLE);
  Now+=1000;
  if (++ConfigCalls==FailConfig) return EFI_DEVICE_ERROR;
  Function[P]=C&15;
  if (!Function[P]) {
    if (NextLow) assert(Latch[P]==0); /* Never drive either wire high. */
    if (P==1 && Low[P] && !NextLow) Clocks[ClockCount++]=!Low[0];
    Low[P]=NextLow;
  } else assert(Function[P]==2 && !NextLow);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Out(UINT32 C,UINT32 V) {
  assert(V==GPIO_LOW_VALUE);
  Latch[PinIndex(C)]=V; Now+=1000; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI In(UINT32 C,UINT32 *Value) {
  unsigned P=PinIndex(C);
  Now+=1000;
  if (P==1) *Value=!Low[1] && !StuckClock;
  else {
    assert(!Low[0] && SampleIndex<SampleCount);
    *Value=Samples[SampleIndex++];
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Stall(UINTN Us) { Now+=Us*1000; return EFI_SUCCESS; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { return Now; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks) { return Ticks; }
static void Reset(void) {
  memset(&mGpioI2c,0,sizeof(mGpioI2c)); mGpioI2c.Tlmm=&Tlmm;
  Low[0]=Low[1]=0; Latch[0]=Latch[1]=1; Function[0]=Function[1]=2;
  SampleCount=SampleIndex=ClockCount=ConfigCalls=FailConfig=StuckClock=0; Now=0;
}
static void Sample(unsigned V) { Samples[SampleCount++]=V; }
static void InputByte(unsigned V) { for (unsigned i=0;i<8;++i) Sample((V>>(7-i))&1); }
static void CheckByte(unsigned *Pos,unsigned V) {
  for (unsigned i=0;i<8;++i) assert(Clocks[(*Pos)++]==((V>>(7-i))&1));
  assert(Clocks[(*Pos)++]==1); /* Master releases SDA for target ACK. */
}
int main(void) {
  VOID *Bus;
  I2C_SLAVE_CONFIG Slave={0}; UINT32 Done; UINT8 Data[2]; unsigned Pos, WriteConfigCalls;
  gBS=&Bs; Bs.Stall=Stall; Tlmm.ConfigGpio=Config; Tlmm.GpioOut=Out; Tlmm.GpioIn=In;
  Slave.Mode=I2C; Slave.SlaveAddress=0x20;
  Reset(); assert(mGpioI2cProtocol.Open(15,&Bus)==I2C_SUCCESS);
  Sample(1); Sample(0); Sample(0); Sample(0); Data[0]=0xa5;
  assert(mGpioI2cProtocol.Write(Bus,&Slave,0x80,1,Data,1,&Done,10)==I2C_SUCCESS && Done==1);
  Pos=0; CheckByte(&Pos,0x40); CheckByte(&Pos,0x80); CheckByte(&Pos,0xa5);
  assert(Clocks[Pos++]==0 && Pos==ClockCount && SampleIndex==SampleCount);
  assert(!Low[0] && !Low[1]);
  WriteConfigCalls=ConfigCalls;
  assert(mGpioI2cProtocol.Close(Bus)==I2C_SUCCESS && Function[0]==2 && Function[1]==2);

  Reset(); assert(mGpioI2cProtocol.Open(15,&Bus)==I2C_SUCCESS);
  Sample(1); Sample(0); Sample(0); Sample(0); FailConfig=WriteConfigCalls;
  assert(mGpioI2cProtocol.Write(Bus,&Slave,0x80,1,Data,1,&Done,10)==I2C_ERROR_COMMAND_FAIL);
  assert(Done==1 && !Low[0] && !Low[1]); /* STOP release failure is reported and retried. */

  Reset(); assert(mGpioI2cProtocol.Open(15,&Bus)==I2C_SUCCESS);
  Sample(1); Sample(0); Sample(0); Sample(1); Sample(0);
  InputByte(0x37); InputByte(0xe9);
  assert(mGpioI2cProtocol.Read(Bus,&Slave,0xe9,1,Data,2,&Done,10)==I2C_SUCCESS);
  assert(Done==2 && Data[0]==0x37 && Data[1]==0xe9 && SampleIndex==SampleCount);
  Pos=0; CheckByte(&Pos,0x40); CheckByte(&Pos,0xe9);
  assert(Clocks[Pos++]==1); /* Repeated START releases both wires first. */
  CheckByte(&Pos,0x41);
  for (unsigned byte=0;byte<2;++byte) {
    for (unsigned i=0;i<8;++i) assert(Clocks[Pos++]==1);
    assert(Clocks[Pos++]==byte); /* ACK first byte; NACK final byte. */
  }
  assert(Clocks[Pos++]==0 && Pos==ClockCount && !Low[0] && !Low[1]);
  assert(mGpioI2cProtocol.Close(Bus)==I2C_SUCCESS);

  Reset(); assert(mGpioI2cProtocol.Open(15,&Bus)==I2C_SUCCESS);
  Sample(1); Sample(1); Done=99;
  assert(mGpioI2cProtocol.Read(Bus,&Slave,0,1,Data,1,&Done,10)==I2C_ERROR_ADDR_NACK);
  assert(Done==0 && !Low[0] && !Low[1]);

  Reset(); assert(mGpioI2cProtocol.Open(15,&Bus)==I2C_SUCCESS); StuckClock=1;
  assert(mGpioI2cProtocol.Read(Bus,&Slave,0,1,Data,1,&Done,1)==I2C_ERROR_TRANSFER_TIMEOUT);
  assert(Done==0 && Now<1100000 && !Low[0] && !Low[1]);
  assert(mGpioI2cProtocol.Read(Bus,&Slave,0,1,Data,1,&Done,0)==I2C_ERROR_INVALID_PARAMETER);
  assert(mGpioI2cProtocol.Read(Bus,&Slave,0,1,Data,1,&Done,101)==I2C_ERROR_INVALID_PARAMETER);

  Reset(); FailConfig=2;
  assert(mGpioI2cProtocol.Open(15,&Bus)==I2C_ERROR_PLATFORM_GPIO_ENABLE_FAIL);
  assert(!mGpioI2c.Open && !Bus && Function[0]==2 && Function[1]==2);
  Reset(); assert(mGpioI2cProtocol.Open(15,&Bus)==I2C_SUCCESS); FailConfig=ConfigCalls+1;
  assert(mGpioI2cProtocol.Close(Bus)==I2C_ERROR_PLATFORM_GPIO_DISABLE_FAIL);
  assert(Function[1]==2 && !mGpioI2c.Open); /* Failure on SDA still restores SCL. */
  puts("PASS: GPIO I2C wire framing, repeated START, ACK/NACK, deadline, open drain and pin restoration");
  return 0;
}
