// SPDX-License-Identifier: BSD-2-Clause-Patent
// Minimal ABI observed in the pinned SDM845 UsbConfigDxe shipped artifact.
// SHA256: 0169293e228bc6a98c5a42613cb222d3afface97e0fda0cc838b8811388ea498
// Revision 0x10003: StartController at 0x50 forwards (Core, Mode) to the
// implementation; core 0, mode 4 installs/connects the peripheral handle.
// Unknown revisions are deliberately rejected rather than guessed.
#ifndef POLARIS_QCOM_USB_CONFIG_H
#define POLARIS_QCOM_USB_CONFIG_H
typedef struct POLARIS_USB_CONFIG POLARIS_USB_CONFIG;
struct POLARIS_USB_CONFIG {
  UINT64 Revision;
  VOID *Reserved[9];
  EFI_STATUS (EFIAPI *StartController)(POLARIS_USB_CONFIG *This,UINT32 Core,UINT32 Mode);
};
STATIC_ASSERT(OFFSET_OF(POLARIS_USB_CONFIG,StartController)==0x50,"Qualcomm USB config ABI");
STATIC EFI_GUID PolarisUsbConfigGuid={0xe722b03f,0xb250,0x42ce,{0x8e,0xbd,0x5b,0xd5,0x18,0x12,0xd0,0x37}};
STATIC EFI_STATUS PolarisConnectUsbDevice(VOID) {
  POLARIS_USB_CONFIG *Config=NULL;
  EFI_STATUS Status=gBS->LocateProtocol(&PolarisUsbConfigGuid,NULL,(VOID **)&Config);
  if (EFI_ERROR(Status)) return Status;
  if (!Config || Config->Revision!=0x10003 || !Config->StartController) return EFI_UNSUPPORTED;
  return Config->StartController(Config,0,4);
}
#endif
