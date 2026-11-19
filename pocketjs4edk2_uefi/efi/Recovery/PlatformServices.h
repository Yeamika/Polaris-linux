// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_PLATFORM_SERVICES_H
#define POLARIS_PLATFORM_SERVICES_H
#include <Uefi.h>
#include "FastbootCore.h"
BOOLEAN PolarisCommand(FB_CORE *Core, CONST UINT8 *Packet, UINTN Size);
BOOLEAN PolarisBusy(VOID);
BOOLEAN PolarisStep(FB_CORE *Core);
VOID PolarisCancel(VOID);
VOID PolarisNext(FB_CORE *Core);
VOID PolarisWatchdog(UINTN Seconds);
VOID PolarisPet(VOID);
VOID PolarisLog(CONST CHAR8 *Text);
#endif
