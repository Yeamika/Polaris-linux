// SPDX-License-Identifier: BSD-2-Clause-Patent
// Polaris platform services: explicit GPT targets, verified full raw writes,
// mapped RAM reads and KPSS watchdog. No storage mutation during download.
#include <Uefi.h>
#include <Protocol/BlockIo.h>
#include <Protocol/PartitionInfo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/IoLib.h>
#include "PlatformServices.h"
#ifdef POLARIS_FASTBOOT_DXE
#include "../../platform/BootConsole.h"
STATIC UINT8 FirmwareLog[POLARIS_CONSOLE_LOG_SIZE];
STATIC UINTN FirmwareLogSize,FirmwareLogOffset;
#endif
#define IO_CHUNK 65536U
#define STREAM_ACTION 4U
#define WDT_BASE 0x17980000U
#define WDT_HZ 32768U
STATIC EFI_BLOCK_IO_PROTOCOL *Disk;
STATIC UINT32 Media,BlockSize;
STATIC UINT64 Offset, Total;
STATIC VOID *BounceRaw, *VerifyRaw;
STATIC UINT8 *Bounce, *Verify;
STATIC BOOLEAN Writing;
STATIC UINT64 ReadAddress;
STATIC UINTN ReadSize, ReadOffset;
STATIC UINT32 ReadCrc;
STATIC UINTN Stream; // 1 begin, 2 data, 3 CRC, 4 final OKAY
STATIC EFI_MEMORY_DESCRIPTOR *Map;
STATIC UINTN MapSize, MapStride, MapOffset;
STATIC UINTN WatchdogSeconds;
STATIC CHAR8 Log[32][60],LogCopy[32][60];
STATIC UINTN LogHead,LogCount,LogIndex,LogSaved;
VOID PolarisLog(CONST CHAR8 *Text) {
  AsciiSPrint(Log[LogHead],sizeof(Log[0]),"%a",Text);
  LogHead=(LogHead+1)%32;if (LogCount<32) ++LogCount;
}

VOID PolarisWatchdog(UINTN Seconds) {
  // Same KPSS layout and 32.768 kHz source as the upstream SDM845 driver.
  if (Seconds>31) Seconds=31;
  MmioWrite32(WDT_BASE+8,0);
  if (Seconds) {
    MmioWrite32(WDT_BASE+4,1);
    MmioWrite32(WDT_BASE+0x10,(UINT32)Seconds*WDT_HZ);
    MmioWrite32(WDT_BASE+0x14,(UINT32)Seconds*WDT_HZ);
    MmioWrite32(WDT_BASE+8,1);
  }
  WatchdogSeconds=Seconds;
}
VOID PolarisPet(VOID) { if (WatchdogSeconds) MmioWrite32(WDT_BASE+4,1); }
STATIC VOID Response(FB_CORE *C, CONST CHAR8 *Code, CONST CHAR8 *Text) {
  C->ReplySize=(UINT32)AsciiSPrint(C->Reply,sizeof(C->Reply),"%a%a",Code,Text);
  C->Action=0;
}
STATIC BOOLEAN Hex(CONST CHAR8 *Text, UINT64 *Value) {
  UINTN I=0; UINT64 V=0; UINT8 D;
  if (Text[0]=='0' && Text[1]=='x') Text+=2;
  if (!*Text) return FALSE;
  for (;Text[I];++I) {
    if (I>=16) return FALSE;
    D=Text[I]>='0'&&Text[I]<='9'?Text[I]-'0':Text[I]>='a'&&Text[I]<='f'?Text[I]-'a'+10:
      Text[I]>='A'&&Text[I]<='F'?Text[I]-'A'+10:16;
    if (D==16) return FALSE;
    V=(V<<4)|D;
  }
  *Value=V; return TRUE;
}
STATIC EFI_STATUS FindPartition(CONST CHAR8 *Name, EFI_BLOCK_IO_PROTOCOL **Out) {
  EFI_HANDLE *Handles=NULL; UINTN Count=0,I,J,Len=AsciiStrLen(Name);
  EFI_BLOCK_IO_PROTOCOL *Block, *Found=NULL;
  EFI_PARTITION_INFO_PROTOCOL *Part;
  EFI_STATUS Status;
  if (!Len || Len>36) return EFI_INVALID_PARAMETER;
  for (I=0;I<Len;++I)
    if (!((Name[I]>='a'&&Name[I]<='z')||(Name[I]>='A'&&Name[I]<='Z')||
          (Name[I]>='0'&&Name[I]<='9')||Name[I]=='_'||Name[I]=='-')) return EFI_INVALID_PARAMETER;
  Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiPartitionInfoProtocolGuid,NULL,&Count,&Handles);
  if (EFI_ERROR(Status)) return Status;
  for (I=0;I<Count;++I) {
    if (EFI_ERROR(gBS->HandleProtocol(Handles[I],&gEfiPartitionInfoProtocolGuid,(VOID **)&Part)) ||
        Part->Type!=PARTITION_TYPE_GPT) continue;
    for (J=0;J<Len && Part->Info.Gpt.PartitionName[J]==(CHAR16)Name[J];++J) {}
    if (J!=Len || (Len<36 && Part->Info.Gpt.PartitionName[Len]!=0)) continue;
    Status=gBS->HandleProtocol(Handles[I],&gEfiBlockIoProtocolGuid,(VOID **)&Block);
    if (EFI_ERROR(Status) || Block->Media==NULL || !Block->Media->LogicalPartition ||
        !Block->Media->MediaPresent || Block->Media->BlockSize==0) continue;
    if (Found!=NULL) { FreePool(Handles); return EFI_NO_MAPPING; }
    Found=Block;
  }
  FreePool(Handles);
  if (Found==NULL) return EFI_NOT_FOUND;
  *Out=Found; return EFI_SUCCESS;
}
STATIC BOOLEAN Capacity(EFI_BLOCK_IO_PROTOCOL *Block, UINT64 *Size) {
  EFI_BLOCK_IO_MEDIA *M=Block->Media;
  if (M->LastBlock==MAX_UINT64 || M->LastBlock+1>MAX_UINT64/M->BlockSize) return FALSE;
  *Size=(M->LastBlock+1)*M->BlockSize; return TRUE;
}
STATIC EFI_STATUS Snapshot(VOID) {
  EFI_STATUS Status; UINTN Key; UINT32 Version;
  if (Map) { FreePool(Map); Map=NULL; }
  MapSize=0;MapStride=0;MapOffset=0;
  Status=gBS->GetMemoryMap(&MapSize,NULL,&Key,&MapStride,&Version);
  if (Status!=EFI_BUFFER_TOO_SMALL || MapStride<sizeof(*Map)) return EFI_DEVICE_ERROR;
  MapSize+=MapStride*32;
  Map=AllocatePool(MapSize);
  if (!Map) return EFI_OUT_OF_RESOURCES;
  Status=gBS->GetMemoryMap(&MapSize,Map,&Key,&MapStride,&Version);
  if (EFI_ERROR(Status)) { FreePool(Map);Map=NULL; }
  return Status;
}
STATIC BOOLEAN Readable(UINT64 Address, UINTN Size) {
  UINTN I; UINT64 End;
  if (!Size || Address>MAX_UINT64-Size) return FALSE;
  End=Address+Size;
  for (I=0;Map && I+MapStride<=MapSize;I+=MapStride) {
    EFI_MEMORY_DESCRIPTOR *D=(VOID *)((UINT8 *)Map+I);
    BOOLEAN Ram=D->Type==EfiLoaderCode || D->Type==EfiLoaderData ||
      D->Type==EfiBootServicesCode || D->Type==EfiBootServicesData || D->Type==EfiConventionalMemory ||
      D->Type==EfiRuntimeServicesCode || D->Type==EfiRuntimeServicesData;
    if (Ram && D->NumberOfPages<=MAX_UINT64/4096 && D->PhysicalStart<=Address &&
        D->PhysicalStart<=MAX_UINT64-D->NumberOfPages*4096 &&
        End<=D->PhysicalStart+D->NumberOfPages*4096 &&
        (D->Attribute&(EFI_MEMORY_WB|EFI_MEMORY_WT)) && !(D->Attribute&EFI_MEMORY_RP)) return TRUE;
  }
  return FALSE;
}
VOID PolarisCancel(VOID) {
  Writing=FALSE;Disk=NULL;Stream=0;
  if (BounceRaw) FreePool(BounceRaw);
  if (VerifyRaw) FreePool(VerifyRaw);
  if (Map) FreePool(Map);
  BounceRaw=VerifyRaw=NULL;Bounce=Verify=NULL;Map=NULL;
}
BOOLEAN PolarisBusy(VOID) { return Writing || Stream!=0 || Map!=NULL; }
BOOLEAN PolarisStep(FB_CORE *C) {
  EFI_STATUS Status; UINTN Size; UINT64 CurrentSize;
  if (!Writing) return FALSE;
  Size=(UINTN)((Total-Offset)>IO_CHUNK?IO_CHUNK:Total-Offset);
  if (!Disk->Media->MediaPresent || Disk->Media->MediaId!=Media || Disk->Media->ReadOnly ||
      Disk->Media->BlockSize!=BlockSize || !Capacity(Disk,&CurrentSize) || CurrentSize!=Total) {
    C->Valid=0;
    Response(C,"FAIL","media changed; image may be incomplete"); PolarisCancel(); return TRUE;
  }
  CopyMem(Bounce,C->Data+(UINTN)Offset,Size);
  Status=Disk->WriteBlocks(Disk,Media,Offset/Disk->Media->BlockSize,Size,Bounce);
  if (!EFI_ERROR(Status)) Status=Disk->FlushBlocks(Disk);
  if (!EFI_ERROR(Status)) Status=Disk->ReadBlocks(Disk,Media,Offset/Disk->Media->BlockSize,Size,Verify);
  if (!EFI_ERROR(Status) && CompareMem(Bounce,Verify,Size)!=0) Status=EFI_CRC_ERROR;
  if (EFI_ERROR(Status)) {
    PolarisLog("flash failed; no automatic retry");C->Valid=0;
    Response(C,"FAIL","write/readback failed; image may be incomplete");PolarisCancel();return TRUE;
  }
  Offset+=Size;PolarisPet();
  if (Offset!=Total) return FALSE;
  PolarisLog("flash complete; every block readback verified");
  PolarisCancel();C->Valid=0;
  Response(C,"OKAY","raw partition written and readback verified"); return TRUE;
}
VOID PolarisNext(FB_CORE *C) {
  UINTN I,Bit; UINT8 Value; CHAR8 Row[49];
#ifdef POLARIS_FASTBOOT_DXE
  if (Stream==6) {
    CHAR8 Line[61];UINTN N=0;
    while (FirmwareLogOffset<FirmwareLogSize && N<sizeof(Line)-1) {
      Value=FirmwareLog[FirmwareLogOffset++];
      if (Value=='\r') continue;
      if (Value=='\n') break;
      Line[N++]=Value>=32?Value:' ';
    }
    Line[N]=0;
    if (N || FirmwareLogOffset<FirmwareLogSize) { Response(C,"INFO",Line);C->Action=STREAM_ACTION; }
    else { Stream=0;Response(C,"OKAY",""); }
    return;
  }
#endif
  if (Stream==5) {
    if (LogIndex<LogSaved) {
      Response(C,"INFO",LogCopy[LogIndex++]);C->Action=STREAM_ACTION;
    } else { Stream=0;Response(C,"OKAY",""); }
    return;
  }
  if (Map && !Stream) {
    if (MapOffset+MapStride<=MapSize) {
      EFI_MEMORY_DESCRIPTOR *D=(VOID *)((UINT8 *)Map+MapOffset);
      C->ReplySize=(UINT32)AsciiSPrint(C->Reply,sizeof(C->Reply),"INFOmap %016lx %016lx %u %lx",
        D->PhysicalStart,D->NumberOfPages*4096,D->Type,D->Attribute);
      MapOffset+=MapStride;C->Action=STREAM_ACTION;return;
    }
    PolarisCancel();Response(C,"OKAY","");return;
  }
  if (Stream==1) {
    C->ReplySize=(UINT32)AsciiSPrint(C->Reply,sizeof(C->Reply),"INFOPJ4EDK2 BEGIN %016lx %08x",ReadAddress,(UINT32)ReadSize);
    Stream=2;
  } else if (Stream==2 && ReadOffset<ReadSize) {
    for (I=0;I<16;++I) {
      Value=((volatile UINT8 *)(UINTN)ReadAddress)[ReadOffset+I];
      Row[I*2]="0123456789abcdef"[Value>>4];Row[I*2+1]="0123456789abcdef"[Value&15];
      ReadCrc^=Value;for (Bit=0;Bit<8;++Bit) ReadCrc=(ReadCrc>>1)^(0xedb88320U&(0U-(ReadCrc&1)));
    }
    Row[32]=0;
    // INFO + the former MEM tag needed 65 bytes, truncating the final nibble.
    // Keep the complete wire packet below fastboot's 64-byte response limit.
    C->ReplySize=(UINT32)AsciiSPrint(C->Reply,sizeof(C->Reply),"INFOPJ4EDK2 M %016lx %a",ReadAddress+ReadOffset,Row);
    ReadOffset+=16;
    if (ReadOffset==ReadSize) Stream=3;
  } else if (Stream==3) {
    C->ReplySize=(UINT32)AsciiSPrint(C->Reply,sizeof(C->Reply),"INFOPJ4EDK2 CRC %08x",~ReadCrc);Stream=4;
  } else { PolarisCancel(); Response(C,"OKAY","");return; }
  C->Action=STREAM_ACTION;
}
BOOLEAN PolarisCommand(FB_CORE *C, CONST UINT8 *Packet, UINTN Size) {
  CHAR8 Cmd[65],Text[61],*Name,*Space; EFI_STATUS Status;
  UINT64 Bytes,Address,Count; UINTN Align,I;
  EFI_BLOCK_IO_PROTOCOL *Block;
  if (!Size || Size>64 || C->State!=FB_IDLE) return FALSE;
  for (I=0;I<Size;++I) if (Packet[I]<32 || Packet[I]>126) {
    Response(C,"FAIL","command encoding");return TRUE;
  }
  CopyMem(Cmd,Packet,Size);Cmd[Size]=0;
  if (AsciiStrnCmp(Cmd,"getvar:partition-size:",22)==0 ||
      AsciiStrnCmp(Cmd,"getvar:partition-type:",22)==0) {
    Name=Cmd+22;Status=FindPartition(Name,&Block);
    if (EFI_ERROR(Status) || !Capacity(Block,&Bytes)) Response(C,"FAIL","unknown or ambiguous GPT partition");
    else if (Cmd[17]=='s') { // partition-size
      AsciiSPrint(Text,sizeof(Text),"0x%lx",Bytes);Response(C,"OKAY",Text);
    } else Response(C,"OKAY","raw");
    return TRUE;
  }
  if (AsciiStrCmp(Cmd,"getvar:polaris-flash-format")==0) { Response(C,"OKAY","full-raw-readback");return TRUE; }
  if (AsciiStrCmp(Cmd,"getvar:polaris-debugger")==0) { Response(C,"OKAY","ram-read,map,efi-probe,watchdog");return TRUE; }
  if (AsciiStrCmp(Cmd,"getvar:polaris-watchdog")==0) {
    // Fixed, known KPSS registers only; generic RAM reads still reject MMIO.
    AsciiSPrint(Text,sizeof(Text),"en=%x sts=%x bark=%x bite=%x",
      MmioRead32(WDT_BASE+8),MmioRead32(WDT_BASE+0xc),
      MmioRead32(WDT_BASE+0x10),MmioRead32(WDT_BASE+0x14));
    Response(C,"OKAY",Text);return TRUE;
  }
  if (AsciiStrnCmp(Cmd,"flash:",6)==0) {
    Name=Cmd+6;Status=FindPartition(Name,&Block);
    if (EFI_ERROR(Status) || !Capacity(Block,&Bytes)) { Response(C,"FAIL","unknown or ambiguous GPT partition");return TRUE; }
    if (Block->Media->ReadOnly || Bytes!=C->Valid || !C->Valid ||
        Block->Media->BlockSize>IO_CHUNK || IO_CHUNK%Block->Media->BlockSize ||
        (C->Valid>=4 && C->Data[0]==0x3a && C->Data[1]==0xff && C->Data[2]==0x26 && C->Data[3]==0xed)) {
      Response(C,"FAIL","need full raw partition image; no sparse");return TRUE;
    }
    Align=Block->Media->IoAlign; if (Align<8) Align=8;
    if (Align>IO_CHUNK || (Align&(Align-1))) { Response(C,"FAIL","unsupported disk alignment");return TRUE; }
    BounceRaw=AllocatePool(IO_CHUNK+Align);VerifyRaw=AllocatePool(IO_CHUNK+Align);
    if (!BounceRaw || !VerifyRaw) { PolarisCancel();Response(C,"FAIL","no I/O buffer memory");return TRUE; }
    Bounce=(VOID *)(((UINTN)BounceRaw+Align-1)&~(Align-1));
    Verify=(VOID *)(((UINTN)VerifyRaw+Align-1)&~(Align-1));
    PolarisLog("flash begin; full raw preflight accepted");
    Disk=Block;Media=Block->Media->MediaId;BlockSize=Block->Media->BlockSize;Offset=0;Total=Bytes;Writing=TRUE;
    C->ReplySize=C->Action=0;return TRUE;
  }
  if (AsciiStrCmp(Cmd,"oem log")==0) {
    UINTN I;LogSaved=LogCount;LogIndex=0;
    for (I=0;I<LogSaved;++I) CopyMem(LogCopy[I],Log[(LogHead+32-LogCount+I)%32],60);
    Stream=5;PolarisNext(C);return TRUE;
  }
#ifdef POLARIS_FASTBOOT_DXE
  if (AsciiStrCmp(Cmd,"oem firmware-log")==0) {
    EFI_TPL Tpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
    FirmwareLogSize=PolarisBootConsoleRead(FirmwareLog,sizeof(FirmwareLog));
    gBS->RestoreTPL(Tpl);
    FirmwareLogOffset=0;Stream=6;PolarisNext(C);return TRUE;
  }
#endif
  if (AsciiStrCmp(Cmd,"oem memory-map")==0) {
    if (EFI_ERROR(Snapshot())) Response(C,"FAIL","memory map unavailable");
    else PolarisNext(C);
    return TRUE;
  }
  if (AsciiStrnCmp(Cmd,"oem read-ram ",13)==0) {
    Name=Cmd+13;Space=Name;
    while (*Space && *Space!=' ') ++Space;
    if (!*Space) { Response(C,"FAIL","read-ram address size (hex)");return TRUE; }
    *Space++=0;
    if (!Hex(Name,&Address)||!Hex(Space,&Count)||!Count||Count>65536||((Address|Count)&15)||
        EFI_ERROR(Snapshot())||!Readable(Address,(UINTN)Count)) {
      PolarisCancel();Response(C,"FAIL","need aligned mapped RAM, up to 64KiB");return TRUE;
    }
    ReadAddress=Address;ReadSize=(UINTN)Count;ReadOffset=0;ReadCrc=MAX_UINT32;Stream=1;
    PolarisNext(C);return TRUE;
  }
  return FALSE;
}
