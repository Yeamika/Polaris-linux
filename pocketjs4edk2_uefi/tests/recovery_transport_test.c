// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#define POLARIS_PLATFORM_SERVICES
#include "../efi/Recovery/Recovery.c"
EFI_BOOT_SERVICES *gBS;
EFI_RUNTIME_SERVICES *gRT;
EFI_SYSTEM_TABLE *gST;
EFI_GUID gEfiUsbFunctionIoProtocolGuid;
static unsigned char R[RX_SIZE],T[FB_COMMAND],Ctl[CONTROL_SIZE],D[1024];
static EFI_USBFN_IO_PROTOCOL Mock;
static EFI_BOOT_SERVICES Bs;
static EFI_RUNTIME_SERVICES Rt;
static EFI_STATUS TransferStatus,StopStatus,LoadStatus;
static unsigned int Transfers,Starts,Stops,Loads,Unloads,Aborts;
static unsigned ImageStarts,RotationStops;
static EFI_STATUS RotationStatus;
static BOOLEAN HasRotator;
static EFI_STATUS EFIAPI StopRotation(PJ4EDK2_ROTATE_PROTOCOL *This) {
 (void)This;++RotationStops;return RotationStatus;
}
static PJ4EDK2_ROTATE_PROTOCOL Rotator={PJ4EDK2_ROTATE_REVISION,sizeof(Rotator),NULL,StopRotation};
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Interface) {
 (void)Guid;(void)Registration;
 if(!HasRotator)return EFI_NOT_FOUND;
 *Interface=&Rotator;return EFI_SUCCESS;
}
static UINT8 LastEp;
static EFI_USBFN_ENDPOINT_DIRECTION LastDirection;
static UINTN LastSize;
static VOID *LastBuffer;
VOID *EFIAPI CopyMem(VOID *Destination,CONST VOID *Source,UINTN Length) { return memmove(Destination,Source,Length); }
VOID *EFIAPI ZeroMem(VOID *Buffer,UINTN Length) { return memset(Buffer,0,Length); }
UINTN EFIAPI StrLen(CONST CHAR16 *String) { UINTN N=0;while(String[N])++N;return N; }
UINTN EFIAPI Print(CONST CHAR16 *Format,...) { (void)Format;return 0; }
static unsigned PlatformCommands;
static BOOLEAN PlatformIsBusy;
VOID PolarisCancel(VOID) { PlatformIsBusy=FALSE; }
static UINTN WatchdogTimeout;
VOID PolarisWatchdog(UINTN Seconds) { WatchdogTimeout=Seconds; }
static unsigned WatchdogPets;
VOID PolarisPet(VOID) {++WatchdogPets;}
static unsigned CapturedLines;
static CHAR8 CapturedLine[60];
VOID PolarisLog(CONST CHAR8 *Text) {
 ++CapturedLines;snprintf(CapturedLine,sizeof(CapturedLine),"%s",Text);
}
BOOLEAN PolarisBusy(VOID) { return PlatformIsBusy; }
BOOLEAN PolarisStep(FB_CORE *C) { (void)C;return FALSE; }
VOID PolarisNext(FB_CORE *C) { C->Action=0;C->ReplySize=4;memcpy(C->Reply,"OKAY",5); }
BOOLEAN PolarisCommand(FB_CORE *C,CONST UINT8 *Packet,UINTN Size) {
 (void)C;(void)Packet;(void)Size;++PlatformCommands;return TRUE;
}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) { return strcmp(A,B); }
static EFI_STATUS EFIAPI Transfer(EFI_USBFN_IO_PROTOCOL *This,UINT8 Ep,EFI_USBFN_ENDPOINT_DIRECTION Direction,UINTN *Size,VOID *Buffer) {
 (void)This;++Transfers;LastEp=Ep;LastDirection=Direction;LastSize=*Size;LastBuffer=Buffer;return TransferStatus;
}
static EFI_STATUS EFIAPI Abort(EFI_USBFN_IO_PROTOCOL *This,UINT8 Ep,EFI_USBFN_ENDPOINT_DIRECTION Direction) {
 (void)This;(void)Ep;(void)Direction;++Aborts;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Stall(EFI_USBFN_IO_PROTOCOL *This,UINT8 Ep,EFI_USBFN_ENDPOINT_DIRECTION Direction,BOOLEAN *State) {
 (void)This;(void)Ep;(void)Direction;(void)State;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Stop(EFI_USBFN_IO_PROTOCOL *This) { (void)This;++Stops;return StopStatus; }
static EFI_STATUS EFIAPI Start(EFI_USBFN_IO_PROTOCOL *This) { (void)This;++Starts;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Configure(EFI_USBFN_IO_PROTOCOL *This,EFI_USB_DEVICE_INFO *Info) {
 (void)This;assert(Info->DeviceDescriptor->NumConfigurations==1);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Load(BOOLEAN BootPolicy,EFI_HANDLE Image,EFI_DEVICE_PATH_PROTOCOL *Path,VOID *Source,UINTN Size,EFI_HANDLE *Handle) {
 (void)BootPolicy;(void)Image;(void)Path;(void)Source;(void)Size;++Loads;
 if (!EFI_ERROR(LoadStatus)) *Handle=(EFI_HANDLE)123;
 return LoadStatus;
}
static EFI_STATUS EFIAPI Unload(EFI_HANDLE Handle) { (void)Handle;++Unloads;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI StartImage(EFI_HANDLE Handle,UINTN *ExitSize,CHAR16 **ExitData) {
 (void)Handle;(void)ExitSize;(void)ExitData;++ImageStarts;assert(!ControllerRunning);
 if(HasRotator)assert(RotationStops==1 && RotationStatus==EFI_SUCCESS);
 return EFI_ABORTED;
}
static void Reset(void) {
 memset(&Mock,0,sizeof(Mock));memset(&Bs,0,sizeof(Bs));memset(&Rt,0,sizeof(Rt));
 Mock.Transfer=Transfer;Mock.AbortTransfer=Abort;Mock.SetEndpointStallState=Stall;
 Mock.StartController=Start;Mock.StopController=Stop;Mock.ConfigureEnableEndpoints=Configure;
 Bs.LoadImage=Load;Bs.UnloadImage=Unload;Bs.StartImage=StartImage;
 Bs.LocateProtocol=Locate;HasRotator=FALSE;RotationStatus=EFI_SUCCESS;ImageStarts=RotationStops=0;
 Usb=&Mock;gBS=&Bs;gRT=&Rt;Rx=R;Tx=T;Control=Ctl;Child=NULL;Done=FALSE;
#ifdef POLARIS_FASTBOOT_DXE
 BackendActive=TRUE;Exited=FALSE;
#endif
 ControllerRunning=TRUE;Configured=TRUE;RxPending=TxPending=FALSE;
 ControlIn=ControlStatus=EnableAfterStatus=FALSE;
 TransferStatus=StopStatus=LoadStatus=EFI_SUCCESS;Transfers=Starts=Stops=Loads=Unloads=Aborts=0;
 FbInit(&Core,D,sizeof(D));
}
static EFI_STATUS Complete(EFI_USBFN_MESSAGE Message,UINT8 Ep,VOID *Buffer,UINTN Bytes,EFI_USBFN_TRANSFER_STATUS Status) {
 EFI_USBFN_MESSAGE_PAYLOAD P;memset(&P,0,sizeof(P));P.utr.EndpointIndex=Ep;
 P.utr.Buffer=Buffer;P.utr.BytesTransferred=Bytes;P.utr.TransferStatus=Status;return Event(Message,&P);
}
int main(void) {
 EFI_USB_DEVICE_REQUEST Request;
 EFI_USBFN_MESSAGE_PAYLOAD P;
 Reset();PlatformCommands=0;memcpy(R,"flash:boot\0bad",14);RxPending=TRUE;RxRequested=14;
 assert(!EFI_ERROR(Complete(EfiUsbMsgEndpointStatusChangedRx,1,R,14,UsbTransferStatusComplete)));
 assert(PlatformCommands==0 && !strncmp(Core.Reply,"FAILcommand encoding",20));
 Reset();memcpy(R,"flash:boot",10);RxPending=TRUE;RxRequested=10;
 assert(!EFI_ERROR(Complete(EfiUsbMsgEndpointStatusChangedRx,1,R,10,UsbTransferStatusComplete)));
 assert(PlatformCommands==1);
 Reset();assert(!EFI_ERROR(ArmRx()));assert(RxPending && LastEp==1 && LastSize==64);
 memcpy(R,"getvar:product",14);
 assert(!EFI_ERROR(Complete(EfiUsbMsgEndpointStatusChangedRx,1,R,14,UsbTransferStatusComplete)));
 assert(TxPending && !RxPending && LastDirection==EfiUsbEndpointDirectionDeviceTx && LastSize==11);
 assert(!memcmp(T,"OKAYpolaris",11));
 assert(!EFI_ERROR(Complete(EfiUsbMsgEndpointStatusChangedTx,1,T,3,UsbTransferStatusComplete)));
 assert(TxPending && LastSize==8 && !memcmp(T,"Ypolaris",8));
 assert(!EFI_ERROR(Complete(EfiUsbMsgEndpointStatusChangedTx,1,T,8,UsbTransferStatusComplete)));
 assert(RxPending && !TxPending);
 Reset();ArmRx();assert(EFI_ERROR(Complete(EfiUsbMsgEndpointStatusChangedRx,1,R,65,UsbTransferStatusComplete)));
 Reset();ArmRx();assert(EFI_ERROR(Complete(EfiUsbMsgEndpointStatusChangedRx,1,R,5,UsbTransferStatusAborted)));
 Reset();Core.Valid=100;Core.State=FB_DOWNLOAD;Core.Expected=500;
 memset(&P,0,sizeof(P));Event(EfiUsbMsgBusEventReset,&P);assert(!Core.Valid && !Configured && !Core.Expected);
 Reset();memset(&Request,0,sizeof(Request));Request.Request=9;Request.Value=1;
 assert(!EFI_ERROR(Setup(&Request)));assert(!Configured && EnableAfterStatus && LastEp==0 && LastSize==0);
 assert(!EFI_ERROR(Complete(EfiUsbMsgEndpointStatusChangedTx,0,Ctl,0,UsbTransferStatusComplete)));
 assert(Configured && RxPending);
 Reset();memset(&Request,0,sizeof(Request));Request.RequestType=0x80;Request.Request=6;Request.Value=0x100;Request.Length=8;
 assert(!EFI_ERROR(Setup(&Request)));assert(LastSize==8 && !memcmp(Ctl,&Device,8));
 assert(!EFI_ERROR(Complete(EfiUsbMsgEndpointStatusChangedTx,0,Ctl,8,UsbTransferStatusComplete)));
 assert(LastSize==0 && LastDirection==EfiUsbEndpointDirectionDeviceRx);
 Reset();Core.Action=FB_BOOT;Core.ReplySize=4;memcpy(Core.Reply,"OKAY",4);Core.Valid=10;LoadStatus=EFI_SECURITY_VIOLATION;
 assert(!EFI_ERROR(Reply()));assert(Loads==1 && !Core.Action && Child==NULL && !memcmp(T,"FAIL",4));
 Reset();Core.Action=FB_BOOT;Core.ReplySize=4;memcpy(Core.Reply,"OKAY",4);assert(!EFI_ERROR(Reply()));
 assert(Child!=NULL && ControllerRunning);
 assert(!EFI_ERROR(Complete(EfiUsbMsgEndpointStatusChangedTx,1,T,4,UsbTransferStatusComplete)));
 assert(Stops==1 && Starts==1 && Child==NULL && !Configured && !Core.Valid);
 Reset();Core.Action=FB_BOOT;Child=(EFI_HANDLE)123;HasRotator=TRUE;RotationStatus=EFI_TIMEOUT;
 assert(Action()==EFI_TIMEOUT && Child==NULL && Unloads==1 && ImageStarts==0 && Stops==0);
 Reset();Core.Action=FB_BOOT;Child=(EFI_HANDLE)123;HasRotator=TRUE;
 assert(!EFI_ERROR(Action()) && RotationStops==1 && ImageStarts==1 && Starts==1);
 Reset();Core.Action=FB_BOOT;Child=(EFI_HANDLE)123;StopStatus=EFI_DEVICE_ERROR;
 assert(EFI_ERROR(Action()));assert(ControllerRunning && Starts==0);
 Reset();Core.Action=FB_EXIT;assert(!EFI_ERROR(Action()) && Done);
 puts("PASS: USB control enumeration, bounded RX, partial TX, reset invalidation, rejected images and EFI return recovery");
 return 0;
}
