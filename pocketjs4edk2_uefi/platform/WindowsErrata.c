// SPDX-License-Identifier: BSD-2-Clause-Patent
// Checked instruction plans for the SDM845 ACTLR/cache/PSCI firmware errata.
// Inputs are mapped ARM64 PE images, never unbounded physical scan windows.
#include "WindowsErrata.h"
#include <Library/BaseMemoryLib.h>

STATIC UINT16 U16(CONST UINT8 *P) {return (UINT16)(P[0]|(P[1]<<8));}
STATIC UINT32 U32(CONST UINT8 *P) {return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
STATIC BOOLEAN Span(UINT32 Size,UINT32 Offset,UINT32 Bytes) {return Offset<=Size && Bytes<=Size-Offset;}
STATIC UINT8 *At(CONST WINDOWS_IMAGE *Image,UINT32 Offset,UINT32 Bytes) {
  UINT32 I,Start,Size;UINT8 *S;
  if(!Span(Image->Size,Offset,Bytes)) return NULL;
  if(Span(Image->Headers,Offset,Bytes)) return Image->Base+Offset;
  for(I=0;I<Image->Count;++I) {
    S=Image->Base+Image->Sections+I*40;Start=U32(S+12);Size=U32(S+8);
    if(Offset>=Start && Span(Size,Offset-Start,Bytes)) return Image->Base+Offset;
  }
  return NULL;
}
BOOLEAN WindowsImageCode(CONST WINDOWS_IMAGE *Image,UINT32 Offset,UINT32 Bytes) {
  UINT32 I,Start,Size;UINT8 *S;
  for(I=0;I<Image->Count;++I) {
    S=Image->Base+Image->Sections+I*40;Start=U32(S+12);Size=U32(S+8);
    if((U32(S+36)&0x20000000) && Offset>=Start && Span(Size,Offset-Start,Bytes)) return TRUE;
  }
  return FALSE;
}
#define Executable WindowsImageCode
STATIC BOOLEAN CodeSection(CONST WINDOWS_IMAGE *Image,UINT32 Index,UINT32 *Start,UINT32 *End) {
  UINT8 *S=Image->Base+Image->Sections+Index*40;
  *Start=U32(S+12);*End=*Start+U32(S+8);
  return (U32(S+36)&0x20000000)!=0 && !(*Start&3);
}

EFI_STATUS WindowsImageOpen(VOID *Data,UINTN Capacity,WINDOWS_IMAGE *Image) {
  UINT8 *B=Data,*O,*S;UINT32 Pe,Opt,Headers,Size,I,Last=0;
  ZeroMem(Image,sizeof(*Image));
  if(!B || Capacity<64 || U16(B)!=0x5a4d) return EFI_UNSUPPORTED;
  Pe=U32(B+60);
  if(Pe>1024*1024 || Pe>Capacity || Capacity-Pe<24 || U32(B+Pe)!=0x4550 || U16(B+Pe+4)!=0xaa64)
    return EFI_COMPROMISED_DATA;
  Opt=U16(B+Pe+20);O=B+Pe+24;
  if(Opt<144 || Opt>4096 || Capacity-Pe-24<Opt || U16(O)!=0x20b || U32(O+108)<4)
    return EFI_COMPROMISED_DATA;
  Size=U32(O+56);Headers=U32(O+60);
  Image->Count=U16(B+Pe+6);Image->Sections=Pe+24+Opt;
  if(Size>64*1024*1024 || Size>Capacity || Headers>Size || !Image->Count || Image->Count>96 ||
     !Span(Headers,Image->Sections,Image->Count*40)) return EFI_COMPROMISED_DATA;
  Image->Base=B;Image->Size=Size;Image->Headers=Headers;
  Image->Exports=U32(O+112);Image->ExportSize=U32(O+116);
  Image->Exception=U32(O+136);Image->ExceptionSize=U32(O+140);
  if(!Span(Size,Image->Exports,Image->ExportSize) || !Span(Size,Image->Exception,Image->ExceptionSize) ||
     Image->ExceptionSize%8) return EFI_COMPROMISED_DATA;
  for(I=0;I<Image->Count;++I) {
    S=B+Image->Sections+I*40;
    if(!Span(Size,U32(S+12),U32(S+8)) || U32(S+12)<Headers || U32(S+12)<Last)
      return EFI_COMPROMISED_DATA;
    Last=U32(S+12)+U32(S+8);
  }
  if((Image->ExportSize && !At(Image,Image->Exports,Image->ExportSize)) ||
     (Image->ExceptionSize && !At(Image,Image->Exception,Image->ExceptionSize))) return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}

STATIC BOOLEAN Function(CONST WINDOWS_IMAGE *Image,UINT32 Address,UINT32 *Start,UINT32 *End) {
  UINT32 I,A=0,B,Unwind=3,Length,Previous=0,Next;UINT8 *X;
  for(I=0;I<Image->ExceptionSize;I+=8) {
    Next=U32(Image->Base+Image->Exception+I);
    if((Next&3) || (I && Next<=Previous)) return FALSE;
    Previous=Next;
    if(Next>Address) break;
    A=Next;Unwind=U32(Image->Base+Image->Exception+I+4);
  }
  if((Unwind&3)==1 || (Unwind&3)==2) Length=((Unwind>>2)&0x7ff)*4;
  else if(!(Unwind&3)) {
    X=At(Image,Unwind,4);if(!X || (U32(X)&0xc0000)) return FALSE;
    Length=(U32(X)&0x3ffff)*4;
  } else {
    // Flag 3 describes a linked jump island, not an ordinary function entry.
    // Unrelated links do not invalidate later records; never patch an island.
    return FALSE;
  }
  B=A+Length;
  if(Length && B>A && Address>=A && Address<B && Executable(Image,A,Length)) {
    *Start=A;*End=B;return TRUE;
  }
  return FALSE;
}

UINT32 WindowsFindContext(CONST WINDOWS_IMAGE *Image) {
  UINT32 I,J,Low,High,Start,End,Found=0,Count=0;UINT8 *P;
  // Current winload uses w19 for the requested context and x23 for its
  // selected context descriptor. Validate the complete entry and decision.
  for(J=0;J<Image->Count;++J) {
   if(!CodeSection(Image,J,&Low,&High)) continue;
   for(I=Low;I+0x74<=High;I+=4) {
    P=Image->Base+I;
    if(U32(P)!=0xa9bd53f3 || U32(P+4)!=0xa9015bf5 || U32(P+8)!=0xf90013f7 ||
       U32(P+12)!=0xa9bf7bfd || U32(P+16)!=0x910003fd ||
       U32(P+0x60)!=0x7100067f || U32(P+0x64)!=0x9a881137 ||
       (U32(P+0x68)&0xfc000000)!=0x94000000 || U32(P+0x6c)!=0x6b13001f ||
       (U32(P+0x70)&0xff00001f)!=0x54000000) continue;
    if(!Function(Image,I,&Start,&End) || Start!=I || End-I<0xd8 || End-I>0x200) continue;
    Found=I;++Count;
   }
  }
  return Count==1?Found:0;
}

STATIC EFI_STATUS Add(WINDOWS_PATCH_PLAN *Plan,UINT32 Offset,UINT32 Before,UINT32 After) {
  UINT32 I;
  if(Plan->Count>=ARRAY_SIZE(Plan->Item)) return EFI_OUT_OF_RESOURCES;
  for(I=0;I<Plan->Count;++I) if(Plan->Item[I].Offset==Offset) return EFI_COMPROMISED_DATA;
  Plan->Item[Plan->Count++]=(WINDOWS_PATCH){Offset,Before,After};return EFI_SUCCESS;
}

STATIC EFI_STATUS Actlr(CONST WINDOWS_IMAGE *Image,WINDOWS_PATCH_PLAN *Plan) {
  UINT32 I,J,Low,High,Word,Start,End,Reg,Store;BOOLEAN Leaf;EFI_STATUS Status;
  for(J=0;J<Image->Count;++J) {
   if(!CodeSection(Image,J,&Low,&High)) continue;
   for(I=Low;I+4<=High;I+=4) {
    Word=U32(Image->Base+I);
    if((Word&0xffffffe0)!=0xd5381020 && (Word&0xffffffe0)!=0xd5181020) continue;
    Reg=Word&31;Leaf=FALSE;
    // Context save/restore leaf routines legitimately have no unwind entry.
    // Accept them only with neighboring SCTLR/CPACR/TCR instructions and
    // matching 64-bit load/store registers, all within this code section.
    if(I>=Low+8 && High-I>=20) {
      Store=U32(Image->Base+I+4);
      if((Word&0xffffffe0)==0xd5381020) {
        Leaf=U32(Image->Base+I-8)==(0xd5381000|Reg) &&
          U32(Image->Base+I+8)==(0xd5381040|Reg) &&
          U32(Image->Base+I+16)==(0xd5382040|Reg) &&
          (Store&0xffc0001f)==(0xf9000000|Reg) &&
          U32(Image->Base+I-4)==Store-0x400 && U32(Image->Base+I+12)==Store+0x400;
      } else {
        Leaf=(U32(Image->Base+I-4)&0xffc0001f)==(0xf9400000|Reg) &&
          U32(Image->Base+I+4)==U32(Image->Base+I-4)+0x400 &&
          U32(Image->Base+I+8)==(0xd5181040|Reg) &&
          U32(Image->Base+I+12)==U32(Image->Base+I-4)+0x800 &&
          U32(Image->Base+I+16)==(0xd5182040|Reg);
      }
    }
    if(!Leaf && !Function(Image,I,&Start,&End)) return EFI_UNSUPPORTED;
    if((Word&0xffffffe0)==0xd5381020) {
      Status=Add(Plan,I,Word,0xd2800000|(Word&31));++Plan->Reads;
    } else {Status=Add(Plan,I,Word,0xd503201f);++Plan->Writes;}
    if(EFI_ERROR(Status)) return Status;
   }
  }
  return Plan->Reads?EFI_SUCCESS:EFI_UNSUPPORTED;
}

STATIC UINT32 Export(CONST WINDOWS_IMAGE *Image,CONST CHAR8 *Name,UINT32 NameSize) {
  UINT8 *D=At(Image,Image->Exports,40),*Names,*Ordinals,*Functions,*P;
  UINT32 I,N,F,Ordinal;
  if(!D || Image->ExportSize<40) return 0;
  N=U32(D+24);F=U32(D+20);
  if(N>65536 || F>65536) return 0;
  Names=At(Image,U32(D+32),N*4);Ordinals=At(Image,U32(D+36),N*2);Functions=At(Image,U32(D+28),F*4);
  if(!Names || !Ordinals || !Functions) return 0;
  for(I=0;I<N;++I) {
    P=At(Image,U32(Names+I*4),NameSize);
    if(!P || CompareMem(P,Name,NameSize)) continue;
    Ordinal=U16(Ordinals+I*2);
    if(Ordinal>=F) return 0;
    return U32(Functions+Ordinal*4);
  }
  return 0;
}

EFI_STATUS WindowsPlanLoader(CONST WINDOWS_IMAGE *Image,WINDOWS_PATCH_PLAN *Plan) {
  ZeroMem(Plan,sizeof(*Plan));return Actlr(Image,Plan);
}

EFI_STATUS WindowsPlanKernel(CONST WINDOWS_IMAGE *Image,WINDOWS_PATCH_PLAN *Plan) {
  UINT32 I,J,Low,High,Word,Target,Start,End,Cache=0,Psci=0,Bugcheck;INT32 Delta;EFI_STATUS Status;
  ZeroMem(Plan,sizeof(*Plan));Status=Actlr(Image,Plan);
  if(EFI_ERROR(Status) || !Plan->Writes) return EFI_UNSUPPORTED;
  Bugcheck=Export(Image,"KeBugCheckEx",13);if(!Bugcheck) return EFI_UNSUPPORTED;
  for(J=0;J<Image->Count;++J) {
   if(!CodeSection(Image,J,&Low,&High)) continue;
   for(I=Low;I+16<=High;I+=4) {
    if(U32(Image->Base+I)==0xd2800004 && U32(Image->Base+I+4)==0xd2800003 &&
       U32(Image->Base+I+8)==0x528007c0) {
      Word=U32(Image->Base+I+12);
      Delta=((INT32)(Word<<6))>>4;Target=I+12+Delta;
      if((Word&0xfc000000)==0x94000000 && Target==Bugcheck && Function(Image,I,&Start,&End)) {
        Status=Add(Plan,I+12,Word,0xd503201f);if(EFI_ERROR(Status)) return Status;++Cache;
      }
    }
    // Find the PSCI MEM_PROTECT query by its actual literal load, zero
    // arguments, unwind function boundary and PAC-aware entry. No byte-offset
    // subtraction from a match and no broad SMC/bugcheck suppression.
    Word=U32(Image->Base+I);
    if((Word&0xff00001f)!=0x18000000 || I<Low+12) continue;
    Delta=((INT32)(Word<<8))>>11;Target=I+Delta;
    if(!At(Image,Target,4) || U32(Image->Base+Target)!=0x84000013 ||
       U32(Image->Base+I-12)!=0xd2800003 || U32(Image->Base+I-8)!=0xd2800002 ||
       U32(Image->Base+I-4)!=0xd2800001 || !Function(Image,I,&Start,&End) ||
       End-Start<32 || End-Start>256 || U32(Image->Base+Start)!=0xd503237f ||
       U32(Image->Base+Start+4)!=0xa9bf53f3 || U32(Image->Base+Start+8)!=0xa9bf7bfd ||
       U32(Image->Base+Start+12)!=0x910003fd) continue;
    Status=Add(Plan,Start,0xd503237f,0x52800000);if(EFI_ERROR(Status)) return Status;
    Status=Add(Plan,Start+4,0xa9bf53f3,0xd65f03c0);if(EFI_ERROR(Status)) return Status;++Psci;
   }
  }
  Plan->Cache=Cache;Plan->Psci=Psci;
  return Cache==1 && Psci==1?EFI_SUCCESS:EFI_UNSUPPORTED;
}

EFI_STATUS WindowsApplyPlan(CONST WINDOWS_IMAGE *Image,CONST WINDOWS_PATCH_PLAN *Plan) {
  UINT32 I,O,V;UINT8 *P;
  if(Plan->Count>ARRAY_SIZE(Plan->Item)) return EFI_COMPROMISED_DATA;
  for(I=0;I<Plan->Count;++I) {
    O=Plan->Item[I].Offset;
    if((O&3) || !Executable(Image,O,4) || U32(Image->Base+O)!=Plan->Item[I].Before)
      return EFI_COMPROMISED_DATA;
  }
  for(I=0;I<Plan->Count;++I) {
    P=Image->Base+Plan->Item[I].Offset;V=Plan->Item[I].After;
    P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);P[2]=(UINT8)(V>>16);P[3]=(UINT8)(V>>24);
  }
  return EFI_SUCCESS;
}
