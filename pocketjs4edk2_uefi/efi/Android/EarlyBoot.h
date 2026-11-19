// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_ANDROID_EARLY_BOOT_H
#define POLARIS_ANDROID_EARLY_BOOT_H
#include <Uefi.h>
#define POLARIS_ANDROID_BINDING_ADDRESS 0xcfffe000ULL
#define POLARIS_ANDROID_FLAG_ADDRESS 0x17fe00000ULL
#define POLARIS_ANDROID_FLAG_VALUE 0x5241
#define POLARIS_ANDROID_FLAG_COUNT 128
typedef struct {
  UINT64 Magic;
  UINT32 Revision, Size;
  UINT64 BootSize;
  UINT32 BootCrc32;
  UINT16 FlagValue, FlagCount;
  UINT64 FlagAddress, KernelEntry, ImageSize, Reserved;
} POLARIS_ANDROID_BINDING;
STATIC inline BOOLEAN PolarisAndroidBindingValid(CONST POLARIS_ANDROID_BINDING *B) {
  return B && sizeof(*B)==64 && B->Magic==0x3152444e414c4f50ULL &&
    B->Revision==1 && B->Size==sizeof(*B) && B->BootSize>=1648 && B->BootSize<=64*1024*1024 &&
    B->FlagValue==POLARIS_ANDROID_FLAG_VALUE && B->FlagCount==POLARIS_ANDROID_FLAG_COUNT &&
    B->FlagAddress==POLARIS_ANDROID_FLAG_ADDRESS && !B->Reserved &&
    B->ImageSize>0x701000 && B->ImageSize<=64*1024*1024 && !(B->ImageSize&4095) &&
    B->KernelEntry>=64 && !(B->KernelEntry&3) && B->KernelEntry<B->ImageSize-0x701000;
}
EFI_STATUS PolarisAndroidEarlyBoot(CONST VOID *Boot, UINTN Size);
#endif
