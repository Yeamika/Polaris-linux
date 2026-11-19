// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../Common/Loader.h"
#include "Acpi.h"
#include "../../platform/WindowsHandoff.h"
#include "pj4edk2_fastboot.h"
#include "pj4edk2_rotate.h"
#include <Guid/EventGroup.h>
#include <Guid/Fdt.h>
#include <Guid/FileInfo.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#define font5x12 PolarisWindowsFont5x12
#include <Resources/font5x12.h>
#undef font5x12

STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
STATIC UINTN Row;
STATIC CONST CHAR16 BootPath[]=L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

// Large, bounded Boot Services text. No framebuffer writes or EBS wrapper
// remain active while Windows takes control of the display and kernel.
STATIC VOID Line(CONST CHAR8 *Text) {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels,White={255,255,255,0};
  UINTN Width,Height=FONT_HEIGHT*4,I,X,Y,Sx,Sy,Bits,Count=AsciiStrLen(Text);
  Print(L"%a\r\n",Text);
  if(!Gop || !Count) return;
  Width=(Gop->Mode->Info->HorizontalResolution-96)/((FONT_WIDTH+1)*4);
  if(Count>Width) Count=Width;
  Width=Count*(FONT_WIDTH+1)*4;
  if(48+Row*(Height+16)+Height>Gop->Mode->Info->VerticalResolution) return;
  Pixels=AllocateZeroPool(Width*Height*sizeof(*Pixels));
  if(!Pixels) return;
  for(I=0;I<Count;++I) {
    UINT8 Ch=Text[I]>=32 && Text[I]<=126?Text[I]:'?';
    for(Y=0;Y<FONT_HEIGHT;++Y) {
      Bits=PolarisWindowsFont5x12[(Ch-32)*2+Y/(FONT_HEIGHT/2)]>>((Y%(FONT_HEIGHT/2))*FONT_WIDTH);
      for(X=0;X<FONT_WIDTH;++X) if(Bits&(1U<<X))
        for(Sy=0;Sy<4;++Sy) for(Sx=0;Sx<4;++Sx)
          Pixels[(Y*4+Sy)*Width+I*(FONT_WIDTH+1)*4+X*4+Sx]=White;
    }
  }
  Gop->Blt(Gop,Pixels,EfiBltBufferToVideo,0,0,48,48+Row*(Height+16),Width,Height,0);
  FreePool(Pixels);++Row;
}

STATIC EFI_STATUS Display(VOID) {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Black={0};EFI_STATUS Status;
  Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if(EFI_ERROR(Status)) return Status;
  if(!Gop || !Gop->Mode || !Gop->Mode->Info || !Gop->Blt ||
     Gop->Mode->Info->HorizontalResolution<320 || Gop->Mode->Info->VerticalResolution<320 ||
     Gop->Mode->Info->PixelsPerScanLine<Gop->Mode->Info->HorizontalResolution ||
     Gop->Mode->Info->PixelFormat>=PixelBltOnly || !Gop->Mode->FrameBufferBase ||
     (UINT64)Gop->Mode->Info->PixelsPerScanLine*Gop->Mode->Info->VerticalResolution*4>
       Gop->Mode->FrameBufferSize) {Gop=NULL;return EFI_UNSUPPORTED;}
  return Gop->Blt(Gop,&Black,EfiBltVideoFill,0,0,0,0,
    Gop->Mode->Info->HorizontalResolution,Gop->Mode->Info->VerticalResolution,0);
}

STATIC EFI_STATUS StopHardware(POLARIS_WINDOWS_HANDOFF **Policy) {
  EFI_GUID FastbootGuid=PJ4EDK2_FASTBOOT_GUID,RotateGuid=PJ4EDK2_ROTATE_GUID;
  EFI_GUID HandoffGuid=POLARIS_WINDOWS_HANDOFF_GUID;
  PJ4EDK2_FASTBOOT_PROTOCOL *Fastboot;PJ4EDK2_ROTATE_PROTOCOL *Rotate;EFI_STATUS Status;
  Status=gBS->LocateProtocol(&HandoffGuid,NULL,(VOID **)Policy);
  if(EFI_ERROR(Status)) return Status;
  if(!*Policy || (*Policy)->Revision!=POLARIS_WINDOWS_HANDOFF_REVISION ||
     (*Policy)->Size<sizeof(**Policy) || !(*Policy)->Set) return EFI_INCOMPATIBLE_VERSION;
  Status=gBS->LocateProtocol(&FastbootGuid,NULL,(VOID **)&Fastboot);
  if(EFI_ERROR(Status)) return Status;
  if(!Fastboot || Fastboot->revision!=PJ4EDK2_FASTBOOT_REVISION ||
     Fastboot->size<sizeof(*Fastboot) || !Fastboot->stop) return EFI_INCOMPATIBLE_VERSION;
  Status=Fastboot->stop(Fastboot);if(EFI_ERROR(Status)) return Status;
  Status=gBS->LocateProtocol(&RotateGuid,NULL,(VOID **)&Rotate);
  if(Status!=EFI_NOT_FOUND) {
    if(EFI_ERROR(Status)) return Status;
    if(!Rotate || Rotate->revision!=PJ4EDK2_ROTATE_REVISION ||
       Rotate->size<sizeof(*Rotate) || !Rotate->stop) return EFI_INCOMPATIBLE_VERSION;
    Status=Rotate->stop(Rotate);if(EFI_ERROR(Status)) return Status;
  }
  return (*Policy)->Set(*Policy,TRUE);
}

STATIC VOID EFIAPI Ready(EFI_EVENT Event,VOID *Context) { (VOID)Event;(VOID)Context; }
STATIC EFI_STATUS ReadyToBoot(VOID) {
  EFI_EVENT Event;EFI_STATUS Status,Closed;
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Ready,NULL,&gEfiEventReadyToBootGuid,&Event);
  if(EFI_ERROR(Status)) return Status;
  Status=gBS->SignalEvent(Event);Closed=gBS->CloseEvent(Event);
  return EFI_ERROR(Status)?Status:Closed;
}

STATIC EFI_STATUS Save(EFI_HANDLE Volume,CONST CHAR16 *Path,VOID *Data,UINTN Size) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;EFI_FILE_PROTOCOL *Root,*Directory=NULL,*File=NULL;
  EFI_FILE_INFO *Info=NULL;UINTN InfoSize=0,Bytes=Size;EFI_STATUS Status;
  Status=gBS->HandleProtocol(Volume,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if(EFI_ERROR(Status)) return Status;
  Status=Fs->OpenVolume(Fs,&Root);if(EFI_ERROR(Status)) return Status;
  Status=Root->Open(Root,&Directory,L"\\EFI\\Windows",EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,EFI_FILE_DIRECTORY);
  if(!EFI_ERROR(Status)) {
    Directory->Close(Directory);
    Status=Root->Open(Root,&File,(CHAR16 *)Path,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
  }
  Root->Close(Root);if(EFI_ERROR(Status)) return Status;
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&InfoSize,NULL);
  if(Status!=EFI_BUFFER_TOO_SMALL || InfoSize<SIZE_OF_EFI_FILE_INFO || InfoSize>65536) {
    Status=EFI_COMPROMISED_DATA;goto Done;
  }
  Info=AllocatePool(InfoSize);if(!Info) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  Status=File->GetInfo(File,&gEfiFileInfoGuid,&InfoSize,Info);if(EFI_ERROR(Status)) goto Done;
  Info->FileSize=Size;
  ZeroMem(&Info->CreateTime,sizeof(Info->CreateTime));
  ZeroMem(&Info->LastAccessTime,sizeof(Info->LastAccessTime));
  ZeroMem(&Info->ModificationTime,sizeof(Info->ModificationTime));
  Status=File->SetInfo(File,&gEfiFileInfoGuid,InfoSize,Info);if(EFI_ERROR(Status)) goto Done;
  Status=File->Write(File,&Bytes,Data);
  if(!EFI_ERROR(Status) && Bytes!=Size) Status=EFI_DEVICE_ERROR;
  if(!EFI_ERROR(Status)) Status=File->Flush(File);
Done:
  if(Info) FreePool(Info);
  File->Close(File);return Status;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
  EFI_HANDLE Volume=NULL,Child=NULL;EFI_DEVICE_PATH_PROTOCOL *Path=NULL;
  POLARIS_WINDOWS_HANDOFF *Policy=NULL;WINDOWS_PLATFORM_INFO Info;
  VOID *Previous=NULL,*Snapshot=NULL;UINTN SnapshotSize=0,N=0,I;
  BOOLEAN Guard=FALSE,FdtRemoved=FALSE;EFI_STATUS Status,Restore;
  CONST CHAR8 *Stage="firmware handoff protocol";CHAR8 Text[1024],Error[128];
  (VOID)System;ZeroMem(&Info,sizeof(Info));
  Status=StopHardware(&Policy);if(EFI_ERROR(Status)) goto Done;
  Guard=TRUE;
  Stage="framebuffer";Status=Display();if(EFI_ERROR(Status)) goto Done;
  Line("POLARIS WINDOWS");Line("Preparing platform");
  Stage="Microsoft boot manager";Status=PolarisFindVolume(Image,BootPath,&Volume);
  if(EFI_ERROR(Status)) goto Done;
  // Snapshot writes are best effort and never a prerequisite for boot.
  if(!EFI_ERROR(WindowsCopyDsdt(&Snapshot,&SnapshotSize))) {
    Save(Volume,L"\\EFI\\Windows\\acpi-before.aml",Snapshot,SnapshotSize);
    FreePool(Snapshot);Snapshot=NULL;
  }
  Stage="ACPI / SoC / reserved memory";Status=WindowsPreparePlatform(&Info);
  if(EFI_ERROR(Status)) {Stage=WindowsPlatformStage();goto Done;}
  Line("ACPI and SoC information ready");
  Stage="ReadyToBoot event";Status=ReadyToBoot();if(EFI_ERROR(Status)) goto Done;
  // Recheck the published tables after all ReadyToBoot observers have run.
  Stage="final ACPI tables";Status=WindowsPreparePlatform(&Info);
  if(EFI_ERROR(Status)) {Stage=WindowsPlatformStage();goto Done;}
  if(!EFI_ERROR(WindowsCopyDsdt(&Snapshot,&SnapshotSize))) {
    Save(Volume,L"\\EFI\\Windows\\acpi-prepared.aml",Snapshot,SnapshotSize);
    FreePool(Snapshot);Snapshot=NULL;
  }
  Stage="ACPI-only handoff";
  Status=EfiGetSystemConfigurationTable(&gFdtTableGuid,&Previous);
  if(Status!=EFI_NOT_FOUND && EFI_ERROR(Status)) goto Done;
  if(Previous) {
    Status=gBS->InstallConfigurationTable(&gFdtTableGuid,NULL);if(EFI_ERROR(Status)) goto Done;
    FdtRemoved=TRUE;
  }
  Stage="LoadImage bootmgfw.efi";Path=FileDevicePath(Volume,(CHAR16 *)BootPath);
  if(!Path) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  // BootPolicy TRUE matches BDS loading an OS boot option; no temporary
  // Boot#### or BootOrder writes are needed for this explicit file path.
  Status=gBS->LoadImage(TRUE,Image,Path,NULL,0,&Child);
  FreePool(Path);Path=NULL;if(EFI_ERROR(Status)) goto Done;
  Stage="UEFI watchdog";Status=gBS->SetWatchdogTimer(300,0,0,NULL);
  if(EFI_ERROR(Status)) goto Done;
  Line("Starting Microsoft boot manager");
  N=AsciiSPrint(Text,sizeof(Text),"POLARIS_WINDOWS_HANDOFF_2\r\nStage=StartImage\r\nReadyToBoot=signalled\r\nCSRT=verified\r\nAPSS=firmware heartbeat; released at EBS to Windows HAL\r\nEBS_wrapper=original assembly; bounded errata plans armed\r\n");
  for(I=0;I<WINDOWS_ACPI_VALUES;++I)
    N+=AsciiSPrint(Text+N,sizeof(Text)-N,"ACPI[%u]=%016lx\r\n",(UINT32)I,Info.Value[I]);
  Save(Volume,L"\\EFI\\Windows\\boot-status.txt",Text,N);
  Stage="Microsoft boot manager returned";
  Status=gBS->StartImage(Child,NULL,NULL);
  // Exit() may already have unloaded the application; never read its image.
  Child=NULL;
Done:
  if(Guard && Policy->Errata && EFI_ERROR(Policy->Errata->Status)) {
    Stage="Windows kernel preparation";Status=Policy->Errata->Status;
  }
  gBS->SetWatchdogTimer(0,0,0,NULL);
  if(Child) gBS->UnloadImage(Child);
  if(FdtRemoved) {
    Restore=gBS->InstallConfigurationTable(&gFdtTableGuid,Previous);
    if(EFI_ERROR(Restore)) Status=Restore;
  }
  if(Guard) {
    Restore=Policy->Set(Policy,FALSE);
    if(EFI_ERROR(Restore)) Status=Restore;
  }
  AsciiSPrint(Error,sizeof(Error),"%a",Stage);Line(Error);
  AsciiSPrint(Error,sizeof(Error),"Status: %r",Status);Line(Error);
  AsciiSPrint(Error,sizeof(Error),"Code: 0x%016lx",(UINT64)Status);Line(Error);
  if(Guard && Policy->Errata) {
    AsciiSPrint(Error,sizeof(Error),"Kernel preparation stage: %u",Policy->Errata->Stage);Line(Error);
  }
  if(Volume) {
    N=AsciiSPrint(Text,sizeof(Text),"POLARIS_WINDOWS_HANDOFF_2\r\nStage=%a\r\nStatus=%r (0x%lx)\r\n",Stage,Status,(UINT64)Status);
    if(Guard && Policy->Errata) {
      POLARIS_WINDOWS_ERRATA *E=Policy->Errata;
      N+=AsciiSPrint(Text+N,sizeof(Text)-N,"ErrataStage=%u\r\nErrataStatus=0x%lx\r\nContextRva=0x%x\r\nLoaderReads=%u\r\nKernelReads=%u\r\nKernelWrites=%u\r\nCache=%u\r\nPsci=%u\r\n",
        E->Stage,(UINT64)E->Status,E->ContextRva,E->LoaderReads,E->KernelReads,E->KernelWrites,E->Cache,E->Psci);
    }
    Save(Volume,L"\\EFI\\Windows\\boot-status.txt",Text,N);
  }
  if(Gop) gBS->Stall(5000000);
  return Status;
}
