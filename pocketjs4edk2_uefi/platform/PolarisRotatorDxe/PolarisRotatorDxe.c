// SPDX-License-Identifier: BSD-2-Clause-Patent
// SDM845 r3 Rotator, using its private VBIF, RegDMA queue and stream 0x1090.
// Register definitions/completion rules: Qualcomm sde_rotator_r3{,_hwio}.
// Keep the inherited display controller and panel configuration untouched.
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/EFIClock.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include "pj4edk2_rotate.h"

#define WIDTH 2160U
#define HEIGHT 1080U
#define IMAGE_BYTES (WIDTH*HEIGHT*4U)
#define DATA_BYTES ALIGN_VALUE(IMAGE_BYTES,4096)
#define BUFFER_BYTES (DATA_BYTES+8192)
#define BUFFER_PAGES (BUFFER_BYTES/4096)
#define MDSS 0x0ae00000U
#define TOP 0x0aea8800U
#define SRC 0x0aea8900U
#define WB 0x0aea8b00U
#define VBIF 0x0aeb8000U
#define REGDMA 0x0aeaae00U
#define CMDRAM 0x0aea8e00U
#define SMMU 0x15000000U
#define SMR (SMMU+0x800)
#define S2CR (SMMU+0xc00)
#define QUEUE_ERRORS 0x000f0000U
#define QUEUE_CLEAR 0x000f0707U

typedef struct { UINTN Address;UINT32 Value,Old; } REGISTER;
STATIC REGISTER Registers[]={
  {VBIF+0x008,3,0},{VBIF+0x0ac,2,0},{VBIF+0x0b0,32,0},{VBIF+0x0c0,32<<8,0},{VBIF+0x160,0x33,0},
  {VBIF+0x550,0x33,0},{VBIF+0x558,0x33,0},{VBIF+0x560,0x33,0},{VBIF+0x568,0x33,0},
  {VBIF+0x570,0x33,0},{VBIF+0x578,0x33,0},{VBIF+0x580,0x33,0},{VBIF+0x588,0x33,0},
  {VBIF+0x590,0x33,0},{VBIF+0x598,0x33,0},{VBIF+0x5a0,0x33,0},{VBIF+0x5a8,0x33,0},
  {VBIF+0x5b0,0x33,0},{VBIF+0x5b8,0x33,0},{VBIF+0x5c0,0x33,0},{VBIF+0x5c8,0x33,0},
  {TOP+0x30,0,0},{TOP+0x40,0,0},{TOP+0x4c,0,0},
  {SRC+0x00,WIDTH|(HEIGHT<<16),0},{SRC+0x04,0,0},{SRC+0x08,0,0},
  {SRC+0x0c,WIDTH|(HEIGHT<<16),0},{SRC+0x10,0,0},
  {SRC+0x14,0,0},{SRC+0x18,0,0},{SRC+0x1c,0,0},{SRC+0x20,0,0},
  {SRC+0x24,WIDTH*4,0},{SRC+0x28,0,0},{SRC+0x2c,0,0},
  {SRC+0x30,0x00023fff,0},{SRC+0x34,0x03020001,0},{SRC+0x38,0x80000000,0},
  {SRC+0x44,0x80000020,0},{SRC+0x48,0x00080087,0},
  {SRC+0x60,0,0},{SRC+0x64,0xffff,0},{SRC+0x6c,0,0},
  {SRC+0x70,0,0},{SRC+0x74,0,0},{SRC+0x78,0,0},{SRC+0x134,9,0},
  {WB+0x00,0x000237ff,0},{WB+0x04,0,0},{WB+0x08,0x03020001,0},
  {WB+0x0c,0,0},{WB+0x10,0,0},{WB+0x14,0,0},{WB+0x18,0,0},
  {WB+0x1c,HEIGHT*4,0},{WB+0x20,0,0},{WB+0x40,0,0},{WB+0x48,0x200,0},
  {WB+0x74,HEIGHT|(WIDTH<<16),0},{WB+0x84,0,0},{WB+0x88,0xffff,0},
  {WB+0x90,0,0},{WB+0x94,0,0},{WB+0x98,0,0},{WB+0x9c,0,0},
  {WB+0x144,0x20,0},{WB+0x2b0,0,0},{WB+0x2b4,9,0},
  {WB+0x2c0,HEIGHT|(WIDTH<<16),0},{WB+0x2c4,0,0},{TOP+0x48,3,0}
};
STATIC EFI_CLOCK_PROTOCOL *Clock;
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
STATIC UINTN RotClock,BusClock,Slot=MAX_UINTN,Context=MAX_UINTN,ContextAddress;
STATIC UINT32 OldHz,OldSmr,OldRoute,OldCbar,OldCba2r,OldContext;
STATIC UINT32 OldRegMode,OldRegEnable,OldCommands[256];
STATIC BOOLEAN RotVote,BusVote,FrequencyChanged,RestoreFrequency,Mapped,ContextOwned,RegistersSaved;
STATIC BOOLEAN Active,Failed,Quarantined;
STATIC EFI_PHYSICAL_ADDRESS Input,Output;
STATIC UINTN CommandWords,Jobs;
STATIC UINT64 LastUs;
STATIC EFI_HANDLE ProviderHandle;
STATIC EFI_EVENT ExitEvent;

STATIC VOID SmmuWrite(UINTN Address,UINT32 Value) {
  // Firmware's SMMU trap does not emulate STR WZR; force a general register.
  __asm__ volatile("str %w0, [%1]"::"r"(Value),"r"(Address):"memory");
  MemoryFence();
}

STATIC BOOLEAN Halt(VOID) {
  UINTN I;UINT32 Old=MmioRead32(VBIF+0x200);
  MmioWrite32(VBIF+0x200,Old|3);MemoryFence();
  for(I=0;I<2000;++I) {
    if((MmioRead32(VBIF+0x204)&3)==3) break;
    gBS->Stall(10);
  }
  MmioWrite32(VBIF+0x200,Old);MemoryFence();
  return I<2000;
}

STATIC BOOLEAN Idle(VOID) {
  return !(MmioRead32(TOP+0x44)&1) && !(MmioRead32(REGDMA+0x5c)&1);
}

STATIC BOOLEAN Quiesce(VOID) {
  if(Idle() && Halt()) return TRUE;
  MmioWrite32(REGDMA+4,0);MmioWrite32(TOP+0x28,1);MemoryFence();
  gBS->Stall(10000);
  MmioWrite32(TOP+0x48,0);MmioWrite32(TOP+0x28,0);MemoryFence();
  gBS->Stall(10000);
  return Idle() && Halt();
}

STATIC BOOLEAN Guards(VOID) {
  UINTN I;
  for(I=0;I<4096;++I)
    if(((UINT8 *)(UINTN)Input)[I]!=0xa5 || ((UINT8 *)(UINTN)Input)[4096+DATA_BYTES+I]!=0xa5 ||
       ((UINT8 *)(UINTN)Output)[I]!=0xa5 || ((UINT8 *)(UINTN)Output)[4096+DATA_BYTES+I]!=0xa5)
      return FALSE;
  return TRUE;
}

STATIC EFI_STATUS Release(BOOLEAN FreeBuffers) {
  UINTN I;EFI_STATUS S=EFI_SUCCESS,E;UINT32 Hz,Cbar;
  if(RegistersSaved) {
    if(!Quiesce()) {Quarantined=TRUE;return EFI_TIMEOUT;}
    // Command RAM is only accessible while RegDMA is enabled.
    MmioWrite32(REGDMA+4,1);MemoryFence();
    MmioWrite32(REGDMA+0x58,QUEUE_CLEAR);
    for(I=0;I<256;++I) MmioWrite32(CMDRAM+I*4,OldCommands[I]);
    for(I=ARRAY_SIZE(Registers);I>0;--I) MmioWrite32(Registers[I-1].Address,Registers[I-1].Old);
    MmioWrite32(REGDMA+0x54,OldRegEnable);MmioWrite32(REGDMA+4,OldRegMode);MemoryFence();
    RegistersSaved=FALSE;
  }
  if(Mapped) {
    SmmuWrite(SMR+Slot*4,OldSmr);
    SmmuWrite(S2CR+Slot*4,OldRoute);
    // Some Qualcomm firmware ignores FAULT writes and turns BYPASS into
    // FAULT. Preserve CBNDX/PRIVCFG and every other saved bit in that encoding.
    if(MmioRead32(S2CR+Slot*4)!=OldRoute && (OldRoute&0x30000)==0x20000)
      SmmuWrite(S2CR+Slot*4,(OldRoute&~0x30000U)|0x10000);
    if(MmioRead32(SMR+Slot*4)!=OldSmr || MmioRead32(S2CR+Slot*4)!=OldRoute) {
      Quarantined=TRUE;return EFI_WRITE_PROTECTED;
    }
    Mapped=FALSE;
  }
  if(ContextOwned) {
    SmmuWrite(ContextAddress,OldContext);
    SmmuWrite(SMMU+0x1000+Context*4,OldCbar);
    SmmuWrite(SMMU+0x1800+Context*4,OldCba2r);
    Cbar=MmioRead32(SMMU+0x1000+Context*4);
    // On a cold boot this unused bank reads CBAR=0. Firmware retains the
    // S1_TRANS_S2_BYPASS type after writing zero back (verified on-device).
    // Accept only that exact normalization: the bank remains disabled and
    // unreferenced, with SCTLR/CBA2R and all stream routes restored.
    if(MmioRead32(ContextAddress)!=OldContext ||
       (Cbar!=OldCbar && !(OldCbar==0 && Cbar==0x10000)) ||
       MmioRead32(SMMU+0x1800+Context*4)!=OldCba2r) {
      Quarantined=TRUE;return EFI_WRITE_PROTECTED;
    }
    if(Cbar!=OldCbar)
      DEBUG((DEBUG_WARN,"POLARIS_ROTATOR inactive context retains firmware-required S1 type\n"));
    ContextOwned=FALSE;
  }
  Active=FALSE;Quarantined=FALSE;
  if(RotVote) {E=Clock->DisableClock(Clock,RotClock);if(EFI_ERROR(E)) S=E;else RotVote=FALSE;}
  if(BusVote) {E=Clock->DisableClock(Clock,BusClock);if(EFI_ERROR(E)) S=E;else BusVote=FALSE;}
  if(FrequencyChanged) {
    if(RestoreFrequency) {
      E=Clock->SetClockFreqHz(Clock,RotClock,OldHz,EFI_CLOCK_FREQUENCY_HZ_EXACT,&Hz);
      if(EFI_ERROR(E)) S=E;else FrequencyChanged=FALSE;
    } else FrequencyChanged=FALSE;
  }
  if(FreeBuffers) {
    if(Input) {gBS->FreePages(Input,BUFFER_PAGES);Input=0;}
    if(Output) {gBS->FreePages(Output,BUFFER_PAGES);Output=0;}
  }
  return S;
}

STATIC EFI_STATUS Enable(VOID) {
  EFI_STATUS S;BOOLEAN RotOn,BusOn,On;UINT32 Hz,Id1,N,V;UINTN I;
  EFI_PHYSICAL_ADDRESS Address;
  if(Quarantined || Failed) return EFI_UNSUPPORTED;
  if(Active) return EFI_SUCCESS;
  if(RotVote || BusVote || FrequencyChanged) return EFI_NOT_READY;
  S=gBS->LocateProtocol(&gEfiClockProtocolGuid,NULL,(VOID **)&Clock);
  if(EFI_ERROR(S) || !Clock || Clock->Version!=0x10006) return EFI_UNSUPPORTED;
  S=Clock->GetClockID(Clock,"disp_cc_mdss_rot_clk",&RotClock);if(EFI_ERROR(S)) return S;
  S=Clock->GetClockID(Clock,"gcc_disp_axi_clk",&BusClock);if(EFI_ERROR(S)) return S;
  S=Clock->IsClockEnabled(Clock,RotClock,&RotOn);if(EFI_ERROR(S)) return S;
  S=Clock->IsClockEnabled(Clock,BusClock,&BusOn);if(EFI_ERROR(S)) return S;
  OldHz=0;S=Clock->GetClockFreqHz(Clock,RotClock,&OldHz);
  RestoreFrequency=!EFI_ERROR(S) && OldHz!=0;
  if(RotOn && !RestoreFrequency) return EFI_UNSUPPORTED;
  S=Clock->SetClockFreqHz(Clock,RotClock,300000000,EFI_CLOCK_FREQUENCY_HZ_AT_MOST,&Hz);
  if(EFI_ERROR(S)) return S;
  FrequencyChanged=TRUE;
  if(!BusOn) {S=Clock->EnableClock(Clock,BusClock);if(EFI_ERROR(S)) goto Failure;BusVote=TRUE;}
  if(!RotOn) {S=Clock->EnableClock(Clock,RotClock);if(EFI_ERROR(S)) goto Failure;RotVote=TRUE;}
  S=Clock->IsClockOn(Clock,RotClock,&On);if(EFI_ERROR(S) || !On) {S=EFI_NOT_READY;goto Failure;}
  if(MmioRead32(TOP)!=0x10010000 || MmioRead32(VBIF)!=0x10000000 ||
     !Idle() || (MmioRead32(REGDMA+4)&1)) {S=EFI_UNSUPPORTED;goto Failure;}
  N=MmioRead32(SMMU+0x20)&255;Id1=MmioRead32(SMMU+0x24);Slot=MAX_UINTN;
  if(!N || N>128 || (Id1&0x80000000) || (Id1&255)!=45 || ((Id1>>28)&7)!=5) {
    S=EFI_UNSUPPORTED;goto Failure;
  }
  for(I=0;I<N;++I) {
    V=MmioRead32(SMR+I*4);
    // A native BYPASS entry may be impossible to restore through TZ's trap.
    if(!(V&0x80000000)) {
      if((MmioRead32(S2CR+I*4)&0x30000)!=0x10000) Slot=I;
    }
    else if((((V&0x7fff)^0x1090)&~((V>>16)&0x7fff))==0) {S=EFI_ACCESS_DENIED;goto Failure;}
  }
  if(Slot==MAX_UINTN) {S=EFI_OUT_OF_RESOURCES;goto Failure;}
  Context=44;ContextAddress=SMMU+0x40000+Context*4096;
  if(MmioRead32(ContextAddress)&1) {S=EFI_ACCESS_DENIED;goto Failure;}
  for(I=0;I<N;++I)
    if((MmioRead32(SMR+I*4)&0x80000000) && (MmioRead32(S2CR+I*4)&0x300ff)==Context) {
      S=EFI_ACCESS_DENIED;goto Failure;
    }
  Address=MAX_UINT32;S=gBS->AllocatePages(AllocateMaxAddress,EfiBootServicesData,BUFFER_PAGES,&Address);
  if(EFI_ERROR(S)) goto Failure;
  Input=Address;
  Address=MAX_UINT32;S=gBS->AllocatePages(AllocateMaxAddress,EfiBootServicesData,BUFFER_PAGES,&Address);
  if(EFI_ERROR(S)) goto Failure;
  Output=Address;
  SetMem((VOID *)(UINTN)Input,BUFFER_BYTES,0xa5);SetMem((VOID *)(UINTN)Output,BUFFER_BYTES,0xa5);
  OldSmr=MmioRead32(SMR+Slot*4);OldRoute=MmioRead32(S2CR+Slot*4);
  OldCbar=MmioRead32(SMMU+0x1000+Context*4);OldCba2r=MmioRead32(SMMU+0x1800+Context*4);
  OldContext=MmioRead32(ContextAddress);ContextOwned=TRUE;
  MmioWrite32(SMMU+0x1000+Context*4,0x10000);MmioWrite32(SMMU+0x1800+Context*4,1);
  SmmuWrite(ContextAddress,0);
  Mapped=TRUE;MmioWrite32(S2CR+Slot*4,(UINT32)Context);MmioWrite32(SMR+Slot*4,0x80001090);MemoryFence();
  if(MmioRead32(SMR+Slot*4)!=0x80001090 || MmioRead32(S2CR+Slot*4)!=Context ||
     MmioRead32(ContextAddress)!=0) {S=EFI_WRITE_PROTECTED;goto Failure;}
  for(I=0;I<ARRAY_SIZE(Registers);++I) Registers[I].Old=MmioRead32(Registers[I].Address);
  OldRegMode=MmioRead32(REGDMA+4);OldRegEnable=MmioRead32(REGDMA+0x54);
  MmioWrite32(REGDMA+4,1);MemoryFence();
  for(I=0;I<256;++I) OldCommands[I]=MmioRead32(CMDRAM+I*4);
  RegistersSaved=TRUE;CommandWords=0;
  for(I=0;I<ARRAY_SIZE(Registers);++I) {
    REGISTER *R=&Registers[I];V=R->Value;
    if(R->Address==SRC+0x14) V=(UINT32)(Input+4096);
    if(R->Address==WB+0x0c) V=(UINT32)(Output+4096);
    if(R->Address==VBIF+0x008 || R->Address==VBIF+0x0ac) V|=R->Old;
    if(R->Address==VBIF+0x0b0) V|=R->Old&~0xffU;
    if(R->Address==VBIF+0x0c0) V|=R->Old&~0xff00U;
    if(R->Address==VBIF+0x160 || (R->Address>=VBIF+0x550 && R->Address<=VBIF+0x5c8)) V|=R->Old&~0x77U;
    if(R->Address>=VBIF || R->Address==TOP+0x30) MmioWrite32(R->Address,V);
    else {
      MmioWrite32(CMDRAM+CommandWords++*4,0x10000000U|(UINT32)(R->Address-MDSS));
      MmioWrite32(CMDRAM+CommandWords++*4,V);
    }
  }
  MmioWrite32(CMDRAM+CommandWords++*4,0x100a8840U);MmioWrite32(CMDRAM+CommandWords++*4,1);
  if(CommandWords>256 || !Halt()) {S=EFI_DEVICE_ERROR;goto Failure;}
  Active=TRUE;
  DEBUG((DEBUG_WARN,"POLARIS_ROTATOR ready Hz=%u stream=1090 context=%u\n",Hz,(UINT32)Context));
  return EFI_SUCCESS;
Failure:
  DEBUG((DEBUG_WARN,"POLARIS_ROTATOR unavailable status=%r\n",S));
  if(EFI_ERROR(Release(TRUE))) Quarantined=TRUE;
  Failed=TRUE;return S;
}

STATIC EFI_STATUS EFIAPI Rotate(PJ4EDK2_ROTATE_PROTOCOL *This,CONST PJ4EDK2_ROTATE_REQUEST *R) {
  EFI_STATUS S;EFI_TPL Tpl;UINT64 Started,JobStart;UINT32 Status,Irq;UINTN Row,X,Y,W,H;
  if(!R || !R->source || !R->destination || R->width!=WIDTH || R->height!=HEIGHT ||
     R->source_stride!=WIDTH*4 || R->source_bytes<IMAGE_BYTES ||
     R->destination_stride!=HEIGHT*4 || R->destination_bytes<IMAGE_BYTES ||
     R->x>WIDTH || R->y>HEIGHT || !R->damage_width || !R->damage_height ||
     R->damage_width>WIDTH-R->x || R->damage_height>HEIGHT-R->y)
    return EFI_INVALID_PARAMETER;
  if(!Gop || !Gop->Mode || !Gop->Mode->Info ||
     Gop->Mode->Info->HorizontalResolution!=HEIGHT || Gop->Mode->Info->VerticalResolution!=WIDTH ||
     Gop->Mode->Info->PixelFormat!=PixelBlueGreenRedReserved8BitPerColor ||
     Gop->Mode->Info->PixelsPerScanLine!=HEIGHT ||
     Gop->Mode->FrameBufferBase!=(UINTN)R->destination || Gop->Mode->FrameBufferSize<R->destination_bytes)
    return EFI_UNSUPPORTED;
  // A full DMA job is slower than a small software damage update.
  if((UINT64)R->damage_width*R->damage_height<600000) return EFI_NOT_READY;
  Started=GetPerformanceCounter();
  S=Enable();if(EFI_ERROR(S)) return S;
  CopyMem((VOID *)(UINTN)(Input+4096),R->source,IMAGE_BYTES);
  WriteBackDataCacheRange((VOID *)(UINTN)Input,BUFFER_BYTES);
  WriteBackInvalidateDataCacheRange((VOID *)(UINTN)Output,BUFFER_BYTES);
  Tpl=gBS->RaiseTPL(TPL_NOTIFY);
  MmioWrite32(TOP+0x38,1);MmioWrite32(REGDMA+0x54,0);
  MmioWrite32(REGDMA+0x58,QUEUE_CLEAR);MemoryFence();
  JobStart=GetPerformanceCounter();
  MmioWrite32(REGDMA+0x10,0x40000000U|((UINT32)CommandWords<<14));
  do {
    Irq=MmioRead32(REGDMA+0x50);Status=MmioRead32(TOP+0x44);
    if((Irq&(QUEUE_ERRORS|1)) && !(Status&1)) break;
    gBS->Stall(10);
  } while(GetTimeInNanoSecond(GetPerformanceCounter()-JobStart)<60000000);
  LastUs=GetTimeInNanoSecond(GetPerformanceCounter()-JobStart)/1000;
  // The RegDMA branch of the native driver uses queue IRQ/errors, not the
  // legacy ROT_ERROR_BIT (TOP 0x100). Retain it in diagnostics, require idle.
  S=(Irq&1) && !(Irq&QUEUE_ERRORS) && Idle() && Halt()?EFI_SUCCESS:EFI_DEVICE_ERROR;
  gBS->RestoreTPL(Tpl);
  InvalidateDataCacheRange((VOID *)(UINTN)Output,BUFFER_BYTES);
  if(!Guards()) S=EFI_COMPROMISED_DATA;
  if(EFI_ERROR(S)) {
    Failed=TRUE;
    DEBUG((DEBUG_WARN,"POLARIS_ROTATOR failed irq=%08x top=%08x us=%Lu\n",Irq,Status,LastUs));
    if(!Quiesce()) Quarantined=TRUE;
    return S; // DMA only accessed owned buffers; no destination writes yet.
  }
  X=HEIGHT-R->y-R->damage_height;Y=R->x;W=R->damage_height;H=R->damage_width;
  for(Row=0;Row<H;++Row)
    CopyMem((UINT8 *)R->destination+(Y+Row)*R->destination_stride+X*4,
            (UINT8 *)(UINTN)(Output+4096)+((Y+Row)*HEIGHT+X)*4,W*4);
  ++Jobs;
  if(Jobs<=3 || Jobs%120==0)
    DEBUG((DEBUG_WARN,"POLARIS_ROTATOR frame=%u dma_us=%Lu total_us=%Lu top=%08x\n",
      (UINT32)Jobs,LastUs,GetTimeInNanoSecond(GetPerformanceCounter()-Started)/1000,Status));
  return EFI_SUCCESS; // GOP BLT performs the ordinary scanout cache maintenance.
}

STATIC EFI_STATUS EFIAPI Stop(PJ4EDK2_ROTATE_PROTOCOL *This) {
  EFI_STATUS S=Release(TRUE);
  if(!EFI_ERROR(S)) Failed=FALSE;
  DEBUG((DEBUG_WARN,"POLARIS_ROTATOR stop=%r jobs=%u\n",S,(UINT32)Jobs));
  return S;
}

STATIC PJ4EDK2_ROTATE_PROTOCOL Protocol={PJ4EDK2_ROTATE_REVISION,sizeof(Protocol),Rotate,Stop};

STATIC VOID EFIAPI ExitBootServices(EFI_EVENT Event,VOID *ContextArg) {
  // Do not alter the memory map in an ExitBootServices notification.
  if(EFI_ERROR(Release(FALSE))) {
    gRT->ResetSystem(EfiResetCold,EFI_DEVICE_ERROR,0,NULL);
    CpuDeadLoop();
  }
}

STATIC EFI_STATUS EFIAPI UnloadPolarisRotator(EFI_HANDLE Image) {
  EFI_GUID Guid=PJ4EDK2_ROTATE_GUID;EFI_STATUS S;
  S=Stop(&Protocol);if(EFI_ERROR(S)) return S;
  S=gBS->UninstallProtocolInterface(ProviderHandle,&Guid,&Protocol);
  if(EFI_ERROR(S)) return S;
  ProviderHandle=NULL;
  if(ExitEvent) {gBS->CloseEvent(ExitEvent);ExitEvent=NULL;}
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI InitializePolarisRotator(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
  EFI_GUID Guid=PJ4EDK2_ROTATE_GUID;EFI_STATUS S;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;VOID *Existing;
  if(!EFI_ERROR(gBS->LocateProtocol(&Guid,NULL,&Existing))) return EFI_ALREADY_STARTED;
  S=gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if(EFI_ERROR(S)) return S;
  S=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if(EFI_ERROR(S)) return S;
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitBootServices,NULL,&gEfiEventExitBootServicesGuid,&ExitEvent);
  if(EFI_ERROR(S)) return S;
  S=gBS->InstallProtocolInterface(&ProviderHandle,&Guid,EFI_NATIVE_INTERFACE,&Protocol);
  if(EFI_ERROR(S)) {gBS->CloseEvent(ExitEvent);ExitEvent=NULL;}
  else Loaded->Unload=UnloadPolarisRotator;
  return S;
}
