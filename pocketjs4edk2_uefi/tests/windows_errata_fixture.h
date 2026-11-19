// SPDX-License-Identifier: BSD-2-Clause-Patent
// Small mapped ARM64 PE fixtures; no Microsoft binaries are redistributed.
#ifndef WINDOWS_ERRATA_FIXTURE_H
#define WINDOWS_ERRATA_FIXTURE_H
#define FIXTURE_BYTES 0x7000
static void Put16(UINT8 *B,UINT32 O,UINT16 V) {memcpy(B+O,&V,2);}
static void Put32(UINT8 *B,UINT32 O,UINT32 V) {memcpy(B+O,&V,4);}
static UINT32 Get32(UINT8 *B,UINT32 O) {UINT32 V;memcpy(&V,B+O,4);return V;}
static void Section(UINT8 *B,UINT32 I,UINT32 Rva,UINT32 Size,UINT32 Flags) {
 UINT32 S=0x80+24+240+I*40;Put32(B,S+8,Size);Put32(B,S+12,Rva);Put32(B,S+36,Flags);
}
static void Pdata(UINT8 *B,UINT32 I,UINT32 Rva,UINT32 Length) {
 Put32(B,0x4000+I*8,Rva);Put32(B,0x4004+I*8,(Length/4<<2)|1);
}
static void Fixture(UINT8 *B,BOOLEAN Kernel) {
 memset(B,0,FIXTURE_BYTES);Put16(B,0,0x5a4d);Put32(B,60,0x80);
 Put32(B,0x80,0x4550);Put16(B,0x84,0xaa64);Put16(B,0x86,3);Put16(B,0x94,240);
 Put16(B,0x98,0x20b);Put32(B,0x98+56,FIXTURE_BYTES);Put32(B,0x98+60,0x1000);Put32(B,0x98+108,16);
 Put32(B,0x98+136,0x4000);Put32(B,0x98+140,Kernel?32:16);
 Section(B,0,0x1000,0x2000,0x60000020);Section(B,1,0x4000,0x1000,0x40000040);
 Section(B,2,0x5000,0x1000,0x40000040);
 // A linked jump island preceding ordinary unwind records must not disable
 // recognition of later functions, or be treated as their function body.
 Put32(B,0x4000,0x1000);Put32(B,0x4004,0x400b);
 Put32(B,0x104c,0xd63f01e0);
 if(!Kernel) {
  Pdata(B,1,0x1100,0xdc);
  Put32(B,0x1100,0xa9bd53f3);Put32(B,0x1104,0xa9015bf5);Put32(B,0x1108,0xf90013f7);
  Put32(B,0x110c,0xa9bf7bfd);Put32(B,0x1110,0x910003fd);
  Put32(B,0x1160,0x7100067f);Put32(B,0x1164,0x9a881137);Put32(B,0x1168,0x9400001e);
  Put32(B,0x116c,0x6b13001f);Put32(B,0x1170,0x54000160);
 }
 // A register-save leaf routine without a .pdata entry.
 Put32(B,0x15f8,0xd5381008);Put32(B,0x15fc,0xf9005408);
 Put32(B,0x1600,0xd5381028);Put32(B,0x1604,0xf9005808);
 Put32(B,0x1608,0xd5381048);Put32(B,0x160c,0xf9005c08);Put32(B,0x1610,0xd5382048);
 if(!Kernel) return;
 Pdata(B,1,0x1800,0x40);Pdata(B,2,0x1a00,0x40);Pdata(B,3,0x1b00,0x7c);
 Put32(B,0x1810,0xd5381025); // Generic ACTLR Rt, not a fixed x8 byte string.
 Put32(B,0x18fc,0xf9405809);Put32(B,0x1900,0xd5181029);
 Put32(B,0x1904,0xf9405c09);Put32(B,0x1908,0xd5181049);
 Put32(B,0x190c,0xf9406009);Put32(B,0x1910,0xd5182049);
 Put32(B,0x1a00,0xd2800004);Put32(B,0x1a04,0xd2800003);Put32(B,0x1a08,0x528007c0);
 Put32(B,0x1a0c,0x94000000|((0x1c00-0x1a0c)/4));
 Put32(B,0x1b00,0xd503237f);Put32(B,0x1b04,0xa9bf53f3);
 Put32(B,0x1b08,0xa9bf7bfd);Put32(B,0x1b0c,0x910003fd);
 Put32(B,0x1b1c,0xd2800003);Put32(B,0x1b20,0xd2800002);Put32(B,0x1b24,0xd2800001);
 Put32(B,0x1b28,0x18000000|((0x1b80-0x1b28)/4<<5));Put32(B,0x1b80,0x84000013);
 Put32(B,0x98+112,0x5000);Put32(B,0x98+116,0x180);
 Put32(B,0x5000+20,1);Put32(B,0x5000+24,1);
 Put32(B,0x5000+28,0x5040);Put32(B,0x5000+32,0x5044);Put32(B,0x5000+36,0x5048);
 Put32(B,0x5040,0x1c00);Put32(B,0x5044,0x5100);Put16(B,0x5048,0);
 memcpy(B+0x5100,"KeBugCheckEx",13);
}
#endif
