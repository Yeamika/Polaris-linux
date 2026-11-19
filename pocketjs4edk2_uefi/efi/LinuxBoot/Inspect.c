// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only ESP evidence exporter. It does not boot a kernel or write storage.
#include "../Common/Loader.h"
#include "ExitTrace.h"
#include "NativeJournal.h"
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>

#define CAPACITY (256*1024)
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
  EFI_HANDLE Volume;
  EFI_STATUS Status;
  CHAR8 *Snapshot;
  VOID *Data;
  UINTN Used=0,Bytes,I,N;
  CONST CHAR16 *Files[]={L"boot-status.txt",L"diagnostics\\dmesg.txt",L"diagnostics\\usb.txt",
                         L"diagnostics\\devices.txt",L"diagnostics\\cmdline.txt"};
  CHAR16 Path[128];
  POLARIS_LINUX_EXIT_TRACE Trace;
  (VOID)System;
  Status=PolarisFindVolume(Image,L"\\EFI\\LinuxBoot\\Image.efi",&Volume);
  if (EFI_ERROR(Status)) return Status;
  Snapshot=AllocatePages(EFI_SIZE_TO_PAGES(CAPACITY));
  if (!Snapshot) return EFI_OUT_OF_RESOURCES;
  ZeroMem(Snapshot,CAPACITY);
  Used=AsciiSPrint(Snapshot,CAPACITY,"POLARIS_LINUX_DIAGNOSTICS_1\n");
  Status=PolarisLinuxExitTraceRead(&Trace);
  Used+=AsciiSPrint(Snapshot+Used,CAPACITY-Used,"EXIT_TRACE STATUS %lx\n",(UINT64)Status);
  if (!EFI_ERROR(Status))
    Used+=AsciiSPrint(Snapshot+Used,CAPACITY-Used,"EXIT_TRACE SEQUENCE %lu PHASE %u CALLS %u EBS %lx RETURN %lx\n",
                     Trace.Sequence,Trace.Phase,Trace.Calls,Trace.ExitStatus,Trace.ReturnStatus);
  for (I=0;I<ARRAY_SIZE(Files);++I) {
    UnicodeSPrint(Path,sizeof(Path),L"\\EFI\\LinuxBoot\\%s",Files[I]);
    Status=PolarisReadFile(Volume,Path,192*1024,&Data,&Bytes);
    Used+=AsciiSPrint(Snapshot+Used,CAPACITY-Used,"FILE %s STATUS %lx BYTES %u\n",Files[I],(UINT64)Status,Bytes);
    if (EFI_ERROR(Status)) continue;
    if (Bytes>CAPACITY-Used-256) { FreePool(Data);break; }
    CopyMem(Snapshot+Used,Data,Bytes);Used+=Bytes;
    Snapshot[Used++]='\n';
    if(I==0) {
      CONST CHAR8 Label[]="NATIVE_JOURNAL_ADDRESS=";
      UINTN Pos,J;UINT64 Address=0;BOOLEAN Found=FALSE;
      POLARIS_NATIVE_JOURNAL Native;
      for(Pos=0;Pos+sizeof(Label)-1+16<=Bytes;++Pos) {
        if(CompareMem((UINT8 *)Data+Pos,Label,sizeof(Label)-1)) continue;
        Address=0;Found=TRUE;
        for(J=0;J<16;++J) {
          UINT8 Ch=((UINT8 *)Data)[Pos+sizeof(Label)-1+J];UINTN Value;
          if(Ch>='0' && Ch<='9') Value=Ch-'0';
          else if(Ch>='A' && Ch<='F') Value=Ch-'A'+10;
          else if(Ch>='a' && Ch<='f') Value=Ch-'a'+10;
          else {Found=FALSE;break;}
          Address=(Address<<4)|Value;
        }
        if(Found) break;
      }
      if(Found) {
        ZeroMem(&Native,sizeof(Native));Status=PolarisNativeJournalRead(Address,&Native);
        Used+=AsciiSPrint(Snapshot+Used,CAPACITY-Used,
          "NATIVE_JOURNAL STATUS %lx ADDRESS %lx MAGIC %lx PHASE %u EL %lx SCTLR %lx ENTRY %lx DTB %lx\n",
          (UINT64)Status,Address,Native.Magic,Native.Phase,Native.El,Native.Sctlr,Native.Entry,Native.Dtb);
      }
    }
    // A small tail fits the backend's bounded RAM log; the checked RAM reader
    // retrieves the complete snapshot, including unabridged early messages.
    if (I==0) {
      CHAR16 Tail[768];
      N=Bytes<ARRAY_SIZE(Tail)-1?Bytes:ARRAY_SIZE(Tail)-1;
      for (UINTN J=0;J<N;++J) Tail[J]=((UINT8 *)Data)[Bytes-N+J];
      Tail[N]=0;Print(L"LINUX EFI LOG TAIL\r\n%s\r\n",Tail);
    }
    FreePool(Data);
  }
  // Keep this data-only BootServices allocation for the host debugger after
  // Exit() unloads the tool. No code pointer remains. Reboot reclaims it.
  Used=ALIGN_VALUE(Used,16);
  Print(L"LINUX_DIAG_ADDRESS=%016lx\r\n",(UINT64)(UINTN)Snapshot);
  Print(L"LINUX_DIAG_BYTES=%08x\r\n",(UINT32)Used);
  return EFI_SUCCESS;
}
