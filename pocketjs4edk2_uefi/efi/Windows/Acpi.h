// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_WINDOWS_ACPI_H
#define POLARIS_WINDOWS_ACPI_H
#include <Uefi.h>

#define WINDOWS_ACPI_VALUES 19U
typedef struct {
  UINT64 Value[WINDOWS_ACPI_VALUES];
  CHAR8 Chip[16];
} WINDOWS_PLATFORM_INFO;

// Only the fixed Name objects in the pinned Polaris DSDT are supported. All
// encodings, bounds and values are checked before changing any byte.
EFI_STATUS WindowsPatchDsdt(VOID *Data,UINTN Size,CONST WINDOWS_PLATFORM_INFO *Info);
EFI_STATUS WindowsPreparePlatform(WINDOWS_PLATFORM_INFO *Info);
CONST CHAR8 *WindowsPlatformStage(VOID);
EFI_STATUS WindowsCopyDsdt(VOID **Data,UINTN *Size);
#endif
