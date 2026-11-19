// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "FastbootCore.h"
static unsigned char Storage[1026];
static FB_CORE C;
static void Cmd(const char *S) { FbReceive(&C,(const unsigned char *)S,(unsigned int)strlen(S)); }
static void Is(const char *S) { assert(C.ReplySize==strlen(S)); assert(!strcmp(C.Reply,S)); }
static void Reset(void) { memset(Storage,0xa5,sizeof(Storage)); FbInit(&C,Storage+1,1024); }
static void Canaries(void) { assert(Storage[0]==0xa5 && Storage[1025]==0xa5); }
static void Put16(unsigned char *P,unsigned int V) { P[0]=(unsigned char)V; P[1]=(unsigned char)(V>>8); }
static void Put32(unsigned char *P,unsigned int V) { Put16(P,V); Put16(P+2,V>>16); }
static void Pe(unsigned char *P) {
 memset(P,0,256); P[0]='M';P[1]='Z';Put32(P+60,64); Put32(P+64,0x4550);
 Put16(P+68,0xaa64);Put16(P+84,112);Put16(P+88,0x20b);Put16(P+156,10);
}
int main(void) {
 unsigned char P[2048]; unsigned int I,Seed=5;
 Reset(); Cmd("getvar:product");Is("OKAYpolaris"); Cmd("getvar:max-download-size");Is("OKAY0x00000400");
 Cmd("getvar:current-slot");Is("FAILspecify explicit partition name");
 Cmd("getvar:has-slot:boot");Is("OKAYno");Cmd("getvar:is-logical:boot");Is("OKAYno");
 Cmd("download:00000004");Is("DATA00000004");
 FbReceive(&C,(const unsigned char *)"ab",2);assert(!C.ReplySize && C.State==FB_DOWNLOAD);
 FbReceive(&C,(const unsigned char *)"",0);assert(!C.ReplySize && C.State==FB_DOWNLOAD && C.Received==2);
 FbReceive(&C,(const unsigned char *)"cd",2);Is("OKAY");assert(C.Valid==4 && !memcmp(C.Data,"abcd",4));
 Cmd("boot");assert(!C.Action && !strncmp(C.Reply,"FAIL",4));
 Cmd("download:00000005");FbReceive(&C,(const unsigned char *)"overflow",8);Is("FAILdownload length");assert(!C.Valid);Canaries();
 Cmd("download:00000000");assert(C.State==FB_IDLE && !C.Valid);Cmd("download:ffffffff");assert(C.State==FB_IDLE);
 Cmd("download:0000000z");Is("FAILdownload hex");Cmd("download:1");Is("FAILdownload size");
 Cmd("download:00000100");Pe(P);FbReceive(&C,P,256);Is("OKAY");Cmd("oem boot-efi");Is("OKAY");assert(C.Action==FB_BOOT);
 FbReset(&C);Cmd("oem boot-efi");assert(!C.Action && !C.Valid);
 Cmd("flash:boot");Is("FAILpartition writes not implemented");
 Cmd("erase:userdata");Is("FAILpartition writes not implemented");
 Cmd("reboot");assert(C.Action==FB_REBOOT);Cmd("continue");assert(C.Action==FB_EXIT);
 Pe(P);assert(FbIsEfi(P,256));assert(!FbIsEfi(P,100));
 Put32(P+60,0xfffffff0);assert(!FbIsEfi(P,256));Pe(P);Put16(P+84,0xffff);assert(!FbIsEfi(P,256));
 Pe(P);Put16(P+68,0x8664);assert(!FbIsEfi(P,256));Pe(P);Put16(P+156,11);assert(!FbIsEfi(P,256));
 // Deterministic mutation corpus exercises command/download bounds under sanitizers.
 for (I=0;I<20000;++I) {
  unsigned int J,N;
  Seed=Seed*1664525U+1013904223U;N=Seed%sizeof(P);
  for (J=0;J<N;++J) {Seed=Seed*1664525U+1013904223U;P[J]=(unsigned char)(Seed>>24);}
  if (!(I%7)) Cmd("download:00000400");
  FbReceive(&C,P,N);Canaries();assert(C.Valid<=C.Capacity && C.Received<=C.Expected);
  assert(C.ReplySize<=FB_COMMAND);if (!(I%31)) FbReset(&C);
 }
 puts("PASS: fastboot bounds, fragmented download, reset invalidation, EFI validation and mutation corpus");
 return 0;
}
