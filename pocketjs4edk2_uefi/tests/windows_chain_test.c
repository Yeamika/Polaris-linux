// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#undef NULL
#include "../efi/Windows/Windows.c"
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiGraphicsOutputProtocolGuid,gEfiSimpleFileSystemProtocolGuid;
EFI_GUID gEfiEventReadyToBootGuid,gFdtTableGuid,gEfiFileInfoGuid;
static EFI_BOOT_SERVICES Bs;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION ModeInfo;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL DisplayProtocol;
static POLARIS_WINDOWS_HANDOFF Policy;
static POLARIS_WINDOWS_ERRATA Errata;
static PJ4EDK2_FASTBOOT_PROTOCOL Fastboot;
static PJ4EDK2_ROTATE_PROTOCOL Rotator;
static unsigned Prepared,Signalled,Started,Loaded,Unloaded,Closed,UsbStops,RotationStops;
static BOOLEAN MissingPolicy,Guarded,FdtAbsent;
static EFI_STATUS PrepareStatus,SignalStatus,LoadStatus,StopStatus;
static EFI_STATUS ErrataStatus;
static UINTN Watchdog;
VOID *EFIAPI AllocateZeroPool(UINTN N) {return calloc(1,N);}
VOID *EFIAPI AllocatePool(UINTN N) {return malloc(N);}
VOID EFIAPI FreePool(VOID *P) {free(P);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N) {return memset(P,0,N);}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S) {return strlen(S);}
UINTN EFIAPI Print(CONST CHAR16 *Fmt,...) {(void)Fmt;return 0;}
UINTN EFIAPI AsciiSPrint(CHAR8 *Dst,UINTN Size,CONST CHAR8 *Fmt,...) {
 char F[1024];unsigned I=0,J=0;va_list Ap;
 for(;Fmt[I];++I) {
  if(Fmt[I]=='%' && Fmt[I+1]=='a') {F[J++]='%';F[J++]='s';++I;}
  else if(Fmt[I]=='%' && Fmt[I+1]=='r') {F[J++]='%';F[J++]='l';F[J++]='x';++I;}
  else F[J++]=Fmt[I];
 }
 F[J]=0;va_start(Ap,Fmt);int N=vsnprintf(Dst,Size,F,Ap);va_end(Ap);
 return N<0?0:(UINTN)N>=Size?Size-1:(UINTN)N;
}
EFI_STATUS PolarisFindVolume(EFI_HANDLE Image,CONST CHAR16 *Path,EFI_HANDLE *Volume) {
 (void)Image;assert(Path==BootPath);*Volume=(EFI_HANDLE)0x44;return EFI_SUCCESS;
}
EFI_STATUS WindowsCopyDsdt(VOID **Data,UINTN *Size) {*Data=NULL;*Size=0;return EFI_NOT_FOUND;}
EFI_STATUS WindowsPreparePlatform(WINDOWS_PLATFORM_INFO *Info) {
 assert(Guarded);++Prepared;memset(Info,0,sizeof(*Info));return PrepareStatus;
}
CONST CHAR8 *WindowsPlatformStage(VOID) {return "fixture ACPI failure";}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI FileDevicePath(EFI_HANDLE Device,CONST CHAR16 *Path) {
 assert(Device==(EFI_HANDLE)0x44 && Path==BootPath);return calloc(1,4);
}
EFI_STATUS EFIAPI EfiGetSystemConfigurationTable(IN EFI_GUID *Guid,IN OUT VOID **Data) {
 assert(Guid==&gFdtTableGuid);*Data=(VOID *)0x55;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI StopUsb(PJ4EDK2_FASTBOOT_PROTOCOL *This) {
 assert(This==&Fastboot);++UsbStops;return StopStatus;
}
static EFI_STATUS EFIAPI StopRotation(PJ4EDK2_ROTATE_PROTOCOL *This) {
 assert(This==&Rotator);++RotationStops;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetPolicy(POLARIS_WINDOWS_HANDOFF *This,BOOLEAN Enable) {
 assert(This==&Policy);assert(UsbStops && RotationStops);Guarded=Enable;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Out) {
 EFI_GUID P=POLARIS_WINDOWS_HANDOFF_GUID,F=PJ4EDK2_FASTBOOT_GUID,R=PJ4EDK2_ROTATE_GUID;
 (void)Registration;
 if(!memcmp(Guid,&P,sizeof(P))) {if(MissingPolicy)return EFI_NOT_FOUND;*Out=&Policy;}
 else if(!memcmp(Guid,&F,sizeof(F))) *Out=&Fastboot;
 else if(!memcmp(Guid,&R,sizeof(R))) *Out=&Rotator;
 else if(Guid==&gEfiGraphicsOutputProtocolGuid) *Out=&DisplayProtocol;
 else return EFI_NOT_FOUND;
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Blt(EFI_GRAPHICS_OUTPUT_PROTOCOL *This,EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer,
 EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op,UINTN Sx,UINTN Sy,UINTN Dx,UINTN Dy,UINTN W,UINTN H,UINTN Delta) {
 (void)Sx;(void)Sy;(void)Delta;assert(This==&DisplayProtocol && Buffer && Op<=EfiBltBufferToVideo);
 assert(Dx+W<=1080 && Dy+H<=2160);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI NoFile(EFI_HANDLE H,EFI_GUID *Guid,VOID **Out) {
 (void)H;(void)Guid;(void)Out;return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,
 CONST EFI_GUID *Group,EFI_EVENT *Out) {
 (void)Context;assert(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_CALLBACK && Notify && Group==&gEfiEventReadyToBootGuid);
 assert(Prepared==1 && Guarded);*Out=(EFI_EVENT)0x66;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Signal(EFI_EVENT Event) {
 assert(Event==(EFI_EVENT)0x66);++Signalled;return SignalStatus;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT Event) {assert(Event==(EFI_EVENT)0x66);++Closed;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Install(EFI_GUID *Guid,VOID *Data) {
 assert(Guid==&gFdtTableGuid);if(Data)assert(Data==(VOID *)0x55);
 FdtAbsent=Data==NULL;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Load(BOOLEAN BootPolicy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,
 VOID *Data,UINTN Size,EFI_HANDLE *Child) {
 (void)Parent;assert(BootPolicy && Path && !Data && !Size && Guarded && FdtAbsent);
 assert(Prepared==2 && Signalled==1 && Closed==1);++Loaded;
 if(LoadStatus==EFI_SECURITY_VIOLATION || !EFI_ERROR(LoadStatus)) *Child=(EFI_HANDLE)0x77;
 return LoadStatus;
}
static EFI_STATUS EFIAPI Unload(EFI_HANDLE Child) {assert(Child==(EFI_HANDLE)0x77);++Unloaded;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI SetWatchdog(UINTN Seconds,UINT64 Code,UINTN Size,CHAR16 *Data) {
 (void)Code;(void)Size;(void)Data;assert(!Seconds || (Seconds==300 && Guarded));Watchdog=Seconds;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OriginalExit(EFI_HANDLE Child,UINTN Key) {(void)Child;(void)Key;abort();}
static EFI_STATUS EFIAPI Start(EFI_HANDLE Child,UINTN *ExitSize,CHAR16 **ExitData) {
 assert(Child==(EFI_HANDLE)0x77 && !ExitSize && !ExitData && Watchdog==300 && Guarded && FdtAbsent);
 assert(Bs.ExitBootServices==OriginalExit);++Started;
 Errata.Stage=WindowsErrataKernel;Errata.Status=ErrataStatus;return EFI_ABORTED;
}
static EFI_STATUS EFIAPI Stall(UINTN Us) {assert(Us==5000000);return EFI_SUCCESS;}
static void Reset(void) {
 memset(&Bs,0,sizeof(Bs));gBS=&Bs;Gop=NULL;Row=0;
 Prepared=Signalled=Started=Loaded=Unloaded=Closed=UsbStops=RotationStops=0;
 MissingPolicy=Guarded=FdtAbsent=FALSE;PrepareStatus=SignalStatus=LoadStatus=StopStatus=EFI_SUCCESS;Watchdog=0;
 ModeInfo=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){0,1080,2160,PixelBlueGreenRedReserved8BitPerColor,{0},1080};
 Mode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){1,0,&ModeInfo,sizeof(ModeInfo),0x9d400000,1080*2160*4};
 DisplayProtocol=(EFI_GRAPHICS_OUTPUT_PROTOCOL){NULL,NULL,Blt,&Mode};
 Errata=(POLARIS_WINDOWS_ERRATA){.Revision=POLARIS_WINDOWS_ERRATA_REVISION,.Size=sizeof(Errata)};
 ErrataStatus=EFI_SUCCESS;
 Policy=(POLARIS_WINDOWS_HANDOFF){POLARIS_WINDOWS_HANDOFF_REVISION,sizeof(Policy),SetPolicy,&Errata};
 Fastboot=(PJ4EDK2_FASTBOOT_PROTOCOL){PJ4EDK2_FASTBOOT_REVISION,sizeof(Fastboot),NULL,NULL,StopUsb};
 Rotator=(PJ4EDK2_ROTATE_PROTOCOL){PJ4EDK2_ROTATE_REVISION,sizeof(Rotator),NULL,StopRotation};
 Bs.LocateProtocol=Locate;Bs.HandleProtocol=NoFile;Bs.CreateEventEx=Create;Bs.SignalEvent=Signal;Bs.CloseEvent=Close;
 Bs.InstallConfigurationTable=Install;Bs.LoadImage=Load;Bs.StartImage=Start;Bs.UnloadImage=Unload;
 Bs.SetWatchdogTimer=SetWatchdog;Bs.ExitBootServices=OriginalExit;Bs.Stall=Stall;
}
int main(void) {
 Reset();assert(UefiMain((EFI_HANDLE)1,NULL)==EFI_ABORTED);
 assert(Started==1 && Loaded==1 && !Unloaded && !Watchdog && !Guarded && !FdtAbsent && Bs.ExitBootServices==OriginalExit);
 Reset();ErrataStatus=EFI_UNSUPPORTED;assert(UefiMain((EFI_HANDLE)1,NULL)==EFI_UNSUPPORTED);
 assert(Started==1 && !Guarded && !FdtAbsent && !Watchdog && Errata.Stage==WindowsErrataKernel);
 Reset();PrepareStatus=EFI_COMPROMISED_DATA;assert(UefiMain((EFI_HANDLE)1,NULL)==PrepareStatus);
 assert(!Signalled && !Loaded && !Started && !Guarded && !FdtAbsent);
 Reset();SignalStatus=EFI_DEVICE_ERROR;assert(UefiMain((EFI_HANDLE)1,NULL)==SignalStatus);
 assert(Closed==1 && Prepared==1 && !Loaded && !Guarded);
 Reset();LoadStatus=EFI_SECURITY_VIOLATION;assert(UefiMain((EFI_HANDLE)1,NULL)==LoadStatus);
 assert(Unloaded==1 && !Started && !Guarded && !FdtAbsent);
 Reset();MissingPolicy=TRUE;assert(UefiMain((EFI_HANDLE)1,NULL)==EFI_NOT_FOUND);
 assert(!Prepared && !UsbStops && !Loaded);
 Reset();StopStatus=EFI_TIMEOUT;assert(UefiMain((EFI_HANDLE)1,NULL)==EFI_TIMEOUT);
 assert(!RotationStops && !Prepared && !Loaded && !Guarded);
 puts("PASS: Windows preparation order, ReadyToBoot, unchanged EBS entry, failed USB/ACPI/LoadImage and return recovery");
 return 0;
}
