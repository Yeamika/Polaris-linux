# SPDX-License-Identifier: BSD-2-Clause-Patent
[Defines]
  PLATFORM_NAME = PolarisEfi
  PLATFORM_GUID = 94dce8f4-aeaf-4973-bcd2-e8b8e33ebd81
  PLATFORM_VERSION = 0.1
  DSC_SPECIFICATION = 0x00010019
  OUTPUT_DIRECTORY = Build/PolarisEfi
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS = RELEASE
  SKUID_IDENTIFIER = DEFAULT
!include MdePkg/MdeLibs.dsc.inc
[BuildOptions]
  GCC:*_*_AARCH64_CC_FLAGS = -fno-jump-tables -ffixed-x18 -I"$(POLARIS_ABI_INCLUDE)" -I"$(POLARIS_LINUX_EFI_INCLUDE)" -I"$(POLARIS_WINDOWS_INCLUDE)"
[LibraryClasses]
  ArmLib|ArmPkg/Library/ArmLib/ArmBaseLib.inf
  CacheMaintenanceLib|ArmPkg/Library/ArmCacheMaintenanceLib/ArmCacheMaintenanceLib.inf
  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsic.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  DxeServicesLib|MdePkg/Library/DxeServicesLib/DxeServicesLib.inf
  UefiRuntimeLib|MdePkg/Library/UefiRuntimeLib/UefiRuntimeLib.inf
  HobLib|MdePkg/Library/DxeHobLib/DxeHobLib.inf
  FdtLib|EmbeddedPkg/Library/FdtLib/FdtLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  MemoryMapHelperLib|Silicon/Qualcomm/QcomPkg/Library/MemoryMapHelperLib/MemoryMapHelperLib.inf
  PlatformMemoryMapLib|Silicon/Qualcomm/sdm845/Library/PlatformMemoryMapLib/PlatformMemoryMapLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  NULL|ArmPkg/Library/CompilerIntrinsicsLib/CompilerIntrinsicsLib.inf
[Components]
  pocketjs4edk2_uefi/efi/ReturnProbe/ReturnProbe.inf
  pocketjs4edk2_uefi/efi/HangProbe/HangProbe.inf
  pocketjs4edk2_uefi/efi/EbsHangProbe/EbsHangProbe.inf
  pocketjs4edk2_uefi/efi/Recovery/Recovery.inf
  pocketjs4edk2_uefi/efi/Windows/Windows.inf
  pocketjs4edk2_uefi/efi/Android/Android.inf
  pocketjs4edk2_uefi/efi/Android/AndroidPrepare.inf
  pocketjs4edk2_uefi/efi/LinuxBoot/LinuxBoot.inf
  pocketjs4edk2_uefi/efi/LinuxBoot/LinuxInspect.inf
  pocketjs4edk2_uefi/efi/LinuxBoot/LinuxPrepare.inf
  pocketjs4edk2_uefi/efi/LinuxBoot/LinuxTrace.inf
  pocketjs4edk2_uefi/efi/LinuxBoot/LinuxNative.inf
  pocketjs4edk2_uefi/efi/LinuxBoot/LinuxNativePrepare.inf
  pocketjs4edk2_uefi/efi/LinuxBoot/LinuxNativeProbe.inf
  pocketjs4edk2_uefi/efi/LinuxBoot/LinuxNativePreExitProbe.inf
  pocketjs4edk2_uefi/efi/LinuxBoot/LinuxNativeExitProbe.inf
  pocketjs4edk2_uefi/efi/LinuxBoot/LinuxNativeKernelProbe.inf
