// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_BOOT_CONSOLE_TEXT_H
#define POLARIS_BOOT_CONSOLE_TEXT_H
#include <Library/SerialPortLib.h>
#include "BootConsole.h"

// GraphicsConsole uses this only while the early framebuffer owns the screen.
// Convert UTF-16 to bounded UTF-8 writes; the small early font renders a single
// '?' for an unsupported code point while the RAM log retains its text.
STATIC EFI_STATUS PolarisBootConsoleOutput(CONST CHAR16 *Text) {
  UINT8 Buffer[128];UINTN N=0,I,Bytes;UINT32 Ch;UINT8 Encoded[4];
  if (!Text) return EFI_INVALID_PARAMETER;
  while (*Text) {
    Ch=*Text++;
    if (Ch>=0xd800 && Ch<=0xdbff && *Text>=0xdc00 && *Text<=0xdfff)
      Ch=0x10000+((Ch-0xd800)<<10)+(*Text++-0xdc00);
    else if (Ch>=0xd800 && Ch<=0xdfff) Ch='?';
    if (Ch<0x80) { Encoded[0]=(UINT8)Ch;Bytes=1; }
    else if (Ch<0x800) { Encoded[0]=0xc0|(Ch>>6);Encoded[1]=0x80|(Ch&63);Bytes=2; }
    else if (Ch<0x10000) { Encoded[0]=0xe0|(Ch>>12);Encoded[1]=0x80|((Ch>>6)&63);Encoded[2]=0x80|(Ch&63);Bytes=3; }
    else { Encoded[0]=0xf0|(Ch>>18);Encoded[1]=0x80|((Ch>>12)&63);Encoded[2]=0x80|((Ch>>6)&63);Encoded[3]=0x80|(Ch&63);Bytes=4; }
    if (N+Bytes>sizeof(Buffer)) { SerialPortWrite(Buffer,N);N=0; }
    for (I=0;I<Bytes;++I) Buffer[N++]=Encoded[I];
  }
  if (N) SerialPortWrite(Buffer,N);
  return EFI_SUCCESS;
}
#endif
