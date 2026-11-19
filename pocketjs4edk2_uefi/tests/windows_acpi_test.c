// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../efi/Windows/Acpi.c"

VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) {return memcpy(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V) {return memset(D,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) {return memcmp(A,B,N);}
UINT32 EFIAPI ReadUnaligned32(CONST UINT32 *P) {UINT32 V;memcpy(&V,P,4);return V;}
UINTN EFIAPI AsciiStrnLenS(CONST CHAR8 *S,UINTN N) {UINTN I=0;while(I<N && S[I]) ++I;return I;}
UINT8 EFIAPI CalculateSum8(CONST UINT8 *D,UINTN N) {UINT8 S=0;while(N--) S+=*D++;return S;}
UINT8 EFIAPI CalculateCheckSum8(CONST UINT8 *D,UINTN N) {return (UINT8)-CalculateSum8(D,N);}

static void Checksum(UINT8 *D,UINTN N) {D[9]=0;D[9]=CalculateCheckSum8(D,N);}
static void Unchanged(UINT8 *D,UINTN N,WINDOWS_PLATFORM_INFO *Info) {
  UINT8 *Before=malloc(N);assert(Before);memcpy(Before,D,N);
  assert(EFI_ERROR(WindowsPatchDsdt(D,N,Info)) && !memcmp(Before,D,N));free(Before);
}
int main(int Argc,char **Argv) {
  FILE *File;long Length;UINT8 *Original,*Data,*Prepared,*Allowed;
  WINDOWS_PLATFORM_INFO Info={0};UINTN I,J;
  // Real pinned Polaris DSDT: regress the observed SOSI PFN and SIDM QWord.
  const UINTN Offset[]={66,98,108,116,124,154,168,182,192,202,212,222,232,242,252,262,272,282,292};
  assert(Argc==2);File=fopen(Argv[1],"rb");assert(File);
  assert(!fseek(File,0,SEEK_END));Length=ftell(File);assert(Length==263439);rewind(File);
  Original=malloc(Length);Data=malloc(Length);Prepared=malloc(Length);Allowed=calloc(Length,1);
  assert(Original && Data && Prepared && Allowed);
  assert(fread(Original,1,Length,File)==(size_t)Length);fclose(File);
  assert(CalculateSum8(Original,Length)==0);
  UINT64 Sosi;memcpy(&Sosi,Original+292,8);
  assert(Sosi==0xdeadbeefffffffffULL && Sosi>>12==0x000deadbeeffffffULL);
  for(I=0;I<WINDOWS_ACPI_VALUES;++I) Info.Value[I]=I+1;
  Info.Value[0]=321;Info.Value[1]=0x20001;Info.Value[2]=2;Info.Value[3]=1;
  Info.Value[5]=0x12345678;Info.Value[6]=0x123456789abcdef0ULL;
  Info.Value[16]=0x8ab00000;Info.Value[17]=0x01400000;Info.Value[18]=0x86001200;
  memcpy(Info.Chip,"SDM845",7);memcpy(Data,Original,Length);
  assert(WindowsPatchDsdt(Data,Length,&Info)==EFI_SUCCESS && CalculateSum8(Data,Length)==0);
  for(I=0;I<WINDOWS_ACPI_VALUES;++I) {
    UINT64 Value=0;memcpy(&Value,Data+Offset[I],Widths[I]);assert(Value==Info.Value[I]);
    for(J=0;J<Widths[I];++J) Allowed[Offset[I]+J]=1;
  }
  for(I=76;I<92;++I) Allowed[I]=1;
  Allowed[9]=1;
  for(I=0;I<(UINTN)Length;++I) if(!Allowed[I]) assert(Data[I]==Original[I]);
  assert(!memcmp(Data+76,"SDM845",7));
  for(I=83;I<92;++I) assert(Data[I]==0xa3);
  memcpy(Prepared,Data,Length);assert(WindowsPatchDsdt(Data,Length,&Info)==EFI_SUCCESS);
  assert(!memcmp(Data,Prepared,Length));
  // Each malformed table is rejected atomically, including errors at the last field.
  memcpy(Data,Original,Length);Data[291]=0x0c;Checksum(Data,Length);Unchanged(Data,Length,&Info);
  memcpy(Data,Original,Length);memcpy(Data+Length-14,"\x08SOSI\x0e",6);Checksum(Data,Length);Unchanged(Data,Length,&Info);
  memcpy(Data,Original,Length);Data[9]^=1;Unchanged(Data,Length,&Info);
  memcpy(Data,Original,Length);Unchanged(Data,Length-1,&Info);
  Info.Value[4]=0x10000;memcpy(Data,Original,Length);Unchanged(Data,Length,&Info);
  Info.Value[4]=1;memset(Info.Chip,'x',sizeof(Info.Chip));Unchanged(Data,Length,&Info);
  free(Allowed);free(Prepared);free(Data);free(Original);
  puts("PASS: real Polaris DSDT, SOSI PFN regression, full SIDM width, idempotence and atomic rejection");
  return 0;
}
