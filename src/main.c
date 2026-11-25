/*
 * main.c - Milestone 1 boot harness.
 *
 * Brings up the 32 MB static arena, runs the allocator and VFPU self-tests
 * (VFPU results are checked against the scalar oracle on the real CPU),
 * benchmarks the VFPU paths and shows everything on the debug screen.
 * Later milestones replace the harness body with the game loop; the
 * self-tests stay reachable from the debug menu.
 */
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include <stdarg.h>
#include <stdio.h>

#include "fmt.h"
#include "mem.h"
#include "selftest.h"

PSP_MODULE_INFO("pspwind", PSP_MODULE_USER, 0, 1);
/* VFPU attribute is mandatory: without it VFPU state is not preserved. */
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
/* newlib heap is kept tiny on purpose: the game never mallocs. */
PSP_HEAP_SIZE_KB(512);

/* The cooked vertex layout must match the GE vertex type bits exactly. */
PW_STATIC_ASSERT(DMESH_VERTEX_GU_FORMAT_BITS ==
                     (GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_NORMAL_8BIT | GU_VERTEX_16BIT),
                 dmesh_vertex_format);
PW_STATIC_ASSERT(DTEX_PSM_T4 == GU_PSM_T4 && DTEX_PSM_T8 == GU_PSM_T8, dtex_psm);

static volatile int g_running = 1;

static int exit_callback(int arg1, int arg2, void *common)
{
    (void)arg1; (void)arg2; (void)common;
    g_running = 0;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp)
{
    int cbid;
    (void)args; (void)argp;
    cbid = sceKernelCreateCallback("exit_cb", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void)
{
    int thid = sceKernelCreateThread("cb_thread", callback_thread, 0x11, 0x1000,
                                     PSP_THREAD_ATTR_USER, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
}

/* Mirrors output to the debug screen and stdout (PSPLink / emulator log). */
static void log_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    pspDebugScreenPrintf("%s", buf);
    printf("%s", buf);
}

static void log_line(const char *line, void *user)
{
    (void)user;
    log_printf(" %s\n", line);
}

static uint64_t clock_us(void)
{
    return (uint64_t)sceKernelGetSystemTimeWide();
}

int main(void)
{
    SelftestResult m, v;
    SceCtrlData pad;
    int all_ok;

    setup_callbacks();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    pspDebugScreenSetTextColor(0xFF40C0FF);
    log_printf("Morrowind PSP Demake - Milestone 1 boot harness\n");
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    log_printf("Arena: %u KB static, newlib heap capped at 512 KB\n\n",
               (unsigned)(MEM_ARENA_SIZE >> 10));

    mem_init();

    log_printf("[mem] arena / regions / pools / buddy\n");
    m = selftest_mem(log_printf);
    log_printf("  %u/%u checks passed\n", (unsigned)(m.checks - m.failures),
               (unsigned)m.checks);

    log_printf("[vfpu] VFPU vs scalar oracle\n");
    v = selftest_vfpu(log_printf);
    log_printf("  %u/%u checks passed\n", (unsigned)(v.checks - v.failures),
               (unsigned)v.checks);

    log_printf("[bench]\n");
    selftest_bench(log_printf, clock_us);

    log_printf("[arena]\n");
    mem_dump(log_line, NULL);

    all_ok = m.failures == 0 && v.failures == 0;
    pspDebugScreenSetTextColor(all_ok ? 0xFF00FF00 : 0xFF0000FF);
    log_printf("\n%s\n", all_ok ? "ALL SELF-TESTS PASSED" : "SELF-TESTS FAILED");
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    log_printf("Press START to exit.\n");

    while (g_running) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Buttons & PSP_CTRL_START)
            break;
        sceDisplayWaitVblankStart();
    }
    sceKernelExitGame();
    return 0;
}
