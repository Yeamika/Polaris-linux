// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Acpi.h"
#include "WindowsCsrt.h"
#include <Guid/Acpi.h>
#include <Guid/SmBios.h>
#include <IndustryStandard/Acpi.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/AcpiTable.h>
#include <Protocol/EFIChipInfo.h>
#include <Protocol/EFIPlatformInfo.h>
#include <Protocol/EFISmem.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

STATIC CONST CHAR8 *PreparationStage="ACPI table discovery";
CONST CHAR8 *WindowsPlatformStage(VOID) {return PreparationStage;}

STATIC EFI_STATUS PublishedDsdt(EFI_ACPI_SDT_HEADER *Dsdt,BOOLEAN Csrt) {
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER *Rsdp=NULL;
  UINT8 *Xsdt,*Table;UINTN Length,I,FadtCount=0,CsrtCount=0;UINT64 Address;
  EFI_STATUS Status=EfiGetSystemConfigurationTable(&gEfiAcpi20TableGuid,(VOID **)&Rsdp);
  if(EFI_ERROR(Status)) return Status;
  if(!Rsdp || CompareMem(Rsdp,"RSD PTR ",8) || Rsdp->Revision<2 ||
     CalculateSum8((UINT8 *)Rsdp,20) || Rsdp->Length<36 || Rsdp->Length>4096 ||
     CalculateSum8((UINT8 *)Rsdp,Rsdp->Length) || !Rsdp->XsdtAddress) return EFI_COMPROMISED_DATA;
  Xsdt=(UINT8 *)(UINTN)Rsdp->XsdtAddress;
  Length=ReadUnaligned32((UINT32 *)(Xsdt+4));
  if(CompareMem(Xsdt,"XSDT",4) || Length<36 || Length>36+8*128 ||
     (Length-36)%8 || CalculateSum8(Xsdt,Length)) return EFI_COMPROMISED_DATA;
  for(I=36;I<Length;I+=8) {
    Table=(UINT8 *)(UINTN)ReadUnaligned64((UINT64 *)(Xsdt+I));
    if(!Table) return EFI_COMPROMISED_DATA;
    if(!CompareMem(Table,"FACP",4)) {
      if(ReadUnaligned32((UINT32 *)(Table+4))<148) return EFI_COMPROMISED_DATA;
      Address=ReadUnaligned64((UINT64 *)(Table+140));
      if(!Address) Address=ReadUnaligned32((UINT32 *)(Table+40));
      if(Address!=(UINTN)Dsdt) return EFI_COMPROMISED_DATA;
      ++FadtCount;
    }
    if(!CompareMem(Table,"CSRT",4)) ++CsrtCount;
  }
  return FadtCount==1 && CsrtCount==(Csrt?1U:0U)?EFI_SUCCESS:EFI_COMPROMISED_DATA;
}

STATIC EFI_STATUS DescriptionTables(EFI_ACPI_SDT_PROTOCOL **Sdt,
                                    EFI_ACPI_SDT_HEADER **Dsdt,UINTN *Key,BOOLEAN *Csrt) {
  STATIC CONST UINT32 Required[]={SIGNATURE_32('F','A','C','P'),SIGNATURE_32('A','P','I','C'),
    SIGNATURE_32('G','T','D','T'),SIGNATURE_32('I','O','R','T'),SIGNATURE_32('P','P','T','T')};
  EFI_ACPI_TABLE_VERSION Version;EFI_ACPI_SDT_HEADER *Table;
  EFI_STATUS Status;UINTN I,J,TableKey,Counts[ARRAY_SIZE(Required)]={0};
  VOID *Smbios=NULL;UINTN DsdtCount=0,CsrtCount=0;
  *Dsdt=NULL;*Csrt=FALSE;
  if(EFI_ERROR(EfiGetSystemConfigurationTable(&gEfiSmbios3TableGuid,&Smbios)) &&
     EFI_ERROR(EfiGetSystemConfigurationTable(&gEfiSmbiosTableGuid,&Smbios))) return EFI_NOT_FOUND;
  if(!Smbios) return EFI_COMPROMISED_DATA;
  Status=gBS->LocateProtocol(&gEfiAcpiSdtProtocolGuid,NULL,(VOID **)Sdt);
  if(EFI_ERROR(Status)) return Status;
  for(I=0;I<128;++I) {
    Status=(*Sdt)->GetAcpiTable(I,&Table,&Version,&TableKey);
    if(Status==EFI_NOT_FOUND) break;
    if(EFI_ERROR(Status)) return Status;
    if(!Table || Table->Length<sizeof(*Table) || Table->Length>1024*1024)
      return EFI_COMPROMISED_DATA;
    // FACS is not an ACPI description header and has no checksum byte.
    if(Table->Signature==SIGNATURE_32('F','A','C','S')) continue;
    if(CalculateSum8((UINT8 *)Table,Table->Length)!=0) return EFI_COMPROMISED_DATA;
    for(J=0;J<ARRAY_SIZE(Required);++J) if(Table->Signature==Required[J]) ++Counts[J];
    if(Table->Signature==SIGNATURE_32('D','S','D','T')) {
      *Dsdt=Table;*Key=TableKey;++DsdtCount;
    }
    if(Table->Signature==SIGNATURE_32('C','S','R','T')) {
      ++CsrtCount;
      // Reject an incompatible pre-existing table instead of installing a duplicate.
      if(Table->Length!=sizeof(WindowsCsrt) || CompareMem(Table,WindowsCsrt,sizeof(WindowsCsrt)))
        return EFI_INCOMPATIBLE_VERSION;
    }
  }
  if(I==128 || DsdtCount!=1 || CsrtCount>1) return EFI_COMPROMISED_DATA;
  for(J=0;J<ARRAY_SIZE(Required);++J) if(Counts[J]!=1) return EFI_NOT_FOUND;
  *Csrt=CsrtCount==1;
  return PublishedDsdt(*Dsdt,*Csrt);
}

STATIC EFI_STATUS Region(CONST CHAR8 *Name,UINT64 *Base,UINT64 *Size,BOOLEAN Optional) {
  ARM_MEMORY_REGION_DESCRIPTOR_EX Map;EFI_STATUS Status;
  Status=LocateMemoryMapAreaByName((CHAR8 *)Name,&Map);
  if(Status==EFI_NOT_FOUND && Optional) { *Base=0;*Size=0;return EFI_SUCCESS; }
  if(EFI_ERROR(Status)) return Status;
  if(!Map.Length || Map.Address>MAX_UINT32 || Map.Length>MAX_UINT32 ||
     Map.Address>MAX_UINT32-Map.Length) return EFI_COMPROMISED_DATA;
  *Base=Map.Address;*Size=Map.Length;return EFI_SUCCESS;
}

STATIC EFI_STATUS PlatformInfo(WINDOWS_PLATFORM_INFO *Info) {
  EFI_CHIPINFO_PROTOCOL *Chip;EFI_SMEM_PROTOCOL *Smem;EFI_PLATFORMINFO_PROTOCOL *Platform;
  EFI_PLATFORMINFO_PLATFORM_INFO_TYPE Board;EFIChipInfoIdType Id;
  EFIChipInfoVersionType Version;EFIChipInfoFamilyType Family;EFIChipInfoModemType Modem;
  EFIChipInfoSerialNumType Serial;EFIChipInfoQFPROMChipIdType Fuse;
  VOID *Shared=NULL;UINT32 SharedSize=0;UINT64 Base,Size,SmemBase,SmemSize;EFI_STATUS Status;
  ZeroMem(Info,sizeof(*Info));
#define GET(Call) do {Status=(Call);if(EFI_ERROR(Status)) return Status;} while(0)
  GET(gBS->LocateProtocol(&gEfiChipInfoProtocolGuid,NULL,(VOID **)&Chip));
  GET(gBS->LocateProtocol(&gQcomSMEMProtocolGuid,NULL,(VOID **)&Smem));
  GET(gBS->LocateProtocol(&gEfiPlatformInfoProtocolGuid,NULL,(VOID **)&Platform));
  if(!Chip || !Smem || !Platform) return EFI_COMPROMISED_DATA;
  // Use the protocol's actual 32-bit output types. Upstream casts UINT16
  // addresses to enum/UINT32 pointers for family/modem, overwriting the stack.
  GET(Chip->GetChipId(Chip,&Id));GET(Chip->GetChipVersion(Chip,&Version));
  GET(Chip->GetChipFamily(Chip,&Family));GET(Chip->GetModemSupport(Chip,&Modem));
  GET(Chip->GetSerialNumber(Chip,&Serial));GET(Chip->GetQFPROMChipId(Chip,&Fuse));
  GET(Chip->GetChipIdString(Chip,Info->Chip,sizeof(Info->Chip)));
  GET(Platform->GetPlatformInfo(Platform,&Board));
  GET(Smem->GetFunc(137,&SharedSize,&Shared));
  GET(Region("SMEM",&SmemBase,&SmemSize,FALSE));
  if(!Id || Id==MAX_UINT32 || !Version || Version==MAX_UINT32 || !SharedSize ||
     SharedSize>SmemSize || (UINTN)Shared<SmemBase ||
     (UINTN)Shared>SmemBase+SmemSize-SharedSize) return EFI_COMPROMISED_DATA;
  Info->Value[0]=Id;Info->Value[1]=Version;Info->Value[2]=Version>>16;
  Info->Value[3]=Version&0xffff;Info->Value[4]=Family;Info->Value[5]=Modem;
  Info->Value[6]=((UINT64)Fuse<<32)|Serial;Info->Value[7]=Board.subtype;
  GET(Region("MPSS_EFS",&Info->Value[8],&Info->Value[9],TRUE));
  GET(Region("ADSP_EFS",&Base,&Size,TRUE));
  Info->Value[10]=Base+Size/2;Info->Value[11]=Size/2;
  Info->Value[12]=Base;Info->Value[13]=Size/2;
  Info->Value[14]=1;Info->Value[15]=0x6654504d;
  GET(Region("TGCM",&Info->Value[16],&Info->Value[17],FALSE));
  Info->Value[18]=(UINTN)Shared;
#undef GET
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ReservedRange(EFI_MEMORY_DESCRIPTOR *Map,UINTN MapSize,UINTN Stride,
                                UINT64 Address,UINT64 Size) {
  UINT64 End,Next;UINTN I;
  if(!Size) return EFI_SUCCESS;
  if(Address>MAX_UINT64-Size) return EFI_COMPROMISED_DATA;
  End=Address+Size;
  while(Address<End) {
    Next=Address;
    for(I=0;I+Stride<=MapSize;I+=Stride) {
      EFI_MEMORY_DESCRIPTOR *D=(VOID *)((UINT8 *)Map+I);UINT64 Limit;
      if(D->NumberOfPages>MAX_UINT64/4096 || D->PhysicalStart>MAX_UINT64-D->NumberOfPages*4096)
        return EFI_COMPROMISED_DATA;
      Limit=D->PhysicalStart+D->NumberOfPages*4096;
      if(D->PhysicalStart>Address || Limit<=Address) continue;
      if(D->Type!=EfiReservedMemoryType && D->Type!=EfiACPIMemoryNVS &&
         D->Type!=EfiMemoryMappedIO) return EFI_COMPROMISED_DATA;
      Next=Limit;break;
    }
    if(Next==Address) return EFI_NOT_FOUND;
    Address=Next;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Reservations(CONST WINDOWS_PLATFORM_INFO *Info) {
  EFI_MEMORY_DESCRIPTOR *Map;EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  UINTN Size=0,Stride=0,Key,Capacity;UINT32 Version;UINT64 SmemBase,SmemSize;EFI_STATUS Status;
  Status=Region("SMEM",&SmemBase,&SmemSize,FALSE);if(EFI_ERROR(Status)) return Status;
  Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&Gop);
  if(EFI_ERROR(Status)) return Status;
  if(!Gop || !Gop->Mode) return EFI_NOT_FOUND;
  Status=gBS->GetMemoryMap(&Size,NULL,&Key,&Stride,&Version);
  if(Status!=EFI_BUFFER_TOO_SMALL || Stride<sizeof(*Map) || Size>1024*1024 || Stride>4096)
    return EFI_COMPROMISED_DATA;
  Capacity=Size+Stride*16;Map=AllocatePool(Capacity);if(!Map) return EFI_OUT_OF_RESOURCES;
  Size=Capacity;Status=gBS->GetMemoryMap(&Size,Map,&Key,&Stride,&Version);
  if(EFI_ERROR(Status)) goto Done;
  if(Stride<sizeof(*Map) || Size%Stride) {Status=EFI_COMPROMISED_DATA;goto Done;}
  Status=ReservedRange(Map,Size,Stride,SmemBase,SmemSize);if(EFI_ERROR(Status)) goto Done;
  Status=ReservedRange(Map,Size,Stride,Info->Value[16],Info->Value[17]);if(EFI_ERROR(Status)) goto Done;
  Status=ReservedRange(Map,Size,Stride,Info->Value[8],Info->Value[9]);if(EFI_ERROR(Status)) goto Done;
  Status=ReservedRange(Map,Size,Stride,Info->Value[12],Info->Value[13]*2);if(EFI_ERROR(Status)) goto Done;
  Status=ReservedRange(Map,Size,Stride,Gop->Mode->FrameBufferBase,Gop->Mode->FrameBufferSize);
Done:
  FreePool(Map);return Status;
}

EFI_STATUS WindowsCopyDsdt(VOID **Data,UINTN *Size) {
  EFI_ACPI_SDT_PROTOCOL *Sdt;EFI_ACPI_SDT_HEADER *Dsdt;UINTN Key;BOOLEAN Csrt;EFI_STATUS Status;
  *Data=NULL;*Size=0;
  Status=DescriptionTables(&Sdt,&Dsdt,&Key,&Csrt);
  if(EFI_ERROR(Status)) return Status;
  *Data=AllocateCopyPool(Dsdt->Length,Dsdt);
  if(!*Data) return EFI_OUT_OF_RESOURCES;
  *Size=Dsdt->Length;return EFI_SUCCESS;
}

EFI_STATUS WindowsPreparePlatform(WINDOWS_PLATFORM_INFO *Info) {
  EFI_ACPI_SDT_PROTOCOL *Sdt;EFI_ACPI_TABLE_PROTOCOL *Acpi;EFI_ACPI_SDT_HEADER *Dsdt;
  UINTN Key,NewKey,Size;BOOLEAN Csrt;VOID *Original=NULL,*Prepared=NULL;
  EFI_STATUS Status,Rollback;
  PreparationStage="ACPI table discovery";Status=DescriptionTables(&Sdt,&Dsdt,&Key,&Csrt);
  if(EFI_ERROR(Status)) return Status;
  PreparationStage="Qualcomm SoC information";Status=PlatformInfo(Info);if(EFI_ERROR(Status)) return Status;
  PreparationStage="reserved memory map";Status=Reservations(Info);if(EFI_ERROR(Status)) return Status;
  Status=gBS->LocateProtocol(&gEfiAcpiTableProtocolGuid,NULL,(VOID **)&Acpi);
  if(EFI_ERROR(Status)) return Status;
  Size=Dsdt->Length;Original=AllocateCopyPool(Size,Dsdt);Prepared=AllocateCopyPool(Size,Dsdt);
  if(!Original || !Prepared) {Status=EFI_OUT_OF_RESOURCES;goto Done;}
  PreparationStage="DSDT fields and checksum";
  Status=WindowsPatchDsdt(Prepared,Size,Info);if(EFI_ERROR(Status)) goto Done;
  // Retrying the Windows card must not replace an already prepared table.
  if(CompareMem(Prepared,Original,Size)!=0) {
    PreparationStage="DSDT installation";
    Status=Acpi->UninstallAcpiTable(Acpi,Key);if(EFI_ERROR(Status)) goto Done;
    Status=Acpi->InstallAcpiTable(Acpi,Prepared,Size,&NewKey);
    if(EFI_ERROR(Status)) {
      Rollback=Acpi->InstallAcpiTable(Acpi,Original,Size,&NewKey);
      if(EFI_ERROR(Rollback)) Status=EFI_ABORTED;
      goto Done;
    }
  }
  if(!Csrt) {
    PreparationStage="CSRT installation";
    Status=Acpi->InstallAcpiTable(Acpi,(VOID *)WindowsCsrt,sizeof(WindowsCsrt),&NewKey);
    if(EFI_ERROR(Status)) goto Done;
  }
  PreparationStage="published ACPI validation";
  Status=DescriptionTables(&Sdt,&Dsdt,&Key,&Csrt);
  if(!EFI_ERROR(Status) && (!Csrt || Dsdt->Length!=Size || CompareMem(Dsdt,Prepared,Size)))
    Status=EFI_COMPROMISED_DATA;
Done:
  if(Original) FreePool(Original);
  if(Prepared) FreePool(Prepared);
  return Status;
}
