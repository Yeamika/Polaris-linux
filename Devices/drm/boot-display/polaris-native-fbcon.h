/* SPDX-License-Identifier: GPL-2.0-only */
/* MSM fbdev keeps the full native KMS buffer; fbcon sees only the boot window.
 * Drawing reuses KernelBoot helpers, with RAM -> native WC presentation. */
#ifndef POLARIS_NATIVE_FBCON_H
#define POLARIS_NATIVE_FBCON_H
#include <linux/of.h>
#include <linux/console.h>
#include <linux/fbcon.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include "common/include/pj4edk2_fbcon.h"

#include "polaris-boot-display.h"

struct polaris_native_fbcon {
    struct fb_info *info;
    struct fb_info draw;
    struct pj4edk2_fbcon drawing;
    struct fb_ops ops;
    const struct fb_ops *original_ops;
    void *shadow, *target;
    size_t bytes;
    unsigned int width, height;
    spinlock_t lock;
    struct work_struct work;
    bool owned, dirty;
    unsigned int first, end;
    void (*damage)(void *, unsigned int, unsigned int, unsigned int, unsigned int);
    void *context;
};
static struct polaris_native_fbcon *polaris_native_console;
static DEFINE_MUTEX(polaris_native_control);
static bool polaris_console_fullscreen;

static void polaris_native_present(struct work_struct *work)
{
    struct polaris_native_fbcon *n = container_of(work, struct polaris_native_fbcon, work);
    struct pj4edk2_fbcon_viewport *v = &n->drawing.view;
    unsigned long flags;
    unsigned int first, end, y;
    size_t offset;

    for (;;) {
        spin_lock_irqsave(&n->lock, flags);
        if (!n->owned || !n->dirty) {
            spin_unlock_irqrestore(&n->lock, flags);
            return;
        }
        first = n->first;
        end = n->end;
        n->dirty = false;
        spin_unlock_irqrestore(&n->lock, flags);
        for (y = first; y < end; ++y) {
            offset = (size_t)y * v->stride + v->x * v->bytes_per_pixel;
            memcpy(n->target + offset, n->shadow + offset, v->width * v->bytes_per_pixel);
        }
        wmb();
        if (n->damage)
            n->damage(n->context, v->x, first, v->width, end - first);
    }
}

static void polaris_native_damage(void *context, unsigned int y, unsigned int height)
{
    struct polaris_native_fbcon *n = context;
    struct pj4edk2_fbcon_viewport *v = &n->drawing.view;
    unsigned long flags;

    if (y < v->y || y - v->y >= v->height || !height || height > v->height - (y - v->y))
        return;
    spin_lock_irqsave(&n->lock, flags);
    if (n->owned) {
        n->first = n->dirty ? min(n->first, y) : y;
        n->end = n->dirty ? max(n->end, y + height) : y + height;
        n->dirty = true;
        schedule_work(&n->work);
    }
    spin_unlock_irqrestore(&n->lock, flags);
}

static void polaris_native_fillrect(struct fb_info *info, const struct fb_fillrect *r)
{
    pj4edk2_fbcon_fillrect(&polaris_native_console->draw, r);
}
static void polaris_native_copyarea(struct fb_info *info, const struct fb_copyarea *r)
{
    pj4edk2_fbcon_copyarea(&polaris_native_console->draw, r);
}
static void polaris_native_imageblit(struct fb_info *info, const struct fb_image *image)
{
    pj4edk2_fbcon_imageblit(&polaris_native_console->draw, image);
}
static int polaris_native_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
    return var->xres == info->var.xres && var->yres == info->var.yres &&
           var->xres_virtual == info->var.xres && var->yres_virtual == info->var.yres &&
           var->bits_per_pixel == 32 && !var->xoffset && !var->yoffset ? 0 : -EINVAL;
}
static int polaris_native_pan(struct fb_var_screeninfo *var, struct fb_info *info)
{
    return var->xoffset || var->yoffset ? -EINVAL : 0;
}

static void polaris_native_destroy(struct fb_info *info)
{
    struct polaris_native_fbcon *n;
    const struct fb_ops *ops;
    unsigned long flags;

    mutex_lock(&polaris_native_control);
    n = polaris_native_console;
    ops = n->original_ops;
    spin_lock_irqsave(&n->lock, flags);
    n->owned = false;
    spin_unlock_irqrestore(&n->lock, flags);
    cancel_work_sync(&n->work);
    info->fbops = ops;
    info->screen_buffer = n->target;
    info->screen_size = n->bytes;
    polaris_native_console = NULL;
    vfree(n->shadow);
    kfree(n);
    polaris_console_fullscreen = false;
    mutex_unlock(&polaris_native_control);
    if (ops->fb_destroy)
        ops->fb_destroy(info);
}

/* Login explicitly retires the inherited UI; native KMS geometry stays fixed.
 * The control mutex pins n across unregister/destroy; console_lock excludes
 * fbcon drawing/cursor work while its RAM view changes. */
static int polaris_native_fullscreen_set(const char *text, const struct kernel_param *kp)
{
    struct polaris_native_fbcon *n;
    struct pj4edk2_fbcon_viewport *v;
    struct fb_info *info;
    struct fb_var_screeninfo var;
    struct fb_videomode mode;
    unsigned long flags;
    bool requested;
    int ret = kstrtobool(text, &requested);

    if (ret)
        return ret;
    mutex_lock(&polaris_native_control);
    if (requested == polaris_console_fullscreen)
        goto done;
    if (!requested) {
        ret = -EPERM; /* Fullscreen ownership is one-way until framebuffer teardown. */
        goto done;
    }
    n = polaris_native_console;
    if (!n || !n->info->fbcon_par) {
        ret = -EAGAIN;
        goto done;
    }
    info = n->info;
    console_lock();
    lock_fb_info(info);
    if (!info->fbcon_par) {
        ret = -ENODEV;
        goto unlock;
    }
    var = info->var;
    var.xres = var.xres_virtual = n->width;
    var.yres = var.yres_virtual = n->height;
    fb_var_to_videomode(&mode, &var);
    ret = fb_add_videomode(&mode, &info->modelist);
    if (ret)
        goto unlock;
    spin_lock_irqsave(&n->lock, flags);
    n->owned = false;
    spin_unlock_irqrestore(&n->lock, flags);
    cancel_work_sync(&n->work);
    v = &n->drawing.view;
    v->x = v->y = 0;
    v->width = n->width;
    v->height = n->height;
    v->offset = 0;
    v->size = (u64)(v->height - 1) * v->stride + v->width * v->bytes_per_pixel;
    memset(n->shadow, 0, n->bytes);
    info->screen_buffer = n->shadow;
    info->screen_size = v->size;
    info->fix.smem_len = v->size;
    info->var = var;
    n->draw.screen_buffer = n->shadow;
    n->draw.screen_size = v->size;
    n->draw.fix = info->fix;
    n->draw.var = info->var;
    spin_lock_irqsave(&n->lock, flags);
    n->dirty = false;
    spin_unlock_irqrestore(&n->lock, flags);
    /* Keep the old scanout until fbcon has redrawn the complete login view.
     * Damage raised by resize/redraw must not present the cleared RAM stage. */
    fbcon_update_vcs(info, true);
    spin_lock_irqsave(&n->lock, flags);
    n->owned = true;
    spin_unlock_irqrestore(&n->lock, flags);
    polaris_native_damage(n, 0, v->height);
    polaris_console_fullscreen = true;
unlock:
    unlock_fb_info(info);
    console_unlock();
    if (!ret)
        pr_info("POLARIS_HANDOFF: fullscreen login console %ux%u rotation=%u\n",
                n->width, n->height, n->drawing.view.rotation * 90);
done:
    mutex_unlock(&polaris_native_control);
    return ret;
}
static const struct kernel_param_ops polaris_fullscreen_ops = {
    .set = polaris_native_fullscreen_set,
    .get = param_get_bool,
};
module_param_cb(polaris_console_fullscreen, &polaris_fullscreen_ops,
                &polaris_console_fullscreen, 0600);
MODULE_PARM_DESC(polaris_console_fullscreen,
                "Retire boot UI and expand the native text console for login");

static int polaris_native_fbcon_prepare(struct fb_info *info,
        void (*damage)(void *, unsigned int, unsigned int, unsigned int, unsigned int),
        void *context)
{
    struct polaris_native_fbcon *n;
    struct pj4edk2_fbcon_viewport *v;
    unsigned int cells[PJ4EDK2_DISPLAY_CELLS];
    unsigned int width = info->var.xres, height = info->var.yres;
    int ret;

    (void)pj4edk2_fbcon_register;
    if (!of_machine_is_compatible("xiaomi,polaris"))
        return 0;
    if (polaris_native_console || width != 1080 || height != 2160 ||
        info->var.bits_per_pixel != 32 || info->var.red.offset != 16 ||
        info->var.green.offset != 8 || info->var.blue.offset != 0 ||
        info->var.red.length != 8 || info->var.green.length != 8 ||
        info->var.blue.length != 8)
        return -EINVAL;
    n = kzalloc(sizeof(*n), GFP_KERNEL);
    if (!n)
        return -ENOMEM;
    n->width = width;
    n->height = height;
    n->bytes = info->screen_size;
    n->shadow = vzalloc(n->bytes);
    if (!n->shadow) {
        ret = -ENOMEM;
        goto fail;
    }
    ret = polaris_boot_display_snapshot(cells, n->shadow, n->bytes, width, height,
                                       info->fix.line_length);
    if (ret)
        goto fail;
    v = &n->drawing.view;
    if (!pj4edk2_fbcon_viewport(cells, v)) {
        ret = -EINVAL;
        goto fail;
    }
    n->target = info->screen_buffer;
    n->info = info;
    n->damage = damage;
    n->context = context;
    n->original_ops = info->fbops;
    n->ops = *info->fbops;
    n->ops.fb_open = pj4edk2_fbcon_open;
    n->ops.fb_release = NULL;
    n->ops.fb_read = NULL;
    n->ops.fb_write = NULL;
    n->ops.fb_mmap = NULL;
    n->ops.fb_check_var = polaris_native_check_var;
    n->ops.fb_pan_display = polaris_native_pan;
    n->ops.fb_fillrect = polaris_native_fillrect;
    n->ops.fb_copyarea = polaris_native_copyarea;
    n->ops.fb_imageblit = polaris_native_imageblit;
    n->ops.fb_destroy = polaris_native_destroy;
    spin_lock_init(&n->lock);
    INIT_WORK(&n->work, polaris_native_present);
    n->owned = true;
    /* Copy the complete page once; subsequent updates are window-only. Native
     * KMS uses this full allocation and pitch, independently of fbcon geometry. */
    memcpy(n->target, n->shadow, n->bytes);
    wmb();
    info->fbops = &n->ops;
    info->flags |= FBINFO_VIRTFB | FBINFO_HIDE_SMEM_START;
    info->screen_buffer = n->shadow + v->offset;
    info->screen_size = v->size;
    info->fix.smem_start = 0;
    info->fix.smem_len = v->size;
    info->fix.xpanstep = info->fix.ypanstep = info->fix.ywrapstep = 0;
    info->var.xres = info->var.xres_virtual = v->width;
    info->var.yres = info->var.yres_virtual = v->height;
    info->var.xoffset = info->var.yoffset = 0;
    info->var.rotate = info->fbcon_rotate_hint = v->rotation;
    info->var.width = info->var.height = -1;
    n->drawing.damage = polaris_native_damage;
    n->drawing.context = n;
    n->draw.par = &n->drawing;
    n->draw.flags = FBINFO_VIRTFB;
    n->draw.var = info->var;
    n->draw.fix = info->fix;
    n->draw.screen_buffer = info->screen_buffer;
    n->draw.screen_size = info->screen_size;
    n->draw.pseudo_palette = info->pseudo_palette;
    mutex_lock(&polaris_native_control);
    polaris_native_console = n;
    mutex_unlock(&polaris_native_control);
    pr_info("POLARIS_HANDOFF: native DRM fbcon window %ux%u at %u,%u rotation=%u; inherited page retained\n",
            v->width, v->height, v->x, v->y, v->rotation * 90);
    return 1;
fail:
    vfree(n->shadow);
    kfree(n);
    return ret;
}
#endif
