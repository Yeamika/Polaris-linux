/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
extern EFI_GUID gQcomI2cProtocolGuid; /* Normally supplied by EDK2 AutoGen. */
#include "../platform/PolarisTouchDxe/PolarisTouchDxe.c"

EFI_BOOT_SERVICES *gBS;
static EFI_BOOT_SERVICES Bs;
static EFI_QCOM_I2C_PROTOCOL I2c;
static UINT8 Report[20];
static unsigned ShortRead, Signals, Cancels, Closes, Rearms;
static unsigned PowerGood, PowerGpios, PowerDelays;
static unsigned FailSchedule;
static unsigned ReportReads, FullReads, ShortReport, IgnoreEnableWrite;
static UINT8 IrqStatus=4, Enables[4]={0x81,0x42,0x24,0x18};
static EFI_STATUS EFIAPI PowerStatus(UINT32 Pmic, EFI_PM_VREG_ID_TYPE Id, EFI_PM_VREG_STATUS_TYPE *State) {
  assert(Pmic==0 && Id==EFI_PM_LDO_28);
  memset(State,0,sizeof(*State)); State->SwEnable=PowerGood; State->VregOk=PowerGood;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI PowerConfig(UINT32 Config, UINT32 Enable) {
  unsigned Pin=(Config>>4)&0x3ff;
  assert(Enable==TLMM_GPIO_ENABLE && (Pin==23 || Pin==99 || Pin==125));
  ++PowerGpios; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI PowerOut(UINT32 Config, UINT32 Value) {
  unsigned Pin=(Config>>4)&0x3ff;
  assert((Pin==23 && Value==1) || (Pin==99 && Value<=1));
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI PowerStall(UINTN Us) { assert(Us==200000); ++PowerDelays; return EFI_SUCCESS; }
static EFI_TPL EFIAPI Raise(EFI_TPL Tpl) { assert(Tpl==TPL_NOTIFY); return TPL_APPLICATION; }
static VOID EFIAPI Restore(EFI_TPL Tpl) { assert(Tpl==TPL_APPLICATION); }
static EFI_STATUS EFIAPI Signal(EFI_EVENT Event) { ++Signals; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Timer(EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 Trigger) {
  assert(Type==TimerCancel || (Type==TimerRelative && Trigger==200000));
  if(Type==TimerCancel) ++Cancels; else ++Rearms;
  if(Type==TimerRelative && FailSchedule) return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}
static I2C_STATUS EFIAPI Read(VOID *Bus, I2C_SLAVE_CONFIG *Cfg, UINT16 Offset,
  UINT16 OffsetLength, UINT8 *Buffer, UINT16 Length, UINT32 *Done, UINT32 Timeout) {
  assert(Bus==(VOID *)1 && OffsetLength==1 && Timeout==TOUCH_I2C_TIMEOUT_MS);
  memset(Buffer,0,Length);
  if (Offset==0x20) {
    assert(Length==12 || Length==sizeof(Report)); memcpy(Buffer,Report,Length);
    ++ReportReads; if(Length==sizeof(Report)) ++FullReads;
  } else if (Offset>=0x31 && Offset<=0x34) {
    assert(Length==1); Buffer[0]=Enables[Offset-0x31];
  } else { assert(Offset==0x10 && Length==2); Buffer[1]=IrqStatus; }
  *Done=(ShortRead || (ShortReport && Offset==0x20)) ? Length-1 : Length;
  return I2C_SUCCESS;
}
static I2C_STATUS EFIAPI Write(VOID *Bus, I2C_SLAVE_CONFIG *Cfg, UINT16 Offset,
  UINT16 OffsetLength, UINT8 *Buffer, UINT16 Length, UINT32 *Done, UINT32 Timeout) {
  assert(Bus==(VOID *)1 && OffsetLength==1 && Timeout==TOUCH_I2C_TIMEOUT_MS);
  assert(Offset>=0x31 && Offset<=0x34 && Length==1);
  if(!IgnoreEnableWrite) Enables[Offset-0x31]=Buffer[0];
  *Done=1; return I2C_SUCCESS;
}
static I2C_STATUS EFIAPI Close(VOID *Bus) { assert(Bus==(VOID *)1); ++Closes; return I2C_SUCCESS; }
VOID *EFIAPI ZeroMem(VOID *Dst,UINTN Size) { return memset(Dst,0,Size); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) { }
VOID EFIAPI DebugAssert(CONST CHAR8 *FileName,UINTN LineNumber,CONST CHAR8 *Description) { abort(); }
BOOLEAN EFIAPI DebugAssertEnabled(VOID) { return TRUE; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { abort(); } /* This test substitutes the bus protocol. */
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks) { abort(); }

int main(void) {
  EFI_ABSOLUTE_POINTER_STATE State;
  EFI_QCOM_PMIC_VREG_PROTOCOL Vreg={0}; EFI_QCOM_TLMM_PROTOCOL Tlmm={0};
  gBS=&Bs; Bs.RaiseTPL=Raise; Bs.RestoreTPL=Restore; Bs.SignalEvent=Signal; Bs.SetTimer=Timer;
  Vreg.VregGetStatus=PowerStatus; mTouch.Vreg=&Vreg;
  Tlmm.ConfigGpio=PowerConfig; Tlmm.GpioOut=PowerOut; mTouch.Tlmm=&Tlmm; Bs.Stall=PowerStall;
  /* No PMIC write functions are provided: initialization must reuse L28A
   * and enable the fixed VIO rail through GPIO23, without touching L6A. */
  assert(TouchPower()==EFI_NOT_READY && PowerGpios==0);
  PowerGood=1; assert(TouchPower()==EFI_SUCCESS && PowerGpios==4 && PowerDelays==2);
  I2c.Read=Read; I2c.Write=Write; I2c.Close=Close;
  mTouch.Ready=TRUE; mTouch.I2c=&I2c; mTouch.Bus=(VOID *)1; mTouch.Page=0;
  mTouch.F01Control=0x30; mTouch.IrqBytes=4;
  mTouch.IrqMask[0]=4; mTouch.IrqMask[1]=0x80; mTouch.IrqMask[3]=0x80;
  assert(TouchEnableInterrupts()==EFI_SUCCESS);
  assert(Enables[0]==0x85 && Enables[1]==0xc2 && Enables[2]==0x24 && Enables[3]==0x98);
  Enables[0]=0; IgnoreEnableWrite=1;
  assert(TouchEnableInterrupts()==EFI_DEVICE_ERROR);
  IgnoreEnableWrite=0; memset(mTouch.IrqMask,0,sizeof(mTouch.IrqMask)); mTouch.IrqMask[0]=4;
  mTouch.F01Data=0x10; mTouch.F12Data=0x20; mTouch.IrqBytes=1;
  mTouch.DataOffset=4; mTouch.DataLength=16;
  mTouch.Mode.AbsoluteMaxX=1080; mTouch.Mode.AbsoluteMaxY=2160;
  assert(TouchGetState(NULL,&State)==EFI_NOT_READY);
  assert(TouchGetState(NULL,NULL)==EFI_INVALID_PARAMETER);
  /* Contact 1 is active while contact 0 is empty; Data0 is skipped. */
  Report[12]=1; Report[13]=123; Report[15]=234;
  TouchPoll(NULL,NULL);
  assert(ReportReads==2 && FullReads==1);
  assert(TouchGetState(NULL,&State)==EFI_SUCCESS && State.ActiveButtons==EFI_ABSP_TouchActive);
  assert(State.CurrentX==123 && State.CurrentY==234);
  memset(Report,0,sizeof(Report)); TouchPoll(NULL,NULL);
  assert(TouchGetState(NULL,&State)==EFI_SUCCESS && State.ActiveButtons==0);
  assert(TouchGetState(NULL,&State)==EFI_NOT_READY);
  /* Idle and another function's interrupt must not read the large report. */
  unsigned SavedReads=ReportReads;
  IrqStatus=0; TouchPoll(NULL,NULL); IrqStatus=1; TouchPoll(NULL,NULL);
  assert(ReportReads==SavedReads && mTouch.Count==0);
  assert(TouchReset(&mTouch.Pointer,FALSE)==EFI_SUCCESS && mTouch.SampleRequired);
  IrqStatus=0; TouchPoll(NULL,NULL);
  assert(ReportReads>SavedReads && !mTouch.SampleRequired && mTouch.Count==0);
  /* A short read after acknowledgement still retries without another IRQ. */
  IrqStatus=4; Report[4]=1; Report[5]=51; ShortReport=1; TouchPoll(NULL,NULL);
  assert(mTouch.SampleRequired && mTouch.Count==0);
  ShortReport=0; IrqStatus=0; TouchPoll(NULL,NULL);
  assert(!mTouch.SampleRequired && mTouch.Errors==0);
  assert(TouchGetState(NULL,&State)==EFI_SUCCESS && State.CurrentX==51 && State.ActiveButtons==1);
  IrqStatus=4; memset(Report,0,sizeof(Report)); TouchPoll(NULL,NULL);
  assert(TouchGetState(NULL,&State)==EFI_SUCCESS && State.ActiveButtons==0);
  /* An entire tap between two UI frames retains both transitions. */
  Report[4]=6; Report[5]=42; TouchPoll(NULL,NULL);
  unsigned SavedFull=FullReads;
  for (unsigned i=0;i<100;++i) { Report[5]=(UINT8)(42+i); TouchPoll(NULL,NULL); }
  assert(FullReads==SavedFull);
  memset(Report,0,sizeof(Report)); TouchPoll(NULL,NULL);
  assert(mTouch.Count==2);
  assert(TouchGetState(NULL,&State)==EFI_SUCCESS && State.ActiveButtons==1);
  assert(TouchGetState(NULL,&State)==EFI_SUCCESS && State.ActiveButtons==0);
  /* Palms do not press buttons; malformed short reads cancel a held touch. */
  Report[4]=3; TouchPoll(NULL,NULL); assert(mTouch.Count==0);
  Report[4]=1; TouchPoll(NULL,NULL);
  assert(TouchGetState(NULL,&State)==EFI_SUCCESS && State.ActiveButtons==1);
  ShortRead=1;
  for (unsigned i=0;i<5;++i) TouchPoll(NULL,NULL);
  assert(mTouch.Stopped && Cancels==1 && Rearms>100);
  assert(TouchGetState(NULL,&State)==EFI_SUCCESS && State.ActiveButtons==0);
  assert(TouchGetState(NULL,&State)==EFI_DEVICE_ERROR);
  /* A failed rearm cancels a held contact and leaves no polling loop. */
  mTouch.Stopped=FALSE; ShortRead=0; FailSchedule=1; Report[4]=1;
  TouchPoll(NULL,NULL);
  assert(mTouch.Stopped);
  assert(TouchGetState(NULL,&State)==EFI_SUCCESS && State.ActiveButtons==1);
  assert(TouchGetState(NULL,&State)==EFI_SUCCESS && State.ActiveButtons==0);
  assert(TouchGetState(NULL,&State)==EFI_DEVICE_ERROR);
  TouchExit(NULL,NULL); TouchExit(NULL,NULL);
  assert(Closes==1 && Signals>=6);
  puts("PASS: Polaris touch IRQ enables/idle, short reports/retry, other contacts, press/release and EBS cleanup");
  return 0;
}
