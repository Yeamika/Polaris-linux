// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
static UINT64 ConsoleStorage[2052];
#define POLARIS_CONSOLE_STATE_ADDRESS ((UINTN)ConsoleStorage)
#include "../platform/BootConsole.h"
// Run the actual framebuffer serial implementation against guarded pixels.
static struct { UINT32 Before[16],Pixels[1080*2160],After[16]; } Display;
#define Shared (*PolarisBootConsole())
#define POLARIS_CONSOLE_PIXELS Display.Pixels
#include "../platform/FrameBufferSerialPortLib.c"
#include "../platform/BootConsoleText.h"
static BOOLEAN Interrupts=TRUE;
BOOLEAN EFIAPI ArmGetInterruptState(VOID) { return Interrupts; }
UINTN EFIAPI ArmDisableInterrupts(VOID) { UINTN Previous=Interrupts;Interrupts=FALSE;return Previous; }
VOID EFIAPI ArmEnableInterrupts(VOID) { Interrupts=TRUE; }
VOID *EFIAPI ZeroMem(VOID *Data,UINTN Size) { return memset(Data,0,Size); }
VOID *EFIAPI CopyMem(VOID *Out,CONST VOID *In,UINTN Size) { return memmove(Out,In,Size); }
VOID *EFIAPI WriteBackInvalidateDataCacheRange(VOID *Data,UINTN Size) {
 assert(Data==Display.Pixels && Size==sizeof(Display.Pixels));return Data;
}
static void Send(const char *Text) { assert(SerialPortWrite((UINT8 *)Text,strlen(Text))==strlen(Text)); }
static void Guard(void) {
 for(unsigned I=0;I<16;++I)assert(Display.Before[I]==0x12345678 && Display.After[I]==0x87654321);
 assert(Shared.Column<COLUMNS && Shared.Row<ROWS);
}
int main(void) {
 for(unsigned I=0;I<16;++I){Display.Before[I]=0x12345678;Display.After[I]=0x87654321;}
 SerialPortInitialize();assert(Interrupts && PolarisBootConsoleEarly());
 Send("PrePi ");SerialPortInitialize();assert(Shared.Column==6);
 PolarisBootConsoleOutput(L"DXE\r\nBDS");assert(Shared.Row==1 && Shared.Column==3);
 // Identical glyphs occupy one grid regardless of output API.
 ResetFb();Send("A");PolarisBootConsoleOutput(L"A");
 for(unsigned Y=0;Y<CELL_HEIGHT;++Y)
  assert(!memcmp(Display.Pixels+Y*1080,Display.Pixels+Y*1080+CELL_WIDTH,CELL_WIDTH*4));
 UINT8 Binary[]={0xff,'\n',1,'X',0x80};
 UINT32 *Saved=malloc(sizeof(Display.Pixels));assert(Saved);memcpy(Saved,Display.Pixels,sizeof(Display.Pixels));
 UINT32 SavedColumn=Shared.Column,SavedRow=Shared.Row;
 SerialPortWrite(Binary,sizeof(Binary));
 assert(Shared.Column==SavedColumn && Shared.Row==SavedRow && !memcmp(Saved,Display.Pixels,sizeof(Display.Pixels)));
 free(Saved);
 UINT8 Raw[5];assert(PolarisBootConsoleRead(Raw,sizeof(Raw))==sizeof(Raw) && !memcmp(Raw,Binary,sizeof(Raw)));
 PolarisBootConsoleOutput(L"\x4e2d\xd83d\xde00");assert(Shared.Column==SavedColumn+2);
 // Newline is consumed once, including at the bottom. Long lines and tabs
 // must never address pixels beyond the real 32-bit scanout buffer.
 ResetFb();for(unsigned I=0;I<ROWS-1;++I)Send("\n");
 Send("A");UINT32 Line[1080*CELL_HEIGHT];memcpy(Line,Display.Pixels+(ROWS-1)*1080*CELL_HEIGHT,sizeof(Line));
 Send("\n");assert(Shared.Row==ROWS-1 && Shared.Column==0 && Shared.Scrolls==1);
 assert(!memcmp(Line,Display.Pixels+(ROWS-2)*1080*CELL_HEIGHT,sizeof(Line)));
 for(unsigned I=(ROWS-1)*1080*CELL_HEIGHT;I<1080*2160;++I)assert(Display.Pixels[I]==0);
 for(unsigned I=0;I<COLUMNS*2+9;++I)Send("X");
 Send("\t\r\n\n");Guard();
 UINT8 Snapshot[POLARIS_CONSOLE_LOG_SIZE];
 UINT32 *Before=malloc(sizeof(Display.Pixels));assert(Before);memcpy(Before,Display.Pixels,sizeof(Display.Pixels));
 PolarisBootConsoleOwner(POLARIS_CONSOLE_UI);Send("hidden debug");
 assert(!memcmp(Display.Pixels,Before,sizeof(Display.Pixels)) && !PolarisBootConsoleEarly());
 PolarisBootConsoleOwner(POLARIS_CONSOLE_TEXT);Send("child debug");
 assert(!memcmp(Display.Pixels,Before,sizeof(Display.Pixels)) && !PolarisBootConsoleEarly());free(Before);
 SerialPortWriteCritical((UINT8 *)"fault",5);assert(PolarisBootConsoleEarly() && Shared.Owner==POLARIS_CONSOLE_CRITICAL);
 assert(Shared.Column==5 && Shared.Row==0);Guard();
 // Ring rollover preserves byte order without touching its neighbors.
 PolarisBootConsoleOwner(POLARIS_CONSOLE_UI);
 for(unsigned I=0;I<POLARIS_CONSOLE_LOG_SIZE+100;++I) { UINT8 Ch=(UINT8)('a'+I%26);SerialPortWrite(&Ch,1); }
 assert(PolarisBootConsoleRead(Snapshot,sizeof(Snapshot))==sizeof(Snapshot));
 for(unsigned I=0;I<sizeof(Snapshot);++I)assert(Snapshot[I]=='a'+(I+100)%26);
 Interrupts=FALSE;Send("masked");assert(!Interrupts);Guard();
 puts("PASS: shared log grid, UTF-16/UTF-8, binary filtering, 32-bit scrolling, guards, UI/child ownership, critical takeover and ring wrap");
 return 0;
}
