// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included by Recovery.c for the resident DXE backend, never a second USB owner.
#include <Guid/EventGroup.h>
#include "../../platform/BootConsole.h"
#include "../../platform/WindowsHandoff.h"
#include "pj4edk2_fastboot.h"
#define PJB_EDK2
#include "pocketjs_boot_context.h"
STATIC EFI_HANDLE BackendImage;
STATIC EFI_EVENT ExitEvent,RetryTimer;
STATIC BOOLEAN BackendActive;
STATIC PJB_PLATFORM_PROTOCOL Platform;
STATIC BOOLEAN Exited;
STATIC BOOLEAN WindowsHandoff;
STATIC EFI_EVENT WindowsHeartbeat;
STATIC POLARIS_WINDOWS_HANDOFF WindowsPolicy;
STATIC PJ4EDK2_FASTBOOT_PROTOCOL Backend;
// Hook the original protocol objects so clients which cached ConOut/StdErr
// also use the RAM log. Do not replace the system table or its CRC.
STATIC EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConsoleOut,*ConsoleErr;
STATIC EFI_TEXT_STRING SavedOut,SavedErr;
STATIC BOOLEAN ConsoleWriting;
STATIC EFI_STATUS EFIAPI LogConsole(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *Text) {
  CHAR8 Line[60];UINTN N=0;UINT32 Ch;UINT8 Bytes[3];UINTN Count,I;
  (VOID)This;
  if (!Text) return EFI_INVALID_PARAMETER;
  if (ConsoleWriting) return EFI_SUCCESS;
  ConsoleWriting=TRUE;
  for (;*Text;++Text) {
    Ch=*Text;
    if (Ch=='\r') continue;
    if (Ch=='\n') {
      if (N) { Line[N]=0;PolarisLog(Line);N=0; }
      continue;
    }
    if (Ch<32) Ch=' ';
    if (Ch>=0xd800 && Ch<=0xdfff) Ch='?';
    if (Ch<0x80) { Bytes[0]=(UINT8)Ch;Count=1; }
    else if (Ch<0x800) { Bytes[0]=0xc0|(Ch>>6);Bytes[1]=0x80|(Ch&63);Count=2; }
    else { Bytes[0]=0xe0|(Ch>>12);Bytes[1]=0x80|((Ch>>6)&63);Bytes[2]=0x80|(Ch&63);Count=3; }
    if (N+Count>=sizeof(Line)) { Line[N]=0;PolarisLog(Line);N=0; }
    for (I=0;I<Count;++I) Line[N++]=(CHAR8)Bytes[I];
  }
  if (N) { Line[N]=0;PolarisLog(Line); }
  ConsoleWriting=FALSE;return EFI_SUCCESS;
}
STATIC VOID CaptureConsole(VOID) {
  if (!gST || ConsoleOut || ConsoleErr) return;
  ConsoleOut=gST->ConOut;ConsoleErr=gST->StdErr;
  if (ConsoleOut) { SavedOut=ConsoleOut->OutputString;ConsoleOut->OutputString=LogConsole; }
  if (ConsoleErr && ConsoleErr!=ConsoleOut) {
    SavedErr=ConsoleErr->OutputString;ConsoleErr->OutputString=LogConsole;
  }
}
STATIC VOID RestoreConsole(VOID) {
  // Never overwrite a hook installed by another owner after ours.
  if (ConsoleOut && ConsoleOut->OutputString==LogConsole) ConsoleOut->OutputString=SavedOut;
  if (ConsoleErr && ConsoleErr!=ConsoleOut && ConsoleErr->OutputString==LogConsole) ConsoleErr->OutputString=SavedErr;
  ConsoleOut=ConsoleErr=NULL;SavedOut=SavedErr=NULL;
}
STATIC EFI_STATUS EFIAPI BackendStart(PJ4EDK2_FASTBOOT_PROTOCOL *This) {
  EFI_STATUS Status;
  if (This!=&Backend || Exited) return EFI_UNSUPPORTED;
  if (BackendActive) return EFI_SUCCESS;
  if (!RetryTimer) {
    Status=gBS->CreateEvent(EVT_TIMER,0,NULL,NULL,&RetryTimer);
    if (EFI_ERROR(Status)) return Status;
  }
  Status=gBS->SetTimer(RetryTimer,TimerPeriodic,10000000);
  if (EFI_ERROR(Status)) return Status;
  BackendActive=TRUE;
  PolarisBootConsoleOwner(POLARIS_CONSOLE_UI);
  CaptureConsole();
  Status=ServiceStart(BackendImage);
  if (EFI_ERROR(Status)) PolarisLog("USB unavailable; scheduled initialization retry");
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI BackendPoll(PJ4EDK2_FASTBOOT_PROTOCOL *This,UINT32 *Flags) {
  EFI_STATUS Status;UINTN Budget;
  if (This!=&Backend || !Flags || Exited || !BackendActive) return EFI_INVALID_PARAMETER;
  *Flags=0;
  if (!ControllerRunning) {
    if (gBS->CheckEvent(RetryTimer)==EFI_SUCCESS) ServiceStart(BackendImage);
    return EFI_SUCCESS;
  }
  // Bounded per UI frame; do not start applications at a raised callback TPL.
  for (Budget=0;Budget<8;++Budget) {
    Status=ServicePoll();
    if (EFI_ERROR(Status)) {
      PolarisLog("USB failure; cancel operation and retry initialization");
      Status=ServiceStop();
      if (EFI_ERROR(Status)) return Status;
      return EFI_SUCCESS;
    }
    if (Done) { *Flags|=PJ4EDK2_FASTBOOT_CONTINUE;Done=FALSE;break; }
  }
  if (Core.State==FB_DOWNLOAD || TxPending || PolarisBusy()) *Flags|=PJ4EDK2_FASTBOOT_BUSY;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI BackendStop(PJ4EDK2_FASTBOOT_PROTOCOL *This) {
  if (This!=&Backend) return EFI_INVALID_PARAMETER;
  EFI_STATUS Status=ServiceStop();
  if (!EFI_ERROR(Status)) {
    RestoreConsole();
    PolarisBootConsoleOwner(POLARIS_CONSOLE_TEXT);
    BackendActive=FALSE;
    if (RetryTimer) { gBS->CloseEvent(RetryTimer);RetryTimer=NULL; }
  }
  return Status;
}
STATIC EFI_STATUS ResumeAfterChild(VOID) {
  if(Exited) return EFI_UNSUPPORTED;
  // A child such as Windows.efi can stop the complete resident service,
  // freeing its DMA/download pools and closing both timers. Recreate those
  // resources before the enclosing UI poll resumes.
  if(!BackendActive) return BackendStart(&Backend);
  return StartUsb();
}
STATIC VOID EFIAPI BackendExit(EFI_EVENT Event,VOID *Context) {
  EFI_STATUS Status;
  (VOID)Event;(VOID)Context;
  // Only stop DMA here. Freeing pools/transfer buffers would invalidate the
  // loader's memory-map key inside ExitBootServices. Normal Stop releases them.
  Status=ServiceQuiesce();RestoreConsole();Exited=TRUE;
  PolarisBootConsoleOwner(POLARIS_CONSOLE_TEXT);
  // ServiceStop disables the UI watchdog while releasing USB resources. Rearm
  // after cleanup: the kernel must take ownership of KPSS before this expires.
  // Do not depend on a UEFI timer once ExitBootServices has been signalled.
  // Windows' CSRT HAL owns this same physical timer after EBS. Inheriting a
  // private 30-second deadline reboots Windows while its drivers initialize.
  PolarisWatchdog(WindowsHandoff?0:30);
  if (EFI_ERROR(Status)) {
    gRT->ResetSystem(EfiResetCold,Status,0,NULL);
    for (;;) {}
  }
}
STATIC VOID EFIAPI WindowsPet(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;
  if(WindowsHandoff && !Exited) PolarisPet();
}
STATIC EFI_STATUS EFIAPI SetWindowsHandoff(POLARIS_WINDOWS_HANDOFF *This,BOOLEAN Enabled) {
  EFI_STATUS Status;EFI_GUID Guid=POLARIS_WINDOWS_ERRATA_GUID;
  if(This!=&WindowsPolicy || Exited) return EFI_UNSUPPORTED;
  if(Enabled) {
    if(WindowsHandoff || BackendActive || ControllerRunning) return EFI_NOT_READY;
    Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&This->Errata);
    if(EFI_ERROR(Status)) return Status;
    if(!This->Errata || This->Errata->Revision!=POLARIS_WINDOWS_ERRATA_REVISION ||
       This->Errata->Size<sizeof(*This->Errata) || !This->Errata->Set) return EFI_INCOMPATIBLE_VERSION;
    Status=gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,WindowsPet,NULL,&WindowsHeartbeat);
    if(EFI_ERROR(Status)) return Status;
    Status=gBS->SetTimer(WindowsHeartbeat,TimerPeriodic,10000000);
    if(EFI_ERROR(Status)) {gBS->CloseEvent(WindowsHeartbeat);WindowsHeartbeat=NULL;return Status;}
    Status=This->Errata->Set(This->Errata,TRUE);
    if(EFI_ERROR(Status)) {gBS->CloseEvent(WindowsHeartbeat);WindowsHeartbeat=NULL;return Status;}
    WindowsHandoff=TRUE;PolarisWatchdog(30);
  } else {
    WindowsHandoff=FALSE;
    if(WindowsHeartbeat) {gBS->CloseEvent(WindowsHeartbeat);WindowsHeartbeat=NULL;}
    PolarisWatchdog(0);
    if(This->Errata) return This->Errata->Set(This->Errata,FALSE);
  }
  return EFI_SUCCESS;
}
STATIC PJB_STATUS PJB_EFIAPI ReadPower(VOID *This,PJB_POWER_SAMPLE *Sample) {
  if (This!=&Platform || !Sample) return EFI_INVALID_PARAMETER;
  ZeroMem(Sample,sizeof(*Sample));Sample->size=sizeof(*Sample);
  return EFI_SUCCESS; // No PMIC estimate presented as a measured percentage.
}
STATIC PJB_STATUS PJB_EFIAPI Quiesce(VOID *This,UINT32 Quiesced) {
  if (This!=&Platform || Quiesced>1 || Exited) return EFI_INVALID_PARAMETER;
  // USB is stopped first by PJ4EDK2. Protect both child EFI and early kernel
  // startup; BackendExit rearms the hardware timer after USB cleanup.
  PolarisWatchdog(30);
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI InitializePolarisFastboot(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table) {
  EFI_GUID Guid=PJ4EDK2_FASTBOOT_GUID,WindowsGuid=POLARIS_WINDOWS_HANDOFF_GUID,PlatformGuid;
  PJB_GUID Wire=PJB_PLATFORM_GUID;EFI_STATUS Status;
  (VOID)Table;
  BackendImage=Image;CopyMem(&PlatformGuid,&Wire,sizeof(Wire));
  Platform=(PJB_PLATFORM_PROTOCOL){PJB_PLATFORM_REVISION,sizeof(Platform),ReadPower,Quiesce};
  Backend=(PJ4EDK2_FASTBOOT_PROTOCOL){PJ4EDK2_FASTBOOT_REVISION,sizeof(Backend),BackendStart,BackendPoll,BackendStop};
  WindowsPolicy=(POLARIS_WINDOWS_HANDOFF){POLARIS_WINDOWS_HANDOFF_REVISION,sizeof(WindowsPolicy),SetWindowsHandoff,NULL};
  Status=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,BackendExit,NULL,&gEfiEventExitBootServicesGuid,&ExitEvent);
  if (EFI_ERROR(Status)) return Status;
  Status=gBS->InstallMultipleProtocolInterfaces(&Image,&Guid,&Backend,&PlatformGuid,&Platform,
                                                &WindowsGuid,&WindowsPolicy,NULL);
  if (EFI_ERROR(Status)) gBS->CloseEvent(ExitEvent);
  return Status;
}
