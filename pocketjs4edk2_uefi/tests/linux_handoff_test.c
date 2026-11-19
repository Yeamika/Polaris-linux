// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>
#include <pocketjs_boot_context.h>
#undef NULL
#include "../efi/LinuxBoot/Handoff.c"
UINTN EFIAPI Print(CONST CHAR16 *Format,...) { (VOID)Format;return 0; }

EFI_GUID gEfiGraphicsOutputProtocolGuid;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
static EFI_MEMORY_DESCRIPTOR Memory;
static EFI_SYSTEM_TABLE System;
static EFI_CONFIGURATION_TABLE Config;
static PJB_BOOT_DISPLAY Request;
static unsigned char Dtb[262144];

VOID *EFIAPI AllocatePool(UINTN Size) { return malloc(Size); }
VOID EFIAPI FreePool(VOID *Buffer) { free(Buffer); }
VOID *EFIAPI CopyMem(VOID *Dst, CONST VOID *Src, UINTN N) { return memcpy(Dst,Src,N); }
VOID *EFIAPI ZeroMem(VOID *Dst, UINTN N) { return memset(Dst,0,N); }
BOOLEAN EFIAPI CompareGuid(CONST GUID *A, CONST GUID *B) { return memcmp(A,B,sizeof(*A))==0; }
static EFI_STATUS EFIAPI Map(UINTN *Size, EFI_MEMORY_DESCRIPTOR *Out, UINTN *Key,
                            UINTN *Step, UINT32 *Version) {
  *Step=sizeof(Memory); *Key=1; *Version=1;
  if (!Out || *Size<sizeof(Memory)) { *Size=sizeof(Memory); return EFI_BUFFER_TOO_SMALL; }
  memcpy(Out,&Memory,sizeof(Memory)); *Size=sizeof(Memory); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid, VOID *Registration, VOID **Out) {
  (VOID)Guid; (VOID)Registration; *Out=&Gop; return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES Services={.GetMemoryMap=Map,.LocateProtocol=Locate};
EFI_BOOT_SERVICES *gBS=&Services;

static void Setup(void) {
  PJB_GUID Id=PJB_BOOT_DISPLAY_GUID;
  assert(!fdt_create_empty_tree(Dtb,sizeof(Dtb)));
  memset(&System,0,sizeof(System));
  memset(&Request,0,sizeof(Request));
  Request.revision=PJB_BOOT_DISPLAY_REVISION; Request.size=sizeof(Request);
  Request.display_width=2160; Request.display_height=1080;
  Request.x=40;Request.y=40;Request.width=1900;Request.height=800;Request.rotation=90;
  memcpy(&Config.VendorGuid,&Id,sizeof(Id));Config.VendorTable=&Request;
  System.NumberOfTableEntries=1;System.ConfigurationTable=&Config;
  Info=(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){.HorizontalResolution=1080,
    .VerticalResolution=2160,.PixelsPerScanLine=1080,
    .PixelFormat=PixelBlueGreenRedReserved8BitPerColor};
  Mode=(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){.Info=&Info,
    .FrameBufferBase=0x9d400000,.FrameBufferSize=1080*2160*4};
  Gop=(EFI_GRAPHICS_OUTPUT_PROTOCOL){.Mode=&Mode};
  Memory=(EFI_MEMORY_DESCRIPTOR){.Type=EfiReservedMemoryType,.PhysicalStart=0x9d400000,
    .NumberOfPages=0x2400000/4096,.Attribute=EFI_MEMORY_WT};
}
static void Check(void) {
  int chosen=fdt_path_offset(Dtb,"/chosen"),size;
  const fdt32_t *p=fdt_getprop(Dtb,chosen,"pocketjs,boot-display-memory",&size);
  assert(p && size==4);
  int node=fdt_node_offset_by_phandle(Dtb,fdt32_to_cpu(*p));
  assert(node>=0 && fdt_getprop(Dtb,node,"no-map",NULL));
  assert(!fdt_getprop(Dtb,node,"pocketjs,coherent-scanout",NULL));
  assert(!fdt_getprop(Dtb,node,"reusable",NULL));
  assert(strcmp(fdt_getprop(Dtb,node,"pocketjs,scanout-mapping",NULL),"write-combine")==0);
  const void *reg=fdt_getprop(Dtb,node,"reg",&size);
  fdt64_t values[2];
  assert(reg && size==16);
  memcpy(values,reg,sizeof(values));
  assert(fdt64_to_cpu(values[0])==0x9d400000);
  assert(fdt64_to_cpu(values[1])==0x8e7000);
  unsigned int cells[20];const void *wire=fdt_getprop(Dtb,chosen,PJ4EDK2_DISPLAY_PROPERTY,&size);
  assert(pj4edk2_display_decode_be(wire,size,cells));
  assert(cells[4]==0x8e7000 && cells[5]==1080*2160*4 && cells[9]==2);
  assert(fdt_num_mem_rsv(Dtb)==1);
}
int main(int argc, char **argv) {
  Setup();assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_SUCCESS);Check();
  Setup();System.NumberOfTableEntries=0;
  assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_SUCCESS);Check();
  Setup();Memory.Attribute=EFI_MEMORY_WB;
  assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_SUCCESS);Check();
  Setup();Memory.Type=EfiConventionalMemory;
  assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_NO_MAPPING);
  Setup();Memory.NumberOfPages=1;
  assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_NO_MAPPING);
  Setup();Memory.Attribute=EFI_MEMORY_UC;
  assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_NO_MAPPING);
  Setup();Mode.FrameBufferBase+=4096;
  assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_UNSUPPORTED);
  Setup();Request.rotation=45;
  assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_UNSUPPORTED);
  Setup();Request.width=3000;
  assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_UNSUPPORTED);
  Setup();Info.PixelFormat=PixelRedGreenBlueReserved8BitPerColor;
  assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_UNSUPPORTED);
  if (argc==2) {
    unsigned char raw[sizeof(Dtb)];
    FILE *file=fopen(argv[1],"rb");assert(file);
    size_t size=fread(raw,1,sizeof(raw),file);assert(!ferror(file));fclose(file);
    assert(size>=sizeof(struct fdt_header) && !fdt_check_header(raw));
    assert((size_t)fdt_totalsize(raw)<=size);
    Setup();assert(!fdt_open_into(raw,Dtb,sizeof(Dtb)));
    assert(PolarisLinuxDisplayHandoff(Dtb,&System)==EFI_SUCCESS);Check();
    puts("PASS: built Polaris DTB accepts the real EFI handoff");
  }
  puts("PASS: Polaris scanout map, full reservation, DT ownership, fallback and invalid requests");
  return 0;
}
