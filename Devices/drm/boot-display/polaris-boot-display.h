/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef POLARIS_BOOT_DISPLAY_H
#define POLARIS_BOOT_DISPLAY_H
#include <linux/types.h>
int polaris_boot_display_snapshot(unsigned int *cells, void *target,
        size_t bytes, unsigned int width, unsigned int height, unsigned int stride);
bool polaris_boot_display_handoff_available(void);
#endif
