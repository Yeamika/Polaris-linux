// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable) {
  Print(L"POLARIS_RAM_BOOT_PROBE: entered downloaded EFI application\n");
  gBS->Stall(1000000);
  Print(L"POLARIS_RAM_BOOT_PROBE: returning to USB recovery\n");
  return EFI_SUCCESS;
}
