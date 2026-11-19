// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include "../efi/LinuxBoot/BootOutputLayout.h"
#include "pj4edk2_linux_display.h"
int main(void) {
 struct polaris_output_layout l;
 unsigned int v[20];
 for(unsigned int i=0;i<4;i++) {
  unsigned int rotation=i*90,w=i%2?2160:1080,h=i%2?1080:2160;
  struct pj4edk2_surface s={0x9d400000,1080*2160*4,1080*2160*4,1080,2160,4320,2,rotation,0};
  struct pj4edk2_region r={w,h,w/3,48,w-w/3,h-48};
  assert(pj4edk2_linux_display_cells(&s,&r,v));
  assert(polaris_output_layout(&l,v));
  assert(l.left==w/3+8 && l.top==56);
  for(unsigned int y=l.top;y<l.top+l.rows*24;y++)
   for(unsigned int x=l.left;x<l.left+l.cols*12;x++) {
    unsigned long long o=polaris_output_offset(&l,x,y);
    assert(o%4==0 && o+4<=1080*2160*4);
    unsigned int px=o%4320/4,py=o/4320,nx=px,ny=py;
    if(rotation==90) {nx=py;ny=1079-px;}
    if(rotation==180) {nx=1079-px;ny=2159-py;}
    if(rotation==270) {nx=2159-py;ny=px;}
    assert(nx>=r.x && nx<r.x+r.w && ny>=r.y && ny<r.y+r.h);
   }
  v[14]=12;assert(!polaris_output_layout(&l,v));
 }
 puts("PASS: boot-output panel bounds and rotations 0/90/180/270");
 return 0;
}
