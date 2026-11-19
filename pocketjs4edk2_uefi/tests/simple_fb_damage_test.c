/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../platform/SimpleFbDamage.h"

static UINT8 Pixels[32 * 24 * 4];
static struct { UINTN Offset, Bytes; } Flushes[24];
static UINTN Count;
VOID *EFIAPI WriteBackInvalidateDataCacheRange(VOID *Address, UINTN Bytes) {
  UINTN Offset = (UINT8 *)Address - Pixels;
  assert(Count < 24 && Offset <= sizeof(Pixels) && Bytes <= sizeof(Pixels) - Offset);
  Flushes[Count].Offset = Offset; Flushes[Count++].Bytes = Bytes;
  return Address;
}

int main(void) {
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info = {0};
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode = {0};
  EFI_GRAPHICS_OUTPUT_PROTOCOL Gop = {0};
  Info.HorizontalResolution = 32; Info.VerticalResolution = 24; Info.PixelsPerScanLine = 32;
  Mode.Info = &Info; Mode.FrameBufferBase = (UINTN)Pixels; Mode.FrameBufferSize = sizeof(Pixels);
  Gop.Mode = &Mode;
  PolarisSimpleFbFlush(&Gop,EfiBltVideoToVideo,5,7,1,1);
  assert(Count == 1 && Flushes[0].Offset == (7 * 32 + 5) * 4 && Flushes[0].Bytes == 4);
  Count = 0;
  PolarisSimpleFbFlush(&Gop,EfiBltBufferToVideo,2,3,7,4);
  assert(Count == 4);
  for (UINTN I=0; I<4; ++I)
    assert(Flushes[I].Offset == ((3 + I) * 32 + 2) * 4 && Flushes[I].Bytes == 7 * 4);
  Count = 0;
  PolarisSimpleFbFlush(&Gop,EfiBltVideoFill,0,0,32,24);
  assert(Count == 1 && Flushes[0].Offset == 0 && Flushes[0].Bytes == sizeof(Pixels));
  Count = 0;
  // A full visible row still excludes scanline padding.
  Info.HorizontalResolution = 29;
  PolarisSimpleFbFlush(&Gop,EfiBltVideoToVideo,0,21,29,3);
  assert(Count == 3 && Flushes[2].Offset == 23 * 32 * 4 && Flushes[2].Bytes == 29 * 4);
  Count = 0;
  PolarisSimpleFbFlush(&Gop,EfiBltVideoToBltBuffer,0,0,29,24);
  PolarisSimpleFbFlush(&Gop,EfiGraphicsOutputBltOperationMax,0,0,29,24);
  PolarisSimpleFbFlush(&Gop,EfiBltVideoToVideo,29,0,1,1);
  PolarisSimpleFbFlush(&Gop,EfiBltVideoToVideo,0,24,1,1);
  PolarisSimpleFbFlush(&Gop,EfiBltVideoToVideo,0,0,0,1);
  PolarisSimpleFbFlush(&Gop,EfiBltVideoToVideo,MAX_UINTN,0,1,1);
  Mode.FrameBufferSize = 127;
  PolarisSimpleFbFlush(&Gop,EfiBltVideoToVideo,0,1,1,1);
  Mode.FrameBufferSize = sizeof(Pixels);
  Mode.FrameBufferBase = MAX_UINTN - 1;
  PolarisSimpleFbFlush(&Gop,EfiBltVideoToVideo,0,0,1,1);
  assert(Count == 0);
  puts("PASS: SimpleFb damage cache ranges, one-pixel/full/padded rows, read-only BLT and bounds");
  return 0;
}
