// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_LINUX_BOOT_OUTPUT_H
#define POLARIS_LINUX_BOOT_OUTPUT_H
#include <Uefi.h>
EFI_STATUS PolarisLinuxOutputBegin(EFI_SYSTEM_TABLE *System);
BOOLEAN PolarisLinuxOutputWrite(CONST CHAR16 *Text);
VOID PolarisLinuxOutputEnd(VOID);
#endif
