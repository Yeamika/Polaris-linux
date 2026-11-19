// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_LINUX_EXIT_TRACE_H
#define POLARIS_LINUX_EXIT_TRACE_H
#include <Uefi.h>

// Last page of Polaris' firmware display reservation, outside the single
// 1080x2160 scanout and its console state. Never use ordinary reclaimable RAM.
#ifndef POLARIS_LINUX_TRACE_ADDRESS
#define POLARIS_LINUX_TRACE_ADDRESS 0x9f7ff000ULL
#endif
#define POLARIS_LINUX_TRACE_MAGIC 0x3245434152544c50ULL
typedef struct {
  UINT64 Magic;
  UINT32 Size,Phase,Calls,Reserved;
  UINT64 ExitStatus,ReturnStatus,Sequence;
} POLARIS_LINUX_EXIT_TRACE;
EFI_STATUS PolarisLinuxExitTraceBegin(VOID);
VOID PolarisLinuxExitTraceEnd(EFI_STATUS Status);
EFI_STATUS PolarisLinuxExitTraceRead(POLARIS_LINUX_EXIT_TRACE *Out);
#endif
