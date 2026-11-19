// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_ANDROID_MEMORY_H
#define POLARIS_ANDROID_MEMORY_H
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define POLARIS_ANDROID_MAX_RESERVED_RANGES 256
typedef struct {
  UINTN Count;
  POLARIS_ANDROID_RANGE Ranges[POLARIS_ANDROID_MAX_RESERVED_RANGES];
} POLARIS_ANDROID_RESERVATIONS;

STATIC BOOLEAN PolarisAndroidCollectReserved(UINT64 Base,UINT64 End,VOID *Context) {
  POLARIS_ANDROID_RESERVATIONS *Set=Context;
  CONST UINT64 Mask=EFI_PAGE_SIZE-1;
  UINTN First=0,Past,I;
  if (End>MAX_UINT64-Mask) return FALSE;
  Base&=~Mask;
  End=(End+Mask)&~Mask;
  // Keep the set sorted and merge overlapping or adjacent page spans.
  while (First<Set->Count && Set->Ranges[First].End<Base) ++First;
  Past=First;
  while (Past<Set->Count && Set->Ranges[Past].Start<=End) {
    if (Set->Ranges[Past].Start<Base) Base=Set->Ranges[Past].Start;
    if (Set->Ranges[Past].End>End) End=Set->Ranges[Past].End;
    ++Past;
  }
  if (Past==First) {
    if (Set->Count==POLARIS_ANDROID_MAX_RESERVED_RANGES) return FALSE;
    for (I=Set->Count;I>First;--I) Set->Ranges[I]=Set->Ranges[I-1];
    ++Set->Count;
  } else {
    for (I=Past;I<Set->Count;++I) Set->Ranges[First+1+I-Past]=Set->Ranges[I];
    Set->Count-=Past-First-1;
  }
  Set->Ranges[First]=(POLARIS_ANDROID_RANGE){Base,End};
  return TRUE;
}

STATIC UINT64 PolarisAndroidLastFit(UINT64 Floor,UINT64 End,UINT64 Bytes,UINT64 Align) {
  if (Floor>=End || End-Floor<Bytes) return 0;
  UINT64 Base=(End-Bytes)&~(Align-1);
  return Base>=Floor?Base:0;
}

// Find the highest aligned fit in the gap between fixed reservations.
STATIC UINT64 PolarisAndroidFindGap(CONST POLARIS_ANDROID_RESERVATIONS *Set,
    UINT64 Floor,UINT64 End,UINT64 Bytes,UINT64 Align) {
  UINT64 Best=0,Base;
  for (UINTN I=0;I<Set->Count && Floor<End;++I) {
    CONST POLARIS_ANDROID_RANGE *R=&Set->Ranges[I];
    if (R->End<=Floor) continue;
    if (R->Start>=End) break;
    Base=PolarisAndroidLastFit(Floor,R->Start,Bytes,Align);
    if (Base>Best) Best=Base;
    Floor=R->End;
  }
  Base=PolarisAndroidLastFit(Floor,End,Bytes,Align);
  return Base>Best?Base:Best;
}

STATIC EFI_PHYSICAL_ADDRESS PolarisAndroidFit(CONST EFI_MEMORY_DESCRIPTOR *Map,
    UINTN MapBytes,UINTN Stride,CONST fdt64_t *Banks,INT32 Cells,
    CONST POLARIS_ANDROID_RESERVATIONS *Reserved,UINT64 Limit,UINT64 Bytes,UINT64 Align) {
  EFI_PHYSICAL_ADDRESS Best=0;
  for (UINTN Off=0;Off<MapBytes;Off+=Stride) {
    CONST EFI_MEMORY_DESCRIPTOR *D=(CONST EFI_MEMORY_DESCRIPTOR *)((CONST UINT8 *)Map+Off);
    UINT64 Start=D->PhysicalStart,End;
    if (D->Type!=EfiConventionalMemory || !(D->Attribute&EFI_MEMORY_WB) ||
        (Start&(EFI_PAGE_SIZE-1)) || D->NumberOfPages>(MAX_UINT64-Start)/EFI_PAGE_SIZE) continue;
    End=Start+D->NumberOfPages*EFI_PAGE_SIZE;
    if (End>Limit) End=Limit;
    for (INT32 I=0;I<Cells;I+=2) {
      UINT64 Bank=PolarisAndroidBe64(Banks+I),BankSize=PolarisAndroidBe64(Banks+I+1);
      if (BankSize>MAX_UINT64-Bank) continue;
      UINT64 Last=Bank+BankSize;
      UINT64 Base=PolarisAndroidFindGap(Reserved,Bank>Start?Bank:Start,Last<End?Last:End,Bytes,Align);
      if (Base>Best) Best=Base;
    }
  }
  return Best;
}

// Only allocate pages that are free in EFI and usable in the native DTB.
// Neither AllocatePages nor FreePages is called on the DT reserves themselves.
STATIC VOID *PolarisAndroidAllocatePages(CONST VOID *Dtb,UINTN Pages,UINTN Align,UINT64 PreferredLimit) {
  POLARIS_ANDROID_RESERVATIONS *Reserved=NULL;
  EFI_MEMORY_DESCRIPTOR *Map=NULL;
  EFI_PHYSICAL_ADDRESS Candidate=0;
  EFI_STATUS Status;
  UINTN MapBytes=0,Key,Stride=0;
  UINT32 Version;
  INT32 Node=fdt_path_offset(Dtb,"/memory"),Len;
  CONST fdt64_t *Banks=fdt_getprop(Dtb,Node,"reg",&Len);
  if (!Pages || Pages>MAX_UINTN/EFI_PAGE_SIZE || Align<EFI_PAGE_SIZE ||
      (Align&(Align-1)) || !Banks || Len<16 || Len%16) return NULL;
  Reserved=AllocateZeroPool(sizeof(*Reserved));
  if (!Reserved || !PolarisAndroidVisitReserved(Dtb,PolarisAndroidCollectReserved,Reserved)) goto Done;
  Status=gBS->GetMemoryMap(&MapBytes,NULL,&Key,&Stride,&Version);
  if (Status!=EFI_BUFFER_TOO_SMALL || Stride<sizeof(*Map) ||
      Stride>MAX_UINTN/8 || MapBytes>MAX_UINTN-8*Stride) goto Done;
  MapBytes+=8*Stride;
  Map=AllocatePool(MapBytes);
  if (!Map) goto Done;
  Status=gBS->GetMemoryMap(&MapBytes,Map,&Key,&Stride,&Version);
  if (EFI_ERROR(Status) || Stride<sizeof(*Map) || (Stride&7) || !MapBytes || MapBytes%Stride) goto Done;

  // Prefer the existing low boot arena below the firmware trace page. If that
  // space is fragmented, use another low 32-bit address for legacy kernels.
  UINT64 Bytes=EFI_PAGES_TO_SIZE(Pages);
  UINT64 Limits[]={PreferredLimit,0x100000000ULL};
  for (UINTN I=0;I<sizeof(Limits)/sizeof(Limits[0]);++I) {
    UINT64 Limit=Limits[I];
    for (UINTN Attempt=0;Attempt<8;++Attempt) {
      Candidate=PolarisAndroidFit(Map,MapBytes,Stride,Banks,Len/8,Reserved,Limit,Bytes,Align);
      if (!Candidate || (I && Candidate+Bytes<=PreferredLimit)) { Candidate=0; break; }
      Status=gBS->AllocatePages(AllocateAddress,EfiBootServicesData,Pages,&Candidate);
      if (!EFI_ERROR(Status)) goto Done;
      Limit=Candidate+Bytes-Align;
      Candidate=0;
    }
  }
Done:
  if (Map) FreePool(Map);
  if (Reserved) FreePool(Reserved);
  return (VOID *)(UINTN)Candidate;
}
#endif
