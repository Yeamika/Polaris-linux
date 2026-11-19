// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../platform/WindowsErrata.c"
#include "windows_errata_fixture.h"
VOID *EFIAPI ZeroMem(VOID *P,UINTN N) {return memset(P,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) {return memcmp(A,B,N);}
int main(void) {
 UINT8 *B=malloc(FIXTURE_BYTES),*Copy=malloc(FIXTURE_BYTES);WINDOWS_IMAGE Image;WINDOWS_PATCH_PLAN Plan;
 assert(B && Copy);Fixture(B,FALSE);
 assert(WindowsImageOpen(B,FIXTURE_BYTES,&Image)==EFI_SUCCESS && WindowsFindContext(&Image)==0x1100);
 assert(WindowsPlanLoader(&Image,&Plan)==EFI_SUCCESS && Plan.Count==1 && Plan.Reads==1);
 assert(WindowsApplyPlan(&Image,&Plan)==EFI_SUCCESS && Get32(B,0x1600)==0xd2800008);
 Fixture(B,TRUE);assert(WindowsImageOpen(B,FIXTURE_BYTES,&Image)==EFI_SUCCESS);
 assert(WindowsPlanKernel(&Image,&Plan)==EFI_SUCCESS && Plan.Count==6 && Plan.Reads==2 && Plan.Writes==1 && Plan.Cache==1 && Plan.Psci==1);
 assert(WindowsApplyPlan(&Image,&Plan)==EFI_SUCCESS && Get32(B,0x1810)==0xd2800005 && Get32(B,0x1900)==0xd503201f);
 assert(Get32(B,0x1a08)==0x528007c0 && Get32(B,0x1a0c)==0xd503201f);
 assert(Get32(B,0x1b00)==0x52800000 && Get32(B,0x1b04)==0xd65f03c0);
 // Unexpected bytes abort the complete plan before its first write.
 Fixture(B,TRUE);assert(WindowsPlanKernel(&Image,&Plan)==EFI_SUCCESS);Put32(B,0x1a0c,0);
 memcpy(Copy,B,FIXTURE_BYTES);assert(EFI_ERROR(WindowsApplyPlan(&Image,&Plan)) && !memcmp(B,Copy,FIXTURE_BYTES));
 Fixture(B,TRUE);Put32(B,0x1a0c,0x94000000);memcpy(Copy,B,FIXTURE_BYTES);
 assert(WindowsPlanKernel(&Image,&Plan)==EFI_UNSUPPORTED && !memcmp(B,Copy,FIXTURE_BYTES));
 Fixture(B,TRUE);Put32(B,0x1b80,0x84000012);assert(WindowsPlanKernel(&Image,&Plan)==EFI_UNSUPPORTED);
 Fixture(B,FALSE);Put32(B,0x160c,0);assert(WindowsPlanLoader(&Image,&Plan)==EFI_UNSUPPORTED);
 Fixture(B,FALSE);Put32(B,0x5200,0xd5381028);
 assert(WindowsPlanLoader(&Image,&Plan)==EFI_SUCCESS && Plan.Count==1); // Data is not code.
 Fixture(B,FALSE);Put32(B,0x98+140,24);Pdata(B,2,0x1800,0xdc);memcpy(B+0x1800,B+0x1100,0xdc);
 assert(WindowsImageOpen(B,FIXTURE_BYTES,&Image)==EFI_SUCCESS && !WindowsFindContext(&Image));
 // Broken PE ranges and directories cannot drive speculative reads.
 Fixture(B,FALSE);assert(WindowsImageOpen(B,63,&Image)==EFI_UNSUPPORTED);
 Put32(B,60,0xfffffff0);assert(EFI_ERROR(WindowsImageOpen(B,FIXTURE_BYTES,&Image)));
 Fixture(B,FALSE);Put32(B,0x98+136,0x3100);assert(EFI_ERROR(WindowsImageOpen(B,FIXTURE_BYTES,&Image)));
 Fixture(B,FALSE);Put32(B,0x98+140,9);assert(EFI_ERROR(WindowsImageOpen(B,FIXTURE_BYTES,&Image)));
 Fixture(B,FALSE);Put32(B,0x98+56,0x1000);assert(EFI_ERROR(WindowsImageOpen(B,FIXTURE_BYTES,&Image)));
 free(B);free(Copy);
 puts("PASS: mapped ARM64 PE bounds, linked unwind records, leaf/generic ACTLR, checked cache/PSCI plans and atomic refusal");
 return 0;
}
