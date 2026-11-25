/*
 * Host test runner: builds src/mem.c, src/vfpu.c and src/selftest.c with
 * the native compiler (scalar paths) and runs the shared self-test suite.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "fmt.h" /* compile-time layout checks (PW_STATIC_ASSERT) */
#include "mem.h"
#include "selftest.h"

static void log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static uint64_t clock_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void print_line(const char *line, void *user)
{
    (void)user;
    printf("  %s\n", line);
}

int main(void)
{
    SelftestResult m, v;
    mem_init();
    m = selftest_mem(log_printf);
    printf("mem : %u/%u checks passed\n", m.checks - m.failures, m.checks);
    v = selftest_vfpu(log_printf);
    printf("math: %u/%u checks passed\n", v.checks - v.failures, v.checks);
    selftest_bench(log_printf, clock_us);
    mem_dump(print_line, NULL);
    if (m.failures || v.failures) {
        printf("HOST TESTS FAILED\n");
        return 1;
    }
    printf("HOST TESTS PASSED\n");
    return 0;
}
