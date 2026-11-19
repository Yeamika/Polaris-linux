// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_LINUX_INITRD_H
#define POLARIS_LINUX_INITRD_H
#include <Uefi.h>
EFI_STATUS PolarisInitrdInstall(VOID *Data,UINTN Size);
EFI_STATUS PolarisInitrdRemove(VOID);
#endif
