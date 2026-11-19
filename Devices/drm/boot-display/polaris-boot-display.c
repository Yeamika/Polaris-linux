// SPDX-License-Identifier: GPL-2.0-only
/* Polaris owner for the shared KernelBoot fbcon window. fbcon draws into RAM;
 * a worker copies damaged rows to the inherited no-map/WC scanout. Native DRM
 * removes this owner through aperture before taking over the display. */
#include <linux/aperture.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/libfdt.h>
#include <linux/memblock.h>
#include <linux/mutex.h>
#include <linux/of_fdt.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include "pj4edk2_fbcon.h"
#include "polaris-boot-display.h"

#define SCANOUT_BASE 0x9d400000ULL
#define SCANOUT_LIMIT 0x9f800000ULL

static unsigned int display_cells[PJ4EDK2_DISPLAY_CELLS];
static struct pj4edk2_fbcon_viewport viewport;
static struct fb_info *boot_fb;
static void __iomem *pixels;
static void *shadow_pixels;
static void *handoff_snapshot;
static bool validated_boot_scanout;
static DEFINE_MUTEX(snapshot_lock);
static DEFINE_SPINLOCK(damage_lock);
static bool owned, dirty;
static unsigned int dirty_first, dirty_end;
static void present(struct work_struct *work);
static DECLARE_WORK(present_work, present);

static void present(struct work_struct *work)
{
    unsigned long flags;
    unsigned int first, end, y;
    size_t offset;

    for (;;) {
        spin_lock_irqsave(&damage_lock, flags);
        if (!owned || !dirty) {
            spin_unlock_irqrestore(&damage_lock, flags);
            return;
        }
        first = dirty_first;
        end = dirty_end;
        dirty = false;
        spin_unlock_irqrestore(&damage_lock, flags);
        /* Copy only window pixels, never the full stride or surrounding UI. */
        for (y = first; y < end; ++y) {
            offset = (size_t)y * viewport.stride +
                     viewport.x * viewport.bytes_per_pixel;
            memcpy_toio(pixels + offset, shadow_pixels + offset,
                        viewport.width * viewport.bytes_per_pixel);
        }
        wmb(); /* WC/no-map scanout has no cached alias. */
    }
}

static void queue_damage(void *context, unsigned int y, unsigned int height)
{
    unsigned long flags;

    if (y < viewport.y || y - viewport.y >= viewport.height ||
        !height || height > viewport.height - (y - viewport.y))
        return;
    spin_lock_irqsave(&damage_lock, flags);
    if (owned) {
        dirty_first = dirty ? min(dirty_first, y) : y;
        dirty_end = dirty ? max(dirty_end, y + height) : y + height;
        dirty = true;
        schedule_work(&present_work);
    }
    spin_unlock_irqrestore(&damage_lock, flags);
}

static void release_display(void)
{
    unsigned long flags;
    bool registered = boot_fb != NULL;

    spin_lock_irqsave(&damage_lock, flags);
    owned = false;
    spin_unlock_irqrestore(&damage_lock, flags);
    /* Detach fbcon before draining the final copies and releasing mappings. */
    pj4edk2_fbcon_unregister(boot_fb);
    boot_fb = NULL;
    cancel_work_sync(&present_work);
    mutex_lock(&snapshot_lock);
    if (registered) {
        vfree(handoff_snapshot);
        handoff_snapshot = shadow_pixels;
    } else {
        vfree(shadow_pixels);
    }
    shadow_pixels = NULL;
    mutex_unlock(&snapshot_lock);
    iounmap(pixels);
    pixels = NULL;
}

/* Copy the retained page into a native buffer with its own pitch. The
 * descriptor was authorized above; the native owner gets RAM, never old MMIO. */
int polaris_boot_display_snapshot(unsigned int *cells, void *target,
                                 size_t bytes, unsigned int width,
                                 unsigned int height, unsigned int stride)
{
    struct pj4edk2_fbcon_viewport view;
    unsigned int y;
    int ret = -ENODEV;

    mutex_lock(&snapshot_lock);
    if (!handoff_snapshot || !target || width != display_cells[6] ||
        height != display_cells[7] || stride < width * 4 || bytes > U32_MAX ||
        (u64)stride * height > bytes)
        goto out;
    memcpy(cells, display_cells, sizeof(display_cells));
    cells[4] = bytes;
    cells[5] = stride * height;
    cells[8] = stride;
    if (!pj4edk2_fbcon_viewport(cells, &view))
        goto out;
    for (y = 0; y < height; ++y)
        memcpy(target + (size_t)y * stride,
               handoff_snapshot + (size_t)y * display_cells[8], width * 4);
    vfree(handoff_snapshot);
    handoff_snapshot = NULL;
    ret = 0;
out:
    mutex_unlock(&snapshot_lock);
    return ret;
}
EXPORT_SYMBOL_GPL(polaris_boot_display_snapshot);

/* The reservation, descriptor and board were checked before registering fbcon.
 * Keep this identity after aperture removal, when the native panel prepares. */
bool polaris_boot_display_handoff_available(void)
{
    return READ_ONCE(validated_boot_scanout);
}
EXPORT_SYMBOL_GPL(polaris_boot_display_handoff_available);


static int display_probe(struct platform_device *pdev)
{
    size_t size = display_cells[4];
    size_t shadow_size = (size_t)display_cells[8] * display_cells[7];
    int ret;

    ret = devm_aperture_acquire_for_platform_device(pdev, SCANOUT_BASE, size);
    if (ret)
        return ret;
    pixels = ioremap_wc(SCANOUT_BASE, size);
    if (!pixels)
        return -ENOMEM;
    shadow_pixels = vzalloc(shadow_size);
    if (!shadow_pixels) {
        ret = -ENOMEM;
        goto release;
    }
    /* Preserve the whole inherited page for the native DRM buffer. Only the
     * fbcon window is ever written by the early present worker. */
    memcpy_fromio(shadow_pixels, pixels, shadow_size);
    WRITE_ONCE(owned, true);
    boot_fb = pj4edk2_fbcon_register(&pdev->dev, THIS_MODULE, display_cells,
                                    shadow_pixels, shadow_size, queue_damage, NULL);
    if (IS_ERR(boot_fb)) {
        ret = PTR_ERR(boot_fb);
        boot_fb = NULL;
        goto release;
    }
    WRITE_ONCE(validated_boot_scanout, true);
    dev_info(&pdev->dev,
             "POLARIS_HANDOFF: shared fbcon window %ux%u at %u,%u rotation=%u; WC present\n",
             viewport.width, viewport.height, viewport.x, viewport.y,
             viewport.rotation * 90);
    return 0;
release:
    release_display();
    return ret;
}

static void display_remove(struct platform_device *pdev)
{
    release_display();
    dev_info(&pdev->dev,
             "POLARIS_HANDOFF: fbcon retired; present worker drained for native DRM\n");
}

static struct platform_driver display_driver = {
    .probe = display_probe, .remove = display_remove,
    .driver = {.name = "polaris-boot-display", .suppress_bind_attrs = true},
};

static int __init display_register(void)
{
    const void *fdt = initial_boot_params, *wire;
    const fdt32_t *phandle;
    const fdt64_t *reg;
    const char *mapping;
    struct platform_device *pdev;
    int chosen, node, parent, bytes, ret;
    u64 address, size, page;

    if (!fdt || fdt_node_check_compatible(fdt, 0, "xiaomi,polaris"))
        return 0;
    chosen = fdt_path_offset(fdt, "/chosen");
    if (chosen < 0)
        return 0;
    wire = fdt_getprop(fdt, chosen, PJ4EDK2_DISPLAY_PROPERTY, &bytes);
    if (!wire || !pj4edk2_display_decode_be(wire, bytes, display_cells) ||
        !pj4edk2_fbcon_viewport(display_cells, &viewport))
        return 0;
    address = ((u64)display_cells[2] << 32) | display_cells[3];
    size = display_cells[4];
    if (address != SCANOUT_BASE || size > SCANOUT_LIMIT - address ||
        display_cells[0] != 2 || display_cells[6] != 1080 ||
        display_cells[7] != 2160 || display_cells[8] != 4320 ||
        display_cells[9] != 2 || display_cells[17] != 0)
        return 0;
    phandle = fdt_getprop(fdt, chosen, "pocketjs,boot-display-memory", &bytes);
    if (!phandle || bytes != 4)
        return 0;
    node = fdt_node_offset_by_phandle(fdt, fdt32_ld(phandle));
    parent = fdt_path_offset(fdt, "/reserved-memory");
    if (node < 0 || parent < 0 || fdt_parent_offset(fdt, node) != parent ||
        fdt_address_cells(fdt, parent) != 2 || fdt_size_cells(fdt, parent) != 2 ||
        !fdt_getprop(fdt, node, "no-map", NULL) ||
        fdt_getprop(fdt, node, "reusable", NULL))
        return 0;
    reg = fdt_getprop(fdt, node, "reg", &bytes);
    if (!reg || bytes != 16 || fdt64_ld(reg) != address || fdt64_ld(reg + 1) != size)
        return 0;
    mapping = fdt_getprop(fdt, node, "pocketjs,scanout-mapping", &bytes);
    if (!mapping || bytes != sizeof("write-combine") ||
        memcmp(mapping, "write-combine", sizeof("write-combine")))
        return 0;
    for (page = address; page < address + size; page += PAGE_SIZE)
        if (memblock_is_map_memory(page))
            return 0;
    ret = platform_driver_register(&display_driver);
    if (ret)
        return ret;
    pdev = platform_device_register_simple("polaris-boot-display", -1, NULL, 0);
    if (IS_ERR(pdev)) {
        ret = PTR_ERR(pdev);
        platform_driver_unregister(&display_driver);
        return ret;
    }
    if (!pdev->dev.driver) {
        platform_device_unregister(pdev);
        platform_driver_unregister(&display_driver);
        return -ENODEV;
    }
    return 0;
}
/* fbmem is ready; fbcon can still draw the built-in logo and replay printk. */
device_initcall(display_register);
