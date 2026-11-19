// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_NATIVE_JOURNAL_H
#define POLARIS_NATIVE_JOURNAL_H
#include <Uefi.h>
#define POLARIS_NATIVE_JOURNAL_MAGIC 0x314a4e544c4f5050ULL
typedef struct {
  UINT64 Magic;
  UINT32 Size,Phase;
  UINT64 El,Sctlr,Entry,Dtb,Reserved0,Reserved1;
} POLARIS_NATIVE_JOURNAL;
STATIC_ASSERT(sizeof(POLARIS_NATIVE_JOURNAL)==64,"Native journal assembly size");
STATIC_ASSERT(OFFSET_OF(POLARIS_NATIVE_JOURNAL,Phase)==12,"Native phase assembly offset");
STATIC_ASSERT(OFFSET_OF(POLARIS_NATIVE_JOURNAL,El)==16,"Native EL assembly offset");
STATIC_ASSERT(OFFSET_OF(POLARIS_NATIVE_JOURNAL,Sctlr)==24,"Native SCTLR assembly offset");
EFI_STATUS PolarisNativeJournalCreate(POLARIS_NATIVE_JOURNAL **Out);
EFI_STATUS PolarisNativeJournalRead(UINT64 Address,POLARIS_NATIVE_JOURNAL *Out);
#endif
