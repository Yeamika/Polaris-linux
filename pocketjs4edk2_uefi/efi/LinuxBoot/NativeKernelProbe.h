// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_NATIVE_KERNEL_PROBE_H
#define POLARIS_NATIVE_KERNEL_PROBE_H
// Stop at one verified instruction address. PSCI SYSTEM_RESET needs no MMIO
// or stack, so a checkpoint works before and after the Linux MMU switch.
typedef struct {
  CHAR8 Magic[16]; UINT32 Revision,Size;
  UINT64 ImageBytes,Memory,Offset;
  UINT8 Before[32]; UINT32 Reset[8]; CHAR8 Symbol[32];
} POLARIS_KERNEL_PROBE;
STATIC_ASSERT(sizeof(POLARIS_KERNEL_PROBE)==144,"Probe image configuration layout");
STATIC volatile POLARIS_KERNEL_PROBE NativeKernelProbe={
  .Magic="POLARIS-PROBE01", .Revision=1,.Size=144,.Offset=MAX_UINT64,
  // movz/movk w0=PSCI SYSTEM_RESET; x1-x3=0; smc #0; wfi; b to wfi.
  .Reset={0x52800120,0x72b08000,0xaa1f03e1,0xaa1f03e2,0xaa1f03e3,
          0xd4000003,0xd503207f,0x17ffffff}
};
STATIC EFI_STATUS NativeKernelProbeApply(VOID *Raw,UINTN Bytes,UINT64 Memory) {
  UINTN I;UINT64 Offset=NativeKernelProbe.Offset;UINT32 Code[8];
  if(NativeKernelProbe.Revision!=1 || NativeKernelProbe.Size!=144 ||
     NativeKernelProbe.ImageBytes!=Bytes || NativeKernelProbe.Memory!=Memory ||
     Offset&3 || Offset>Bytes || Bytes-Offset<32) return EFI_INCOMPATIBLE_VERSION;
  for(I=0;I<32;++I) if(((UINT8 *)Raw)[Offset+I]!=NativeKernelProbe.Before[I]) return EFI_COMPROMISED_DATA;
  for(I=0;I<8;++I) Code[I]=NativeKernelProbe.Reset[I];
  CopyMem((UINT8 *)Raw+(UINTN)Offset,Code,sizeof(Code));
  Print(L"NATIVE_KERNEL_PROBE: %a offset=%lx; intentional PSCI reset, no continuation\r\n",
      (CONST CHAR8 *)(UINTN)NativeKernelProbe.Symbol,Offset);
  return EFI_SUCCESS;
}
#endif
