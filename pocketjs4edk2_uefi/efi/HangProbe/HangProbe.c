// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/UefiLib.h>
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table) {
  (VOID)Image;(VOID)Table;
  Print(L"POLARIS_WATCHDOG_HANG: deliberate stall; expect hardware reset in 30 seconds\n");
  for (;;) { __asm__ volatile("yield"); }
}
