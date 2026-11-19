// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "FastbootCore.h"
static unsigned int Length(const char *S) {
  unsigned int N=0; while (S[N]) ++N; return N;
}
static int Equal(const unsigned char *P, unsigned int N, const char *S) {
  unsigned int I; if (Length(S)!=N) return 0;
  for (I=0; I<N; ++I) if (P[I]!=(unsigned char)S[I]) return 0;
  return 1;
}
static int Prefix(const unsigned char *P, unsigned int N, const char *S) {
  unsigned int I, L=Length(S); if (N<L) return 0;
  for (I=0; I<L; ++I) if (P[I]!=(unsigned char)S[I]) return 0;
  return 1;
}
static void Reply(FB_CORE *C, const char *S) {
  unsigned int I=0; while (S[I] && I<FB_COMMAND) { C->Reply[I]=S[I]; ++I; }
  C->Reply[I]=0; C->ReplySize=I;
}
void FbReset(FB_CORE *C) {
  C->Expected=C->Received=C->Valid=C->Action=C->ReplySize=0; C->State=FB_IDLE;
}
void FbInit(FB_CORE *C, unsigned char *Data, unsigned int Capacity) {
  C->Data=Data; C->Capacity=Capacity<FB_LIMIT?Capacity:FB_LIMIT; FbReset(C);
}
static unsigned int Le16(const unsigned char *P) { return P[0]|((unsigned int)P[1]<<8); }
static unsigned int Le32(const unsigned char *P) { return Le16(P)|(Le16(P+2)<<16); }
int FbIsEfi(const unsigned char *P, unsigned int N) {
  unsigned int O, Optional;
  if (N<64 || P[0]!='M' || P[1]!='Z') return 0;
  O=Le32(P+60);
  if (O>N || N-O<24+112 || Le32(P+O)!=0x4550 || Le16(P+O+4)!=0xaa64) return 0;
  Optional=Le16(P+O+20);
  if (Optional<112 || Optional>N-O-24) return 0;
  return Le16(P+O+24)==0x20b && Le16(P+O+24+68)==10;
}
void FbReceive(FB_CORE *C, const unsigned char *P, unsigned int N) {
  unsigned int I, Value=0, Digit;
  C->ReplySize=C->Action=0;
  if (C->State==FB_DOWNLOAD) {
    // USB zero-length packets delimit transfers; they are not download bytes.
    if (!N) return;
    if (N>C->Expected-C->Received) {
      FbReset(C); Reply(C,"FAILdownload length"); return;
    }
    for (I=0; I<N; ++I) C->Data[C->Received+I]=P[I];
    C->Received+=N;
    if (C->Received==C->Expected) {
      C->Valid=C->Received; C->State=FB_IDLE; Reply(C,"OKAY");
    }
    return;
  }
  if (!N || N>FB_COMMAND) { Reply(C,"FAILcommand length"); return; }
  for (I=0; I<N; ++I) if (P[I]<32 || P[I]>126) { Reply(C,"FAILcommand encoding"); return; }
  if (Prefix(P,N,"download:")) {
    C->Valid=C->Expected=C->Received=0;
    if (N!=17) { Reply(C,"FAILdownload size"); return; }
    for (I=9; I<N; ++I) {
      if (P[I]>='0' && P[I]<='9') Digit=P[I]-'0';
      else if (P[I]>='a' && P[I]<='f') Digit=P[I]-'a'+10;
      else if (P[I]>='A' && P[I]<='F') Digit=P[I]-'A'+10;
      else { Reply(C,"FAILdownload hex"); return; }
      Value=(Value<<4)|Digit;
    }
    if (!Value || Value>C->Capacity || !C->Data) { Reply(C,"FAILdownload too large"); return; }
    C->Expected=Value; C->State=FB_DOWNLOAD;
    Reply(C,"DATA00000000");
    for (I=0; I<8; ++I) C->Reply[4+I]="0123456789abcdef"[(Value>>(28-4*I))&15];
  } else if (Equal(P,N,"getvar:version")) Reply(C,"OKAY0.4");
  else if (Equal(P,N,"getvar:product")) Reply(C,"OKAYpolaris");
  else if (Equal(P,N,"getvar:version-bootloader")) Reply(C,"OKAYpolaris-uefi-recovery");
  else if (Equal(P,N,"getvar:max-download-size")) {
    Value=C->Capacity; Reply(C,"OKAY0x00000000");
    for (I=0; I<8; ++I) C->Reply[6+I]="0123456789abcdef"[(Value>>(28-4*I))&15];
  } else if (Equal(P,N,"getvar:is-userspace")) Reply(C,"OKAYno");
  else if (Equal(P,N,"getvar:current-slot")) Reply(C,"FAILspecify explicit partition name");
  else if (Prefix(P,N,"getvar:has-slot:") || Prefix(P,N,"getvar:is-logical:")) Reply(C,"OKAYno");
  else if (Equal(P,N,"getvar:polaris-boot-format")) Reply(C,"OKAYefi-aa64");
  else if (Equal(P,N,"boot") || Equal(P,N,"oem boot-efi")) {
    if (!FbIsEfi(C->Data,C->Valid)) Reply(C,"FAILneed AArch64 EFI application");
    else { C->Action=FB_BOOT; Reply(C,"OKAY"); }
  } else if (Equal(P,N,"reboot")) { C->Action=FB_REBOOT; Reply(C,"OKAY"); }
  else if (Equal(P,N,"reboot-bootloader")) { C->Action=FB_BOOTLOADER; Reply(C,"OKAY"); }
  else if (Equal(P,N,"continue")) { C->Action=FB_EXIT; Reply(C,"OKAY"); }
  else if (Prefix(P,N,"flash:") || Prefix(P,N,"erase:")) Reply(C,"FAILpartition writes not implemented");
  else Reply(C,"FAILunsupported command");
}
