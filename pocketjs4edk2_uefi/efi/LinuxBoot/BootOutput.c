// SPDX-License-Identifier: BSD-2-Clause-Patent
// One bounded, rotated boot-output renderer for EFI preparation and EFI stub.
#include "BootOutput.h"
#include "BootOutputLayout.h"
#include "../../platform/BootConsole.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Resources/font5x12.h>
#define PJB_EDK2
#include "pocketjs_boot_context.h"
#include "pj4edk2_linux_display.h"
STATIC struct polaris_output_layout Layout;
STATIC UINT8 Cells[90][180];
STATIC UINT8 *Pixels;
STATIC UINTN Column,Row,First,Last;
STATIC UINT32 PreviousOwner;
STATIC BOOLEAN Owned;

STATIC VOID Pixel(UINTN X,UINTN Y,UINT32 Color) {
  UINTN Offset=(UINTN)polaris_output_offset(&Layout,X,Y);
  *(volatile UINT32 *)(Pixels+Offset)=Color;
  if (Offset<First) First=Offset;
  if (Offset+4>Last) Last=Offset+4;
}
STATIC VOID Flush(VOID) {
  if (Last>First) WriteBackDataCacheRange(Pixels+First,Last-First);
  First=MAX_UINTN;Last=0;
}
STATIC VOID Cell(UINTN R,UINTN C) {
  UINTN X,Y;
  UINT8 Ch=Cells[R][C];
  for (Y=0;Y<24;++Y) {
    UINTN Fy=Y/2;
    UINT32 Bits=font5x12[(Ch-32)*2+Fy/6]>>((Fy%6)*5);
    for (X=0;X<12;++X)
      Pixel(Layout.left+C*12+X,Layout.top+R*24+Y,
            X<10 && (Bits&(1U<<(X/2)))?0x00dedede:0);
  }
}
STATIC VOID NewLine(VOID) {
  UINTN R,C;
  Column=0;
  if (++Row<Layout.rows) return;
  Row=Layout.rows-1;
  for (R=1;R<Layout.rows;++R) CopyMem(Cells[R-1],Cells[R],Layout.cols);
  SetMem(Cells[Row],Layout.cols,' ');
  for (R=0;R<Layout.rows;++R) for (C=0;C<Layout.cols;++C) Cell(R,C);
}
STATIC VOID Character(UINT16 Ch) {
  if (Ch=='\r') {Column=0;return;}
  if (Ch=='\n') {NewLine();return;}
  if (Ch=='\b') {if(Column)--Column;return;}
  if (Ch=='\t') {UINTN N=4-Column%4;while(N--)Character(' ');return;}
  if (Ch<32) return;
  if (Column==Layout.cols) NewLine();
  Cells[Row][Column]=Ch<127?Ch:'?';Cell(Row,Column++);
}
EFI_STATUS PolarisLinuxOutputBegin(EFI_SYSTEM_TABLE *System) {
  PJB_GUID Wire=PJB_BOOT_DISPLAY_GUID;
  EFI_GUID Guid;
  PJB_BOOT_DISPLAY *Request=NULL,Fallback;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  struct pj4edk2_surface Surface;
  struct pj4edk2_region Region;
  UINT32 V[20];UINTN I,R,C;
  EFI_STATUS Status;
  if (Owned) return EFI_ALREADY_STARTED;
  CopyMem(&Guid,&Wire,sizeof(Guid));
  for(I=0;I<System->NumberOfTableEntries;++I)
    if(CompareGuid(&Guid,&System->ConfigurationTable[I].VendorGuid))
      Request=System->ConfigurationTable[I].VendorTable;
  Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if(EFI_ERROR(Status) || !Gop || !Gop->Mode || !Gop->Mode->Info) return EFI_UNSUPPORTED;
  Info=Gop->Mode->Info;
  if(Gop->Mode->FrameBufferBase!=POLARIS_CONSOLE_FB || Info->HorizontalResolution!=1080 ||
      Info->VerticalResolution!=2160 || Info->PixelsPerScanLine!=1080 ||
      Gop->Mode->FrameBufferSize<POLARIS_CONSOLE_BYTES ||
      Gop->Mode->FrameBufferSize>0x02400000 ||
      Info->PixelFormat!=PixelBlueGreenRedReserved8BitPerColor) return EFI_UNSUPPORTED;
  if(!Request) {
    ZeroMem(&Fallback,sizeof(Fallback));Fallback.revision=PJB_BOOT_DISPLAY_REVISION;
    Fallback.size=sizeof(Fallback);Fallback.rotation=90;
    Fallback.display_width=Fallback.width=2160;Fallback.display_height=Fallback.height=1080;
    Request=&Fallback;
  }
  if(Request->revision!=PJB_BOOT_DISPLAY_REVISION || Request->size<sizeof(*Request)) return EFI_UNSUPPORTED;
  Surface=(struct pj4edk2_surface){Gop->Mode->FrameBufferBase,Gop->Mode->FrameBufferSize,
      Gop->Mode->FrameBufferSize,1080,2160,4320,2,Request->rotation,0};
  Region=(struct pj4edk2_region){Request->display_width,Request->display_height,
      Request->x,Request->y,Request->width,Request->height};
  if(!pj4edk2_linux_display_cells(&Surface,&Region,V) || !polaris_output_layout(&Layout,V)) return EFI_UNSUPPORTED;
  Pixels=(VOID *)(UINTN)Surface.address;Column=Row=0;First=MAX_UINTN;Last=0;
  if(PolarisBootConsole()->Magic==POLARIS_CONSOLE_MAGIC) {
    PreviousOwner=PolarisBootConsole()->Owner;PolarisBootConsoleOwner(POLARIS_CONSOLE_TEXT);
  }
  Owned=TRUE;
  SetMem(Cells,sizeof(Cells),' ');
  // Clear only the declared log grid. The adjacent UI card remains intact.
  for(R=0;R<Layout.rows;++R) for(C=0;C<Layout.cols;++C) Cell(R,C);
  Flush();return EFI_SUCCESS;
}
BOOLEAN PolarisLinuxOutputWrite(CONST CHAR16 *Text) {
  if(!Owned) return FALSE;
  while(*Text) Character(*Text++);
  Flush();return TRUE;
}
VOID PolarisLinuxOutputEnd(VOID) {
  if(Owned) PolarisBootConsoleOwner(PreviousOwner);
  Owned=FALSE;Pixels=NULL;
}
