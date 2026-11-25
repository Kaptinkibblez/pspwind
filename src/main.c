/*
 * main.c - boot, PSP input and the frame loop.
 *
 * Boot runs the allocator / VFPU self-tests, then (with cooked data in
 * <eboot dir>/data) starts first-person exploration (game.c). Gameplay is
 * platform independent; this file only turns the pad into InputState,
 * shows loading cards when the game requests a zone change, draws the HUD
 * and runs the pause menu (START).
 */
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <pspthreadman.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fmt.h"
#include "game.h"
#include "gfx.h"
#include "mem.h"
#include "selftest.h"
#include "zone.h"

PSP_MODULE_INFO("pspwind", PSP_MODULE_USER, 0, 3);
/* VFPU attribute is mandatory: without it VFPU state is not preserved. */
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);
/* newlib heap is kept tiny on purpose: the game never mallocs. */
PSP_HEAP_SIZE_KB(512);

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

static uint32_t wait_buttons(uint32_t mask)
{
    SceCtrlData pad;
    do {
        sceCtrlReadBufferPositive(&pad, 1);
    } while ((pad.Buttons & mask) && g_running);
    for (;;) {
        sceCtrlReadBufferPositive(&pad, 1);
        if ((pad.Buttons & mask) || !g_running)
            return pad.Buttons;
        sceDisplayWaitVblankStart();
    }
}

static int run_selftests(void)
{
    SelftestResult m, v;
    int ok;
    pspDebugScreenSetTextColor(0xFF40C0FF);
    log_printf("Morrowind PSP Demake - boot self-test\n");
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    m = selftest_mem(log_printf);
    log_printf("[mem]  %u/%u checks passed\n", (unsigned)(m.checks - m.failures), (unsigned)m.checks);
    v = selftest_vfpu(log_printf);
    log_printf("[vfpu] %u/%u checks passed (VFPU vs scalar oracle)\n",
               (unsigned)(v.checks - v.failures), (unsigned)v.checks);
    selftest_bench(log_printf, clock_us);
    ok = m.failures == 0 && v.failures == 0;
    pspDebugScreenSetTextColor(ok ? 0xFF00FF00 : 0xFF0000FF);
    log_printf("%s\n", ok ? "ALL SELF-TESTS PASSED" : "SELF-TESTS FAILED");
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    return ok;
}

/* ------------------------------------------------------------------------ */
/* app state                                                                */
/* ------------------------------------------------------------------------ */
enum { MENU_RESUME, MENU_SCHEME, MENU_INVERT, MENU_CULL, MENU_OVERLAY, MENU_QUIT, MENU_COUNT };

typedef struct {
    World world;
    Game game;
    GfxOptions opt;
    GfxStats stats;
    int overlay;
    int paused;
    int menu;
    int selftest_ok;
    float fps;
    char error[120];
} App;

static App g_app;

static void hud(void *user)
{
    App *a = (App *)user;
    const Game *g = &a->game;
    const char *label = game_target_label(g);
    static const char *cull_names[] = { "CW", "CCW", "off" };
    pspDebugScreenEnableBackColor(0);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenSetXY(29, 16);
    pspDebugScreenPrintf("+");
    if (label && !a->paused) {
        pspDebugScreenSetTextColor(0xFF80E0FF);
        pspDebugScreenSetXY(34 - (int)(strlen(label) + 4) / 2, 19);
        pspDebugScreenPrintf("[R] %.40s", label);
    }
    if (g->message_time > 0.0f) {
        pspDebugScreenSetTextColor(0xFFC0F0C0);
        pspDebugScreenSetXY(1, 31);
        pspDebugScreenPrintf("%.66s", g->message);
    }
    if (a->error[0]) {
        pspDebugScreenSetTextColor(0xFF4060FF);
        pspDebugScreenSetXY(1, 32);
        pspDebugScreenPrintf("%.66s", a->error);
    }
    if (a->overlay) {
        MemStats zs;
        mem_stats(MEM_REGION_ZONE, &zs);
        pspDebugScreenSetTextColor(0xFFFFFFFF);
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenPrintf("%-34.34s %4.1f fps %s%s", g->zone.name, a->fps,
                             g->flying ? "FLY " : "", g->body.swimming ? "SWIM" : "");
        pspDebugScreenSetXY(0, 1);
        pspDebugScreenPrintf("pos %7.0f %7.0f %6.0f %s%s  tests %u",
                             g->body.pos.x, g->body.pos.y, g->body.pos.z,
                             g->body.on_ground ? "ground" : "air",
                             g->body.steep ? " steep" : "", (unsigned)g->phys.tests);
        pspDebugScreenSetXY(0, 2);
        pspDebugScreenPrintf("draw %4u tri %6u cull %4u tex %3u actors %2u dl %3uK",
                             (unsigned)a->stats.draws, (unsigned)a->stats.triangles,
                             (unsigned)a->stats.culled, (unsigned)a->stats.tex_binds,
                             (unsigned)a->stats.actors, (unsigned)(a->stats.list_bytes >> 10));
        pspDebugScreenSetXY(0, 3);
        pspDebugScreenPrintf("zone %4uK/%uK tex %4uK coll %u tris cull %s%s",
                             (unsigned)((zs.used_lo + zs.used_hi) >> 10), (unsigned)(zs.size >> 10),
                             (unsigned)(texcache_bytes_used(&a->world.tex) >> 10),
                             (unsigned)g->phys.tri_count, cull_names[a->opt.cull_mode],
                             a->selftest_ok ? "" : " SELFTEST FAIL");
    }
    if (a->paused) {
        static const char *items[MENU_COUNT] = { "Resume", "Controls", "Invert look", "Face culling",
                                                 "Debug overlay", "Quit" };
        int i;
        pspDebugScreenSetTextColor(0xFFFFFFFF);
        pspDebugScreenSetXY(26, 9);
        pspDebugScreenPrintf("-- Paused --");
        for (i = 0; i < MENU_COUNT; ++i) {
            char value[24] = "";
            if (i == MENU_SCHEME)
                snprintf(value, sizeof(value), "%s", g->controls.scheme == CONTROL_SCHEME_A
                                                         ? "A: stick moves" : "B: stick looks");
            else if (i == MENU_INVERT)
                snprintf(value, sizeof(value), "%s", g->controls.invert_y ? "on" : "off");
            else if (i == MENU_CULL)
                snprintf(value, sizeof(value), "%s", cull_names[a->opt.cull_mode]);
            else if (i == MENU_OVERLAY)
                snprintf(value, sizeof(value), "%s", a->overlay ? "on" : "off");
            pspDebugScreenSetTextColor(i == a->menu ? 0xFF40E0FF : 0xFFB0B0B0);
            pspDebugScreenSetXY(20, 11 + i);
            pspDebugScreenPrintf("%c %-14s %s", i == a->menu ? '>' : ' ', items[i], value);
        }
    }
}

static int enter_zone(App *a, uint32_t zone, const float *pos, const float *rot)
{
    char err[120];
    gfx_loading_screen(world_zone_name(&a->world, zone), "", 0xFF101418);
    if (game_enter_zone(&a->game, zone, pos, rot, err, sizeof(err)) != 0) {
        snprintf(a->error, sizeof(a->error), "zone load failed: %.90s", err);
        return -1;
    }
    a->error[0] = '\0';
    if (a->game.zone.missing_meshes || a->world.tex.misses || a->game.zone.missing_anims)
        snprintf(a->error, sizeof(a->error), "missing: %u meshes, %u textures, %u anims",
                 (unsigned)a->game.zone.missing_meshes, (unsigned)a->world.tex.misses,
                 (unsigned)a->game.zone.missing_anims);
    return 0;
}

static uint32_t map_buttons(uint32_t b)
{
    uint32_t out = 0;
    if (b & PSP_CTRL_UP) out |= GB_UP;
    if (b & PSP_CTRL_DOWN) out |= GB_DOWN;
    if (b & PSP_CTRL_LEFT) out |= GB_LEFT;
    if (b & PSP_CTRL_RIGHT) out |= GB_RIGHT;
    if (b & PSP_CTRL_TRIANGLE) out |= GB_TRIANGLE;
    if (b & PSP_CTRL_CROSS) out |= GB_CROSS;
    if (b & PSP_CTRL_SQUARE) out |= GB_SQUARE;
    if (b & PSP_CTRL_CIRCLE) out |= GB_CIRCLE;
    if (b & PSP_CTRL_LTRIGGER) out |= GB_L;
    if (b & PSP_CTRL_RTRIGGER) out |= GB_R;
    if (b & PSP_CTRL_START) out |= GB_START;
    if (b & PSP_CTRL_SELECT) out |= GB_SELECT;
    return out;
}

static float stick(unsigned char v, int invert)
{
    float f = ((float)v - 128.0f) / 127.0f;
    if (invert)
        f = -f;
    if (fabsf(f) < 0.2f)
        return 0.0f;
    /* rescale outside the dead zone so small pushes still start slow */
    f = (f > 0.0f ? f - 0.2f : f + 0.2f) / 0.8f;
    return f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
}

/* Returns 0 to keep running, 1 to quit. */
static int pause_menu(App *a, const InputState *in)
{
    Game *g = &a->game;
    if (in->pressed & GB_UP)
        a->menu = (a->menu + MENU_COUNT - 1) % MENU_COUNT;
    if (in->pressed & GB_DOWN)
        a->menu = (a->menu + 1) % MENU_COUNT;
    if (in->pressed & (GB_CROSS | GB_R | GB_LEFT | GB_RIGHT)) {
        switch (a->menu) {
        case MENU_RESUME:
            a->paused = 0;
            break;
        case MENU_SCHEME:
            g->controls.scheme = g->controls.scheme == CONTROL_SCHEME_A ? CONTROL_SCHEME_B
                                                                         : CONTROL_SCHEME_A;
            break;
        case MENU_INVERT:
            g->controls.invert_y = !g->controls.invert_y;
            break;
        case MENU_CULL:
            a->opt.cull_mode = (a->opt.cull_mode + 1) % 3;
            break;
        case MENU_OVERLAY:
            a->overlay = !a->overlay;
            break;
        case MENU_QUIT:
            return 1;
        default:
            break;
        }
    }
    if (in->pressed & (GB_START | GB_CIRCLE))
        a->paused = 0;
    return 0;
}

static void data_dir_from_argv(char *out, size_t n, int argc, char **argv)
{
    char *slash;
    if (argc > 0 && argv[0] && strchr(argv[0], '/')) {
        snprintf(out, n, "%s", argv[0]);
        slash = strrchr(out, '/');
        *slash = '\0';
        snprintf(out + strlen(out), n - strlen(out), "/data");
    } else {
        snprintf(out, n, "data");
    }
}

static void run(App *a, const char *data_dir)
{
    char err[200];
    SceCtrlData pad;
    uint32_t prev = 0;
    uint64_t last, fps_t0;
    int frames = 0, zi;

    if (world_open(&a->world, data_dir, err, sizeof(err)) != 0) {
        pspDebugScreenSetTextColor(0xFF4080FF);
        log_printf("\nNo cooked world: %s\n", err);
        pspDebugScreenSetTextColor(0xFFFFFFFF);
        log_printf("Cook one with tools/demake_cooker/cook_all.py and copy the .dpak\n"
                   "files to %s\n\nPress START to exit.\n", data_dir);
        wait_buttons(PSP_CTRL_START);
        return;
    }
    log_printf("\nWorld: %u zones%s. Press X to start.\n", (unsigned)world_zone_count(&a->world),
               a->world.anims.handle ? "" : " (no anims.dpak: actors stay in bind pose)");
    wait_buttons(PSP_CTRL_CROSS | PSP_CTRL_START);

    gfx_init();
    gfx_default_options(&a->opt);
    game_init(&a->game, &a->world);
    zi = world_find_exterior(&a->world, -2, -9); /* Seyda Neen with retail data */
    if (zi < 0)
        zi = world_find_exterior(&a->world, 0, 0);
    if (zi < 0)
        zi = 0;
    if (enter_zone(a, (uint32_t)zi, NULL, NULL) != 0 && world_zone_count(&a->world) > 1)
        enter_zone(a, 1, NULL, NULL);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    last = fps_t0 = clock_us();
    while (g_running) {
        uint64_t now = clock_us();
        float dt = (float)(now - last) * 1e-6f;
        InputState in;
        Camera cam;
        uint32_t held;
        last = now;
        if (dt > 0.1f)
            dt = 0.1f; /* long frames (loading) must not launch the player */
        sceCtrlReadBufferPositive(&pad, 1);
        held = map_buttons(pad.Buttons);
        in.held = held;
        in.pressed = held & ~prev;
        prev = held;
        in.ax = stick(pad.Lx, 0);
        in.ay = stick(pad.Ly, 1);

        if (a->paused) {
            if (pause_menu(a, &in))
                break;
        } else if (in.pressed & GB_START) {
            a->paused = 1;
            a->menu = MENU_RESUME;
        } else if (a->game.zone.hdr) {
            game_update(&a->game, &in, dt);
            if (a->game.request.pending) {
                ZoneRequest r = a->game.request;
                enter_zone(a, r.zone, r.has_pos ? r.pos : NULL, r.has_pos ? r.rot : NULL);
                last = clock_us();
            }
        }

        gfx_begin_frame(a->game.zone.hdr ? a->game.zone.hdr->fog_color : 0xFF202020u);
        if (a->game.zone.hdr) {
            game_camera(&a->game, &cam);
            gfx_draw_zone(&a->game.zone, &a->game.actors, &cam, &a->opt, &a->stats);
        }
        gfx_end_frame(hud, a);

        frames++;
        if (now - fps_t0 >= 1000000u) {
            a->fps = (float)frames * 1e6f / (float)(now - fps_t0);
            frames = 0;
            fps_t0 = now;
        }
    }
    gfx_shutdown();
    world_close(&a->world);
}

int main(int argc, char **argv)
{
    char data_dir[160];
    setup_callbacks();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    mem_init();
    g_app.selftest_ok = run_selftests();
    log_printf("\n[arena]\n");
    mem_dump(log_line, NULL);
    data_dir_from_argv(data_dir, sizeof(data_dir), argc, argv);
    run(&g_app, data_dir);
    sceKernelExitGame();
    return 0;
}
