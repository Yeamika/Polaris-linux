// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Protocol/UsbFunctionIo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiLib.h>
#include "FastbootCore.h"
#include "QcomUsbConfig.h"
#include "pj4edk2_rotate.h"
#ifdef POLARIS_PLATFORM_SERVICES
#include "PlatformServices.h"
#endif

#define RX_SIZE 65536U
#define CONTROL_SIZE 512U
#define COMMAND_TIMEOUT 30U

typedef struct {
  EFI_USB_CONFIG_DESCRIPTOR Config;
  EFI_USB_INTERFACE_DESCRIPTOR Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR In, Out;
} USB_CONFIG;
STATIC_ASSERT(sizeof(USB_CONFIG)==32, "USB descriptors must be packed");
STATIC EFI_USB_DEVICE_DESCRIPTOR Device = {
  18, 1, 0x0200, 0, 0, 0, 64, 0x18d1, 0xd00d, 0x0100, 1, 2, 3, 1
};
STATIC USB_CONFIG Config = {
  {9, 2, 32, 1, 1, 0, 0x80, 250},
  {9, 4, 0, 0, 2, 0xff, 0x42, 3, 0},
  {7, 5, 0x81, 2, 512, 0}, {7, 5, 0x01, 2, 512, 0}
};
STATIC EFI_USB_ENDPOINT_DESCRIPTOR *Endpoints[] = {&Config.In, &Config.Out};
STATIC EFI_USB_INTERFACE_INFO InterfaceInfo = {&Config.Interface, Endpoints};
STATIC EFI_USB_INTERFACE_INFO *Interfaces[] = {&InterfaceInfo};
STATIC EFI_USB_CONFIG_INFO ConfigInfo = {&Config.Config, Interfaces};
STATIC EFI_USB_CONFIG_INFO *Configurations[] = {&ConfigInfo};
STATIC EFI_USB_DEVICE_INFO DeviceInfo = {&Device, Configurations};
STATIC EFI_USBFN_IO_PROTOCOL *Usb;
STATIC VOID *Rx, *Tx, *Control;
STATIC FB_CORE Core;
STATIC BOOLEAN Configured, RxPending, TxPending, ControlIn, ControlStatus;
STATIC BOOLEAN EnableAfterStatus;
STATIC UINTN RxRequested, TxSize, IdleTicks;
STATIC EFI_HANDLE Parent, Child;
STATIC EFI_STATUS LoopStatus;
STATIC BOOLEAN Done;
STATIC BOOLEAN ControllerRunning;
STATIC CONST CHAR16 *RecoveryStage;
STATIC UINTN UsbEvents,SetupPackets,BusResets;
STATIC EFI_USBFN_MESSAGE LastUsbMessage;
STATIC EFI_STATUS LastUsbError;
#ifdef POLARIS_FASTBOOT_DXE
STATIC EFI_STATUS ResumeAfterChild(VOID);
#endif

STATIC EFI_STATUS StopUsb(VOID) {
  EFI_STATUS Status=Usb->StopController(Usb);
  if (!EFI_ERROR(Status)) ControllerRunning=FALSE;
  return Status;
}

STATIC EFI_STATUS StopDisplayRotator(VOID) {
  EFI_GUID Guid=PJ4EDK2_ROTATE_GUID;
  PJ4EDK2_ROTATE_PROTOCOL *Rotator=NULL;
  EFI_STATUS Status=gBS->LocateProtocol(&Guid,NULL,(VOID **)&Rotator);
  if(Status==EFI_NOT_FOUND) return EFI_SUCCESS;
  if(EFI_ERROR(Status)) return Status;
  if(!Rotator || Rotator->revision!=PJ4EDK2_ROTATE_REVISION ||
     Rotator->size<sizeof(*Rotator) || !Rotator->stop) return EFI_UNSUPPORTED;
  return Rotator->stop(Rotator);
}

STATIC VOID Invalidate(VOID) {
#ifdef POLARIS_PLATFORM_SERVICES
  PolarisCancel();
#endif
  FbReset(&Core);
  IdleTicks=0;
  if (Child!=NULL) { gBS->UnloadImage(Child); Child=NULL; }
}
STATIC EFI_STATUS Submit(UINT8 Endpoint, EFI_USBFN_ENDPOINT_DIRECTION Direction,
                         VOID *Buffer, UINTN Size) {
  UINTN Accepted=Size;
  EFI_STATUS Status=Usb->Transfer(Usb, Endpoint, Direction, &Accepted, Buffer);
  if (!EFI_ERROR(Status) && Accepted!=Size) {
    Usb->AbortTransfer(Usb, Endpoint, Direction);
    return EFI_BAD_BUFFER_SIZE;
  }
  return Status;
}
STATIC EFI_STATUS ArmRx(VOID) {
  EFI_STATUS Status;
  if (!Configured || RxPending || TxPending) return EFI_SUCCESS;
#ifdef POLARIS_PLATFORM_SERVICES
  if (PolarisBusy()) return EFI_SUCCESS;
#endif
  RxRequested=Core.State==FB_DOWNLOAD?Core.Expected-Core.Received:FB_COMMAND;
  if (RxRequested>RX_SIZE) RxRequested=RX_SIZE;
  Status=Submit(1,EfiUsbEndpointDirectionDeviceRx,Rx,RxRequested);
  if (!EFI_ERROR(Status)) RxPending=TRUE;
  return Status;
}
STATIC VOID AbortBulk(VOID) {
  Configured=FALSE;
  Usb->AbortTransfer(Usb,1,EfiUsbEndpointDirectionDeviceRx);
  Usb->AbortTransfer(Usb,1,EfiUsbEndpointDirectionDeviceTx);
  RxPending=TxPending=FALSE;
  Invalidate();
}
STATIC EFI_STATUS Reply(VOID) {
  EFI_STATUS Status;
  if (!Core.ReplySize) return ArmRx();
  if (Core.Action==FB_BOOT) {
    Status=gBS->LoadImage(FALSE,Parent,NULL,Core.Data,Core.Valid,&Child);
    if (EFI_ERROR(Status)) {
      CONST CHAR8 Failure[]="FAILUEFI rejected downloaded image";
      CopyMem(Core.Reply,Failure,sizeof(Failure));
      Core.ReplySize=sizeof(Failure)-1; Core.Action=0; Child=NULL;
    }
  }
  TxSize=Core.ReplySize;
  CopyMem(Tx,Core.Reply,TxSize);
  Status=Submit(1,EfiUsbEndpointDirectionDeviceTx,Tx,TxSize);
  if (!EFI_ERROR(Status)) TxPending=TRUE;
  return Status;
}
STATIC EFI_STATUS StallControl(VOID) {
  BOOLEAN Stall=TRUE;
  return Usb->SetEndpointStallState(Usb,0,EfiUsbEndpointDirectionDeviceTx,&Stall);
}
STATIC EFI_STATUS ControlSend(CONST VOID *Data, UINTN Size, UINTN Requested) {
  if (Size>Requested) Size=Requested;
  if (Size>CONTROL_SIZE) return EFI_BAD_BUFFER_SIZE;
  if (Size) CopyMem(Control,Data,Size);
  ControlIn=TRUE; ControlStatus=FALSE;
  return Submit(0,EfiUsbEndpointDirectionDeviceTx,Control,Size);
}
STATIC EFI_STATUS ControlAck(VOID) {
  ControlIn=FALSE; ControlStatus=TRUE;
  return Submit(0,EfiUsbEndpointDirectionDeviceTx,Control,0);
}
STATIC EFI_STATUS StringDescriptor(UINT8 Index, UINT16 Language, UINT16 Requested) {
  CONST CHAR16 *Text;
  UINT8 Descriptor[128];
  UINTN I, N;
  if (Index==0) { CONST UINT8 Languages[]={4,3,9,4}; return ControlSend(Languages,4,Requested); }
  if (Language!=0x409) return StallControl();
  // Stable board identity; unique hardware serial retrieval is not yet wired.
  if (Index==1) Text=L"Polaris";
  else if (Index==2) Text=L"Polaris UEFI Fastboot";
  else if (Index==3) Text=L"polaris-uefi";
  else return StallControl();
  N=StrLen(Text); Descriptor[0]=(UINT8)(2+N*2); Descriptor[1]=3;
  for (I=0; I<N; ++I) { Descriptor[2+I*2]=(UINT8)Text[I]; Descriptor[3+I*2]=(UINT8)(Text[I]>>8); }
  return ControlSend(Descriptor,Descriptor[0],Requested);
}
STATIC EFI_STATUS Setup(EFI_USB_DEVICE_REQUEST *R) {
  UINT8 Type=(UINT8)(R->Value>>8), Index=(UINT8)R->Value;
  UINT8 Value;
  UINT16 StatusValue=0;
  BOOLEAN Stall;
  Usb->AbortTransfer(Usb,0,EfiUsbEndpointDirectionDeviceRx);
  Usb->AbortTransfer(Usb,0,EfiUsbEndpointDirectionDeviceTx);
  ControlIn=ControlStatus=EnableAfterStatus=FALSE;
  if (R->RequestType==0x80 && R->Request==6) {
    if (Type==1 && Index==0 && R->Index==0) return ControlSend(&Device,sizeof(Device),R->Length);
    if (Type==2 && Index==0 && R->Index==0) return ControlSend(&Config,sizeof(Config),R->Length);
    if (Type==3) return StringDescriptor(Index,R->Index,R->Length);
    if (Type==6 && Index==0 && R->Index==0) {
      CONST UINT8 Qualifier[]={10,6,0,2,0,0,0,64,1,0};
      return ControlSend(Qualifier,sizeof(Qualifier),R->Length);
    }
    if (Type==7 && Index==0 && R->Index==0) {
      USB_CONFIG Other=Config;
      Other.Config.DescriptorType=7;
      Other.In.MaxPacketSize=Other.Out.MaxPacketSize=Config.In.MaxPacketSize==512?64:512;
      return ControlSend(&Other,sizeof(Other),R->Length);
    }
  }
  if (R->RequestType==0 && R->Request==5 && R->Value<=127 && !R->Index && !R->Length)
    return ControlAck(); // Address programming belongs to the USBFn controller.
  if (R->RequestType==0 && R->Request==9 && R->Value<=1 && !R->Index && !R->Length) {
    AbortBulk(); EnableAfterStatus=R->Value==1; return ControlAck();
  }
  if (R->RequestType==0x80 && R->Request==8 && !R->Value && !R->Index && R->Length==1) {
    Value=Configured?1:0; return ControlSend(&Value,1,1);
  }
  if (R->Request==0 && R->Value==0 && R->Length==2) {
    if (R->RequestType==0x80 && R->Index==0) return ControlSend(&StatusValue,2,2);
    if (R->RequestType==0x82 && (R->Index==0 || R->Index==0x80)) return ControlSend(&StatusValue,2,2);
    if (R->RequestType==0x81 && R->Index==0 && Configured) return ControlSend(&StatusValue,2,2);
    if (R->RequestType==0x82 && (R->Index==0x81 || R->Index==1) && Configured) {
      EFI_STATUS S=Usb->GetEndpointStallState(Usb,1,(R->Index&0x80)?EfiUsbEndpointDirectionDeviceTx:EfiUsbEndpointDirectionDeviceRx,&Stall);
      if (EFI_ERROR(S)) return S;
      StatusValue=Stall?1:0; return ControlSend(&StatusValue,2,2);
    }
  }
  if (R->RequestType==2 && (R->Request==1 || R->Request==3) && !R->Value &&
      !R->Length && Configured && (R->Index==1 || R->Index==0x81)) {
    EFI_STATUS S;
    Stall=R->Request==3;
    S=Usb->SetEndpointStallState(Usb,1,(R->Index&0x80)?EfiUsbEndpointDirectionDeviceTx:EfiUsbEndpointDirectionDeviceRx,&Stall);
    if (EFI_ERROR(S)) return S;
    return ControlAck();
  }
  if (R->RequestType==0x81 && R->Request==10 && !R->Value && !R->Index && R->Length==1 && Configured) {
    Value=0; return ControlSend(&Value,1,1);
  }
  if (R->RequestType==1 && R->Request==11 && !R->Value && !R->Index && !R->Length && Configured)
    return ControlAck();
  return StallControl();
}
STATIC EFI_STATUS StartUsb(VOID) {
  EFI_STATUS Status;
  Configured=RxPending=TxPending=ControlIn=ControlStatus=EnableAfterStatus=FALSE;
  RecoveryStage=L"USBFn StartController";
  Status=Usb->StartController(Usb);
  if (EFI_ERROR(Status)) return Status;
  ControllerRunning=TRUE;
  RecoveryStage=L"USBFn ConfigureEnableEndpoints";
  Status=Usb->ConfigureEnableEndpoints(Usb,&DeviceInfo);
  if (EFI_ERROR(Status)) StopUsb();
  return Status;
}
STATIC EFI_STATUS Action(VOID) {
  EFI_STATUS Status;
  UINT32 Pending=Core.Action;
#ifdef POLARIS_PLATFORM_SERVICES
  if (Pending==4) { PolarisNext(&Core); return Reply(); }
#endif
  if (!Pending) return ArmRx();
  Core.Action=0;
  if (Pending==FB_REBOOT || Pending==FB_BOOTLOADER) {
    struct { CHAR16 Parameter[12];UINT8 Reason; } Reset={{L'R',L'E',L'S',L'E',L'T',L'_',L'P',L'A',L'R',L'A',L'M',0},2};
    Status=StopUsb();if (EFI_ERROR(Status)) return Status;
#ifdef POLARIS_PLATFORM_SERVICES
    PolarisLog("host requested reset after reply completion");PolarisWatchdog(1);
#endif
    gRT->ResetSystem(EfiResetCold,EFI_SUCCESS,Pending==FB_BOOTLOADER?sizeof(Reset):0,
                     Pending==FB_BOOTLOADER?&Reset:NULL);
    return EFI_DEVICE_ERROR;
  }
  if (Pending==FB_EXIT) { Done=TRUE; return EFI_SUCCESS; }
  if (Pending!=FB_BOOT || Child==NULL) return EFI_PROTOCOL_ERROR;
  // RAM EFI execution bypasses the menu's ordinary chainloader. It needs the
  // same DMA/SMMU handoff before the child can replace those resources.
  Status=StopDisplayRotator();
  if (EFI_ERROR(Status)) { Invalidate();return Status; }
  Status=StopUsb();
  if (EFI_ERROR(Status)) return Status;
  Configured=RxPending=TxPending=FALSE;
#ifdef POLARIS_PLATFORM_SERVICES
  PolarisLog("EFI RAM image entered; 30 second watchdog");
  PolarisWatchdog(30);
#endif
  Print(L"Starting downloaded EFI image from RAM\n");
  { EFI_HANDLE Running=Child;Child=NULL;
    Status=gBS->StartImage(Running,NULL,NULL);
  }
#ifdef POLARIS_PLATFORM_SERVICES
  PolarisLog("EFI RAM image returned; USB restart");
  PolarisWatchdog(30);
#endif
  Print(L"EFI image returned: %r; restarting recovery USB\n",Status);
  Invalidate();
#ifdef POLARIS_FASTBOOT_DXE
  return ResumeAfterChild();
#else
  return StartUsb();
#endif
}
STATIC EFI_STATUS Event(EFI_USBFN_MESSAGE Message, EFI_USBFN_MESSAGE_PAYLOAD *P) {
  EFI_USBFN_TRANSFER_RESULT *T=&P->utr;
  EFI_STATUS Status;
  UINTN Size;
  if (Message!=EfiUsbMsgNone) { ++UsbEvents;LastUsbMessage=Message; }
  if (Message==EfiUsbMsgSetupPacket) ++SetupPackets;
  if (Message==EfiUsbMsgBusEventReset) ++BusResets;
  switch (Message) {
  case EfiUsbMsgSetupPacket: return Setup(&P->udr);
  case EfiUsbMsgBusEventDetach:
  case EfiUsbMsgBusEventReset:
#ifdef POLARIS_PLATFORM_SERVICES
    PolarisLog("USB reset/detach; download invalidated");
#endif
    AbortBulk(); ControlIn=ControlStatus=EnableAfterStatus=FALSE; return EFI_SUCCESS;
  case EfiUsbMsgBusEventSpeed:
    if (P->ubs!=UsbBusSpeedFull && P->ubs!=UsbBusSpeedHigh) return EFI_UNSUPPORTED;
    Device.MaxPacketSize0=64;
    Config.In.MaxPacketSize=Config.Out.MaxPacketSize=P->ubs==UsbBusSpeedHigh?512:64;
    return EFI_SUCCESS;
  case EfiUsbMsgEndpointStatusChangedRx:
  case EfiUsbMsgEndpointStatusChangedTx:
    if (T->TransferStatus==UsbTransferStatusActive) return EFI_SUCCESS;
    if (T->EndpointIndex==0) {
      if (T->Buffer!=Control || T->TransferStatus!=UsbTransferStatusComplete) return EFI_SUCCESS;
      if (Message==EfiUsbMsgEndpointStatusChangedTx && ControlIn) {
        ControlIn=FALSE; ControlStatus=TRUE;
        return Submit(0,EfiUsbEndpointDirectionDeviceRx,Control,0);
      }
      if (Message==EfiUsbMsgEndpointStatusChangedTx && ControlStatus && EnableAfterStatus) {
        EnableAfterStatus=ControlStatus=FALSE; Configured=TRUE; return ArmRx();
      }
      ControlStatus=FALSE; return EFI_SUCCESS;
    }
    if (T->EndpointIndex!=1 || !Configured) return EFI_SUCCESS;
    if (Message==EfiUsbMsgEndpointStatusChangedRx && T->Buffer==Rx && RxPending) {
      RxPending=FALSE;
      if (T->TransferStatus!=UsbTransferStatusComplete || T->BytesTransferred>RxRequested) return EFI_DEVICE_ERROR;
      IdleTicks=0;
#ifdef POLARIS_PLATFORM_SERVICES
      {
        BOOLEAN Command=Core.State==FB_IDLE;
        FbReceive(&Core,Rx,(UINT32)T->BytesTransferred);
        if (Command && (AsciiStrCmp(Core.Reply,"FAILunsupported command")==0 ||
                        AsciiStrCmp(Core.Reply,"FAILpartition writes not implemented")==0))
          PolarisCommand(&Core,Rx,T->BytesTransferred);
      }
#else
      FbReceive(&Core,Rx,(UINT32)T->BytesTransferred);
#endif
      return Reply();
    }
    if (Message==EfiUsbMsgEndpointStatusChangedTx && T->Buffer==Tx && TxPending) {
      TxPending=FALSE;
      if (T->TransferStatus!=UsbTransferStatusComplete || !T->BytesTransferred || T->BytesTransferred>TxSize) return EFI_DEVICE_ERROR;
      if (T->BytesTransferred<TxSize) {
        Size=TxSize-T->BytesTransferred;
        CopyMem(Tx,(UINT8 *)Tx+T->BytesTransferred,Size); TxSize=Size;
        Status=Submit(1,EfiUsbEndpointDirectionDeviceTx,Tx,TxSize);
        if (!EFI_ERROR(Status)) TxPending=TRUE;
        return Status;
      }
      IdleTicks=0; return Action();
    }
    return EFI_SUCCESS;
  default: return EFI_SUCCESS;
  }
}
STATIC EFI_EVENT ServiceTimer;
STATIC UINT8 *Download;
STATIC EFI_STATUS ServiceQuiesce(VOID) {
  EFI_STATUS Status=EFI_SUCCESS;
  if (ControllerRunning) {
    AbortBulk();Status=StopUsb();
    // A failed stop retains ownership of every DMA buffer and descriptor.
    if (EFI_ERROR(Status)) return Status;
  }
  return Status;
}
STATIC EFI_STATUS ServiceStop(VOID) {
  EFI_STATUS Status=ServiceQuiesce();
  if (EFI_ERROR(Status)) return Status;
#ifdef POLARIS_PLATFORM_SERVICES
  PolarisCancel();PolarisWatchdog(0);
#endif
  if (ServiceTimer) { gBS->CloseEvent(ServiceTimer);ServiceTimer=NULL; }
  if (Control) { Usb->FreeTransferBuffer(Usb,Control);Control=NULL; }
  if (Tx) { Usb->FreeTransferBuffer(Usb,Tx);Tx=NULL; }
  if (Rx) { Usb->FreeTransferBuffer(Usb,Rx);Rx=NULL; }
  if (Child) { gBS->UnloadImage(Child);Child=NULL; }
  if (Download) { FreePool(Download);Download=NULL; }
  return Status;
}
STATIC EFI_STATUS ServiceStart(EFI_HANDLE Image) {
  EFI_STATUS Status;UINTN MaxTransfer;
  if (ControllerRunning) return EFI_SUCCESS;
  Parent=Image;Child=NULL;Done=FALSE;LoopStatus=EFI_SUCCESS;
  UsbEvents=SetupPackets=BusResets=0;LastUsbMessage=EfiUsbMsgNone;LastUsbError=EFI_SUCCESS;
  RecoveryStage=L"Locate USBFn protocol";
  Status=gBS->LocateProtocol(&gEfiUsbFunctionIoProtocolGuid,NULL,(VOID **)&Usb);
  if (Status==EFI_NOT_FOUND) {
    RecoveryStage=L"Qualcomm USBConfig device mode (core 0, mode 4)";
    Status=PolarisConnectUsbDevice();
    if (EFI_ERROR(Status)) return Status;
    RecoveryStage=L"Locate USBFn after device-mode connect";
    Status=gBS->LocateProtocol(&gEfiUsbFunctionIoProtocolGuid,NULL,(VOID **)&Usb);
  }
  if (EFI_ERROR(Status)) return Status;
  RecoveryStage=L"Validate USBFn revision";
  if (Usb->Revision<EFI_USBFN_IO_PROTOCOL_REVISION) return EFI_UNSUPPORTED;
  RecoveryStage=L"Allocate 64 MiB download buffer";
  Download=AllocatePool(FB_LIMIT);
  if (!Download) return EFI_OUT_OF_RESOURCES;
  FbInit(&Core,Download,FB_LIMIT);
  RecoveryStage=L"Allocate RX DMA buffer";
  Status=Usb->AllocateTransferBuffer(Usb,RX_SIZE,&Rx);if (EFI_ERROR(Status)) goto Fail;
  RecoveryStage=L"Allocate TX DMA buffer";
  Status=Usb->AllocateTransferBuffer(Usb,RX_SIZE,&Tx);if (EFI_ERROR(Status)) goto Fail;
  RecoveryStage=L"Allocate control DMA buffer";
  Status=Usb->AllocateTransferBuffer(Usb,CONTROL_SIZE,&Control);if (EFI_ERROR(Status)) goto Fail;
  RecoveryStage=L"Create service timer";
  Status=gBS->CreateEvent(EVT_TIMER,0,NULL,NULL,&ServiceTimer);if (EFI_ERROR(Status)) goto Fail;
  Status=gBS->SetTimer(ServiceTimer,TimerPeriodic,10000000);if (EFI_ERROR(Status)) goto Fail;
  RecoveryStage=L"Query USBFn transfer limit";
  Status=Usb->GetMaxTransferSize(Usb,&MaxTransfer);if (EFI_ERROR(Status)) goto Fail;
  if (MaxTransfer<RX_SIZE) { Status=EFI_UNSUPPORTED;goto Fail; }
  Status=StartUsb();if (EFI_ERROR(Status)) goto Fail;
#ifdef POLARIS_PLATFORM_SERVICES
  PolarisWatchdog(30);
#endif
  RecoveryStage=L"USB event loop";
#ifdef POLARIS_PLATFORM_SERVICES
  PolarisLog("USB recovery service started");
#endif
  return EFI_SUCCESS;
Fail:
  { EFI_STATUS Stopped=ServiceStop();if (EFI_ERROR(Stopped)) return Stopped; }
  return Status;
}
STATIC EFI_STATUS ServicePoll(VOID) {
  EFI_USBFN_MESSAGE Message;EFI_USBFN_MESSAGE_PAYLOAD Payload;
  EFI_STATUS Status;UINTN Size;
  if (!ControllerRunning) return EFI_NOT_READY;
  Size=sizeof(Payload);ZeroMem(&Payload,Size);
  Status=Usb->EventHandler(Usb,&Message,&Size,&Payload);
  if (!EFI_ERROR(Status)) Status=Event(Message,&Payload);
  else if (Status==EFI_NOT_READY) Status=EFI_SUCCESS;
#ifdef POLARIS_PLATFORM_SERVICES
  if (!EFI_ERROR(Status) && PolarisStep(&Core)) Status=Reply();
#endif
  if (gBS->CheckEvent(ServiceTimer)==EFI_SUCCESS && (Core.State==FB_DOWNLOAD || TxPending)) {
    if (++IdleTicks>=COMMAND_TIMEOUT) Status=EFI_TIMEOUT;
  }
  if (EFI_ERROR(Status)) {
    LastUsbError=Status;
    // Never retry a partially completed flash. Reset parser before reconnect.
    AbortBulk();
    Status=StopUsb();if (EFI_ERROR(Status)) return Status;
    Status=StartUsb();if (EFI_ERROR(Status)) return Status;
  }
#ifdef POLARIS_PLATFORM_SERVICES
  PolarisPet();
#endif
  return Status;
}
#ifndef POLARIS_FASTBOOT_DXE
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status,Stopped;EFI_INPUT_KEY Key;
  EFI_EVENT DiagnosticTimer=NULL;
  (VOID)SystemTable;
  Status=ServiceStart(Image);
  Print(L"Polaris USB recovery: %r; ESC returns to menu.\n",Status);
  Print(L"Stage: %s; status: 0x%lx\n",RecoveryStage,(UINT64)Status);
  if (Usb) Print(L"USBFn revision: 0x%x (required 0x%x)\n",Usb->Revision,EFI_USBFN_IO_PROTOCOL_REVISION);
  Print(L"USB serial: polaris-uefi; RAM boot supports EFI images.\n");
  if (!EFI_ERROR(Status) && !EFI_ERROR(gBS->CreateEvent(EVT_TIMER,0,NULL,NULL,&DiagnosticTimer)))
    gBS->SetTimer(DiagnosticTimer,TimerPeriodic,10000000);
  while (!EFI_ERROR(Status) && !Done) {
    Status=ServicePoll();
    if (DiagnosticTimer && gBS->CheckEvent(DiagnosticTimer)==EFI_SUCCESS)
      Print(L"\rUSB events=%lu setup=%lu reset=%lu last=%u configured=%u error=0x%lx   ",
        (UINT64)UsbEvents,(UINT64)SetupPackets,(UINT64)BusResets,(UINT32)LastUsbMessage,
        (UINT32)Configured,(UINT64)LastUsbError);
    if (gST->ConIn && !EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn,&Key)) && Key.ScanCode==SCAN_ESC) break;
    gBS->Stall(100);
  }
  if (DiagnosticTimer) gBS->CloseEvent(DiagnosticTimer);
  Print(L"Recovery stage: %s; status: %r\n",RecoveryStage,Status);
  Stopped=ServiceStop();
  if (EFI_ERROR(Stopped)) {
    gRT->ResetSystem(EfiResetCold,Stopped,0,NULL);
    for (;;) gBS->Stall(1000000);
  }
  return Status;
}
#else
#include "Backend.c"
#endif
