// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_WINDOWS_ERRATA_PROTOCOL_H
#define POLARIS_WINDOWS_ERRATA_PROTOCOL_H
#include <Uefi.h>

#define POLARIS_WINDOWS_ERRATA_GUID \
  {0xe4687d12,0x9c17,0x4c18,{0x98,0x02,0x70,0xdf,0x24,0x45,0x8e,0x65}}
#define POLARIS_WINDOWS_ERRATA_REVISION 1U
enum {
  WindowsErrataIdle,WindowsErrataArmed,WindowsErrataLoader,
  WindowsErrataContext,WindowsErrataModule,WindowsErrataKernel,
  WindowsErrataApply,WindowsErrataReady,WindowsErrataEbs
};
typedef struct POLARIS_WINDOWS_ERRATA POLARIS_WINDOWS_ERRATA;
struct POLARIS_WINDOWS_ERRATA {
  UINT32 Revision,Size;
  EFI_STATUS (EFIAPI *Set)(POLARIS_WINDOWS_ERRATA *This,BOOLEAN Enabled);
  UINT32 Stage,ContextRva,LoaderReads,KernelReads,KernelWrites,Cache,Psci;
  EFI_STATUS Status;
};
#endif
