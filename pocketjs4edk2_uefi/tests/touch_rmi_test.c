/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define STATIC static
#define CONST const
#define TRUE 1
#define FALSE 0
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef size_t UINTN;
typedef int BOOLEAN;
#include "../platform/PolarisTouchDxe/TouchRmi.h"

int main(void)
{
  /* Data0 is four bytes, Data1 holds ten eight-byte contacts. */
  UINT8 presence[] = {5, 3};
  UINT8 structure[] = {4, 1, 80, 0xff, 7};
  RMI_PACKET p = {0};
  assert(RmiDecodeDescriptor(presence, 2, structure, 5, 1, &p));
  assert(p.ByteOffset == 4 && p.RegisterOffset == 1);
  assert(p.Size == 80 && p.Subpackets == 10 && p.Subpacket0);
  assert(!RmiDecodeDescriptor(presence, 2, structure, 5, 2, &p));
  for (UINTN n = 0; n < sizeof(structure); ++n)
    assert(!RmiDecodeDescriptor(presence, 2, structure, n, 1, &p));
  UINT8 extended_presence[] = {0, 4, 0, 1};
  UINT8 extended[] = {0, 0x00, 0x01, 1};
  assert(RmiDecodeDescriptor(extended_presence, 4, extended, 4, 0, &p));
  assert(p.Size == 256 && p.Subpackets == 1);
  UINT8 short_presence[] = {0, 4};
  assert(!RmiDecodeDescriptor(short_presence, 2, extended, 4, 0, &p));
  UINT8 unterminated[] = {4, 1, 80, 0xff, 0x87};
  assert(!RmiDecodeDescriptor(presence, 2, unterminated, 5, 1, &p));
  /* The complete 32-bit size encoding must stay bounded. */
  UINT8 large_presence[] = {8, 1};
  UINT8 large[] = {0, 0, 0, 0xff, 0xff, 0xff, 0xff, 1};
  assert(!RmiDecodeDescriptor(large_presence, 2, large, 8, 0, &p));
  puts("RMI4 descriptor offsets and malformed packet bounds passed");
  return 0;
}
