// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_BOOT_CONSOLE_H
#define POLARIS_BOOT_CONSOLE_H
#include <Uefi.h>

// Polaris' display reservation is 0x9d400000..0x9f800000. The shared
// cursor/log follows the single 1080x2160 BGRX scanout, outside GOP's buffer.
// It is reset by PrePi, never by an individual DXE library constructor.
#define POLARIS_CONSOLE_FB 0x9d400000U
#define POLARIS_CONSOLE_WIDTH 1080U
#define POLARIS_CONSOLE_HEIGHT 2160U
#define POLARIS_CONSOLE_BYTES (POLARIS_CONSOLE_WIDTH * POLARIS_CONSOLE_HEIGHT * 4U)
#define POLARIS_CONSOLE_MAGIC 0x31474f4c43424c50ULL
#define POLARIS_CONSOLE_LOG_SIZE 16384U
#define POLARIS_CONSOLE_BOOT 0U
#define POLARIS_CONSOLE_UI 1U
#define POLARIS_CONSOLE_TEXT 2U
#define POLARIS_CONSOLE_CRITICAL 3U

typedef struct {
  UINT64 Magic;
  UINT32 Owner, Column, Row, Head, Count, Scrolls;
  UINT8 Log[POLARIS_CONSOLE_LOG_SIZE];
} POLARIS_BOOT_CONSOLE;

#ifndef POLARIS_CONSOLE_STATE_ADDRESS
#define POLARIS_CONSOLE_STATE_ADDRESS (POLARIS_CONSOLE_FB + POLARIS_CONSOLE_BYTES)
#endif

STATIC inline POLARIS_BOOT_CONSOLE *PolarisBootConsole(VOID) {
  return (VOID *)(UINTN)POLARIS_CONSOLE_STATE_ADDRESS;
}

STATIC inline BOOLEAN PolarisBootConsoleEarly(VOID) {
  POLARIS_BOOT_CONSOLE *State=PolarisBootConsole();
  return State->Magic==POLARIS_CONSOLE_MAGIC &&
    (State->Owner==POLARIS_CONSOLE_BOOT || State->Owner==POLARIS_CONSOLE_CRITICAL);
}

STATIC inline VOID PolarisBootConsoleOwner(UINT32 Owner) {
  POLARIS_BOOT_CONSOLE *State=PolarisBootConsole();
  if (State->Magic==POLARIS_CONSOLE_MAGIC) State->Owner=Owner;
}

// Caller serializes ring access (the framebuffer serial writer masks IRQs).
STATIC inline VOID PolarisBootConsoleByte(UINT8 Byte) {
  POLARIS_BOOT_CONSOLE *State=PolarisBootConsole();
  State->Log[State->Head]=Byte;
  State->Head=(State->Head+1)%POLARIS_CONSOLE_LOG_SIZE;
  if (State->Count<POLARIS_CONSOLE_LOG_SIZE) ++State->Count;
}

STATIC inline UINTN PolarisBootConsoleRead(UINT8 *Buffer,UINTN Capacity) {
  POLARIS_BOOT_CONSOLE *State=PolarisBootConsole();
  UINTN I,Count;
  if (!Buffer || State->Magic!=POLARIS_CONSOLE_MAGIC ||
      State->Head>=POLARIS_CONSOLE_LOG_SIZE || State->Count>POLARIS_CONSOLE_LOG_SIZE) return 0;
  Count=State->Count<Capacity?State->Count:Capacity;
  for (I=0;I<Count;++I)
    Buffer[I]=State->Log[(State->Head+POLARIS_CONSOLE_LOG_SIZE-Count+I)%POLARIS_CONSOLE_LOG_SIZE];
  return Count;
}
#endif
