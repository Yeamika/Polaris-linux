// SPDX-License-Identifier: BSD-2-Clause-Patent
// The field set follows Renegade's SDM845 MsPlatformDevicesLib (Microsoft
// Corporation, BSD-2-Clause-Patent). Patch a checked copy in one transaction;
// preserve the actual AML integer widths, including the QWord SIDM object.
#include "Acpi.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

STATIC CONST CHAR8 Names[WINDOWS_ACPI_VALUES][5]={
  "SOID","SIDV","SVMJ","SVMI","SDFE","SIDM","SOSN","PLST","RMTB",
  "RMTX","RFMB","RFMS","RFAB","RFAS","TPMA","TDTV","TCMA","TCML","SOSI"
};
STATIC CONST UINT8 Widths[WINDOWS_ACPI_VALUES]={4,4,2,2,2,8,8,4,4,4,4,4,4,4,4,4,4,4,8};

STATIC EFI_STATUS FindName(UINT8 *Data,UINTN Size,CONST CHAR8 *Name,
                           UINT8 Op,UINTN Bytes,UINTN *Offset) {
  UINTN I,Count=0;
  for(I=36;I+5<=Size;++I) {
    if(Data[I]!=8 || CompareMem(Data+I+1,Name,4)!=0) continue;
    if(Size-I<6 || Data[I+5]!=Op || Size-I-6<Bytes) return EFI_COMPROMISED_DATA;
    if(Op==0x0d) {
      UINTN Length=AsciiStrnLenS((CHAR8 *)Data+I+6,Bytes),J;
      // The pinned fixed-size slot ends immediately before Name(SIDV,...).
      if(Length==Bytes || Size-I-6-Bytes<6 ||
         CompareMem(Data+I+6+Bytes,"\x08SIDV\x0c",6)) return EFI_COMPROMISED_DATA;
      for(J=Length+1;J<Bytes;++J)
        if(Data[I+6+J]!=0 && Data[I+6+J]!=0xa3) return EFI_COMPROMISED_DATA;
    }
    *Offset=I+6;
    ++Count;
  }
  return Count==1?EFI_SUCCESS:EFI_COMPROMISED_DATA;
}

EFI_STATUS WindowsPatchDsdt(VOID *Buffer,UINTN Size,CONST WINDOWS_PLATFORM_INFO *Info) {
  UINT8 *Data=Buffer,Op;
  UINTN I,J,Offsets[WINDOWS_ACPI_VALUES],StringOffset,ChipSize;
  EFI_STATUS Status;
  if(!Data || !Info) return EFI_INVALID_PARAMETER;
  if(Size<36 || Size>1024*1024 || CompareMem(Data,"DSDT",4)!=0 ||
     ReadUnaligned32((UINT32 *)(Data+4))!=Size || CalculateSum8(Data,Size)!=0)
    return EFI_COMPROMISED_DATA;
  ChipSize=AsciiStrnLenS(Info->Chip,sizeof(Info->Chip));
  if(!ChipSize || ChipSize==sizeof(Info->Chip)) return EFI_COMPROMISED_DATA;
  for(I=0;I<ChipSize;++I) if(Info->Chip[I]<32 || Info->Chip[I]>126) return EFI_COMPROMISED_DATA;
  for(I=0;I<WINDOWS_ACPI_VALUES;++I) {
    if(Widths[I]<8 && Info->Value[I]>>((UINTN)Widths[I]*8)) return EFI_BAD_BUFFER_SIZE;
    Op=Widths[I]==2?0x0b:Widths[I]==4?0x0c:0x0e;
    Status=FindName(Data,Size,Names[I],Op,Widths[I],&Offsets[I]);
    if(EFI_ERROR(Status)) return Status;
  }
  // A previously prepared string can be shorter than the original placeholder.
  Status=FindName(Data,Size,"SIDS",0x0d,sizeof(Info->Chip),&StringOffset);
  if(EFI_ERROR(Status)) return Status;
  for(I=0;I<WINDOWS_ACPI_VALUES;++I)
    for(J=0;J<Widths[I];++J) Data[Offsets[I]+J]=(UINT8)(Info->Value[I]>>(J*8));
  // End the chip string at its real length. AML Noop opcodes fill the spare
  // space in this root TermList without changing any enclosing package size.
  SetMem(Data+StringOffset,sizeof(Info->Chip),0xa3);
  CopyMem(Data+StringOffset,Info->Chip,ChipSize+1);
  Data[9]=0;
  Data[9]=CalculateCheckSum8(Data,Size);
  return EFI_SUCCESS;
}
