// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_LINUX_DIAGNOSTICS_H
#define POLARIS_LINUX_DIAGNOSTICS_H
#include <Uefi.h>
// Best-effort, bounded ESP log. No filesystem operations at ExitBootServices.
EFI_STATUS PolarisLinuxConsoleBegin(EFI_SYSTEM_TABLE *System);
EFI_STATUS PolarisLinuxLogBegin(EFI_HANDLE Volume);
// Stop ESP writes while retaining the bounded output through handoff.
VOID PolarisLinuxLogSeal(VOID);
VOID PolarisLinuxLogEnd(VOID);
#endif
