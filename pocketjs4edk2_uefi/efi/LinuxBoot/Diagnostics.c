// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Diagnostics.h"
#include "BootOutput.h"
#include <Guid/EventGroup.h>
#include <Guid/FileInfo.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC EFI_FILE_PROTOCOL *LogFile;
STATIC EFI_EVENT ExitEvent;
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConsoleOut,*ConsoleErr;
STATIC EFI_TEXT_STRING SavedOut,SavedErr;
STATIC BOOLEAN Writing,Exited;
STATIC UINTN TotalBytes;
#define LOG_LIMIT (32*1024)

STATIC VOID Restore(VOID);
STATIC EFI_STATUS EFIAPI Capture(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *Text) {
  EFI_TEXT_STRING Original=This==ConsoleOut?SavedOut:SavedErr;
  EFI_STATUS Result=EFI_SUCCESS,Status;
  EFI_TPL Tpl;
  CHAR8 Bytes[256];
  UINTN N,Count,I;
  if (!Text) return EFI_INVALID_PARAMETER;
  if (Writing) return EFI_SUCCESS;
  Writing=TRUE;
  if (!PolarisLinuxOutputWrite(Text) && Original) Result=Original(This,Text);
  Tpl=TPL_HIGH_LEVEL;
  if (!Exited) {
    Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
    gBS->RestoreTPL(Tpl);
  }
  // Firmware callbacks can print at raised TPL. In particular, never touch
  // storage or allocate memory from an ExitBootServices notification.
  if (!Exited && LogFile && Tpl<=TPL_CALLBACK) {
    for (I=0;Text[I] && TotalBytes<LOG_LIMIT;) {
      N=0;
      while (Text[I] && N<sizeof(Bytes) && TotalBytes+N<LOG_LIMIT) {
        UINT16 Ch=Text[I++];
        Bytes[N++]=Ch<=0x7f?(CHAR8)Ch:'?';
      }
      Count=N;Status=LogFile->Write(LogFile,&Count,Bytes);
      if (EFI_ERROR(Status) || Count!=N) break;
      TotalBytes+=Count;
    }
    LogFile->Flush(LogFile);
  }
  Writing=FALSE;
  return Result;
}

STATIC VOID Restore(VOID) {
  if (ConsoleOut && ConsoleOut->OutputString==Capture) ConsoleOut->OutputString=SavedOut;
  if (ConsoleErr && ConsoleErr!=ConsoleOut && ConsoleErr->OutputString==Capture)
    ConsoleErr->OutputString=SavedErr;
  ConsoleOut=ConsoleErr=NULL;SavedOut=SavedErr=NULL;
  PolarisLinuxOutputEnd();
}
STATIC VOID EFIAPI ExitBoot(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;
  Exited=TRUE;
  // Later EBS notifications may still print. Keep the bounded renderer and
  // ownership installed until the kernel takes over, so no callback falls
  // back to the firmware's unrelated cursor or orientation.
  // Deliberately leave the handle alone. Closing it could free pools and
  // invalidate the final EFI memory-map key. The kernel reclaims that memory.
}

EFI_STATUS PolarisLinuxConsoleBegin(EFI_SYSTEM_TABLE *System) {
  EFI_STATUS Status;
  if (ExitEvent || ConsoleOut || ConsoleErr) return EFI_ALREADY_STARTED;
  Status=PolarisLinuxOutputBegin(System);
  if (EFI_ERROR(Status)) return Status;
  Exited=FALSE;Writing=FALSE;
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitBoot,NULL,
                           &gEfiEventExitBootServicesGuid,&ExitEvent);
  if (EFI_ERROR(Status)) {PolarisLinuxOutputEnd();return Status;}
  ConsoleOut=gST->ConOut;ConsoleErr=gST->StdErr;
  if (ConsoleOut) { SavedOut=ConsoleOut->OutputString;ConsoleOut->OutputString=Capture; }
  if (ConsoleErr && ConsoleErr!=ConsoleOut) {
    SavedErr=ConsoleErr->OutputString;ConsoleErr->OutputString=Capture;
  }
  return EFI_SUCCESS;
}

EFI_STATUS PolarisLinuxLogBegin(EFI_HANDLE Volume) {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root;
  EFI_FILE_INFO *Info=NULL;
  UINTN Size=0;
  EFI_STATUS Status;
  CONST CHAR16 *Stage=L"read file metadata";
  if (LogFile) return EFI_ALREADY_STARTED;
  Exited=FALSE;Writing=FALSE;TotalBytes=0;
  Status=gBS->HandleProtocol(Volume,&gEfiSimpleFileSystemProtocolGuid,(VOID **)&Fs);
  if (EFI_ERROR(Status)) return Status;
  Status=Fs->OpenVolume(Fs,&Root);
  if (EFI_ERROR(Status)) return Status;
  Status=Root->Open(Root,&LogFile,L"\\EFI\\LinuxBoot\\boot-status.txt",
                    EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,0);
  Root->Close(Root);
  if (EFI_ERROR(Status)) { LogFile=NULL;return Status; }
  Status=LogFile->GetInfo(LogFile,&gEfiFileInfoGuid,&Size,NULL);
  if (Status!=EFI_BUFFER_TOO_SMALL || Size<SIZE_OF_EFI_FILE_INFO || Size>65536) {
    Status=EFI_COMPROMISED_DATA;goto Fail;
  }
  Info=AllocatePool(Size);
  if (!Info) { Status=EFI_OUT_OF_RESOURCES;goto Fail; }
  Status=LogFile->GetInfo(LogFile,&gEfiFileInfoGuid,&Size,Info);
  if (EFI_ERROR(Status)) goto Fail;
  if (Info->Attribute&EFI_FILE_DIRECTORY) { Status=EFI_COMPROMISED_DATA;goto Fail; }
  Info->FileSize=0;
  // Zero timestamps request preservation. Boards without a working RTC can
  // return invalid dates for a new FAT file; passing those back would reject
  // a size-only update before a single diagnostic byte could be written.
  Info->CreateTime=(EFI_TIME){0};
  Info->LastAccessTime=(EFI_TIME){0};
  Info->ModificationTime=(EFI_TIME){0};
  Stage=L"truncate log";
  Status=LogFile->SetInfo(LogFile,&gEfiFileInfoGuid,Size,Info);
  if (EFI_ERROR(Status)) goto Fail;
  FreePool(Info);Info=NULL;
  {
    CHAR8 Header[]="POLARIS_LINUX_LOG_BEGIN\r\n";
    UINTN N=sizeof(Header)-1;
    Stage=L"write log header";
    Status=LogFile->Write(LogFile,&N,Header);
    if (EFI_ERROR(Status) || N!=sizeof(Header)-1) {
      if (!EFI_ERROR(Status)) Status=EFI_DEVICE_ERROR;
      goto Fail;
    }
    TotalBytes=N;
    Stage=L"flush log header";
    Status=LogFile->Flush(LogFile);
    if (EFI_ERROR(Status)) goto Fail;
  }
  return EFI_SUCCESS;
Fail:
  if (Info) FreePool(Info);
  if (LogFile) {LogFile->Close(LogFile);LogFile=NULL;}
  Print(L"Linux log initialization failed at %s: %r (0x%lx)\r\n",Stage,Status,(UINT64)Status);
  return Status;
}

VOID PolarisLinuxLogSeal(VOID) {
  // Close storage before taking the final memory-map key. Console capture
  // remains active; neither this close nor EBS restores firmware rendering.
  if (!Exited && LogFile) { LogFile->Flush(LogFile);LogFile->Close(LogFile);LogFile=NULL; }
}

VOID PolarisLinuxLogEnd(VOID) {
  if (Exited) return;
  Restore();
  if (ExitEvent) { gBS->CloseEvent(ExitEvent);ExitEvent=NULL; }
  PolarisLinuxLogSeal();
}
