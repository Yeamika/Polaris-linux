// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Handoff.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>
#include <libfdt.h>
#define PJB_EDK2
#include "pocketjs_boot_context.h"
#include "pj4edk2_linux_display.h"
#include "pj4edk2_linux_fdt.h"

STATIC BOOLEAN ReservedScanoutRam(UINT64 Base, UINT64 Bytes) {
  EFI_MEMORY_DESCRIPTOR *Map = NULL, *D;
  UINTN Size = 0, Key, Step = 0, Offset;
  UINT32 Version;
  UINT64 Cursor = Base, End;
  EFI_STATUS Status;
  if (!Bytes || Base > MAX_UINT64 - Bytes) return FALSE;
  End = Base + Bytes;
  Status = gBS->GetMemoryMap(&Size, NULL, &Key, &Step, &Version);
  if (Status != EFI_BUFFER_TOO_SMALL || Step < sizeof(*D) || Step > MAX_UINTN/8 || Size > MAX_UINTN - Step*8)
    return FALSE;
  Size += Step*8;
  Map = AllocatePool(Size);
  if (Map == NULL) return FALSE;
  Status = gBS->GetMemoryMap(&Size, Map, &Key, &Step, &Version);
  if (EFI_ERROR(Status) || Step < sizeof(*D) || Size % Step) { FreePool(Map); return FALSE; }
  while (Cursor < End) {
    BOOLEAN Found = FALSE;
    for (Offset = 0; Offset < Size; Offset += Step) {
      UINT64 Limit;
      D = (VOID *)((UINT8 *)Map + Offset);
      if (D->NumberOfPages > (MAX_UINT64 - D->PhysicalStart)/EFI_PAGE_SIZE) continue;
      Limit = D->PhysicalStart + EFI_PAGES_TO_SIZE(D->NumberOfPages);
      if (D->Type == EfiReservedMemoryType &&
          (D->Attribute & (EFI_MEMORY_WB | EFI_MEMORY_WT | EFI_MEMORY_WC)) &&
          D->PhysicalStart <= Cursor && Limit > Cursor) {
        Cursor = MIN(Limit, End);
        Found = TRUE;
        break;
      }
    }
    if (!Found) { FreePool(Map); return FALSE; }
  }
  FreePool(Map);
  return TRUE;
}

EFI_STATUS PolarisLinuxDisplayHandoff(VOID *Dtb, EFI_SYSTEM_TABLE *System) {
  PJB_GUID Wire = PJB_BOOT_DISPLAY_GUID;
  EFI_GUID Guid;
  PJB_BOOT_DISPLAY *Request = NULL;
  PJB_BOOT_DISPLAY Fallback;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
  struct pj4edk2_surface Surface;
  struct pj4edk2_region Region;
  UINTN I;
  UINT32 Cells[20], Phandle = 1;
  INT32 Chosen, Reserved, Node, Depth, End;
  fdt64_t Reg[2];
  EFI_STATUS Status;
  CopyMem(&Guid, &Wire, sizeof(Guid));
  Chosen = fdt_path_offset(Dtb, "/chosen");
  if (Chosen == -FDT_ERR_NOTFOUND) Chosen = fdt_add_subnode(Dtb, 0, "chosen");
  if (Chosen < 0) return EFI_VOLUME_CORRUPTED;
  // Never propagate stale display requests from an on-disk device tree.
  End = fdt_delprop(Dtb, Chosen, PJ4EDK2_DISPLAY_PROPERTY);
  if (End && End != -FDT_ERR_NOTFOUND) return EFI_VOLUME_CORRUPTED;
  End = fdt_delprop(Dtb, Chosen, "pocketjs,boot-display-memory");
  if (End && End != -FDT_ERR_NOTFOUND) return EFI_VOLUME_CORRUPTED;
  for (I = 0; I < System->NumberOfTableEntries; ++I)
    if (CompareGuid(&Guid, &System->ConfigurationTable[I].VendorGuid))
      Request = System->ConfigurationTable[I].VendorTable;
  Status = gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (EFI_ERROR(Status) || Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL ||
      !Gop->Mode->FrameBufferBase || !Gop->Mode->FrameBufferSize ||
      Gop->Mode->FrameBufferSize > MAX_UINT32) return EFI_UNSUPPORTED;
  Info = Gop->Mode->Info;
  // Polaris-specific inherited scanout. The common WB/coherent adapter cannot
  // consume the SDM845 WT mapping; pair this with the board's no-map/WC owner.
  if (Gop->Mode->FrameBufferBase != 0x9d400000ULL ||
      Info->HorizontalResolution != 1080 || Info->VerticalResolution != 2160 ||
      Info->PixelsPerScanLine != 1080 ||
      Gop->Mode->FrameBufferSize < 1080U*2160U*4U ||
      Gop->Mode->FrameBufferSize > 0x02400000U) return EFI_UNSUPPORTED;
  if (Request == NULL) {
    ZeroMem(&Fallback, sizeof(Fallback));
    Fallback.revision = PJB_BOOT_DISPLAY_REVISION;
    Fallback.size = sizeof(Fallback);
    Fallback.display_width = Fallback.width = Info->VerticalResolution;
    Fallback.display_height = Fallback.height = Info->HorizontalResolution;
    Fallback.rotation = 90;
    Request = &Fallback;
  }
  if (Request->revision != PJB_BOOT_DISPLAY_REVISION || Request->size < sizeof(*Request)) return EFI_UNSUPPORTED;
  Surface = (struct pj4edk2_surface){Gop->Mode->FrameBufferBase,
    ALIGN_VALUE(Gop->Mode->FrameBufferSize, EFI_PAGE_SIZE),
    Gop->Mode->FrameBufferSize, Info->HorizontalResolution, Info->VerticalResolution,
    Info->PixelsPerScanLine*4, 0, Request->rotation, 0};
  if (Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) Surface.format = 2;
  else return EFI_UNSUPPORTED;
  Region = (struct pj4edk2_region){Request->display_width, Request->display_height,
    Request->x, Request->y, Request->width, Request->height};
  if (!pj4edk2_linux_display_cells(&Surface, &Region, Cells)) return EFI_UNSUPPORTED;
  if (!ReservedScanoutRam(Surface.address, Surface.capacity)) return EFI_NO_MAPPING;
  Reserved = fdt_path_offset(Dtb, "/reserved-memory");
  if (Reserved == -FDT_ERR_NOTFOUND) {
    Reserved = fdt_add_subnode(Dtb, 0, "reserved-memory");
    if (Reserved < 0 || fdt_setprop_u32(Dtb, Reserved, "#address-cells", 2) ||
        fdt_setprop_u32(Dtb, Reserved, "#size-cells", 2) ||
        fdt_setprop(Dtb, Reserved, "ranges", NULL, 0)) return EFI_VOLUME_CORRUPTED;
  } else if (Reserved < 0 || fdt_address_cells(Dtb, Reserved) != 2 || fdt_size_cells(Dtb, Reserved) != 2)
    return EFI_UNSUPPORTED;
  Node = -1; Depth = 0;
  while ((Node = fdt_next_node(Dtb, Node, &Depth)) >= 0) {
    UINT32 P = fdt_get_phandle(Dtb, Node);
    if (P >= Phandle) {
      if (P >= 0xfffffffeU) return EFI_UNSUPPORTED;
      Phandle = P+1;
    }
  }
  if (Node != -FDT_ERR_NOTFOUND) return EFI_VOLUME_CORRUPTED;
  // Node offsets can change after edits: resolve them again before each use.
  Reserved = fdt_path_offset(Dtb, "/reserved-memory");
  Node = fdt_add_subnode(Dtb, Reserved, "pj4edk2-scanout");
  if (Node == -FDT_ERR_EXISTS) Node = fdt_subnode_offset(Dtb, Reserved, "pj4edk2-scanout");
  Reg[0] = cpu_to_fdt64(Surface.address); Reg[1] = cpu_to_fdt64(Surface.capacity);
  if (Node < 0 || fdt_setprop(Dtb, Node, "reg", Reg, sizeof(Reg)) ||
      fdt_setprop_u32(Dtb, Node, "phandle", Phandle) ||
      fdt_setprop(Dtb, Node, "no-map", NULL, 0) ||
      fdt_setprop_string(Dtb, Node, "pocketjs,scanout-mapping", "write-combine")) return EFI_VOLUME_CORRUPTED;
  End = fdt_delprop(Dtb, Node, "pocketjs,coherent-scanout");
  if (End && End != -FDT_ERR_NOTFOUND) return EFI_VOLUME_CORRUPTED;
  End = fdt_delprop(Dtb, Node, "reusable");
  if (End && End != -FDT_ERR_NOTFOUND) return EFI_VOLUME_CORRUPTED;
  Chosen = fdt_path_offset(Dtb, "/chosen");
  if (Chosen < 0 || fdt_setprop_u32(Dtb, Chosen, "pocketjs,boot-display-memory", Phandle) ||
      pj4edk2_linux_fdt_display(Dtb, Cells) || fdt_add_mem_rsv(Dtb, Surface.address, Surface.capacity))
    return EFI_VOLUME_CORRUPTED;
  Print(L"Boot-output: %s logical=%ux%u region=%u,%u %ux%u rotation=%u\r\n",
      Request==&Fallback?L"standalone fallback":L"pj4edk2",
      Request->display_width,Request->display_height,Request->x,Request->y,
      Request->width,Request->height,Request->rotation);
  return EFI_SUCCESS;
}
