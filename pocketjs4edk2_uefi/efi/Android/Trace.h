// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_ANDROID_TRACE_H
#define POLARIS_ANDROID_TRACE_H
// Last page of SDM845's reserved UEFI FD mapping [CE000000,D0000000).
// The actual firmware image occupies [CE000000,CE700000), and the allocator
// cannot use this page. build.py checks the input memory-map contract.
#define POLARIS_ANDROID_TRACE_ADDRESS 0xcffff000ULL
#define POLARIS_ANDROID_TRACE_MAGIC 0x335243544c4f5055ULL
STATIC volatile UINT64 *PolarisAndroidTrace(VOID) {
  return (volatile UINT64 *)(UINTN)POLARIS_ANDROID_TRACE_ADDRESS;
}
STATIC VOID PolarisAndroidTraceStage(UINT64 Stage,UINT64 Status) {
  volatile UINT64 *Trace=PolarisAndroidTrace();
  Trace[1]=Stage;Trace[6]=Status;
  __asm__ volatile("dc cvac, %0\n\tdsb sy"::"r"(Trace):"memory");
}
#endif
