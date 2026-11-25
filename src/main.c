/*
 * main.c - boot + Milestone 2 zone viewer.
 *
 * Boot: arena init and the allocator / VFPU self-tests (VFPU results are
 * checked against the scalar oracle on the real CPU). Then, if cooked data
 * is present in <eboot dir>/data, a free-fly viewer over the zones:
 *
 *   analog       move / strafe          D-pad   turn / look up-down
 *   R / L        fly up / down          X       use door (debug teleport)
 *   TRIANGLE     next zone              SQUARE  previous zone
 *   CIRCLE       toggle overlay         SELECT  cycle face culling CW/CCW/off
 *   START        quit
 *
 * Walking past the edge of an exterior zone loads the neighbouring zone
 * behind a loading card (discrete zones instead of open-world streaming).
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
#include "gfx.h"
#include "mem.h"
#include "selftest.h"
#include "zone.h"

PSP_MODULE_INFO("pspwind", PSP_MODULE_USER, 0, 2);
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
    } while ((pad.Buttons & mask) && g_running); /* release first */
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
/* viewer                                                                   */
/* ------------------------------------------------------------------------ */
typedef struct {
    World world;
    Zone zone;
    Camera cam;
    GfxOptions opt;
    GfxStats stats;
    int show_overlay;
    int selftest_ok;
    float fps;
    char msg[96];
} Viewer;

static Viewer g_v;

static void overlay(void *user)
{
    Viewer *v = (Viewer *)user;
    MemStats zs;
    static const char *cull_names[] = { "CW", "CCW", "off" };
    if (!v->show_overlay)
        return;
    mem_stats(MEM_REGION_ZONE, &zs);
    pspDebugScreenEnableBackColor(0);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenPrintf("%-36.36s %4.1f fps", v->zone.name, v->fps);
    pspDebugScreenSetXY(0, 1);
    pspDebugScreenPrintf("pos %7.0f %7.0f %6.0f  cell %d,%d",
                         v->cam.pos.x, v->cam.pos.y, v->cam.pos.z,
                         (int)floorf(v->cam.pos.x / 8192.0f), (int)floorf(v->cam.pos.y / 8192.0f));
    pspDebugScreenSetXY(0, 2);
    pspDebugScreenPrintf("draw %4u tri %6u cull %4u tex %3u near %3u dl %3uK",
                         (unsigned)v->stats.draws, (unsigned)v->stats.triangles,
                         (unsigned)v->stats.culled, (unsigned)v->stats.tex_binds,
                         (unsigned)v->stats.near_quads, (unsigned)(v->stats.list_bytes >> 10));
    pspDebugScreenSetXY(0, 3);
    pspDebugScreenPrintf("zone mem %4uK/%uK  tex %4uK  cull %s%s",
                         (unsigned)((zs.used_lo + zs.used_hi) >> 10), (unsigned)(zs.size >> 10),
                         (unsigned)(texcache_bytes_used(&v->world.tex) >> 10),
                         cull_names[v->opt.cull_mode], v->selftest_ok ? "" : "  SELFTEST FAIL");
    if (v->msg[0]) {
        pspDebugScreenSetXY(0, 33);
        pspDebugScreenPrintf("%s", v->msg);
    }
}

static int enter_zone(Viewer *v, uint32_t zi, const float *pos, const float *rot)
{
    char err[160];
    const char *name = world_zone_name(&v->world, zi);
    gfx_loading_screen(name, "", 0xFF101418);
    if (zone_load(&v->world, &v->zone, zi, err, sizeof(err)) != 0) {
        snprintf(v->msg, sizeof(v->msg), "load failed: %.80s", err);
        return -1;
    }
    if (pos) {
        float ground;
        v->cam.pos = vec4(pos[0], pos[1], pos[2] + 128.0f, 1.0f);
        if (zone_terrain_height(&v->zone, pos[0], pos[1], &ground) && v->cam.pos.z < ground + 64.0f)
            v->cam.pos.z = ground + 128.0f;
        /* Morrowind z rotation is clockwise from north */
        v->cam.yaw = rot ? rot[2] : v->cam.yaw;
    } else {
        zone_spawn_point(&v->zone, &v->cam.pos, &v->cam.yaw);
    }
    v->cam.pitch = 0.0f;
    snprintf(v->msg, sizeof(v->msg), "%s%s", v->zone.missing_meshes ? "some meshes missing" : "",
             v->world.tex.misses ? "  some textures missing" : "");
    return 0;
}

static void try_door(Viewer *v)
{
    const DZoneHeader *h = v->zone.hdr;
    uint32_t i;
    for (i = 0; i < h->door_count; ++i) {
        const DZoneDoor *d = &v->zone.doors[i];
        const DZoneInstance *in = &v->zone.instances[d->instance];
        float dx = in->pos[0] - v->cam.pos.x, dy = in->pos[1] - v->cam.pos.y;
        if (dx * dx + dy * dy < 250.0f * 250.0f && d->dest_zone != DZONE_NO_ZONE) {
            float pos[3], rot[3];
            memcpy(pos, d->dest_pos, sizeof(pos));
            memcpy(rot, d->dest_rot, sizeof(rot));
            enter_zone(v, d->dest_zone, pos, rot);
            return;
        }
    }
    snprintf(v->msg, sizeof(v->msg), "no door within reach");
}

static void check_border(Viewer *v)
{
    const DZoneHeader *h = v->zone.hdr;
    int cx = (int)floorf(v->cam.pos.x / 8192.0f), cy = (int)floorf(v->cam.pos.y / 8192.0f);
    int zi;
    float pos[3];
    if (h->kind != DZONE_KIND_EXTERIOR)
        return;
    if (cx >= h->cell_x0 && cx <= h->cell_x1 && cy >= h->cell_y0 && cy <= h->cell_y1)
        return;
    zi = world_find_exterior(&v->world, cx, cy);
    if (zi < 0 || (uint32_t)zi == v->zone.index)
        return;
    pos[0] = v->cam.pos.x;
    pos[1] = v->cam.pos.y;
    pos[2] = v->cam.pos.z - 128.0f;
    enter_zone(v, (uint32_t)zi, pos, NULL);
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

static void run_viewer(Viewer *v, const char *data_dir)
{
    char err[200];
    SceCtrlData pad;
    uint32_t prev = 0;
    uint64_t last = clock_us(), fps_t0 = last;
    int frames = 0, zi;

    if (world_open(&v->world, data_dir, err, sizeof(err)) != 0) {
        pspDebugScreenSetTextColor(0xFF4080FF);
        log_printf("\nNo cooked world: %s\n", err);
        pspDebugScreenSetTextColor(0xFFFFFFFF);
        log_printf("Cook one with tools/demake_cooker/cook_all.py and copy\n"
                   "meshes.dpak, textures.dpak, zones.dpak to %s\n\nPress START to exit.\n",
                   data_dir);
        wait_buttons(PSP_CTRL_START);
        return;
    }
    log_printf("\nWorld: %u zones. Press X to enter the viewer.\n",
               (unsigned)world_zone_count(&v->world));
    wait_buttons(PSP_CTRL_CROSS | PSP_CTRL_START);

    gfx_init();
    gfx_default_options(&v->opt);
    v->cam.fov_y = 60.0f * PW_DEG2RAD;
    v->show_overlay = 1;
    zi = world_find_exterior(&v->world, -2, -9); /* Seyda Neen when cooked from retail data */
    if (zi < 0)
        zi = world_find_exterior(&v->world, 0, 0);
    if (zi < 0)
        zi = 0;
    if (enter_zone(v, (uint32_t)zi, NULL, NULL) != 0 && world_zone_count(&v->world) > 1)
        enter_zone(v, 1, NULL, NULL);

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    while (g_running) {
        uint64_t now = clock_us();
        float dt = (float)(now - last) * 1e-6f, ax, ay, speed = 700.0f;
        uint32_t pressed;
        Vec4 fwd, right;
        last = now;
        if (dt > 0.1f)
            dt = 0.1f;
        sceCtrlReadBufferPositive(&pad, 1);
        pressed = pad.Buttons & ~prev;
        prev = pad.Buttons;
        if (pressed & PSP_CTRL_START)
            break;

        if (v->zone.hdr) {
            if (pressed & PSP_CTRL_TRIANGLE)
                enter_zone(v, (v->zone.index + 1) % world_zone_count(&v->world), NULL, NULL);
            else if (pressed & PSP_CTRL_SQUARE)
                enter_zone(v, (v->zone.index + world_zone_count(&v->world) - 1) %
                                  world_zone_count(&v->world), NULL, NULL);
            else if (pressed & PSP_CTRL_CROSS)
                try_door(v);
        }
        if (pressed & PSP_CTRL_CIRCLE)
            v->show_overlay = !v->show_overlay;
        if (pressed & PSP_CTRL_SELECT)
            v->opt.cull_mode = (v->opt.cull_mode + 1) % 3;

        if (pad.Buttons & PSP_CTRL_LEFT)
            v->cam.yaw -= 1.8f * dt;
        if (pad.Buttons & PSP_CTRL_RIGHT)
            v->cam.yaw += 1.8f * dt;
        if (pad.Buttons & PSP_CTRL_UP)
            v->cam.pitch = fminf(v->cam.pitch + 1.2f * dt, 1.4f);
        if (pad.Buttons & PSP_CTRL_DOWN)
            v->cam.pitch = fmaxf(v->cam.pitch - 1.2f * dt, -1.4f);
        v->cam.yaw = pw_wrap_angle(v->cam.yaw);
        ax = ((float)pad.Lx - 128.0f) / 128.0f;
        ay = (128.0f - (float)pad.Ly) / 128.0f;
        if (fabsf(ax) < 0.25f)
            ax = 0.0f;
        if (fabsf(ay) < 0.25f)
            ay = 0.0f;
        fwd = vec4(pw_sinf(v->cam.yaw), pw_cosf(v->cam.yaw), 0.0f, 0.0f);
        right = vec4(fwd.y, -fwd.x, 0.0f, 0.0f);
        v->cam.pos.x += (fwd.x * ay + right.x * ax) * speed * dt;
        v->cam.pos.y += (fwd.y * ay + right.y * ax) * speed * dt;
        if (pad.Buttons & PSP_CTRL_RTRIGGER)
            v->cam.pos.z += speed * dt;
        if (pad.Buttons & PSP_CTRL_LTRIGGER)
            v->cam.pos.z -= speed * dt;
        if (v->zone.hdr)
            check_border(v);

        gfx_begin_frame(v->zone.hdr ? v->zone.hdr->fog_color : 0xFF202020u);
        if (v->zone.hdr)
            gfx_draw_zone(&v->zone, &v->cam, &v->opt, &v->stats);
        gfx_end_frame(overlay, v);

        frames++;
        if (now - fps_t0 >= 1000000u) {
            v->fps = (float)frames * 1e6f / (float)(now - fps_t0);
            frames = 0;
            fps_t0 = now;
        }
    }
    gfx_shutdown();
    world_close(&v->world);
}

int main(int argc, char **argv)
{
    char data_dir[160];
    setup_callbacks();
    pspDebugScreenInit();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    mem_init();
    g_v.selftest_ok = run_selftests();
    log_printf("\n[arena]\n");
    mem_dump(log_line, NULL);
    data_dir_from_argv(data_dir, sizeof(data_dir), argc, argv);
    run_viewer(&g_v, data_dir);
    sceKernelExitGame();
    return 0;
}
