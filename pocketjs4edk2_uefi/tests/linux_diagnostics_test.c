// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../efi/LinuxBoot/Diagnostics.c"

static BOOLEAN RenderHandled;
static unsigned RenderWrites,RenderEnds;
EFI_STATUS PolarisLinuxOutputBegin(EFI_SYSTEM_TABLE *System) { (VOID)System;return EFI_SUCCESS; }
BOOLEAN PolarisLinuxOutputWrite(CONST CHAR16 *Text) { (VOID)Text;++RenderWrites;return RenderHandled; }
VOID PolarisLinuxOutputEnd(VOID) { ++RenderEnds; }

EFI_GUID gEfiFileInfoGuid,gEfiSimpleFileSystemProtocolGuid,gEfiEventExitBootServicesGuid;
static EFI_TPL CurrentTpl=TPL_APPLICATION;
static EFI_STATUS WriteStatus,EventStatus;
static unsigned Writes,Flushes,Closes,EventCloses,OriginalCalls;
static size_t Used;
static char Disk[LOG_LIMIT];
static EFI_FILE_PROTOCOL Root,File;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Out,Err;
static EFI_SYSTEM_TABLE Table;
EFI_SYSTEM_TABLE *gST=&Table;
UINTN EFIAPI Print(CONST CHAR16 *Format,...) { (VOID)Format;return 0; }

VOID *EFIAPI AllocatePool(UINTN Size) { return malloc(Size); }
VOID EFIAPI FreePool(VOID *Buffer) { free(Buffer); }
static EFI_TPL EFIAPI Raise(EFI_TPL New) { assert(!Exited);EFI_TPL Old=CurrentTpl;CurrentTpl=New;return Old; }
static VOID EFIAPI RestoreTpl(EFI_TPL Old) { CurrentTpl=Old; }
static EFI_STATUS EFIAPI Original(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *Text) {
  (VOID)This;(VOID)Text;++OriginalCalls;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *This) { if(This==&File) ++Closes;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Flush(EFI_FILE_PROTOCOL *This) {
  assert(This==&File && CurrentTpl<=TPL_CALLBACK && !Exited);++Flushes;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(EFI_FILE_PROTOCOL *This,UINTN *N,VOID *Data) {
  assert(This==&File && CurrentTpl<=TPL_CALLBACK && !Exited);++Writes;
  if(EFI_ERROR(WriteStatus)) return WriteStatus;
  assert(Used+*N<=sizeof(Disk));memcpy(Disk+Used,Data,*N);Used+=*N;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GetInfo(EFI_FILE_PROTOCOL *This,EFI_GUID *Guid,UINTN *N,VOID *Data) {
  assert(This==&File && Guid==&gEfiFileInfoGuid);
  *N=sizeof(EFI_FILE_INFO)+16;
  if(!Data) return EFI_BUFFER_TOO_SMALL;
  memset(Data,0,*N);
  EFI_FILE_INFO *Info=Data;
  Info->FileSize=1234;
  // A FAT date produced without an RTC: the size-only update must preserve
  // it instead of asking the filesystem to validate and rewrite it.
  Info->CreateTime.Year=Info->ModificationTime.Year=1980;
  Info->LastAccessTime.Year=1980;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetInfo(EFI_FILE_PROTOCOL *This,EFI_GUID *Guid,UINTN N,VOID *Data) {
  assert(This==&File && Guid==&gEfiFileInfoGuid && N>=sizeof(EFI_FILE_INFO));
  EFI_FILE_INFO *Info=Data;
  EFI_TIME Zero={0};
  assert(!Info->FileSize);
  assert(!memcmp(&Info->CreateTime,&Zero,sizeof(Zero)));
  assert(!memcmp(&Info->LastAccessTime,&Zero,sizeof(Zero)));
  assert(!memcmp(&Info->ModificationTime,&Zero,sizeof(Zero)));
  Used=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *This,EFI_FILE_PROTOCOL **Child,CHAR16 *Path,UINT64 Mode,UINT64 Attr) {
  assert(This==&Root && Path && !Attr && (Mode&EFI_FILE_MODE_CREATE));*Child=&File;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenVolume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,EFI_FILE_PROTOCOL **Child) {
  assert(This==&Fs);*Child=&Root;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE Volume,EFI_GUID *Guid,VOID **OutPtr) {
  assert(Volume==(VOID *)1 && Guid==&gEfiSimpleFileSystemProtocolGuid);*OutPtr=&Fs;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Event(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,
                             CONST EFI_GUID *Guid,EFI_EVENT *OutEvent) {
  assert(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY && Notify==ExitBoot && !Context);
  assert(Guid==&gEfiEventExitBootServicesGuid);
  if(EFI_ERROR(EventStatus)) return EventStatus;
  *OutEvent=(VOID *)2;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E) { assert(E==(VOID *)2);++EventCloses;return EFI_SUCCESS; }
static EFI_BOOT_SERVICES Bs={.HandleProtocol=Handle,.CreateEventEx=Event,.CloseEvent=CloseEvent,
                            .RaiseTPL=Raise,.RestoreTPL=RestoreTpl};
EFI_BOOT_SERVICES *gBS=&Bs;
static void Setup(void) {
  Root=(EFI_FILE_PROTOCOL){.Open=Open,.Close=Close};
  File=(EFI_FILE_PROTOCOL){.GetInfo=GetInfo,.SetInfo=SetInfo,.Write=Write,.Flush=Flush,.Close=Close};
  Fs=(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL){.OpenVolume=OpenVolume};
  Out=(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){.OutputString=Original};Err=Out;
  Table.ConOut=&Out;Table.StdErr=&Err;
  LogFile=NULL;ExitEvent=NULL;Exited=FALSE;Writing=FALSE;ConsoleOut=ConsoleErr=NULL;
  CurrentTpl=TPL_APPLICATION;Used=0;Writes=Flushes=Closes=EventCloses=OriginalCalls=0;
  WriteStatus=EventStatus=EFI_SUCCESS;RenderHandled=FALSE;RenderWrites=RenderEnds=0;
}
int main(void) {
  Setup();assert(PolarisLinuxConsoleBegin(&Table)==EFI_SUCCESS);assert(PolarisLinuxLogBegin((VOID *)1)==EFI_SUCCESS);
  assert(PolarisLinuxLogBegin((VOID *)1)==EFI_ALREADY_STARTED);
  size_t Start=Used;
  assert(Out.OutputString(&Out,L"EFI stub: Exiting boot services...\n")==EFI_SUCCESS);
  assert(Used==Start+35 && !memcmp(Disk+Start,"EFI stub: Exiting boot services...\n",35));
  CurrentTpl=TPL_CALLBACK;Out.OutputString(&Out,L"callback-level application\n");
  assert(Used>Start+35);
  unsigned Before=Writes,BeforeFlush=Flushes;
  CurrentTpl=TPL_NOTIFY;Err.OutputString(&Err,L"high-TPL callback");
  assert(Writes==Before && Flushes==BeforeFlush && OriginalCalls==3);
  ExitBoot((VOID *)2,NULL);
  assert(Out.OutputString==Capture && Err.OutputString==Capture && Exited);
  PolarisLinuxLogEnd();assert(Writes==Before && Flushes==BeforeFlush && !Closes && !EventCloses);

  Setup();RenderHandled=TRUE;
  assert(PolarisLinuxConsoleBegin(&Table)==EFI_SUCCESS);assert(PolarisLinuxLogBegin((VOID *)1)==EFI_SUCCESS);
  PolarisLinuxLogSeal();assert(!LogFile && Closes==1 && !EventCloses && !RenderEnds);
  Before=Writes;BeforeFlush=Flushes;
  Out.OutputString(&Out,L"after storage seal");
  assert(RenderWrites==1 && !OriginalCalls && Writes==Before && Flushes==BeforeFlush);
  ExitBoot((VOID *)2,NULL);
  Err.OutputString(&Err,L"later EBS callback");
  PolarisLinuxLogEnd();
  assert(RenderWrites==2 && !OriginalCalls && !RenderEnds && !EventCloses && Closes==1);
  assert(Out.OutputString==Capture && Err.OutputString==Capture);

  Setup();assert(PolarisLinuxConsoleBegin(&Table)==EFI_SUCCESS);assert(PolarisLinuxLogBegin((VOID *)1)==EFI_SUCCESS);
  for(unsigned I=0;I<LOG_LIMIT/4+100;++I) Out.OutputString(&Out,L"abcd");
  assert(Used==LOG_LIMIT);
  PolarisLinuxLogEnd();assert(Closes==1 && EventCloses==1 && Out.OutputString==Original);

  Setup();assert(PolarisLinuxConsoleBegin(&Table)==EFI_SUCCESS);assert(PolarisLinuxLogBegin((VOID *)1)==EFI_SUCCESS);
  Start=Used;WriteStatus=EFI_DEVICE_ERROR;assert(Out.OutputString(&Out,L"disk failure")==EFI_SUCCESS && Used==Start);
  PolarisLinuxLogEnd();assert(Closes==1 && EventCloses==1);

  Setup();EventStatus=EFI_OUT_OF_RESOURCES;
  assert(PolarisLinuxConsoleBegin(&Table)==EFI_OUT_OF_RESOURCES);
  assert(!LogFile && !ExitEvent && Closes==0 && Out.OutputString==Original);
  puts("PASS: bounded Linux ESP log, console forwarding, failed writes and bounded output retained across storage close and EBS");
  return 0;
}
