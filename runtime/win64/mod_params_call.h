/* Task 11: the recorded-call tuple shared by the runtime selftest (mock sink in mod_params.c) and the native DLL oracle
 * (tools/native_params_oracle.c).  Plain C99 + stdint only so both programs can include it. */
#pragma once
#include <stdint.h>
#define KH_MP_MAXCALLS 16
struct kh_mp_call {
    uint32_t rva;           /* guest RVA called; 0x40d810 / 0x40dd70 mean "forward to the original" */
    uint64_t a[5];          /* integer arguments rcx,rdx,r8,r9,[rsp+0x20] as passed (unmasked; unused registers are 0 in the runtime, garbage in the oracle) */
    uint32_t f2, f3;        /* raw float bits of xmm2 / xmm3 (3c6dc0 / 3c8310 / 409b00) */
    uint64_t x0;            /* raw double bits of xmm0 (409b00 only) */
    uint64_t mem[3];        /* canonical contents behind pointer args: 3e1840 args array (3 qwords), 3db2f0 string (5 bytes) */
};
