/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EFII2C.h>
#include <Protocol/EFIPmicVreg.h>
#include <Protocol/EFITlmm.h>
#include "TouchRmi.h"
#include "TouchGpioI2c.h"
#include "../BootConsole.h"

#define TOUCH_I2C_CORE 15 /* IC15 / QUPv3 SE14: firmware instances are one-based. */
#define TOUCH_IRQ 125
#define TOUCH_RESET 99
#define TOUCH_VIO 23
#define TOUCH_QUEUE_SIZE 32
#define TOUCH_PACKET_MAX 128
#define TOUCH_I2C_TIMEOUT_MS 50

typedef struct {
  EFI_ABSOLUTE_POINTER_PROTOCOL Pointer;
  EFI_ABSOLUTE_POINTER_MODE Mode;
  EFI_ABSOLUTE_POINTER_STATE Latest;
  EFI_ABSOLUTE_POINTER_STATE Queue[TOUCH_QUEUE_SIZE];
  UINTN Head, Count;
  EFI_QCOM_I2C_PROTOCOL *I2c;
  EFI_QCOM_TLMM_PROTOCOL *Tlmm;
  EFI_QCOM_PMIC_VREG_PROTOCOL *Vreg;
  I2C_SLAVE_CONFIG Slave;
  VOID *Bus;
  EFI_HANDLE Handle;
  EFI_EVENT Timer, ExitEvent;
  UINT16 F01Query, F01Control, F01Data;
  UINT16 F12Query, F12Control, F12Data;
  UINT16 DataOffset, DataLength, IrqBytes;
  UINT8 IrqMask[4];
  UINT8 Page;
  UINTN Errors;
  BOOLEAN Stopped, Ready, SampleRequired;
} POLARIS_TOUCH;

STATIC POLARIS_TOUCH mTouch;
STATIC struct {
  VENDOR_DEVICE_PATH Vendor;
  EFI_DEVICE_PATH_PROTOCOL End;
} mDevicePath = {
  {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof(VENDOR_DEVICE_PATH), 0}},
   {0xb930d3c4, 0x53fa, 0x4d65, {0xb3,0x2b,0x19,0xd1,0x4c,0x80,0x48,0xe7}}},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}}
};

STATIC EFI_STATUS
TouchWriteRaw (UINT8 Address, UINT8 *Data, UINT16 Length)
{
  UINT32 Written = 0;
  I2C_STATUS Result = mTouch.I2c->Write(mTouch.Bus, &mTouch.Slave,
    Address, 1, Data, Length, &Written, TOUCH_I2C_TIMEOUT_MS);
  /* BSP revisions count the register prefix either separately or as part
   * of the write transfer; neither permits a short payload. */
  if (I2C_ERROR(Result) || (Written != Length && Written != (UINT32)Length + 1))
    return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
TouchPage (UINT16 Address)
{
  UINT8 Page = Address >> 8;
  EFI_STATUS Status;
  if (mTouch.Page == Page) return EFI_SUCCESS;
  Status = TouchWriteRaw(0xff, &Page, 1);
  if (!EFI_ERROR(Status)) mTouch.Page = Page;
  return Status;
}

STATIC EFI_STATUS
TouchRead (UINT16 Address, UINT8 *Data, UINT16 Length)
{
  UINT32 Read = 0;
  I2C_STATUS Result;
  EFI_STATUS Status = TouchPage(Address);
  if (EFI_ERROR(Status)) return Status;
  Result = mTouch.I2c->Read(mTouch.Bus, &mTouch.Slave,
    Address & 0xff, 1, Data, Length, &Read, TOUCH_I2C_TIMEOUT_MS);
  if (I2C_ERROR(Result) || Read != Length) return EFI_DEVICE_ERROR;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
TouchWrite (UINT16 Address, UINT8 Value)
{
  EFI_STATUS Status = TouchPage(Address);
  if (EFI_ERROR(Status)) return Status;
  return TouchWriteRaw(Address & 0xff, &Value, 1);
}

STATIC EFI_STATUS
TouchGpio (UINT32 Pin, UINT32 Direction, UINT32 Pull, UINT32 Value)
{
  UINT32 Config = EFI_GPIO_CFG(Pin, 0, Direction, Pull, GPIO_2MA);
  EFI_STATUS Status = mTouch.Tlmm->ConfigGpio(Config, TLMM_GPIO_ENABLE);
  if (EFI_ERROR(Status) || Direction == GPIO_INPUT) return Status;
  return mTouch.Tlmm->GpioOut(Config, Value);
}

STATIC EFI_STATUS
TouchPower (VOID)
{
  EFI_STATUS Status;
  EFI_PM_VREG_STATUS_TYPE Power;
  /* The display boot chain already supplies L28A. Its inherited PMIC write
   * path can hang, so validate that supply without changing it. VIO is the
   * GPIO23-controlled fixed 1.8 V rail, not L6A. */
  ZeroMem(&Power, sizeof(Power));
  Status = mTouch.Vreg->VregGetStatus(0, EFI_PM_LDO_28, &Power);
  if (EFI_ERROR(Status)) return Status;
  if (!Power.SwEnable || !Power.VregOk) return EFI_NOT_READY;
  DEBUG((DEBUG_ERROR, "POLARIS_TOUCH GPIO power/reset\n"));
  Status = TouchGpio(TOUCH_VIO, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_HIGH_VALUE);
  if (EFI_ERROR(Status)) return Status;
  Status = TouchGpio(TOUCH_IRQ, GPIO_INPUT, GPIO_PULL_UP, 0);
  if (EFI_ERROR(Status)) return Status;
  Status = TouchGpio(TOUCH_RESET, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_LOW_VALUE);
  if (EFI_ERROR(Status)) return Status;
  gBS->Stall(200000);
  Status = TouchGpio(TOUCH_RESET, GPIO_OUTPUT, GPIO_NO_PULL, GPIO_HIGH_VALUE);
  if (EFI_ERROR(Status)) return Status;
  gBS->Stall(200000);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
TouchDescriptor (UINT16 Address, UINT16 Register, RMI_PACKET *Packet)
{
  UINT8 Size, Presence[35], Structure[512];
  UINTN Length;
  EFI_STATUS Status = TouchRead(Address, &Size, 1);
  if (EFI_ERROR(Status)) return Status;
  if (Size < 2 || Size > sizeof(Presence)) return EFI_UNSUPPORTED;
  Status = TouchRead(Address + 1, Presence, Size);
  if (EFI_ERROR(Status)) return Status;
  if (!Presence[0] && Size < 4) return EFI_UNSUPPORTED;
  Length = Presence[0] ? Presence[0] : Presence[1] | ((UINTN)Presence[2] << 8);
  if (!Length || Length > sizeof(Structure)) return EFI_UNSUPPORTED;
  Status = TouchRead(Address + 2, Structure, (UINT16)Length);
  if (EFI_ERROR(Status)) return Status;
  return RmiDecodeDescriptor(Presence, Size, Structure, Length, Register, Packet)
    ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
TouchEnableInterrupts (VOID)
{
  UINTN I;
  UINT8 Value, Verify;
  EFI_STATUS Status;
  for (I = 0; I < mTouch.IrqBytes; ++I) {
    if (!mTouch.IrqMask[I]) continue;
    Status = TouchRead(mTouch.F01Control + 1 + I, &Value, 1);
    if (EFI_ERROR(Status)) return Status;
    Status = TouchWrite(mTouch.F01Control + 1 + I, Value | mTouch.IrqMask[I]);
    if (EFI_ERROR(Status)) return Status;
    Status = TouchRead(mTouch.F01Control + 1 + I, &Verify, 1);
    if (EFI_ERROR(Status)) return Status;
    if ((Verify & mTouch.IrqMask[I]) != mTouch.IrqMask[I]) return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
TouchProbe (VOID)
{
  UINTN Page, Interrupts = 0;
  INTN Entry;
  UINT16 Base;
  UINT8 Pdt[6], Value, Bounds[4], Identity[21];
  RMI_PACKET Packet;
  BOOLEAN HaveF01 = FALSE, HaveF12 = FALSE;
  EFI_STATUS Status;
  /* Polaris uses page-zero F01/F12. Bound discovery so a missing device
   * cannot trap the boot menu in an unbounded scan. */
  for (Page = 0; Page < 4; ++Page) {
    Base = (UINT16)(Page << 8);
    for (Entry = 0xe9; Entry >= 0x05; Entry -= 6) {
      Status = TouchRead(Base | Entry, Pdt, sizeof(Pdt));
      if (EFI_ERROR(Status)) return Status;
      if (!Pdt[5]) break;
      if (Interrupts > 32 - (Pdt[4] & 7U)) return EFI_UNSUPPORTED;
      if (Pdt[5] == 0x01) {
        mTouch.F01Query = Base | Pdt[0];
        mTouch.F01Control = Base | Pdt[2];
        mTouch.F01Data = Base | Pdt[3];
        HaveF01 = TRUE;
      } else if (Pdt[5] == 0x12) {
        mTouch.F12Query = Base | Pdt[0];
        mTouch.F12Control = Base | Pdt[2];
        mTouch.F12Data = Base | Pdt[3];
        /* F12's first interrupt source is absolute object data; the next
         * source, when present, is relative motion. Preserve other enables. */
        if (Pdt[4] & 7)
          mTouch.IrqMask[Interrupts / 8] |= (UINT8)(1U << (Interrupts % 8));
        HaveF12 = TRUE;
      }
      Interrupts += Pdt[4] & 7;
    }
    if (HaveF01 && HaveF12) break;
  }
  if (!HaveF01 || !HaveF12 || Interrupts > 32) return EFI_NOT_FOUND;
  mTouch.IrqBytes = (UINT16)((Interrupts + 7) / 8);
  Status = TouchRead(mTouch.F01Query, Identity, sizeof(Identity));
  if (EFI_ERROR(Status)) return Status;
  if (Identity[0] != 1) return EFI_UNSUPPORTED;
  Identity[20] = 0;
  DEBUG((DEBUG_INFO, "POLARIS_TOUCH RMI4 product=%a core=%u F01=%x F12=%x\n",
    Identity + 11, TOUCH_I2C_CORE, mTouch.F01Control, mTouch.F12Data));
  Status = TouchRead(mTouch.F01Data, &Value, 1);
  if (EFI_ERROR(Status)) return Status;
  if (Value & 0x40) return EFI_NOT_READY; /* Do not touch firmware-update mode. */
  Status = TouchRead(mTouch.F12Query, &Value, 1);
  if (EFI_ERROR(Status)) return Status;
  if (!(Value & 1)) return EFI_UNSUPPORTED;
  Status = TouchDescriptor(mTouch.F12Query + 7, 1, &Packet);
  if (EFI_ERROR(Status)) return Status;
  if (!Packet.Subpackets || Packet.Subpackets > 10 ||
      Packet.Size != (UINT32)Packet.Subpackets * 8 ||
      Packet.ByteOffset + Packet.Size > TOUCH_PACKET_MAX) return EFI_UNSUPPORTED;
  mTouch.DataOffset = (UINT16)Packet.ByteOffset;
  mTouch.DataLength = (UINT16)Packet.Size;
  Status = TouchDescriptor(mTouch.F12Query + 4, 8, &Packet);
  if (EFI_ERROR(Status)) return Status;
  if (!Packet.Subpacket0 || Packet.Size < 4) return EFI_UNSUPPORTED;
  Status = TouchRead(mTouch.F12Control + Packet.RegisterOffset, Bounds, sizeof(Bounds));
  if (EFI_ERROR(Status)) return Status;
  mTouch.Mode.AbsoluteMaxX = Bounds[0] | ((UINT16)Bounds[1] << 8);
  mTouch.Mode.AbsoluteMaxY = Bounds[2] | ((UINT16)Bounds[3] << 8);
  if (!mTouch.Mode.AbsoluteMaxX || !mTouch.Mode.AbsoluteMaxY ||
      mTouch.Mode.AbsoluteMaxX > 16384 || mTouch.Mode.AbsoluteMaxY > 16384)
    return EFI_UNSUPPORTED;
  Status = TouchDescriptor(mTouch.F12Query + 4, 28, &Packet);
  if (!EFI_ERROR(Status)) {
    if (Packet.Size != 1) return EFI_UNSUPPORTED;
    Status = TouchWrite(mTouch.F12Control + Packet.RegisterOffset, 0xff);
    if (EFI_ERROR(Status)) return Status;
  } else if (Status != EFI_UNSUPPORTED) return Status;
  Status = TouchRead(mTouch.F01Control, &Value, 1);
  if (EFI_ERROR(Status)) return Status;
  Status = TouchWrite(mTouch.F01Control, (Value & ~3U) | 0x84); /* configured, nosleep */
  if (EFI_ERROR(Status)) return Status;
  Status = TouchEnableInterrupts();
  if (EFI_ERROR(Status)) return Status;
  mTouch.SampleRequired = TRUE;
  DEBUG((DEBUG_ERROR, "POLARIS_TOUCH ready max=%Lu,%Lu data_offset=%u bytes=%u\n",
    mTouch.Mode.AbsoluteMaxX, mTouch.Mode.AbsoluteMaxY, mTouch.DataOffset, mTouch.DataLength));
  return EFI_SUCCESS;
}

STATIC VOID
TouchQueue (EFI_ABSOLUTE_POINTER_STATE *State)
{
  UINTN Tail;
  EFI_TPL OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  if (State->ActiveButtons == mTouch.Latest.ActiveButtons &&
      (!State->ActiveButtons || (State->CurrentX == mTouch.Latest.CurrentX &&
                                State->CurrentY == mTouch.Latest.CurrentY))) {
    gBS->RestoreTPL(OldTpl);
    return;
  }
  /* Coalesce moves, keeping down/up transitions as separate reports. */
  Tail = (mTouch.Head + mTouch.Count + TOUCH_QUEUE_SIZE - 1) % TOUCH_QUEUE_SIZE;
  if (mTouch.Count && mTouch.Queue[Tail].ActiveButtons == State->ActiveButtons) {
    mTouch.Queue[Tail] = *State;
  } else {
    if (mTouch.Count == TOUCH_QUEUE_SIZE) {
      mTouch.Head = (mTouch.Head + 1) % TOUCH_QUEUE_SIZE;
      --mTouch.Count;
    }
    mTouch.Queue[(mTouch.Head + mTouch.Count++) % TOUCH_QUEUE_SIZE] = *State;
  }
  mTouch.Latest = *State;
  gBS->SignalEvent(mTouch.Pointer.WaitForInput);
  gBS->RestoreTPL(OldTpl);
}

STATIC VOID
TouchSchedule (VOID)
{
  /* GPIO transfers may take longer than the polling interval. A periodic
   * timer would keep firing overdue callbacks and starve the UI/USB loop.
   * Schedule a fresh interval only after the preceding transfer finishes. */
  if (!mTouch.Stopped &&
      EFI_ERROR(gBS->SetTimer(mTouch.Timer, TimerRelative, 200000))) {
    EFI_ABSOLUTE_POINTER_STATE State = mTouch.Latest;
    mTouch.Stopped = TRUE;
    State.ActiveButtons = 0;
    TouchQueue(&State);
    DEBUG((DEBUG_ERROR, "POLARIS_TOUCH cannot schedule poll timer\n"));
  }
}

STATIC VOID EFIAPI
TouchPoll (EFI_EVENT Event, VOID *Context)
{
  UINT8 Data[TOUCH_PACKET_MAX], Ack[5], *Finger;
  UINTN I;
  EFI_STATUS Status;
  BOOLEAN Pending = FALSE, HasMask = FALSE;
  EFI_ABSOLUTE_POINTER_STATE State = mTouch.Latest;
  if (mTouch.Stopped || !mTouch.Ready) return;
  Status = TouchRead(mTouch.F01Data, Ack, 1 + mTouch.IrqBytes);
  if (!EFI_ERROR(Status)) {
    for (I = 0; I < mTouch.IrqBytes; ++I) {
      HasMask |= mTouch.IrqMask[I] != 0;
      Pending |= (Ack[1 + I] & mTouch.IrqMask[I]) != 0;
    }
    if (HasMask && !Pending && !mTouch.SampleRequired) {
      mTouch.Errors = 0;
      TouchSchedule();
      return;
    }
    /* F01 acknowledges the interrupt before data is read. Retain the need
     * to sample if a later transfer fails, even after attention is cleared. */
    mTouch.SampleRequired = TRUE;
    Status = TouchRead(mTouch.F12Data, Data, mTouch.DataOffset + 8);
    Finger = Data + mTouch.DataOffset;
    if (!EFI_ERROR(Status) && Finger[0] != 1 && Finger[0] != 6 && mTouch.DataLength > 8)
      Status = TouchRead(mTouch.F12Data, Data, mTouch.DataOffset + mTouch.DataLength);
  }
  if (EFI_ERROR(Status)) {
    if (++mTouch.Errors == 5) {
      State.ActiveButtons = 0;
      TouchQueue(&State);
      mTouch.Stopped = TRUE;
      gBS->SetTimer(mTouch.Timer, TimerCancel, 0);
      DEBUG((DEBUG_ERROR, "POLARIS_TOUCH stopped after repeated I2C errors\n"));
    }
    TouchSchedule();
    return;
  }
  mTouch.Errors = 0;
  mTouch.SampleRequired = FALSE;
  State.ActiveButtons = 0;
  /* A valid first contact completes the short report. Otherwise all slots
   * were read so a second contact or the release is still handled. */
  for (I = 0; I < mTouch.DataLength; I += 8) {
    Finger = Data + mTouch.DataOffset + I;
    if (Finger[0] != 1 && Finger[0] != 6) continue; /* finger / gloved finger */
    State.CurrentX = Finger[1] | ((UINT16)Finger[2] << 8);
    State.CurrentY = Finger[3] | ((UINT16)Finger[4] << 8);
    if (State.CurrentX > mTouch.Mode.AbsoluteMaxX) State.CurrentX = mTouch.Mode.AbsoluteMaxX;
    if (State.CurrentY > mTouch.Mode.AbsoluteMaxY) State.CurrentY = mTouch.Mode.AbsoluteMaxY;
    State.ActiveButtons = EFI_ABSP_TouchActive;
    break;
  }
  TouchQueue(&State); /* includes the zero-button release needed for a click */
  TouchSchedule();
}

STATIC EFI_STATUS EFIAPI
TouchReset (EFI_ABSOLUTE_POINTER_PROTOCOL *This, BOOLEAN ExtendedVerification)
{
  EFI_TPL OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  mTouch.Head = mTouch.Count = 0;
  ZeroMem(&mTouch.Latest, sizeof(mTouch.Latest));
  mTouch.SampleRequired = TRUE;
  gBS->RestoreTPL(OldTpl);
  return mTouch.Stopped ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS
TouchInitialize (VOID)
{
  EFI_STATUS Status;
  I2C_STATUS BusStatus;
  CHAR8 *Stage = "power/reset";
  /* Defer the power/reset delay and bounded bus probe until the UI owns
   * input. Missing touch hardware must not block early boot dispatch. */
  if (mTouch.Stopped) return EFI_DEVICE_ERROR;
  if (mTouch.Ready) return EFI_SUCCESS;
  if (PolarisBootConsole()->Owner != POLARIS_CONSOLE_UI) return EFI_NOT_READY;
  Status = TouchPower();
  if (EFI_ERROR(Status)) goto Fail;
  Stage = "I2C open";
  DEBUG((DEBUG_ERROR, "POLARIS_TOUCH GPIO I2C open core=%u\n", TOUCH_I2C_CORE));
  BusStatus = mTouch.I2c->Open(TOUCH_I2C_CORE, &mTouch.Bus);
  Status = I2C_ERROR(BusStatus) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
  if (EFI_ERROR(Status) || !mTouch.Bus) { Status = EFI_DEVICE_ERROR; goto Fail; }
  Stage = "RMI4 probe";
  DEBUG((DEBUG_ERROR, "POLARIS_TOUCH RMI4 probe\n"));
  Status = TouchProbe();
  if (EFI_ERROR(Status)) goto Fail;
  Stage = "poll timer";
  Status = gBS->SetTimer(mTouch.Timer, TimerRelative, 200000);
  if (EFI_ERROR(Status)) goto Fail;
  mTouch.Ready = TRUE;
  return EFI_SUCCESS;
Fail:
  mTouch.Stopped = TRUE;
  if (mTouch.Bus) { mTouch.I2c->Close(mTouch.Bus); mTouch.Bus = NULL; }
  DEBUG((DEBUG_ERROR, "POLARIS_TOUCH failed stage=%a status=%r\n", Stage, Status));
  return Status;
}

STATIC EFI_STATUS EFIAPI
TouchGetState (EFI_ABSOLUTE_POINTER_PROTOCOL *This, EFI_ABSOLUTE_POINTER_STATE *State)
{
  EFI_TPL OldTpl;
  EFI_STATUS Status = EFI_NOT_READY;
  if (!State) return EFI_INVALID_PARAMETER;
  Status = TouchInitialize();
  if (EFI_ERROR(Status) && !mTouch.Count) return Status;
  Status = EFI_NOT_READY;
  OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  if (mTouch.Count) {
    *State = mTouch.Queue[mTouch.Head];
    mTouch.Head = (mTouch.Head + 1) % TOUCH_QUEUE_SIZE;
    --mTouch.Count;
    Status = EFI_SUCCESS;
  } else if (mTouch.Stopped) Status = EFI_DEVICE_ERROR;
  gBS->RestoreTPL(OldTpl);
  return Status;
}

STATIC VOID EFIAPI
TouchWait (EFI_EVENT Event, VOID *Context)
{
  if (mTouch.Count) gBS->SignalEvent(Event);
}

STATIC VOID EFIAPI
TouchExit (EFI_EVENT Event, VOID *Context)
{
  mTouch.Stopped = TRUE;
  if (mTouch.Timer) gBS->SetTimer(mTouch.Timer, TimerCancel, 0);
  if (mTouch.Bus) {
    mTouch.I2c->Close(mTouch.Bus);
    mTouch.Bus = NULL;
  }
  /* Leave power/reset pins available for the operating system to take over. */
}

EFI_STATUS EFIAPI
PolarisTouchEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  CHAR8 *Stage = "protocols";
  ZeroMem(&mTouch, sizeof(mTouch));
  mTouch.Page = 0xff;
  Status = gBS->LocateProtocol(&gQcomTlmmProtocolGuid, NULL, (VOID **)&mTouch.Tlmm);
  if (EFI_ERROR(Status)) goto Fail;
  mGpioI2c.Tlmm = mTouch.Tlmm;
  mTouch.I2c = &mGpioI2cProtocol;
  Status = gBS->LocateProtocol(&gQcomPmicVregProtocolGuid, NULL, (VOID **)&mTouch.Vreg);
  if (EFI_ERROR(Status)) goto Fail;
  mTouch.Slave.BusFrequency = 400;
  mTouch.Slave.SlaveAddress = 0x20;
  mTouch.Slave.Mode = I2C;
  mTouch.Slave.SlaveMaxClockStretch = 500;
  mTouch.Mode.AbsoluteMaxX = 1080;
  mTouch.Mode.AbsoluteMaxY = 2160;
  mTouch.Pointer.Reset = TouchReset;
  mTouch.Pointer.GetState = TouchGetState;
  mTouch.Pointer.Mode = &mTouch.Mode;
  Stage = "events";
  Status = gBS->CreateEvent(EVT_NOTIFY_WAIT, TPL_NOTIFY, TouchWait, NULL, &mTouch.Pointer.WaitForInput);
  if (EFI_ERROR(Status)) goto Fail;
  Status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, TouchPoll, NULL, &mTouch.Timer);
  if (EFI_ERROR(Status)) goto Fail;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, TouchExit, NULL,
    &gEfiEventExitBootServicesGuid, &mTouch.ExitEvent);
  if (EFI_ERROR(Status)) goto Fail;
  Stage = "absolute pointer";
  Status = gBS->InstallMultipleProtocolInterfaces(&mTouch.Handle,
    &gEfiAbsolutePointerProtocolGuid, &mTouch.Pointer,
    &gEfiDevicePathProtocolGuid, &mDevicePath, NULL);
  if (!EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "POLARIS_TOUCH absolute pointer installed; hardware setup deferred\n"));
    return EFI_SUCCESS;
  }
Fail:
  DEBUG((DEBUG_ERROR, "POLARIS_TOUCH failed stage=%a status=%r\n", Stage, Status));
  TouchExit(NULL, NULL);
  if (mTouch.Timer) gBS->CloseEvent(mTouch.Timer);
  if (mTouch.ExitEvent) gBS->CloseEvent(mTouch.ExitEvent);
  if (mTouch.Pointer.WaitForInput) gBS->CloseEvent(mTouch.Pointer.WaitForInput);
  return Status;
}
