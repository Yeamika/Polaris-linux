// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included in the prepared SimpleInit source, before graphical initialization.
#include "linux_boot.h"
#include "confd.h"
#include "logger.h"
#include <Protocol/LoadedImage.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "BootRequest.h"
#include <comp_libfdt.h>
#include "../linux-boot/internal.h"
#include "NativeFdt.h"
#include "SimpleInitHandoff.h"
extern EFI_GUID gFdtTableGuid;
extern VOID polaris_linux_set_android_memory(CONST VOID *Dtb,UINT64 Limit);
static EFI_STATUS polaris_android_dispatch(EFI_HANDLE Image,BOOLEAN *Handled) {
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;POLARIS_ANDROID_REQUEST *R;
  linux_config *Cfg;linux_boot *Boot;EFI_STATUS Status,Restore;
  CONST fdt64_t *Memory;INT32 Node,Length;UINTN I;
  VOID *OriginalFdt=NULL;
  int Prepared;
  *Handled=FALSE;
  Status=gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if (EFI_ERROR(Status) || Loaded->LoadOptionsSize<32 || !Loaded->LoadOptions) return EFI_SUCCESS;
  R=Loaded->LoadOptions;
  if (R->Magic!=POLARIS_ANDROID_MAGIC) return EFI_SUCCESS;
  *Handled=TRUE;
  if (!PolarisAndroidRequestValid(R,Loaded->LoadOptionsSize)) return EFI_UNSUPPORTED;
  if (!PolarisAndroidImageValid((VOID *)(UINTN)R->Image,(UINTN)R->ImageSize) ||
      !PolarisAndroidDtbValid((VOID *)(UINTN)R->Dtb,(UINTN)R->DtbSize) ||
      !PolarisAndroidDtbSanitize((VOID *)(UINTN)R->Dtb)) return EFI_COMPROMISED_DATA;
  confd_init();logger_init();
  Cfg=linux_config_new();if (!Cfg) return EFI_OUT_OF_RESOURCES;
  Cfg->arch=ARCH_ARM64;Cfg->use_uefi=false;
  // The captured native tree already includes ABL board fixups and bootargs.
  Cfg->skip_kfdt_cmdline=true;Cfg->skip_abootimg_cmdline=true;
  AsciiStrCpyS(Cfg->cmdline,sizeof(Cfg->cmdline),"polaris.uefi_android=1");
  Cfg->skip_kfdt_memory=true;Cfg->skip_efi_memory_map=true;
  Cfg->screen.update_splash=false;Cfg->match_kfdt_model=false;
  Cfg->skip_dtb_after_kernel=true;
  Cfg->dtb.enabled=true;Cfg->dtb.type=FROM_POINTER;
  Cfg->dtb.pointer=(VOID *)(UINTN)R->Dtb;Cfg->dtb.size=(UINTN)R->DtbSize;
  Node=fdt_path_offset(Cfg->dtb.pointer,"/memory");
  Memory=fdt_getprop(Cfg->dtb.pointer,Node,"reg",&Length);
  for (I=0;I<(UINTN)Length/16;++I) {
    Cfg->memory[I].start=PolarisAndroidBe64(Memory+2*I);
    Cfg->memory[I].end=Cfg->memory[I].start+PolarisAndroidBe64(Memory+2*I+1);
  }
  Cfg->abootimg.enabled=true;Cfg->abootimg.type=FROM_POINTER;
  Cfg->abootimg.pointer=(VOID *)(UINTN)R->Image;Cfg->abootimg.size=(UINTN)R->ImageSize;
  Boot=linux_boot_new(Cfg);
  if (!Boot) { linux_config_free(Cfg);return EFI_OUT_OF_RESOURCES; }
  EfiGetSystemConfigurationTable(&gFdtTableGuid,&OriginalFdt);
  Print(L"Android native loader: preparing kernel, ramdisk and native MP v2.1 DTB\n");
  Status=EFI_LOAD_ERROR;
  polaris_linux_set_android_memory(Cfg->dtb.pointer,POLARIS_ANDROID_TRACE_ADDRESS);
  Prepared=linux_boot_prepare(Boot);
  polaris_linux_set_android_memory(NULL,0);
  if (Prepared==0) {
    Print(L"Android native loader: kernel, ramdisk and DTB prepared\n");
    Status=PolarisAndroidHandoff(Boot,Image,(R->Flags&POLARIS_ANDROID_PREPARE_ONLY)!=0);
  }
  // Preparing a kernel installs its DTB in the EFI configuration table. Restore
  // the original before releasing that allocation on a diagnostic return.
  Restore=gBS->InstallConfigurationTable(&gFdtTableGuid,OriginalFdt);
  if (EFI_ERROR(Restore) && Restore!=EFI_NOT_FOUND) return Restore;
  linux_boot_free(Boot);linux_config_free(Cfg);
  return Status;
}
