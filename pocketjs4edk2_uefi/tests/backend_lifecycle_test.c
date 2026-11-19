// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <stdlib.h>
static unsigned long long ConsoleStorage[2052];
#define POLARIS_CONSOLE_STATE_ADDRESS ((UINTN)ConsoleStorage)
#define POLARIS_FASTBOOT_DXE
#define main RunTransportTests
#include "recovery_transport_test.c"
#undef main

EFI_GUID gEfiEventExitBootServicesGuid;
static unsigned LocateAttempts,AllocatedDma,FreedDma,TimerClosed;
static BOOLEAN Available,Tick;
static BOOLEAN ConfigAvailable;
static unsigned DeviceConnects;
static POLARIS_USB_CONFIG MockConfig;
static POLARIS_WINDOWS_ERRATA MockErrata;
static BOOLEAN ErrataEnabled;
static EFI_STATUS EFIAPI ErrataSet(POLARIS_WINDOWS_ERRATA *This,BOOLEAN Enable) {
 assert(This==&MockErrata);ErrataEnabled=Enable;return EFI_SUCCESS;
}
VOID *EFIAPI AllocatePool(UINTN Size) { return malloc(Size); }
VOID EFIAPI FreePool(VOID *Data) { free(Data); }
static EFI_STATUS EFIAPI LocateUsb(EFI_GUID *Guid,VOID *Registration,VOID **Out) {
 (void)Registration;
 EFI_GUID ErrataGuid=POLARIS_WINDOWS_ERRATA_GUID;
 if(!memcmp(Guid,&ErrataGuid,sizeof(*Guid))) {*Out=&MockErrata;return EFI_SUCCESS;}
 if(!memcmp(Guid,&PolarisUsbConfigGuid,sizeof(*Guid))) {
  if(!ConfigAvailable)return EFI_NOT_FOUND;
  *Out=&MockConfig;return EFI_SUCCESS;
 }
 if(memcmp(Guid,&gEfiUsbFunctionIoProtocolGuid,sizeof(*Guid))) return EFI_NOT_FOUND;
 ++LocateAttempts;
 if(!Available)return EFI_NOT_FOUND;
 *Out=&Mock;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ConnectDevice(POLARIS_USB_CONFIG *This,UINT32 CoreId,UINT32 Mode) {
 assert(This==&MockConfig && CoreId==0 && Mode==4);
 ++DeviceConnects;Available=TRUE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Dma(EFI_USBFN_IO_PROTOCOL *This,UINTN Size,VOID **Out) {
 (void)This;*Out=malloc(Size);++AllocatedDma;return *Out?EFI_SUCCESS:EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS EFIAPI FreeDma(EFI_USBFN_IO_PROTOCOL *This,VOID *Data) {
 (void)This;free(Data);++FreedDma;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MaxTransfer(EFI_USBFN_IO_PROTOCOL *This,UINTN *Size) { (void)This;*Size=RX_SIZE;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,VOID *Context,EFI_EVENT *Out) {
 (void)Type;(void)Tpl;(void)Notify;(void)Context;*Out=malloc(1);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetTimer(EFI_EVENT Event,EFI_TIMER_DELAY Type,UINT64 Time) {
 (void)Event;(void)Type;(void)Time;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT Event) { ++TimerClosed;free(Event);return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Check(EFI_EVENT Event) { assert(Event);return Tick?EFI_SUCCESS:EFI_NOT_READY; }
static EFI_STATUS EFIAPI NoEvent(EFI_USBFN_IO_PROTOCOL *This,EFI_USBFN_MESSAGE *Message,UINTN *Size,EFI_USBFN_MESSAGE_PAYLOAD *Payload) {
 (void)This;(void)Message;(void)Size;(void)Payload;return EFI_NOT_READY;
}
static unsigned ScreenWrites;
static EFI_STATUS EFIAPI Screen(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *Text) {
 (void)This;(void)Text;++ScreenWrites;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ReturningWindows(EFI_HANDLE Handle,UINTN *ExitSize,CHAR16 **ExitData) {
 (void)Handle;(void)ExitSize;(void)ExitData;
 assert(!ControllerRunning && BackendActive);
 assert(BackendStop(&Backend)==EFI_SUCCESS && !BackendActive && !Rx && !Tx && !Control && !Download && !ServiceTimer);
 assert(SetWindowsHandoff(&WindowsPolicy,TRUE)==EFI_SUCCESS && ErrataEnabled);
 assert(SetWindowsHandoff(&WindowsPolicy,FALSE)==EFI_SUCCESS && !ErrataEnabled);
 return EFI_ABORTED;
}
int main(void) {
 UINT32 Flags;
 EFI_SYSTEM_TABLE Table={0};EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Out={0},Err={0};
 RunTransportTests();Reset();
 memset(ConsoleStorage,0,sizeof(ConsoleStorage));
 PolarisBootConsole()->Magic=POLARIS_CONSOLE_MAGIC;
 Out.OutputString=Err.OutputString=Screen;Table.ConOut=&Out;Table.StdErr=&Err;gST=&Table;
 // Use a fresh stopped driver, not the transport fixture's static DMA arrays.
 ControllerRunning=FALSE;Rx=Tx=Control=NULL;Download=NULL;ServiceTimer=NULL;
 Usb=NULL;RetryTimer=NULL;BackendActive=Exited=FALSE;
 MockErrata=(POLARIS_WINDOWS_ERRATA){.Revision=POLARIS_WINDOWS_ERRATA_REVISION,.Size=sizeof(MockErrata),.Set=ErrataSet};
 WindowsPolicy=(POLARIS_WINDOWS_HANDOFF){POLARIS_WINDOWS_HANDOFF_REVISION,sizeof(WindowsPolicy),SetWindowsHandoff,NULL};
 Backend=(PJ4EDK2_FASTBOOT_PROTOCOL){PJ4EDK2_FASTBOOT_REVISION,sizeof(Backend),BackendStart,BackendPoll,BackendStop};
 Platform=(PJB_PLATFORM_PROTOCOL){PJB_PLATFORM_REVISION,sizeof(Platform),ReadPower,Quiesce};
 Bs.LocateProtocol=LocateUsb;Bs.CreateEvent=Create;Bs.SetTimer=SetTimer;Bs.CloseEvent=Close;Bs.CheckEvent=Check;
 Mock.AllocateTransferBuffer=Dma;Mock.FreeTransferBuffer=FreeDma;Mock.GetMaxTransferSize=MaxTransfer;
 Mock.EventHandler=NoEvent;Mock.Revision=EFI_USBFN_IO_PROTOCOL_REVISION;
 Available=FALSE;assert(!EFI_ERROR(BackendStart(&Backend)) && BackendActive && !ControllerRunning);
 assert(PolarisBootConsole()->Owner==POLARIS_CONSOLE_UI);
 CapturedLines=0;Out.OutputString(&Out,L"child output\r\n");
 assert(CapturedLines==1 && !strcmp(CapturedLine,"child output") && !ScreenWrites);
 Err.OutputString(&Err,L"error output");assert(!strcmp(CapturedLine,"error output") && !ScreenWrites);
 assert(LocateAttempts==1);Tick=FALSE;assert(!EFI_ERROR(BackendPoll(&Backend,&Flags)) && LocateAttempts==1);
 Tick=TRUE;Available=TRUE;assert(!EFI_ERROR(BackendPoll(&Backend,&Flags)) && ControllerRunning && AllocatedDma==3);
 Tick=FALSE;assert(!EFI_ERROR(BackendPoll(&Backend,&Flags)) && Flags==0);
 PlatformIsBusy=TRUE;assert(!EFI_ERROR(BackendPoll(&Backend,&Flags)) && (Flags&PJ4EDK2_FASTBOOT_BUSY));
 StopStatus=EFI_DEVICE_ERROR;assert(EFI_ERROR(BackendStop(&Backend)) && BackendActive && !FreedDma && ControllerRunning);
 assert(PolarisBootConsole()->Owner==POLARIS_CONSOLE_UI);
 assert(Out.OutputString==LogConsole && Err.OutputString==LogConsole);
 StopStatus=EFI_SUCCESS;assert(!EFI_ERROR(BackendStop(&Backend)) && !BackendActive && FreedDma==3 && !ControllerRunning);
 assert(PolarisBootConsole()->Owner==POLARIS_CONSOLE_TEXT);
 assert(Out.OutputString==Screen && Err.OutputString==Screen);
 assert(WatchdogTimeout==0); // Ordinary UI stop relinquishes the watchdog.
 // Exercise the real nested path: a RAM child stops and frees the entire
 // backend, then returns into the poll/action frame that launched it.
 assert(BackendStart(&Backend)==EFI_SUCCESS && ControllerRunning);
 Bs.StartImage=ReturningWindows;Core.Action=FB_BOOT;Child=(EFI_HANDLE)123;
 unsigned BeforeChildDma=AllocatedDma;
 assert(Action()==EFI_SUCCESS && BackendActive && ControllerRunning && Rx && Tx && Control && Download && ServiceTimer && RetryTimer);
 assert(AllocatedDma==BeforeChildDma+3 && PolarisBootConsole()->Owner==POLARIS_CONSOLE_UI);
 Tick=FALSE;assert(BackendPoll(&Backend,&Flags)==EFI_SUCCESS);
 assert(BackendStop(&Backend)==EFI_SUCCESS && AllocatedDma==FreedDma);
 unsigned DmaBaseline=AllocatedDma;
 // Device mode is requested only after USBFn is absent. Reject an unknown ABI
 // without invoking its function pointer or allocating DMA buffers.
 Available=FALSE;ConfigAvailable=TRUE;
 MockConfig.Revision=0x10002;MockConfig.StartController=ConnectDevice;
 assert(!EFI_ERROR(BackendStart(&Backend)) && !ControllerRunning && !DeviceConnects && AllocatedDma==DmaBaseline);
 MockConfig.Revision=0x10003;Tick=TRUE;
 assert(!EFI_ERROR(BackendPoll(&Backend,&Flags)) && DeviceConnects==1 && ControllerRunning && AllocatedDma==DmaBaseline+3);
 unsigned ClosedAtEbs=TimerClosed;
 BackendExit(NULL,NULL);assert(Exited && FreedDma==DmaBaseline && !ControllerRunning);
 assert(TimerClosed==ClosedAtEbs); // EBS quiesces without changing the memory map.
 assert(PolarisBootConsole()->Owner==POLARIS_CONSOLE_TEXT);
 assert(WatchdogTimeout==30); // EBS cleanup must not leave the kernel unguarded.
 assert(Out.OutputString==Screen && Err.OutputString==Screen);
 assert(BackendStart(&Backend)==EFI_UNSUPPORTED);
 assert(!EFI_ERROR(BackendStop(&Backend)) && FreedDma==DmaBaseline+3);
 // Windows gets a live firmware watchdog during loading, then CSRT HAL owns
 // the physical timer. A failed/returning child can revoke and retry policy.
 Exited=FALSE;
 WindowsPolicy=(POLARIS_WINDOWS_HANDOFF){POLARIS_WINDOWS_HANDOFF_REVISION,sizeof(WindowsPolicy),SetWindowsHandoff,NULL};
 assert(SetWindowsHandoff(&WindowsPolicy,TRUE)==EFI_SUCCESS && WatchdogTimeout==30 && WindowsHeartbeat && ErrataEnabled);
 unsigned Pets=WatchdogPets;WindowsPet(NULL,NULL);assert(WatchdogPets==Pets+1);
 assert(SetWindowsHandoff(&WindowsPolicy,TRUE)==EFI_NOT_READY);
 assert(SetWindowsHandoff(&WindowsPolicy,FALSE)==EFI_SUCCESS && WatchdogTimeout==0 && !WindowsHeartbeat);
 assert(SetWindowsHandoff(&WindowsPolicy,TRUE)==EFI_SUCCESS);
 ClosedAtEbs=TimerClosed;BackendExit(NULL,NULL);
 assert(Exited && !WatchdogTimeout && TimerClosed==ClosedAtEbs);
 Pets=WatchdogPets;WindowsPet(NULL,NULL);assert(WatchdogPets==Pets);
 assert(SetWindowsHandoff(&WindowsPolicy,FALSE)==EFI_UNSUPPORTED);
 if(WindowsHeartbeat){Close(WindowsHeartbeat);WindowsHeartbeat=NULL;}
 if(RetryTimer){Close(RetryTimer);RetryTimer=NULL;}
 // Shared stdout/stderr objects must be restored only once.
 Table.StdErr=&Out;CaptureConsole();RestoreConsole();assert(Out.OutputString==Screen);
 gST=NULL;
 puts("PASS: real backend deferred USB discovery, retry, busy, failed-stop DMA retention and ExitBootServices cleanup");
 return 0;
}
