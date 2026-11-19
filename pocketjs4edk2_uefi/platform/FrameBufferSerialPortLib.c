// SPDX-License-Identifier: BSD-2-Clause-Patent
// Polaris early log console. Every linked module uses the same RAM cursor.
#include <PiDxe.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/SerialPortLib.h>
#include <Resources/font5x12.h>
#include "BootConsole.h"

#ifndef POLARIS_CONSOLE_PIXELS
#define POLARIS_CONSOLE_PIXELS ((UINT32 *)(UINTN)POLARIS_CONSOLE_FB)
#endif
#define CELL_WIDTH ((FONT_WIDTH+1)*SCALE_FACTOR)
#define CELL_HEIGHT (FONT_HEIGHT*SCALE_FACTOR)
#define COLUMNS (POLARIS_CONSOLE_WIDTH/CELL_WIDTH)
#define ROWS (POLARIS_CONSOLE_HEIGHT/CELL_HEIGHT)

STATIC VOID Flush(VOID) {
  WriteBackInvalidateDataCacheRange(POLARIS_CONSOLE_PIXELS,POLARIS_CONSOLE_BYTES);
}

VOID ResetFb(VOID) {
  POLARIS_BOOT_CONSOLE *State=PolarisBootConsole();
  ZeroMem(POLARIS_CONSOLE_PIXELS,POLARIS_CONSOLE_BYTES);
  State->Column=State->Row=0;
  Flush();
}

RETURN_STATUS EFIAPI SerialPortInitialize(VOID) {
  POLARIS_BOOT_CONSOLE *State=PolarisBootConsole();
  BOOLEAN Interrupts=ArmGetInterruptState();
  ArmDisableInterrupts();
  if (State->Magic!=POLARIS_CONSOLE_MAGIC) {
    ZeroMem(State,sizeof(*State));
    State->Magic=POLARIS_CONSOLE_MAGIC;
    State->Owner=POLARIS_CONSOLE_BOOT;
    ResetFb();
  }
  if (Interrupts) ArmEnableInterrupts();
  return RETURN_SUCCESS;
}

STATIC VOID NewLine(VOID) {
  POLARIS_BOOT_CONSOLE *State=PolarisBootConsole();
  UINTN LineBytes=POLARIS_CONSOLE_WIDTH*CELL_HEIGHT*sizeof(UINT32);
  UINT8 *Pixels=(VOID *)POLARIS_CONSOLE_PIXELS;
  State->Column=0;
  if (++State->Row>=ROWS) {
    // CopyMem supports overlap; use the actual 32-bit stride, not UINT16.
    CopyMem(Pixels,Pixels+LineBytes,LineBytes*(ROWS-1));
    State->Row=ROWS-1;
    ++State->Scrolls;
  }
  ZeroMem(Pixels+State->Row*LineBytes,LineBytes);
}

STATIC VOID Paint(UINT8 Ch) {
  POLARIS_BOOT_CONSOLE *State=PolarisBootConsole();
  UINTN X,Y,I,J,Offset;
  UINT32 Bits,Color=State->Owner==POLARIS_CONSOLE_CRITICAL?0x00ffff00U:0x00ffffffU;
  if (State->Column>=COLUMNS || State->Row>=ROWS) ResetFb();
  if (Ch=='\r') { State->Column=0;return; }
  if (Ch=='\n') { NewLine();return; }
  if (Ch=='\t') {
    UINTN Spaces=4-State->Column%4;
    while (Spaces--) Paint(' ');
    return;
  }
  if (Ch=='\b') { if (State->Column) --State->Column;return; }
  if (Ch<32 || Ch==127) return;
  if (Ch>=0x80 && Ch<0xc0) return; // UTF-8 continuation; one placeholder per glyph.
  if (Ch>126) Ch='?';
  Offset=State->Row*CELL_HEIGHT*POLARIS_CONSOLE_WIDTH+State->Column*CELL_WIDTH;
  for (Y=0;Y<FONT_HEIGHT;++Y) {
    Bits=font5x12[(Ch-32)*2+Y/(FONT_HEIGHT/2)] >> ((Y%(FONT_HEIGHT/2))*FONT_WIDTH);
    for (I=0;I<SCALE_FACTOR;++I)
      for (X=0;X<CELL_WIDTH;++X) {
        J=X/SCALE_FACTOR;
        POLARIS_CONSOLE_PIXELS[Offset+(Y*SCALE_FACTOR+I)*POLARIS_CONSOLE_WIDTH+X]=
          J<FONT_WIDTH && (Bits&(1U<<J))?Color:0;
      }
  }
  if (++State->Column==COLUMNS) NewLine();
}

STATIC BOOLEAN IsText(CONST UINT8 *Buffer,UINTN Size) {
  UINTN I=0,Extra,J;UINT32 Ch,Minimum;UINT8 First;
  while (I<Size) {
    First=Buffer[I++];
    if (First<0x80) {
      if (First>=32 && First<127) continue;
      if (First=='\n' || First=='\r' || First=='\t' || First=='\b') continue;
      return FALSE;
    }
    if (First>=0xc2 && First<=0xdf) { Extra=1;Ch=First&31;Minimum=0x80; }
    else if (First>=0xe0 && First<=0xef) { Extra=2;Ch=First&15;Minimum=0x800; }
    else if (First>=0xf0 && First<=0xf4) { Extra=3;Ch=First&7;Minimum=0x10000; }
    else return FALSE;
    if (Size-I<Extra) return FALSE;
    for (J=0;J<Extra;++J) {
      if ((Buffer[I]&0xc0)!=0x80) return FALSE;
      Ch=(Ch<<6)|(Buffer[I++]&63);
    }
    if (Ch<Minimum || Ch>0x10ffff || (Ch>=0xd800 && Ch<=0xdfff)) return FALSE;
  }
  return TRUE;
}

STATIC UINTN Write(UINT8 *Buffer,UINTN Size,BOOLEAN Critical) {
  BOOLEAN Interrupts,Text;
  UINTN I;
  POLARIS_BOOT_CONSOLE *State=PolarisBootConsole();
  if (!Buffer) return 0;
  // SerialPortLib also carries non-text traffic from firmware clients. Keep
  // its bytes in the RAM log, but never interpret binary LF/CR as screen moves.
  Text=IsText(Buffer,Size);
  SerialPortInitialize();
  Interrupts=ArmGetInterruptState();
  ArmDisableInterrupts();
  if (Critical && State->Owner!=POLARIS_CONSOLE_CRITICAL) {
    State->Owner=POLARIS_CONSOLE_CRITICAL;
    ResetFb();
  }
  for (I=0;I<Size;++I) {
    PolarisBootConsoleByte(Buffer[I]);
    if (Text && PolarisBootConsoleEarly()) Paint(Buffer[I]);
  }
  if (Text && PolarisBootConsoleEarly()) Flush();
  if (Interrupts) ArmEnableInterrupts();
  return Size;
}

UINTN EFIAPI SerialPortWrite(UINT8 *Buffer,UINTN Size) { return Write(Buffer,Size,FALSE); }
UINTN EFIAPI SerialPortWriteCritical(UINT8 *Buffer,UINTN Size) { return Write(Buffer,Size,TRUE); }
UINTN EFIAPI SerialPortRead(UINT8 *Buffer,UINTN Size) { return 0; }
BOOLEAN EFIAPI SerialPortPoll(VOID) { return FALSE; }
RETURN_STATUS EFIAPI SerialPortSetControl(UINT32 Control) { return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortGetControl(UINT32 *Control) { return RETURN_UNSUPPORTED; }
RETURN_STATUS EFIAPI SerialPortSetAttributes(UINT64 *BaudRate,UINT32 *ReceiveFifoDepth,
  UINT32 *Timeout,EFI_PARITY_TYPE *Parity,UINT8 *DataBits,EFI_STOP_BITS_TYPE *StopBits) {
  return RETURN_UNSUPPORTED;
}
UINTN SerialPortFlush(VOID) { Flush();return 0; }
VOID EnableSynchronousSerialPortIO(VOID) {}
