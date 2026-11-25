/*
 * selftest.h - allocator and math self-tests.
 *
 * Shared by the host test runner (tests/host) and the PSP boot harness
 * (main.c). On PSP the VFPU implementations are compared with the scalar
 * ref_* oracle; on the host the same suite validates the math itself.
 */
#ifndef PSPWIND_SELFTEST_H
#define PSPWIND_SELFTEST_H

#include "common.h"

typedef void (*SelftestPrintf)(const char *fmt, ...);
typedef uint64_t (*SelftestClock)(void); /* microseconds */

typedef struct {
    uint32_t checks;
    uint32_t failures;
} SelftestResult;

/* Requires mem_init(); resets every region before returning. */
SelftestResult selftest_mem(SelftestPrintf log);
SelftestResult selftest_vfpu(SelftestPrintf log);
/* Micro-benchmarks of the public (VFPU on PSP) paths vs the scalar ref. */
void selftest_bench(SelftestPrintf log, SelftestClock clock_us);

#endif /* PSPWIND_SELFTEST_H */
