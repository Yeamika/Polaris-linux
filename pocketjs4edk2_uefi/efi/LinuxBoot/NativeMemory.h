// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_NATIVE_MEMORY_H
#define POLARIS_NATIVE_MEMORY_H
#include <Uefi.h>
#define POLARIS_NATIVE_MAX_RANGES 256
typedef struct {UINT64 Base, End;} POLARIS_NATIVE_RANGE;
STATIC inline UINT64 NativeLe(CONST UINT8 *P, UINTN N) {
  UINT64 V=0; while(N) {--N;V=(V<<8)|P[N];} return V;
}
STATIC inline BOOLEAN NativeImage(CONST VOID *Data, UINTN Bytes, UINT64 *Offset, UINT64 *Memory) {
  CONST UINT8 *P=Data;UINT64 Flags;
  if(!P || Bytes<64 || NativeLe(P+56,4)!=0x644d5241) return FALSE;
  *Offset=NativeLe(P+8,8);*Memory=NativeLe(P+16,8);Flags=NativeLe(P+24,8);
  return *Offset<=0x200000 && !(*Offset&4095) && *Memory>=Bytes &&
    *Memory<=0x20000000 && !(Flags&~0xfULL) && !(Flags&1) && ((Flags>>1)&3)==1;
}
STATIC inline BOOLEAN NativeRamType(UINT32 Type) {
  return Type==EfiLoaderCode || Type==EfiLoaderData || Type==EfiBootServicesCode ||
    Type==EfiBootServicesData || Type==EfiConventionalMemory || Type==EfiACPIReclaimMemory;
}
STATIC inline BOOLEAN NativeCacheType(UINT32 Type) {
  return NativeRamType(Type) || Type==EfiRuntimeServicesCode || Type==EfiRuntimeServicesData ||
    Type==EfiACPIMemoryNVS;
}
STATIC inline BOOLEAN NativeRanges(CONST VOID *Map, UINTN Bytes, UINTN Step,
    POLARIS_NATIVE_RANGE *Ranges, UINTN *Count) {
  UINTN I,J,K,N=0;CONST UINT8 *D;UINT32 Type;UINT64 Base,Pages,End,Attr;
  if(!Map || !Ranges || !Count || Step<40 || Step%8 || !Bytes || Bytes%Step) return FALSE;
  for(I=0;I<Bytes;I+=Step) {
    D=(CONST UINT8 *)Map+I;Type=(UINT32)NativeLe(D,4);Attr=NativeLe(D+32,8);
    if(!NativeCacheType(Type) || !(Attr&(EFI_MEMORY_WB|EFI_MEMORY_WT))) continue;
    Base=NativeLe(D+8,8);Pages=NativeLe(D+24,8);
    if((Base&4095) || !Pages || Pages>(MAX_UINT64-Base)/4096) return FALSE;
    if(!NativeRamType(Type)) continue;
    End=Base+Pages*4096;
    if(N==POLARIS_NATIVE_MAX_RANGES) return FALSE;
    J=N;while(J && Ranges[J-1].Base>Base) {Ranges[J]=Ranges[J-1];--J;}
    Ranges[J]=(POLARIS_NATIVE_RANGE){Base,End};++N;
  }
  if(!N) return FALSE;
  K=0;
  for(I=1;I<N;++I) {
    if(Ranges[I].Base<Ranges[K].End) return FALSE;
    if(Ranges[I].Base==Ranges[K].End) Ranges[K].End=Ranges[I].End;
    else Ranges[++K]=Ranges[I];
  }
  *Count=K+1;return TRUE;
}
STATIC inline BOOLEAN NativeCovered(CONST POLARIS_NATIVE_RANGE *R,UINTN N,UINT64 Base,UINT64 Size) {
  UINTN I;if(!Size || Size>MAX_UINT64-Base) return FALSE;
  for(I=0;I<N;++I) if(Base>=R[I].Base && Base<R[I].End && Size<=R[I].End-Base) return TRUE;
  return FALSE;
}
#endif
