// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_ANDROID_NATIVE_FDT_H
#define POLARIS_ANDROID_NATIVE_FDT_H
// Include the caller's libfdt interface first (SimpleInit uses comp_libfdt.h).
STATIC inline UINT64 PolarisAndroidBe64(CONST VOID *Data) {
  CONST UINT8 *P=Data;UINT64 Value=0;UINTN I;
  for (I=0;I<8;++I) Value=(Value<<8)|P[I];
  return Value;
}
STATIC inline BOOLEAN PolarisAndroidDtbValid(CONST VOID *Dtb,UINTN Size) {
  CONST fdt32_t *Id;CONST fdt64_t *Reg;CONST CHAR8 *Model,*Args;
  CONST CHAR8 Expected[]="Xiaomi Technologies, Inc. Polaris MP v2.1";
  INT32 Len,Node;UINTN I;UINT64 Last=0;
  if (!Dtb || Size<sizeof(struct fdt_header) || Size>POLARIS_ANDROID_DTB_LIMIT ||
      fdt_check_header(Dtb) || fdt_totalsize(Dtb)!=Size) return FALSE;
  // Traverse before reading properties; reject truncated structure blocks.
  Node=-1;while ((Node=fdt_next_node(Dtb,Node,NULL))>=0) {}
  if (Node!=-FDT_ERR_NOTFOUND || fdt_num_mem_rsv(Dtb)<0) return FALSE;
  Id=fdt_getprop(Dtb,0,"#address-cells",&Len);
  if (!Id || Len!=4 || fdt32_to_cpu(*Id)!=2) return FALSE;
  Id=fdt_getprop(Dtb,0,"#size-cells",&Len);
  if (!Id || Len!=4 || fdt32_to_cpu(*Id)!=2) return FALSE;
  Model=fdt_getprop(Dtb,0,"model",&Len);
  if (!Model || Len!=sizeof(Expected)) return FALSE;
  for (I=0;I<sizeof(Expected);++I) if (Model[I]!=Expected[I]) return FALSE;
  // This bring-up profile is explicitly Polaris MP / SDM845 v2.1.
  Id=fdt_getprop(Dtb,0,"qcom,board-id",&Len);
  if (!Id || Len!=8 || fdt32_to_cpu(Id[0])!=0x2c || fdt32_to_cpu(Id[1])) return FALSE;
  Id=fdt_getprop(Dtb,0,"qcom,msm-id",&Len);
  if (!Id || Len!=8 || fdt32_to_cpu(Id[0])!=0x141 || fdt32_to_cpu(Id[1])!=0x20001) return FALSE;
  Node=fdt_path_offset(Dtb,"/chosen");
  if (Node<0) return FALSE;
  Args=fdt_getprop(Dtb,Node,"bootargs",&Len);
  if (!Args || Len<2 || Len>4096 || Args[Len-1]) return FALSE;
  Node=fdt_path_offset(Dtb,"/memory");
  if (Node<0) return FALSE;
  Reg=fdt_getprop(Dtb,Node,"reg",&Len);
  if (!Reg || Len<16 || Len>8*16 || Len%16) return FALSE;
  for (I=0;I<(UINTN)Len/8;I+=2) {
    // Properties need not be naturally aligned for 64-bit loads.
    UINT64 Base=PolarisAndroidBe64(Reg+I),Bytes=PolarisAndroidBe64(Reg+I+1);
    if (!Base || !Bytes || Base>MAX_UINT64-Bytes || Base<Last) return FALSE;
    Last=Base+Bytes;
  }
  return TRUE;
}
typedef BOOLEAN (*POLARIS_ANDROID_RESERVED_VISITOR)(UINT64 Base,UINT64 End,VOID *Context);
STATIC inline BOOLEAN PolarisAndroidVisitReserved(CONST VOID *Dtb,
    POLARIS_ANDROID_RESERVED_VISITOR Visit,VOID *Context) {
  CONST fdt64_t *Reg;uint64_t Base,Bytes;INT32 Node,Len,I,Parent,Count;
  Count=fdt_num_mem_rsv(Dtb);
  if (Count<0) return FALSE;
  for (I=0;I<Count;++I) {
    if (fdt_get_mem_rsv(Dtb,I,&Base,&Bytes) || Bytes>MAX_UINT64-Base ||
        (Bytes && !Visit(Base,Base+Bytes,Context))) return FALSE;
  }
  Parent=fdt_path_offset(Dtb,"/reserved-memory");
  if (Parent<0) return FALSE;
  for (Node=fdt_first_subnode(Dtb,Parent);Node>=0;Node=fdt_next_subnode(Dtb,Node)) {
    Reg=fdt_getprop(Dtb,Node,"reg",&Len);
    if (!Reg) continue; // Dynamic reservations use size/alignment, not reg.
    if (Len<0 || Len%16) return FALSE;
    for (I=0;I<Len/8;I+=2) {
      Base=PolarisAndroidBe64(Reg+I);Bytes=PolarisAndroidBe64(Reg+I+1);
      if (Bytes>MAX_UINT64-Base || (Bytes && !Visit(Base,Base+Bytes,Context))) return FALSE;
    }
  }
  return Node==-FDT_ERR_NOTFOUND;
}
typedef struct { UINT64 Start,End; } POLARIS_ANDROID_RANGE;
STATIC inline BOOLEAN PolarisAndroidOutsideReserved(UINT64 Base,UINT64 End,VOID *Context) {
  CONST POLARIS_ANDROID_RANGE *Range=Context;
  return Range->Start>=End || Base>=Range->End;
}
STATIC inline BOOLEAN PolarisAndroidDramRange(CONST VOID *Dtb,UINT64 Start,UINT64 Size) {
  CONST fdt64_t *Reg;POLARIS_ANDROID_RANGE Range;UINT64 Base,Bytes;INT32 Node,Len,I;
  if (!Size || Start>MAX_UINT64-Size) return FALSE;
  Range=(POLARIS_ANDROID_RANGE){Start,Start+Size};
  Node=fdt_path_offset(Dtb,"/memory");Reg=fdt_getprop(Dtb,Node,"reg",&Len);
  if (!Reg || Len<16 || Len%16) return FALSE;
  for (I=0;I<Len/8;I+=2) {
    Base=PolarisAndroidBe64(Reg+I);Bytes=PolarisAndroidBe64(Reg+I+1);
    if (Base<=Start && Bytes<=MAX_UINT64-Base && Range.End<=Base+Bytes)
      return PolarisAndroidVisitReserved(Dtb,PolarisAndroidOutsideReserved,&Range);
  }
  return FALSE;
}
STATIC inline BOOLEAN PolarisAndroidDtbSanitize(VOID *Dtb) {
  CONST CHAR8 *Drop[]={"kaslr-seed","rng-seed","linux,initrd-start","linux,initrd-end",
    "linux,uefi-system-table","linux,uefi-mmap-start","linux,uefi-mmap-size",
    "linux,uefi-mmap-desc-size","linux,uefi-mmap-desc-ver"};
  INT32 Node=fdt_path_offset(Dtb,"/chosen"),Error;UINTN I;
  if (Node<0) return FALSE;
  for (I=0;I<sizeof(Drop)/sizeof(Drop[0]);++I) {
    Error=fdt_delprop(Dtb,Node,Drop[I]);
    if (Error && Error!=-FDT_ERR_NOTFOUND) return FALSE;
  }
  return TRUE;
}
#endif
