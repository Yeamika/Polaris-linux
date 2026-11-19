// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_LINUX_HANDOFF_H
#define POLARIS_LINUX_HANDOFF_H
#include <Uefi.h>
// Validate the Polaris scanout and reserve a no-map region for the board's
// aperture-managed WC console. Missing region requests use the full GOP area.
EFI_STATUS PolarisLinuxDisplayHandoff(VOID *Dtb, EFI_SYSTEM_TABLE *System);
#endif
