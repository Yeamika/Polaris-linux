// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroIoLibV8.h>
.text
.align 4
// Probe deadlines use the verified SDM845 KPSS watchdog, independently of
// whether DRAM records survive reboot. Normal Linux entry never uses them.
.macro native_deadline seconds
#ifdef POLARIS_NATIVE_ENTRY_PROBE
  mov x14, #0x17980000
  str wzr, [x14, #8]
  mov w15, #1
  str w15, [x14, #4]
  mov w16, #\seconds
  lsl w16, w16, #15
  str w16, [x14, #0x10]
  str w16, [x14, #0x14]
  str w15, [x14, #8]
  dsb sy
#endif
.endm
ASM_FUNC(PolarisNativeEnter)
  // x0 entry, x1 DTB, x2 map, x3 bytes, x4 stride, x5 journal.
  // No stack or firmware/data-table access follows MMU shutdown.
  mov x19, x0
  mov x20, x1
  mov x24, x5
  msr daifset, #0xf
  mrs x6, CurrentEL
  str x6, [x24, #16]
  cmp x6, #8
  b.eq 1f
  mrs x6, sctlr_el1
  b 2f
1:
  mrs x6, sctlr_el2
2:
  str x6, [x24, #24]
  mov w6, #2
  str w6, [x24, #12]
  dc cvac, x24
  dsb sy
  native_deadline 23
  // Clean the identity-mapped transition code to PoC before disabling
  // caches. Dirty payload data is flushed by set/way before Linux reads it.
  adr x6, PolarisNativeEnter
  adr x7, PolarisNativeTransitionEnd
  mrs x4, ctr_el0
  ubfx x4, x4, #16, #4
  mov x5, #4
  lsl x5, x5, x4
  sub x4, x5, #1
  bic x6, x6, x4
3:
  dc cvac, x6
  add x6, x6, x5
  cmp x6, x7
  b.lo 3b
  dsb sy
  mrs x6, CurrentEL
  cmp x6, #8
  b.eq 4f
  mrs x4, sctlr_el1
  bic x4, x4, #1
  bic x4, x4, #(1 << 2)
  bic x4, x4, #(1 << 12)
  msr sctlr_el1, x4
  isb
  tlbi vmalle1
  b 5f
4:
  mrs x4, sctlr_el2
  bic x4, x4, #1
  bic x4, x4, #(1 << 2)
  bic x4, x4, #(1 << 12)
  msr sctlr_el2, x4
  isb
  tlbi alle2
5:
  dsb sy
  isb
  native_deadline 15
  // EDK II's ArmCleanInvalidateDataCache likewise requires MMU off. Inline
  // its CLIDR/CCSIDR algorithm without C calls, stacks or a DRAM-wide scan.
  mrs x0, clidr_el1
  ubfx x3, x0, #24, #3
  lsl x3, x3, #1
  cbz x3, 10f
  mov x10, #0
6:
  add x2, x10, x10, lsr #1
  lsr x1, x0, x2
  and x1, x1, #7
  cmp x1, #2
  b.lo 9f
  msr csselr_el1, x10
  isb
  mrs x1, ccsidr_el1
  and x2, x1, #7
  add x2, x2, #4
  ubfx x4, x1, #3, #10
  clz w5, w4
  ubfx x6, x1, #13, #15
7:
  mov x7, x4
8:
  lsl x8, x7, x5
  orr x8, x8, x10
  lsl x9, x6, x2
  orr x8, x8, x9
  dc cisw, x8
  subs x7, x7, #1
  b.ge 8b
  subs x6, x6, #1
  b.ge 7b
9:
  add x10, x10, #2
  cmp x10, x3
  b.lo 6b
10:
  msr csselr_el1, xzr
  dsb sy
  ic iallu
  dsb sy
  isb
  native_deadline 11
  mov w6, #4
  str w6, [x24, #12]
  dsb sy
  mov x0, x20
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  br x19
PolarisNativeTransitionEnd:
#ifdef POLARIS_NATIVE_ENTRY_PROBE
// Copied after the raw Image, then reached through its first branch.
// Linux body is intentionally skipped: the five-second reset is success.
.align 4
ASM_FUNC(PolarisNativeProbe)
  native_deadline 5
11:
  wfi
  b 11b
GCC_ASM_EXPORT(PolarisNativeProbeEnd)
ASM_PFX(PolarisNativeProbeEnd):
#endif
