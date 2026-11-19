// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_ANDROID_REQUEST_H
#define POLARIS_ANDROID_REQUEST_H
#include <Uefi.h>
#define POLARIS_ANDROID_MAGIC 0x314452414c4f5055ULL
#define POLARIS_ANDROID_PREPARE_ONLY 1U
#define POLARIS_ANDROID_REVISION 3U
#define POLARIS_ANDROID_DTB_LIMIT (1024U*1024U)
typedef struct {
  UINT64 Magic;
  UINT32 Revision,Size;
  UINT64 Image,ImageSize;
  UINT32 Flags,Reserved;
  UINT64 Dtb,DtbSize;
} POLARIS_ANDROID_REQUEST;
STATIC inline BOOLEAN PolarisAndroidRequestValid(CONST POLARIS_ANDROID_REQUEST *R,UINTN Size) {
  // Check the complete envelope before accessing fields added by this revision.
  if (!R || Size!=sizeof(*R)) return FALSE;
  if (R->Magic!=POLARIS_ANDROID_MAGIC || R->Revision!=POLARIS_ANDROID_REVISION ||
      R->Size!=sizeof(*R) || R->Reserved || (R->Flags&~POLARIS_ANDROID_PREPARE_ONLY)) return FALSE;
  return R->Image && R->ImageSize>=1648 && R->ImageSize<=64*1024*1024 &&
    R->Image<=MAX_UINT64-R->ImageSize && R->Dtb && R->DtbSize>=40 &&
    R->DtbSize<=POLARIS_ANDROID_DTB_LIMIT && R->Dtb<=MAX_UINT64-R->DtbSize;
}
STATIC inline UINT32 PolarisAndroidU32(CONST UINT8 *P) {
  return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);
}
STATIC inline BOOLEAN PolarisAndroidImageValid(CONST VOID *Image,UINTN Size) {
  CONST UINT8 *P=Image;UINT32 Page,Version,Parts[3];UINT64 End;UINTN I;
  CONST CHAR8 *Magic="ANDROID!";
  if (!P || Size<1648 || Size>64*1024*1024) return FALSE;
  for (I=0;I<8;++I) if (P[I]!=(UINT8)Magic[I]) return FALSE;
  Page=PolarisAndroidU32(P+36);Version=PolarisAndroidU32(P+40);
  if (Version>1 || Page<2048 || Page>65536 || (Page&(Page-1))) return FALSE;
  Parts[0]=PolarisAndroidU32(P+8);Parts[1]=PolarisAndroidU32(P+16);Parts[2]=PolarisAndroidU32(P+24);
  if (!Parts[0] || !Parts[1] || Parts[2]) return FALSE;
  End=Page;
  for (I=0;I<3;++I) {
    End+=((UINT64)Parts[I]+Page-1)&~((UINT64)Page-1);
    if (End>Size) return FALSE;
  }
  // Only legacy v0/v1 with kernel-appended DTBs, no recovery_dtbo or second.
  if (Version==1 && PolarisAndroidU32(P+1632)) return FALSE;
  return TRUE;
}
#endif
