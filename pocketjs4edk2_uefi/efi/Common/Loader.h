// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_LOADER_H
#define POLARIS_LOADER_H
#include <Uefi.h>
EFI_STATUS PolarisFindVolume(EFI_HANDLE Image, CONST CHAR16 *Path, EFI_HANDLE *Volume);
EFI_STATUS PolarisReadFile(EFI_HANDLE Volume, CONST CHAR16 *Path, UINTN Limit, VOID **Data, UINTN *Size);
EFI_STATUS PolarisStartImage(EFI_HANDLE Parent, EFI_HANDLE Volume, CONST CHAR16 *Path, CONST CHAR16 *Options);
#endif
