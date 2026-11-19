// SPDX-License-Identifier: BSD-2-Clause-Patent
// SDM845 raw Image entry: no stack accesses or firmware calls after EBS.
.text
.align 6
.global PolarisAndroidEnter
.type PolarisAndroidEnter, %function
PolarisAndroidEnter:
  mov x19, x0
  mov x20, x1
  msr daifset, #0xf
  dsb sy
  ldr x21, =0xcffff000
  mov x22, #0x50
  str x22, [x21, #8]
  dc cvac, x21
  dsb sy

  // Clean/invalidate each data/unified cache with the SDM845 CCSIDR layout.
  // Keep all state in registers, so the last clean never dirties a C stack.
  mrs x6, clidr_el1
  ubfx x3, x6, #24, #3
  lsl x3, x3, #1
  cbz x3, 5f
  mov x10, #0
1:
  add x2, x10, x10, lsr #1
  lsr x12, x6, x2
  and x12, x12, #7
  cmp x12, #2
  b.lo 4f
  msr csselr_el1, x10
  isb
  mrs x12, ccsidr_el1
  and x2, x12, #7
  add x2, x2, #4
  ubfx x4, x12, #3, #10
  clz w5, w4
  ubfx x7, x12, #13, #15
2:
  mov x9, x4
3:
  lsl x11, x9, x5
  orr x0, x10, x11
  lsl x11, x7, x2
  orr x0, x0, x11
  dc cisw, x0
  subs x9, x9, #1
  b.ge 3b
  subs x7, x7, #1
  b.ge 2b
  dsb sy
4:
  add x10, x10, #2
  cmp x10, x3
  b.lo 1b
5:
  msr csselr_el1, xzr
  dsb sy
  mov x22, #0x52
  str x22, [x21, #8]
  dc cvac, x21
  dsb sy
  ic iallu
  dsb sy
  isb
  mrs x1, CurrentEL
  cmp x1, #8
  b.eq 6f
  mrs x0, sctlr_el1
  bic x0, x0, #1
  bic x0, x0, #4
  bic x0, x0, #0x1000
  msr sctlr_el1, x0
  isb
  tlbi vmalle1
  b 7f
6:
  mrs x0, sctlr_el2
  bic x0, x0, #1
  bic x0, x0, #4
  bic x0, x0, #0x1000
  msr sctlr_el2, x0
  isb
  tlbi alle2
7:
  dsb sy
  isb
  // This physical marker survives a watchdog reset before Android comes up.
  str x0, [x21, #48]
  mov x22, #0x54
  str x22, [x21, #8]
  dsb sy
  mov x0, x20
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  br x19
.size PolarisAndroidEnter, .-PolarisAndroidEnter
