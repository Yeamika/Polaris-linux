// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_WINDOWS_HANDOFF_H
#define POLARIS_WINDOWS_HANDOFF_H
#include <Uefi.h>
#include "WindowsErrataProtocol.h"

// The resident board driver owns the APSS timer throughout the handoff. This
// policy applies only to Windows, whose CSRT HAL takes over after EBS.
#define POLARIS_WINDOWS_HANDOFF_GUID \
  {0xd6eea124,0x23b0,0x4c4e,{0x83,0x56,0x2e,0x96,0xe3,0x94,0xb6,0x43}}
#define POLARIS_WINDOWS_HANDOFF_REVISION 2U
typedef struct POLARIS_WINDOWS_HANDOFF POLARIS_WINDOWS_HANDOFF;
struct POLARIS_WINDOWS_HANDOFF {
  UINT32 Revision,Size;
  EFI_STATUS (EFIAPI *Set)(POLARIS_WINDOWS_HANDOFF *This,BOOLEAN Enabled);
  POLARIS_WINDOWS_ERRATA *Errata;
};
#endif
