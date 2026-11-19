// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_WINDOWS_ERRATA_H
#define POLARIS_WINDOWS_ERRATA_H
#include <Uefi.h>

typedef struct {
  UINT8 *Base;
  UINT32 Size,Headers,Sections,Count,Exception,ExceptionSize,Exports,ExportSize;
} WINDOWS_IMAGE;
typedef struct {UINT32 Offset,Before,After;} WINDOWS_PATCH;
typedef struct {
  WINDOWS_PATCH Item[64];
  UINT32 Count,Reads,Writes,Cache,Psci;
} WINDOWS_PATCH_PLAN;

EFI_STATUS WindowsImageOpen(VOID *Data,UINTN Capacity,WINDOWS_IMAGE *Image);
BOOLEAN WindowsImageCode(CONST WINDOWS_IMAGE *Image,UINT32 Offset,UINT32 Bytes);
UINT32 WindowsFindContext(CONST WINDOWS_IMAGE *Image);
EFI_STATUS WindowsPlanKernel(CONST WINDOWS_IMAGE *Image,WINDOWS_PATCH_PLAN *Plan);
EFI_STATUS WindowsPlanLoader(CONST WINDOWS_IMAGE *Image,WINDOWS_PATCH_PLAN *Plan);
EFI_STATUS WindowsApplyPlan(CONST WINDOWS_IMAGE *Image,CONST WINDOWS_PATCH_PLAN *Plan);
#endif
