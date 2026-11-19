// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included in the private platform BDS build by build.py.
#include <PiDxe.h>
#include <Library/DxeServicesLib.h>
#include "BootConsole.h"

STATIC CONST CHAR16 *PolarisEntryStage;
STATIC EFI_STATUS PolarisEntryStatus;
STATIC EFI_GUID PolarisEntryGuid;

STATIC EFI_STATUS PolarisStartBuiltin(CONST EFI_GUID *Guid) {
  VOID *Buffer=NULL;
  UINTN Size=0;
  EFI_HANDLE Child;
  EFI_STATUS Status;
  PolarisEntryGuid=*Guid;
  PolarisEntryStage=L"FV read";
  Status=GetSectionFromAnyFv(Guid,EFI_SECTION_PE32,0,&Buffer,&Size);
  PolarisEntryStatus=Status;
  Print(L"POLARIS_ENTRY_DIAG %g: %s %r\n",Guid,PolarisEntryStage,Status);
  if (EFI_ERROR(Status)) return Status;
  PolarisEntryStage=L"LoadImage";
  Status=gBS->LoadImage(FALSE,gImageHandle,NULL,Buffer,Size,&Child);
  FreePool(Buffer);
  PolarisEntryStatus=Status;
  Print(L"POLARIS_ENTRY_DIAG: %s %r\n",PolarisEntryStage,Status);
  if (EFI_ERROR(Status)) return Status;
  PolarisEntryStage=L"StartImage / application return";
  Status=gBS->StartImage(Child,NULL,NULL);
  gBS->UnloadImage(Child);
  PolarisEntryStatus=Status;
  return Status;
}

#define POLARIS_RETRY 1
#define POLARIS_MENU 2
#define POLARIS_SHELL 3
STATIC UINTN PolarisWaitForChoice(VOID) {
  EFI_INPUT_KEY Key;
  gBS->SetWatchdogTimer(0,0,0,NULL);
  Print(L"\nPOLARIS_ENTRY_DIAG: built-in entry returned\n");
  Print(L"Firmware: %s\n",(CHAR16 *)PcdGetPtr(PcdFirmwareVersionString));
  Print(L"Application: %g\nStage: %s\nStatus: %r (0x%lx)\n",
        &PolarisEntryGuid,PolarisEntryStage,PolarisEntryStatus,(UINT64)PolarisEntryStatus);
  Print(L"Volume UP / R: retry; Volume DOWN / M: menu; ESC / S: UEFI Shell\n");
  Print(L"This page stays visible; no automatic Shell fallback.\n");
  for (;;) {
    if (gST->ConIn!=NULL && !EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn,&Key))) {
      if (Key.ScanCode==SCAN_UP || Key.UnicodeChar==L'r' || Key.UnicodeChar==L'R') return POLARIS_RETRY;
      if (Key.ScanCode==SCAN_DOWN || Key.UnicodeChar==L'm' || Key.UnicodeChar==L'M') return POLARIS_MENU;
      if (Key.ScanCode==SCAN_ESC || Key.UnicodeChar==L's' || Key.UnicodeChar==L'S') return POLARIS_SHELL;
    }
    gBS->Stall(10000);
  }
}

STATIC VOID PolarisRunEntry(BOOLEAN RecoveryFirst) {
  CONST EFI_GUID RecoveryGuid={0xbbb59c78,0xc763,0x47aa,{0x91,0xb1,0xb7,0x46,0xf0,0x25,0x18,0x2e}};
#ifdef POLARIS_USE_POCKETJS
  CONST EFI_GUID PocketGuid={0xedb979d9,0x310e,0x4145,{0x95,0x0e,0xac,0x16,0x91,0xa1,0x01,0x0a}};
  CONST EFI_GUID *MenuGuid=&PocketGuid;
#else
  CONST EFI_GUID *MenuGuid=&gSimpleInitFileGuid;
#endif
  BOOLEAN Recovery=RecoveryFirst;
  EFI_STATUS Status;
  UINTN Choice;
  for (;;) {
    Status=PolarisStartBuiltin(Recovery?&RecoveryGuid:MenuGuid);
    // A successful recovery return is an explicit "continue"/ESC request.
    if (Recovery && !EFI_ERROR(Status)) { Recovery=FALSE; continue; }
    Choice=PolarisWaitForChoice();
    while (Choice==POLARIS_SHELL) {
      PolarisBootConsoleOwner(POLARIS_CONSOLE_TEXT);
      PolarisStartBuiltin(&gUefiShellFileGuid);
      // Returning from an explicitly requested shell returns to the entry page.
      Choice=PolarisWaitForChoice();
    }
    if (Choice==POLARIS_MENU) Recovery=FALSE;
  }
}
