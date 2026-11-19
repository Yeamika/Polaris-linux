// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_ANDROID_KERNEL_H
#define POLARIS_ANDROID_KERNEL_H
STATIC UINT64 PolarisAndroidLe64(CONST UINT8 *P) {
  return PolarisAndroidU32(P)|((UINT64)PolarisAndroidU32(P+4)<<32);
}
STATIC BOOLEAN PolarisAndroidKernelValid(CONST VOID *Image,UINTN Size,
    UINT64 Address,UINT64 Capacity,UINT64 *Footprint) {
  CONST UINT8 *P=Image;UINT64 Offset,Bytes,Flags;
  if (!P || Size<64 || PolarisAndroidU32(P+56)!=0x644d5241) return FALSE;
  Offset=PolarisAndroidLe64(P+8);Bytes=PolarisAndroidLe64(P+16);Flags=PolarisAndroidLe64(P+24);
  // SimpleInit reserves a 2 MiB-aligned allocation plus the legacy text offset.
  if (Offset!=0x80000 || Address<Offset || ((Address-Offset)&0x1fffff) ||
      (Flags&~0xeULL) || Bytes<Size || Bytes>Capacity || Address>MAX_UINT64-Bytes) return FALSE;
  *Footprint=Bytes;
  return TRUE;
}
#endif
