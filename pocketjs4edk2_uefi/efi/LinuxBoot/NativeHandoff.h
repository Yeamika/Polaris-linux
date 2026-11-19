// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_NATIVE_HANDOFF_H
#define POLARIS_NATIVE_HANDOFF_H
#include <Uefi.h>
EFI_STATUS PolarisLinuxNativeStart(EFI_HANDLE Image,EFI_HANDLE Volume,VOID *Dtb,
  CONST CHAR16 *Options,VOID *Initrd,UINTN InitrdBytes,BOOLEAN PrepareOnly);
#endif
