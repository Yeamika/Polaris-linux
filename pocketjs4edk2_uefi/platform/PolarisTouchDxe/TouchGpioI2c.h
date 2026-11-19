/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef POLARIS_TOUCH_GPIO_I2C_H
#define POLARIS_TOUCH_GPIO_I2C_H

#include <Library/TimerLib.h>

/* Polaris leaves QUP SE14 in GPI-only mode. The inherited FIFO-only EFI I2C
 * driver cannot service that engine. These two dedicated touchscreen pins
 * provide a bounded open-drain bus until a GPI DMA transport is available.
 * A high level always releases the pin; neither line is ever driven high. */
#define GPIO_I2C_SDA 33
#define GPIO_I2C_SCL 34
#define GPIO_I2C_HALF_US 2

STATIC struct {
  EFI_QCOM_TLMM_PROTOCOL *Tlmm;
  BOOLEAN Open, SdaLow, SclLow;
  UINT64 Start;
  UINT32 Timeout;
} mGpioI2c;

STATIC BOOLEAN
GpioI2cExpired (VOID)
{
  return GetTimeInNanoSecond(GetPerformanceCounter() - mGpioI2c.Start) >=
    (UINT64)mGpioI2c.Timeout * 1000000;
}

STATIC EFI_STATUS
GpioI2cLine (UINT32 Pin, BOOLEAN Low)
{
  BOOLEAN *Previous = Pin == GPIO_I2C_SDA ? &mGpioI2c.SdaLow : &mGpioI2c.SclLow;
  UINT32 Config = EFI_GPIO_CFG(Pin, 0, Low ? GPIO_OUTPUT : GPIO_INPUT,
                               GPIO_NO_PULL, GPIO_2MA);
  EFI_STATUS Status;
  if (*Previous == Low) return EFI_SUCCESS;
  if (Low) {
    /* Set the output latch while released, then enable the low driver. */
    Status = mGpioI2c.Tlmm->GpioOut(Config, GPIO_LOW_VALUE);
    if (EFI_ERROR(Status)) return Status;
  }
  Status = mGpioI2c.Tlmm->ConfigGpio(Config, TLMM_GPIO_ENABLE);
  if (!EFI_ERROR(Status)) *Previous = Low;
  return Status;
}

STATIC EFI_STATUS
GpioI2cSample (UINT32 Pin, UINT32 *Value)
{
  return mGpioI2c.Tlmm->GpioIn(
    EFI_GPIO_CFG(Pin, 0, GPIO_INPUT, GPIO_NO_PULL, GPIO_2MA), Value);
}

STATIC EFI_STATUS
GpioI2cHighClock (VOID)
{
  UINT32 High;
  EFI_STATUS Status = GpioI2cLine(GPIO_I2C_SCL, FALSE);
  if (EFI_ERROR(Status)) return Status;
  do {
    if (GpioI2cExpired()) return EFI_TIMEOUT;
    Status = GpioI2cSample(GPIO_I2C_SCL, &High);
    if (EFI_ERROR(Status)) return Status;
    if (!High) gBS->Stall(GPIO_I2C_HALF_US);
  } while (!High);
  gBS->Stall(GPIO_I2C_HALF_US);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GpioI2cStart (VOID)
{
  EFI_STATUS Status;
  UINT32 High;
  Status = GpioI2cLine(GPIO_I2C_SDA, FALSE);
  if (EFI_ERROR(Status)) return Status;
  gBS->Stall(GPIO_I2C_HALF_US);
  Status = GpioI2cHighClock();
  if (EFI_ERROR(Status)) return Status;
  Status = GpioI2cSample(GPIO_I2C_SDA, &High);
  if (EFI_ERROR(Status)) return Status;
  if (!High) return EFI_NOT_READY;
  Status = GpioI2cLine(GPIO_I2C_SDA, TRUE);
  if (EFI_ERROR(Status)) return Status;
  gBS->Stall(GPIO_I2C_HALF_US);
  return GpioI2cLine(GPIO_I2C_SCL, TRUE);
}

STATIC EFI_STATUS
GpioI2cStop (VOID)
{
  EFI_STATUS Released;
  EFI_STATUS Status = GpioI2cLine(GPIO_I2C_SCL, TRUE);
  if (EFI_ERROR(Status)) return Status;
  Status = GpioI2cLine(GPIO_I2C_SDA, TRUE);
  if (EFI_ERROR(Status)) return Status;
  gBS->Stall(GPIO_I2C_HALF_US);
  Status = GpioI2cHighClock();
  /* Always release SDA, including when the target holds SCL low. */
  Released = GpioI2cLine(GPIO_I2C_SDA, FALSE);
  gBS->Stall(GPIO_I2C_HALF_US);
  return EFI_ERROR(Status) ? Status : Released;
}

STATIC EFI_STATUS
GpioI2cSend (UINT8 Value)
{
  EFI_STATUS Status;
  UINT32 Bit, Ack;
  for (Bit = 0; Bit < 8; ++Bit) {
    Status = GpioI2cLine(GPIO_I2C_SDA, !(Value & 0x80));
    if (EFI_ERROR(Status)) return Status;
    gBS->Stall(GPIO_I2C_HALF_US);
    Status = GpioI2cHighClock();
    if (EFI_ERROR(Status)) return Status;
    Status = GpioI2cLine(GPIO_I2C_SCL, TRUE);
    if (EFI_ERROR(Status)) return Status;
    Value <<= 1;
  }
  Status = GpioI2cLine(GPIO_I2C_SDA, FALSE);
  if (EFI_ERROR(Status)) return Status;
  gBS->Stall(GPIO_I2C_HALF_US);
  Status = GpioI2cHighClock();
  if (EFI_ERROR(Status)) return Status;
  Status = GpioI2cSample(GPIO_I2C_SDA, &Ack);
  if (EFI_ERROR(Status)) return Status;
  Status = GpioI2cLine(GPIO_I2C_SCL, TRUE);
  return EFI_ERROR(Status) ? Status : Ack ? EFI_NO_RESPONSE : EFI_SUCCESS;
}

STATIC EFI_STATUS
GpioI2cReceive (UINT8 *Value, BOOLEAN Last)
{
  EFI_STATUS Status = GpioI2cLine(GPIO_I2C_SDA, FALSE);
  UINT32 Bit, High;
  if (EFI_ERROR(Status)) return Status;
  *Value = 0;
  for (Bit = 0; Bit < 8; ++Bit) {
    gBS->Stall(GPIO_I2C_HALF_US);
    Status = GpioI2cHighClock();
    if (EFI_ERROR(Status)) return Status;
    Status = GpioI2cSample(GPIO_I2C_SDA, &High);
    if (EFI_ERROR(Status)) return Status;
    *Value = (UINT8)((*Value << 1) | (High & 1));
    Status = GpioI2cLine(GPIO_I2C_SCL, TRUE);
    if (EFI_ERROR(Status)) return Status;
  }
  Status = GpioI2cLine(GPIO_I2C_SDA, !Last);
  if (EFI_ERROR(Status)) return Status;
  gBS->Stall(GPIO_I2C_HALF_US);
  Status = GpioI2cHighClock();
  if (EFI_ERROR(Status)) return Status;
  Status = GpioI2cLine(GPIO_I2C_SCL, TRUE);
  if (EFI_ERROR(Status)) return Status;
  return GpioI2cLine(GPIO_I2C_SDA, FALSE);
}

STATIC I2C_STATUS EFIAPI
GpioI2cRestore (VOID)
{
  EFI_STATUS Status;
  UINT32 Pins[] = {GPIO_I2C_SDA, GPIO_I2C_SCL};
  UINTN I;
  I2C_STATUS Result = I2C_SUCCESS;
  for (I = 0; I < 2; ++I) {
    /* Restore SE14 function 2 even after a partial GPIO setup failure. */
    Status = mGpioI2c.Tlmm->ConfigGpio(
      EFI_GPIO_CFG(Pins[I], 2, GPIO_INPUT, GPIO_NO_PULL, GPIO_2MA), TLMM_GPIO_ENABLE);
    if (EFI_ERROR(Status)) Result = I2C_ERROR_PLATFORM_GPIO_DISABLE_FAIL;
  }
  return Result;
}

STATIC I2C_STATUS EFIAPI
GpioI2cOpen (UINT32 Core, VOID **Bus)
{
  EFI_STATUS Status;
  UINT32 Pins[] = {GPIO_I2C_SDA, GPIO_I2C_SCL};
  UINTN I;
  if (Core != 15 || !Bus || !mGpioI2c.Tlmm || mGpioI2c.Open)
    return I2C_ERROR_INVALID_PARAMETER;
  *Bus = NULL;
  for (I = 0; I < 2; ++I) {
    Status = mGpioI2c.Tlmm->ConfigGpio(
      EFI_GPIO_CFG(Pins[I], 0, GPIO_INPUT, GPIO_NO_PULL, GPIO_2MA), TLMM_GPIO_ENABLE);
    if (EFI_ERROR(Status)) {
      GpioI2cRestore();
      return I2C_ERROR_PLATFORM_GPIO_ENABLE_FAIL;
    }
  }
  mGpioI2c.SdaLow = mGpioI2c.SclLow = FALSE;
  mGpioI2c.Open = TRUE;
  *Bus = &mGpioI2c;
  return I2C_SUCCESS;
}

STATIC I2C_STATUS EFIAPI
GpioI2cClose (VOID *Bus)
{
  I2C_STATUS Result;
  if (Bus != &mGpioI2c || !mGpioI2c.Open) return I2C_ERROR_INVALID_PARAMETER;
  Result = GpioI2cRestore();
  mGpioI2c.Open = FALSE;
  return Result;
}

STATIC I2C_STATUS
GpioI2cTransfer (VOID *Bus, I2C_SLAVE_CONFIG *Config, UINT16 Offset,
                UINT16 OffsetLength, UINT8 *Data, UINT16 Length,
                UINT32 *Done, UINT32 Timeout, BOOLEAN Read)
{
  EFI_STATUS Status, StopStatus;
  UINTN I;
  if (Done) *Done = 0;
  if (Bus != &mGpioI2c || !mGpioI2c.Open || !Config || !Data || !Length ||
      !Done || OffsetLength != 1 || Offset > 255 || Config->SlaveAddress != 0x20 ||
      Config->Mode != I2C || !Timeout || Timeout > 100) return I2C_ERROR_INVALID_PARAMETER;
  mGpioI2c.Start = GetPerformanceCounter();
  mGpioI2c.Timeout = Timeout;
  Status = GpioI2cStart();
  if (!EFI_ERROR(Status)) Status = GpioI2cSend((UINT8)(Config->SlaveAddress << 1));
  if (!EFI_ERROR(Status)) Status = GpioI2cSend((UINT8)Offset);
  if (!EFI_ERROR(Status) && Read) {
    Status = GpioI2cStart();
    if (!EFI_ERROR(Status)) Status = GpioI2cSend((UINT8)((Config->SlaveAddress << 1) | 1));
  }
  for (I = 0; I < Length && !EFI_ERROR(Status); ++I) {
    Status = Read ? GpioI2cReceive(&Data[I], I + 1 == Length) : GpioI2cSend(Data[I]);
    if (!EFI_ERROR(Status)) ++*Done;
  }
  StopStatus = GpioI2cStop();
  /* GPIO errors must not leave either open-drain output asserted. */
  if (EFI_ERROR(Status) || EFI_ERROR(StopStatus)) {
    GpioI2cLine(GPIO_I2C_SCL, FALSE);
    GpioI2cLine(GPIO_I2C_SDA, FALSE);
  }
  if (!EFI_ERROR(Status)) Status = StopStatus;
  if (Status == EFI_TIMEOUT) return I2C_ERROR_TRANSFER_TIMEOUT;
  if (Status == EFI_NO_RESPONSE) return I2C_ERROR_ADDR_NACK;
  return EFI_ERROR(Status) ? I2C_ERROR_COMMAND_FAIL : I2C_SUCCESS;
}

STATIC I2C_STATUS EFIAPI
GpioI2cRead (VOID *Bus, I2C_SLAVE_CONFIG *Config, UINT16 Offset,
            UINT16 OffsetLength, UINT8 *Data, UINT16 Length, UINT32 *Done, UINT32 Timeout)
{
  return GpioI2cTransfer(Bus, Config, Offset, OffsetLength, Data, Length, Done, Timeout, TRUE);
}

STATIC I2C_STATUS EFIAPI
GpioI2cWrite (VOID *Bus, I2C_SLAVE_CONFIG *Config, UINT16 Offset,
             UINT16 OffsetLength, UINT8 *Data, UINT16 Length, UINT32 *Done, UINT32 Timeout)
{
  return GpioI2cTransfer(Bus, Config, Offset, OffsetLength, Data, Length, Done, Timeout, FALSE);
}

STATIC EFI_QCOM_I2C_PROTOCOL mGpioI2cProtocol = {
  I2C_DXE_REVISION, GpioI2cOpen, GpioI2cRead, GpioI2cWrite, NULL, GpioI2cClose
};

#endif
