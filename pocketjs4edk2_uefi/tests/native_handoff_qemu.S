// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercise the real stackless handoff with identity MMU and caches enabled.
.text
.global _start
_start:
  msr daifset, #0xf
  adr x0, vectors
  mrs x1, CurrentEL
  cmp x1, #8
  b.eq setup_el2
  msr vbar_el1, x0
  mov x0, #0xff00
  msr mair_el1, x0
  adr x0, pagetable
  msr ttbr0_el1, x0
  ldr x0, =0x200803519
  msr tcr_el1, x0
  isb
  mrs x0, sctlr_el1
  orr x0, x0, #1
  orr x0, x0, #4
  orr x0, x0, #0x1000
  msr sctlr_el1, x0
  b setup_done
setup_el2:
  msr vbar_el2, x0
  mov x0, #0xff00
  msr mair_el2, x0
  adr x0, pagetable
  msr ttbr0_el2, x0
  ldr x0, =0x80823519
  msr tcr_el2, x0
  isb
  mrs x0, sctlr_el2
  orr x0, x0, #1
  orr x0, x0, #4
  orr x0, x0, #0x1000
  msr sctlr_el2, x0
setup_done:
  isb
  adr x0, dirty_data
  mov x1, #0x1234
  str x1, [x0]
  adr x0, landed
  adr x1, dtb
  mov x2, xzr
  mov x3, xzr
  mov x4, #48
  adr x5, journal
  // Poison SP: any hidden push/pop or C helper will take a synchronous fault.
  mov x7, xzr
  mov sp, x7
  b PolarisNativeEnter
landed:
  adr x4, dtb
  cmp x0, x4
  b.ne fail
  orr x4, x1, x2
  orr x4, x4, x3
  cbnz x4, fail
  mrs x6, CurrentEL
  cmp x6, #8
  b.eq check_el2
  mrs x4, sctlr_el1
  b check_control
check_el2:
  mrs x4, sctlr_el2
check_control:
  ldr x5, =0x1005
  tst x4, x5
  b.ne fail
  adr x4, journal
  ldr w7, [x4, #12]
  cmp w7, #4
  b.ne fail
  ldr x7, [x4, #16]
  cmp x7, x6
  b.ne fail
  ldr x7, [x4, #24]
  and x7, x7, x5
  cmp x7, x5
  b.ne fail
  adr x4, dirty_data
  ldr x4, [x4]
  mov x7, #0x1234
  cmp x4, x7
  b.ne fail
  adr x0, success
  bl puts
  mov x2, #0
  b finish
fail:
  adr x0, failure
  bl puts
  mov x2, #1
finish:
  adr x1, exit_block
  str x2, [x1, #8]
  mov x0, #0x20
  hlt #0xf000
  b .
puts:
  mov x4, #0x09000000
1:
  ldrb w5, [x0], #1
  cbz w5, 2f
  str w5, [x4]
  b 1b
2:
  ret
.balign 2048
vectors:
.rept 16
  b fail
  .space 124
.endr
.data
success: .asciz "PASS: real native handoff; MMU/cache off; x0=DTB; x1-x3=0; no stack; journal and dirty payload intact\n"
failure: .asciz "FAIL: native handoff or CPU exception\n"
.balign 16
exit_block: .quad 0x20026, 0
dirty_data: .quad 0
dtb: .quad 0xedfe0dd0
.balign 64
journal: .space 64
.balign 4096
pagetable:
  .quad 0x401, 0x40000705
  .space 4096-16
