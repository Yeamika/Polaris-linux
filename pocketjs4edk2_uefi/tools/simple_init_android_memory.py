"""Wire native Android page placement into the private SimpleInit build copy."""

WRAPPER = '''
#include <comp_libfdt.h>
#include "../main/BootRequest.h"
#include "../main/NativeFdt.h"
#include "../main/AndroidMemory.h"

STATIC CONST VOID *mPolarisAndroidDtb;
STATIC UINT64 mPolarisAndroidMemoryLimit;

VOID polaris_linux_set_android_memory(CONST VOID *Dtb,UINT64 Limit) {
  mPolarisAndroidDtb=Dtb;
  mPolarisAndroidMemoryLimit=Limit;
}

VOID *polaris_linux_allocate_aligned_pages(UINTN Pages,UINTN Align) {
  if (mPolarisAndroidDtb)
    return PolarisAndroidAllocatePages(mPolarisAndroidDtb,Pages,Align,mPolarisAndroidMemoryLimit);
  return AllocateAlignedPages(Pages,Align);
}
'''


def patch_android_allocation(simple):
    source = simple / 'src/linux-boot'
    function = 'polaris_linux_allocate_aligned_pages('
    for name in ('loader.c', 'compress.c', 'fdt.c', 'dtb_sel.c'):
        path = source / name
        text = path.read_text()
        if function in text:
            continue
        if text.count('AllocateAlignedPages(') != 1:
            raise ValueError('Unexpected SimpleInit page allocator in ' + name)
        text = text.replace('AllocateAlignedPages(', function, 1)
        if name == 'loader.c':
            anchor = '#define TAG "loader"\n'
            if text.count(anchor) != 1:
                raise ValueError('Unexpected SimpleInit memory loader')
            text = text.replace(anchor, anchor + WRAPPER, 1)
        path.write_text(text)
    header = source / 'internal.h'
    text = header.read_text()
    if function not in text:
        anchor = 'extern bool linux_file_allocate(linux_file_info*fi,size_t size);'
        if text.count(anchor) != 1:
            raise ValueError('Unexpected SimpleInit file allocator declaration')
        text = text.replace(anchor, anchor + '\nextern VOID *' + function + 'UINTN Pages,UINTN Align);', 1)
        header.write_text(text)
