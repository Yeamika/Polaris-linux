// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#undef NULL
#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
static const CHAR16 Version[]=L"host-test";
#define PcdGetPtr(Token) Version
EFI_GUID gSimpleInitFileGuid={1,0,0,{0}};
EFI_GUID gUefiShellFileGuid={2,0,0,{0}};
EFI_HANDLE gImageHandle=(EFI_HANDLE)9;
static UINT64 ConsoleStorage[2052];
#define POLARIS_CONSOLE_STATE_ADDRESS ((UINTN)ConsoleStorage)
#include "../platform/BootEntry.c"
EFI_BOOT_SERVICES *gBS;
EFI_SYSTEM_TABLE *gST;
static EFI_BOOT_SERVICES Bs;
static EFI_SYSTEM_TABLE St;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Input;
static EFI_STATUS FvStatus,LoadStatus,AppStatus;
static unsigned int Reads,Loads,Runs,Unloads,Frees,Watchdogs,Stalls;
static unsigned char Image[128];
static EFI_GUID LastGuid;
static jmp_buf Escape;
UINTN EFIAPI Print(CONST CHAR16 *Format,...) { (void)Format;return 0; }
VOID EFIAPI FreePool(VOID *Buffer) { assert(Buffer==Image);++Frees; }
EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *Guid,EFI_SECTION_TYPE Type,UINTN Instance,VOID **Buffer,UINTN *Size) {
 ++Reads;LastGuid=*Guid;assert(Type==EFI_SECTION_PE32 && !Instance);
 if (!EFI_ERROR(FvStatus)) { *Buffer=Image;*Size=sizeof(Image); }
 return FvStatus;
}
static EFI_STATUS EFIAPI Load(BOOLEAN Policy,EFI_HANDLE Parent,EFI_DEVICE_PATH_PROTOCOL *Path,VOID *Buffer,UINTN Size,EFI_HANDLE *Child) {
 ++Loads;assert(!Policy && Parent==gImageHandle && Path==NULL && Buffer==Image && Size==sizeof(Image));
 if (!EFI_ERROR(LoadStatus)) *Child=(EFI_HANDLE)10;
 return LoadStatus;
}
static EFI_STATUS EFIAPI Run(EFI_HANDLE Child,UINTN *ExitSize,CHAR16 **ExitData) {
 (void)ExitSize;(void)ExitData;assert(Child==(EFI_HANDLE)10);++Runs;return AppStatus;
}
static EFI_STATUS EFIAPI Unload(EFI_HANDLE Child) { assert(Child==(EFI_HANDLE)10);++Unloads;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Watchdog(UINTN Timeout,UINT64 Code,UINTN Size,CHAR16 *Data) {
 (void)Code;(void)Size;(void)Data;assert(Timeout==0);++Watchdogs;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ReadKey(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,EFI_INPUT_KEY *Key) {
 (void)This;(void)Key;return EFI_NOT_READY;
}
static EFI_STATUS EFIAPI Stall(UINTN Microseconds) {
 assert(Microseconds==10000);if (++Stalls==3) longjmp(Escape,1);return EFI_SUCCESS;
}
static void Reset(void) {
 memset(&Bs,0,sizeof(Bs));memset(&St,0,sizeof(St));memset(&Input,0,sizeof(Input));
 Bs.LoadImage=Load;Bs.StartImage=Run;Bs.UnloadImage=Unload;Bs.SetWatchdogTimer=Watchdog;Bs.Stall=Stall;
 Input.ReadKeyStroke=ReadKey;St.ConIn=&Input;gBS=&Bs;gST=&St;
 FvStatus=LoadStatus=AppStatus=EFI_SUCCESS;Reads=Loads=Runs=Unloads=Frees=Watchdogs=Stalls=0;
}
int main(void) {
 EFI_STATUS Status;
 Reset();FvStatus=EFI_NOT_FOUND;Status=PolarisStartBuiltin(&gSimpleInitFileGuid);
 assert(Status==EFI_NOT_FOUND && Reads==1 && !Loads && !Runs && !Frees && PolarisEntryStage[0]==L'F');
 Reset();LoadStatus=EFI_LOAD_ERROR;Status=PolarisStartBuiltin(&gSimpleInitFileGuid);
 assert(Status==EFI_LOAD_ERROR && Frees==1 && Loads==1 && !Runs && !Unloads && PolarisEntryStage[0]==L'L');
 Reset();AppStatus=EFI_UNSUPPORTED;Status=PolarisStartBuiltin(&gSimpleInitFileGuid);
 assert(Status==EFI_UNSUPPORTED && Frees==1 && Runs==1 && Unloads==1 && PolarisEntryStage[0]==L'S');
 Reset();Status=PolarisStartBuiltin(&gSimpleInitFileGuid);assert(!EFI_ERROR(Status) && PolarisEntryStatus==Status);
 // Reproduce all-apps-fail without input. It must stay on diagnostics rather than
 // launch the menu/Shell or return to the ordinary BootOrder processing.
 Reset();FvStatus=EFI_NOT_FOUND;
 if (!setjmp(Escape)) { PolarisRunEntry(TRUE);assert(0); }
 assert(Reads==1 && LastGuid.Data1==0xbbb59c78 && Watchdogs==1 && Stalls==3 && !Runs);
 assert(PolarisEntryStatus==EFI_NOT_FOUND);
 puts("PASS: exact FV/load/application failure stages, source-buffer loading, no silent Shell fallback");
 return 0;
}
