/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Standard host environment for compiling the pinned EDK2 libfdt sources. */
#ifndef POLARIS_TEST_LIBFDT_ENV_H
#define POLARIS_TEST_LIBFDT_ENV_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint16_t fdt16_t;
typedef uint32_t fdt32_t;
typedef uint64_t fdt64_t;
#define fdt16_to_cpu(x) __builtin_bswap16(x)
#define fdt32_to_cpu(x) __builtin_bswap32(x)
#define fdt64_to_cpu(x) __builtin_bswap64(x)
#define cpu_to_fdt16(x) fdt16_to_cpu(x)
#define cpu_to_fdt32(x) fdt32_to_cpu(x)
#define cpu_to_fdt64(x) fdt64_to_cpu(x)
#endif
