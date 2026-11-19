// SPDX-License-Identifier: BSD-2-Clause-Patent
// Entered by the original Image's first branch. x0-x3 remain untouched on
// the Android path, including ABL's board-selected DT and dynamic bootargs.
.section .text
.balign 16
.global _Start
_Start:
    mov     x4, #0x7fe00000
    movk    x4, #1, lsl #32
    mov     x6, #128
    mov     w7, #0x5241
.Lcheck:
    ldrh    w5, [x4], #2
    cmp     w5, w7
    b.ne    .Luefi
    subs    x6, x6, #1
    b.ne    .Lcheck

    // Consume the entire request before entering Android. A later reboot
    // returns to UEFI rather than repeatedly selecting Android.
    mov     x4, #0x7fe00000
    movk    x4, #1, lsl #32
    mov     x6, #16
.Lclear:
    stp     xzr, xzr, [x4], #16
    subs    x6, x6, #1
    b.ne    .Lclear
    dsb     sy
    adr     x4, _Start
    ldr     x5, .Lnative_delta
    sub     x4, x4, x5
    br      x4

.Luefi:
    // Publish the binding to this boot payload in the reserved FD mapping.
    adr     x4, .Lmanifest
    ldr     x5, .Lmanifest_address
    mov     x6, #64
.Lmanifest_copy:
    ldp     x8, x9, [x4], #16
    stp     x8, x9, [x5], #16
    subs    x6, x6, #16
    b.ne    .Lmanifest_copy

    adr     x4, _Payload
    ldr     x5, .Luefi_base
    ldr     x6, .Luefi_size
.Lcopy:
    ldp     x8, x9, [x4], #16
    stp     x8, x9, [x5], #16
    subs    x6, x6, #16
    b.ne    .Lcopy
    dsb     sy
    ic      iallu
    dsb     sy
    isb
    ldr     x5, .Luefi_base
    br      x5

.balign 8
.Lnative_delta:     .quad SHIM_OFFSET - KERNEL_ENTRY
.Lmanifest_address: .quad 0xcfffe000
.Luefi_base:        .quad 0xce000000
.Luefi_size:        .quad 0x00700000
.Lmanifest:
    .ascii "POLANDR1"
    .long 1, 64
    .quad ANDROID_BOOT_SIZE
    .long ANDROID_BOOT_CRC
    .short 0x5241, 128
    .quad 0x17fe00000
    .quad KERNEL_ENTRY
    .quad PACKED_IMAGE_SIZE
    .quad 0
.balign 4096
_Payload:
