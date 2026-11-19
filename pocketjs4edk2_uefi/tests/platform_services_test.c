// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#undef NULL
#define POLARIS_FASTBOOT_DXE
static unsigned long long ConsoleStorage[2052];
#define POLARIS_CONSOLE_STATE_ADDRESS ((UINTN)ConsoleStorage)
#include "../efi/Recovery/PlatformServices.c"
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiPartitionInfoProtocolGuid,gEfiBlockIoProtocolGuid;
static EFI_BOOT_SERVICES Bs;
static EFI_PARTITION_INFO_PROTOCOL Part;
static EFI_BLOCK_IO_PROTOCOL Block;
static EFI_BLOCK_IO_MEDIA DiskMedia;
static unsigned char Storage[131072],Payload[131072];
static unsigned Writes,Reads,Flushes,Count,FailRead,CorruptRead;
static unsigned MIO[8];
static EFI_MEMORY_DESCRIPTOR Descriptor;
static EFI_TPL EFIAPI Raise(EFI_TPL Tpl) { assert(Tpl==TPL_HIGH_LEVEL);return TPL_APPLICATION; }
static VOID EFIAPI Restore(EFI_TPL Tpl) { assert(Tpl==TPL_APPLICATION); }
VOID *EFIAPI AllocatePool(UINTN Size) { return malloc(Size); }
VOID EFIAPI FreePool(VOID *Ptr) { free(Ptr); }
VOID *EFIAPI CopyMem(VOID *Dst,CONST VOID *Src,UINTN Size) { return memmove(Dst,Src,Size); }
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN Size) { return memcmp(A,B,Size); }
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S) { return strlen(S); }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) { return strcmp(A,B); }
INTN EFIAPI AsciiStrnCmp(CONST CHAR8 *A,CONST CHAR8 *B,UINTN N) { return strncmp(A,B,N); }
UINTN EFIAPI AsciiSPrint(CHAR8 *Dst,UINTN Size,CONST CHAR8 *Fmt,...) {
 char F[256];unsigned I=0,J=0;va_list Ap;
 for (;Fmt[I];++I) { F[J++]=Fmt[I];if(Fmt[I]=='%' && Fmt[I+1]=='a') { F[J++]='s';++I; } }
 F[J]=0;va_start(Ap,Fmt);int R=vsnprintf(Dst,Size,F,Ap);va_end(Ap);
 return R<0?0:(UINTN)R>=Size?Size-1:(UINTN)R;
}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {
 assert(Address>=WDT_BASE && Address<WDT_BASE+32);MIO[(Address-WDT_BASE)/4]=Value;return Value;
}
UINT32 EFIAPI MmioRead32(UINTN Address) {
 assert(Address>=WDT_BASE && Address<WDT_BASE+32);return MIO[(Address-WDT_BASE)/4];
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Key,UINTN *N,EFI_HANDLE **Handles) {
 (void)Type;(void)Guid;(void)Key;*N=Count;*Handles=malloc(sizeof(EFI_HANDLE)*Count);
 for (unsigned I=0;I<Count;++I) (*Handles)[I]=(EFI_HANDLE)(UINTN)(I+1);
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *Guid,VOID **Out) {
 (void)H;*Out=Guid==&gEfiPartitionInfoProtocolGuid?(VOID *)&Part:(VOID *)&Block;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_BLOCK_IO_PROTOCOL *This,UINT32 Id,EFI_LBA Lba,UINTN Size,VOID *Data) {
 assert(This==&Block && Id==7 && Size%512==0 && Lba*512+Size<=sizeof(Storage));
 ++Reads;if(FailRead)return EFI_DEVICE_ERROR;memcpy(Data,Storage+Lba*512,Size);
 if(CorruptRead)((unsigned char *)Data)[0]^=1;
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(EFI_BLOCK_IO_PROTOCOL *This,UINT32 Id,EFI_LBA Lba,UINTN Size,VOID *Data) {
 assert(This==&Block && Id==7 && Size%512==0 && Lba*512+Size<=sizeof(Storage));
 assert(((UINTN)Data&4095)==0);++Writes;memcpy(Storage+Lba*512,Data,Size);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Flush(EFI_BLOCK_IO_PROTOCOL *This) { (void)This;++Flushes;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI GetMap(UINTN *Size,EFI_MEMORY_DESCRIPTOR *Data,UINTN *Key,UINTN *Stride,UINT32 *Version) {
 *Stride=sizeof(Descriptor);*Key=1;*Version=1;
 if(!Data || *Size<sizeof(Descriptor)) { *Size=sizeof(Descriptor);return EFI_BUFFER_TOO_SMALL; }
 memcpy(Data,&Descriptor,sizeof(Descriptor));*Size=sizeof(Descriptor);return EFI_SUCCESS;
}
static void Init(FB_CORE *C) {
 PolarisCancel();memset(&Part,0,sizeof(Part));memset(&Block,0,sizeof(Block));memset(&DiskMedia,0,sizeof(DiskMedia));
 Part.Type=PARTITION_TYPE_GPT;memcpy(Part.Info.Gpt.PartitionName,L"",0);
 const char *Name="boot";for(unsigned I=0;Name[I];++I) Part.Info.Gpt.PartitionName[I]=Name[I];
 DiskMedia.MediaId=7;DiskMedia.MediaPresent=TRUE;DiskMedia.LogicalPartition=TRUE;
 DiskMedia.BlockSize=512;DiskMedia.LastBlock=255;DiskMedia.IoAlign=4096;
 Block.Media=&DiskMedia;Block.ReadBlocks=Read;Block.WriteBlocks=Write;Block.FlushBlocks=Flush;
 Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=Handle;Bs.GetMemoryMap=GetMap;gBS=&Bs;
 Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;
 Writes=Reads=Flushes=FailRead=CorruptRead=0;Count=1;FbInit(C,Payload,sizeof(Payload));C->Valid=sizeof(Payload);
 for(unsigned I=0;I<sizeof(Payload);++I)Payload[I]=(unsigned char)I;
 memset(&Descriptor,0,sizeof(Descriptor));Descriptor.Type=EfiBootServicesData;
 Descriptor.PhysicalStart=(UINTN)Payload;Descriptor.NumberOfPages=32;Descriptor.Attribute=EFI_MEMORY_WB;
}
static void Cmd(FB_CORE *C,const char *Text) { assert(PolarisCommand(C,(const UINT8 *)Text,strlen(Text))); }
int main(void) {
 FB_CORE C;char Command[65];UINT64 V;
 Init(&C);memset(ConsoleStorage,0,sizeof(ConsoleStorage));
 PolarisBootConsole()->Magic=POLARIS_CONSOLE_MAGIC;
 const char *Early="PrePi\r\nBDS\n";
 for(unsigned I=0;Early[I];++I)PolarisBootConsoleByte(Early[I]);
 Cmd(&C,"oem firmware-log");assert(!strcmp(C.Reply,"INFOPrePi"));
 PolarisBootConsoleByte('!'); // Streaming uses the frozen snapshot.
 PolarisNext(&C);assert(!strcmp(C.Reply,"INFOBDS"));
 PolarisNext(&C);assert(!strcmp(C.Reply,"OKAY") && !PolarisBusy());
 Cmd(&C,"oem firmware-log");PolarisCancel();assert(!PolarisBusy());
 Init(&C);Cmd(&C,"getvar:partition-size:boot");assert(!strcmp(C.Reply,"OKAY0x20000"));
 Cmd(&C,"getvar:partition-type:boot");assert(!strcmp(C.Reply,"OKAYraw"));
 Cmd(&C,"flash:boot");assert(PolarisBusy() && Writes==0 && C.ReplySize==0);
 assert(!PolarisStep(&C) && Writes==1);assert(PolarisStep(&C) && Writes==2 && Reads==2 && Flushes==2);
 assert(!memcmp(Payload,Storage,sizeof(Payload)) && !strncmp(C.Reply,"OKAY",4) && !PolarisBusy() && C.Valid==0);
 Init(&C);--C.Valid;Cmd(&C,"flash:boot");assert(!strncmp(C.Reply,"FAIL",4) && !Writes);
 Init(&C);Count=2;Cmd(&C,"flash:boot");assert(!strncmp(C.Reply,"FAIL",4) && !Writes);
 Init(&C);Cmd(&C,"flash:../boot");assert(!strncmp(C.Reply,"FAIL",4) && !Writes);
 Init(&C);Payload[0]=0x3a;Payload[1]=0xff;Payload[2]=0x26;Payload[3]=0xed;
 Cmd(&C,"flash:boot");assert(!strncmp(C.Reply,"FAIL",4) && !Writes);
 Init(&C);CorruptRead=1;Cmd(&C,"flash:boot");assert(PolarisStep(&C));assert(!strncmp(C.Reply,"FAIL",4) && Writes==1 && !PolarisBusy());
 Init(&C);Cmd(&C,"flash:boot");PolarisCancel();assert(!PolarisStep(&C) && !Writes);
 Init(&C);Cmd(&C,"flash:boot");++DiskMedia.MediaId;assert(PolarisStep(&C) && !Writes && !strncmp(C.Reply,"FAIL",4));
 assert(!C.Valid);
 Init(&C);Cmd(&C,"flash:boot");assert(!PolarisStep(&C));--DiskMedia.LastBlock;
 assert(PolarisStep(&C) && Writes==1 && !C.Valid && !PolarisBusy() && !strncmp(C.Reply,"FAIL",4));
 Init(&C);assert(PolarisCommand(&C,(const UINT8 *)"flash:boot\0ignored",18));
 assert(!strncmp(C.Reply,"FAIL",4) && !PolarisBusy() && !Writes);
 Init(&C);assert(!Hex("10000000000000000",&V) && !Hex("1g",&V));
 Cmd(&C,"oem memory-map");assert(!strncmp(C.Reply,"INFOmap",7));PolarisNext(&C);assert(!strcmp(C.Reply,"OKAY"));
 Init(&C);snprintf(Command,sizeof(Command),"oem read-ram %lx 20",(unsigned long)Payload);
 Cmd(&C,Command);assert(!strncmp(C.Reply,"INFOPJ4EDK2 BEGIN",17));
 PolarisNext(&C);assert(!strncmp(C.Reply,"INFOPJ4EDK2 M ",13));
 assert(C.ReplySize==63 && strlen(C.Reply)==63);
 assert(!strcmp(C.Reply+C.ReplySize-32,"000102030405060708090a0b0c0d0e0f"));
 PolarisNext(&C);PolarisNext(&C);assert(!strncmp(C.Reply,"INFOPJ4EDK2 CRC",15));
 PolarisNext(&C);assert(!strcmp(C.Reply,"OKAY") && !PolarisBusy());
 Init(&C);Descriptor.Type=EfiMemoryMappedIO;Cmd(&C,Command);assert(!strncmp(C.Reply,"FAIL",4));
 PolarisWatchdog(30);assert(MIO[2]==1 && MIO[5]==30*32768 && MIO[1]==1);
 Cmd(&C,"getvar:polaris-watchdog");assert(!strncmp(C.Reply,"OKAYen=1 ",9));
 PolarisPet();PolarisWatchdog(0);assert(MIO[2]==0);
 puts("PASS: GPT uniqueness, aligned full raw flash/readback, failure/cancel, mapped RAM streaming and watchdog");
 return 0;
}
