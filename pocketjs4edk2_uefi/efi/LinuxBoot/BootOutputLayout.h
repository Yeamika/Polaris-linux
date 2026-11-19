// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_BOOT_OUTPUT_LAYOUT_H
#define POLARIS_BOOT_OUTPUT_LAYOUT_H
#include "pj4edk2_boot_display.h"
struct polaris_output_layout {
  unsigned int physical_w,physical_h,stride,rotation,left,top,cols,rows;
};
static inline int polaris_output_layout(struct polaris_output_layout *out,const unsigned int *v) {
  unsigned int w,h,right,bottom;
  unsigned long long left,top;
  if (!out || !pj4edk2_display_validate_cells(v,20) || v[9]!=2 || v[17]!=0) return 0;
  w=(v[16]==90 || v[16]==270)?v[7]:v[6];
  h=(v[16]==90 || v[16]==270)?v[6]:v[7];
  left=((unsigned long long)v[12]*w+v[10]-1)/v[10]+8;
  top=((unsigned long long)v[13]*h+v[11]-1)/v[11]+8;
  right=(unsigned long long)(v[12]+v[14])*w/v[10];
  bottom=(unsigned long long)(v[13]+v[15])*h/v[11];
  if (right<8 || bottom<8 || right-8<=left || bottom-8<=top) return 0;
  *out=(struct polaris_output_layout){v[6],v[7],v[8],v[16],left,top,
      (right-8-left)/12,(bottom-8-top)/24};
  return out->cols && out->rows && out->cols<=180 && out->rows<=90;
}
static inline unsigned long long polaris_output_offset(const struct polaris_output_layout *l,
        unsigned int x,unsigned int y) {
  unsigned int px=x,py=y;
  if (l->rotation==90) {px=l->physical_w-1-y;py=x;}
  else if (l->rotation==180) {px=l->physical_w-1-x;py=l->physical_h-1-y;}
  else if (l->rotation==270) {px=y;py=l->physical_h-1-x;}
  return (unsigned long long)py*l->stride+px*4;
}
#endif
