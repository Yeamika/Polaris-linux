/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef POLARIS_TOUCH_RMI_H
#define POLARIS_TOUCH_RMI_H

typedef struct {
  UINT32 Size;
  UINT32 ByteOffset;
  UINT16 RegisterOffset;
  UINT16 Subpackets;
  BOOLEAN Subpacket0;
} RMI_PACKET;

/* RMI4 packet register addresses advance by one per present register;
 * offsets inside an attention report advance by each register's byte size. */
STATIC BOOLEAN
RmiDecodeDescriptor (
  CONST UINT8 *Presence, UINTN PresenceSize,
  CONST UINT8 *Structure, UINTN StructureSize,
  UINT16 Wanted, RMI_PACKET *Result
  )
{
  UINTN MapStart, Expected, Cursor = 0, Bit, Pos, I;
  UINT32 Bytes = 0, Size;
  UINT16 Registers = 0, Subpackets;
  UINT8 Map;
  BOOLEAN Found = FALSE, Subpacket0;
  RMI_PACKET Packet = {0};
  if (PresenceSize < 1 || PresenceSize > 35) return FALSE;
  MapStart = Presence[0] ? 1 : 3;
  if (PresenceSize <= MapStart) return FALSE;
  Expected = Presence[0] ? Presence[0] : Presence[1] | ((UINTN)Presence[2] << 8);
  if (!Expected || Expected != StructureSize || Expected > 512) return FALSE;
  for (Bit = 0; Bit < (PresenceSize - MapStart) * 8; ++Bit) {
    if (!(Presence[MapStart + Bit / 8] & (1U << (Bit % 8)))) continue;
    if (Cursor >= StructureSize) return FALSE;
    Size = Structure[Cursor++];
    if (!Size) {
      if (StructureSize - Cursor < 2) return FALSE;
      Size = Structure[Cursor] | ((UINT32)Structure[Cursor + 1] << 8);
      Cursor += 2;
    }
    if (!Size) {
      if (StructureSize - Cursor < 4) return FALSE;
      for (I = 0; I < 4; ++I) Size |= (UINT32)Structure[Cursor++] << (8 * I);
    }
    if (!Size || Size > 65535 || Bytes > 65535 - Size) return FALSE;
    Subpackets = 0;
    Pos = 0;
    Subpacket0 = FALSE;
    do {
      if (Cursor >= StructureSize || Pos >= 256) return FALSE;
      Map = Structure[Cursor++];
      if (!Pos) Subpacket0 = (Map & 1) != 0;
      for (I = 0; I < 7; ++I, ++Pos) {
        if (Map & (1U << I)) {
          if (Pos >= 256) return FALSE;
          ++Subpackets;
        }
      }
    } while (Map & 0x80);
    if (Bit == Wanted) {
      Packet.Size = Size;
      Packet.ByteOffset = Bytes;
      Packet.RegisterOffset = Registers;
      Packet.Subpackets = Subpackets;
      Packet.Subpacket0 = Subpacket0;
      Found = TRUE;
    }
    Bytes += Size;
    ++Registers;
  }
  if (Found) *Result = Packet;
  return Found;
}

#endif
