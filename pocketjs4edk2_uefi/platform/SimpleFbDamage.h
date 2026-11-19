/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef POLARIS_SIMPLE_FB_DAMAGE_H
#define POLARIS_SIMPLE_FB_DAMAGE_H

#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/CacheMaintenanceLib.h>

/* The inherited SimpleFb scanout is linear BGRX. CPU rendering (including
 * rotated damage) is presented by a same-rectangle VideoToVideo BLT. Clean
 * only the written rows; even a one-pixel BLT must not evict the full screen.
 * Keep the original cache maintenance operation for this platform mapping. */
STATIC VOID
PolarisSimpleFbFlush (
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION Operation,
  UINTN X, UINTN Y, UINTN Width, UINTN Height
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode = Gop->Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info = Mode->Info;
  UINTN Stride, Offset, Bytes, Row;
  UINT8 *Base;
  if (Operation != EfiBltVideoFill && Operation != EfiBltBufferToVideo &&
      Operation != EfiBltVideoToVideo) return;
  if (!Width || !Height || X > Info->HorizontalResolution ||
      Width > Info->HorizontalResolution - X || Y > Info->VerticalResolution ||
      Height > Info->VerticalResolution - Y ||
      Info->PixelsPerScanLine < Info->HorizontalResolution ||
      (UINTN)Info->PixelsPerScanLine > MAX_UINTN / 4) return;
  Stride = (UINTN)Info->PixelsPerScanLine * 4;
  Bytes = Width * 4;
  if (!Stride || Y > Mode->FrameBufferSize / Stride) return;
  Offset = Y * Stride + X * 4;
  if (Offset > Mode->FrameBufferSize || Bytes > Mode->FrameBufferSize - Offset ||
      Height - 1 > (Mode->FrameBufferSize - Offset - Bytes) / Stride) return;
  if (Mode->FrameBufferBase > MAX_UINTN ||
      (UINTN)Mode->FrameBufferBase > MAX_UINTN - Mode->FrameBufferSize) return;
  Base = (UINT8 *)(UINTN)Mode->FrameBufferBase + Offset;
  if (Bytes == Stride) {
    WriteBackInvalidateDataCacheRange(Base, Height * Stride);
  } else {
    for (Row = 0; Row < Height; ++Row)
      WriteBackInvalidateDataCacheRange(Base + Row * Stride, Bytes);
  }
}

#endif
