// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef POLARIS_FASTBOOT_CORE_H
#define POLARIS_FASTBOOT_CORE_H
#define FB_LIMIT (64U * 1024U * 1024U)
#define FB_COMMAND 64U
#define FB_REPLY 65U
#define FB_IDLE 0U
#define FB_DOWNLOAD 1U
#define FB_BOOT 1U
#define FB_REBOOT 2U
#define FB_EXIT 3U
#define FB_BOOTLOADER 5U

typedef struct {
  unsigned char *Data;
  unsigned int Capacity, Expected, Received, Valid, State, Action;
  char Reply[FB_REPLY];
  unsigned int ReplySize;
} FB_CORE;
void FbInit(FB_CORE *Core, unsigned char *Data, unsigned int Capacity);
void FbReset(FB_CORE *Core);
void FbReceive(FB_CORE *Core, const unsigned char *Packet, unsigned int Size);
int FbIsEfi(const unsigned char *Data, unsigned int Size);
#endif
