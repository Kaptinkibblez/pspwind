/*
 * gfx.c - PS1/PSP-style sceGu renderer (see gfx.h). PSP only.
 */
#include "gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>

#define BUF_W 512
#define LIST_WORDS (256 * 1024) /* 1 MB display list (static, outside the arena) */
#define NEAR_Z 12.0f
#define MAX_BLEND 768
#define MAX_PATCH_TEX 64
#define DYN_FORMAT (GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D)
#define MESH_FORMAT (DMESH_VERTEX_GU_FORMAT_BITS | GU_INDEX_16BIT | GU_TRANSFORM_3D)

typedef struct {
    float u, v;
    uint32_t color;
    float x, y, z;
} DynVertex;

typedef struct {
    float dist;
    const DMeshBatch *batch;
    const uint8_t *base;
    const Mat4 *world; /* NULL for world-space zone batches */
    const Mat4 *model; /* batch model (dequant or node*dequant) */
    const DTexHeader *tex;
    uint8_t lit;
} BlendItem;

static unsigned int __attribute__((aligned(64))) g_list[LIST_WORDS];
static void *g_fb[2];
static void *g_zb;
static void *g_draw_rel; /* relative VRAM address of the buffer being drawn */

/* per-frame state cache */
static const DTexHeader *g_bound_tex;
static int g_tex_enabled, g_lighting, g_blend, g_alpha_test, g_cull, g_cull_base;
static int g_wrap_u, g_wrap_v;
static GfxStats *g_stats;
static BlendItem g_blend_items[MAX_BLEND];
static uint32_t g_blend_count;

/* ------------------------------------------------------------------------ */
/* setup                                                                    */
/* ------------------------------------------------------------------------ */
int gfx_init(void)
{
    g_fb[0] = guGetStaticVramBuffer(BUF_W, GFX_SCREEN_H, GU_PSM_5650);
    g_fb[1] = guGetStaticVramBuffer(BUF_W, GFX_SCREEN_H, GU_PSM_5650);
    g_zb = guGetStaticVramBuffer(BUF_W, GFX_SCREEN_H, GU_PSM_4444);
    g_draw_rel = g_fb[0];

    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_5650, g_fb[0], BUF_W);
    sceGuDispBuffer(GFX_SCREEN_W, GFX_SCREEN_H, g_fb[1], BUF_W);
    sceGuDepthBuffer(g_zb, BUF_W);
    sceGuOffset(2048 - (GFX_SCREEN_W / 2), 2048 - (GFX_SCREEN_H / 2));
    sceGuViewport(2048, 2048, GFX_SCREEN_W, GFX_SCREEN_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, GFX_SCREEN_W, GFX_SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuFrontFace(GU_CW);
    sceGuEnable(GU_CULL_FACE);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuTexLevelMode(GU_TEXTURE_AUTO, 0.0f);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    pspDebugScreenInitEx((void *)(0x44000000u | (uintptr_t)g_fb[1]), PSP_DISPLAY_PIXEL_FORMAT_565, 0);
    return 0;
}

void gfx_shutdown(void)
{
    sceGuTerm();
}

void gfx_default_options(GfxOptions *o)
{
    o->cull_mode = GFX_CULL_CW;
    o->near_terrain = 1;
    o->draw_water = 1;
    o->near_radius = 1400.0f;
}

void gfx_camera_forward(const Camera *cam, Vec4 *fwd)
{
    float sy, cy, sp, cp;
    pw_sincosf(cam->yaw, &sy, &cy);
    pw_sincosf(cam->pitch, &sp, &cp);
    *fwd = vec4(sy * cp, cy * cp, sp, 0.0f);
}

void gfx_begin_frame(uint32_t clear_abgr)
{
    sceGuStart(GU_DIRECT, g_list);
    sceGuClearColor(clear_abgr);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    g_bound_tex = NULL;
    g_tex_enabled = -1;
    g_lighting = -1;
    g_blend = -1;
    g_alpha_test = -1;
    g_cull = -1;
    g_wrap_u = g_wrap_v = -1;
}

void gfx_end_frame(void (*overlay)(void *user), void *user)
{
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    if (overlay) {
        pspDebugScreenSetBase((u32 *)(0x44000000u | (uintptr_t)g_draw_rel));
        overlay(user);
    }
    sceDisplayWaitVblankStart();
    g_draw_rel = sceGuSwapBuffers();
}

void gfx_loading_screen(const char *title, const char *subtitle, uint32_t bg_abgr)
{
    gfx_begin_frame(bg_abgr);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WAIT);
    pspDebugScreenSetBase((u32 *)(0x44000000u | (uintptr_t)g_draw_rel));
    pspDebugScreenEnableBackColor(0);
    pspDebugScreenSetTextColor(0xFFC8E0F0);
    pspDebugScreenSetXY(4, 15);
    pspDebugScreenPrintf("Loading %s", title ? title : "");
    pspDebugScreenSetTextColor(0xFF8098A8);
    pspDebugScreenSetXY(4, 17);
    pspDebugScreenPrintf("%s", subtitle ? subtitle : "");
    sceDisplayWaitVblankStart();
    g_draw_rel = sceGuSwapBuffers();
}

/* ------------------------------------------------------------------------ */
/* cached state                                                             */
/* ------------------------------------------------------------------------ */
static void set_flag(int state, int on, int *cache)
{
    if (*cache == on)
        return;
    *cache = on;
    if (on)
        sceGuEnable(state);
    else
        sceGuDisable(state);
}

static void bind_texture(const DTexHeader *t)
{
    const uint8_t *base;
    uint32_t i;
    if (!t) {
        set_flag(GU_TEXTURE_2D, 0, &g_tex_enabled);
        return;
    }
    set_flag(GU_TEXTURE_2D, 1, &g_tex_enabled);
    if (t == g_bound_tex)
        return;
    g_bound_tex = t;
    g_stats->tex_binds++;
    base = (const uint8_t *)t;
    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
    sceGuClutLoad(t->clut_entries / 8, base + t->clut_ofs);
    sceGuTexMode(t->psm, t->level_count - 1, 0, (t->flags & DTEX_F_SWIZZLED) ? GU_TRUE : GU_FALSE);
    for (i = 0; i < t->level_count; ++i)
        sceGuTexImage((int)i, t->level[i].width, t->level[i].height, t->level[i].buf_width,
                      base + t->level[i].ofs);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST_MIPMAP_NEAREST, GU_NEAREST); /* crunchy PS1 texels */
}

static void set_wrap(int clamp_u, int clamp_v)
{
    if (clamp_u == g_wrap_u && clamp_v == g_wrap_v)
        return;
    g_wrap_u = clamp_u;
    g_wrap_v = clamp_v;
    sceGuTexWrap(clamp_u ? GU_CLAMP : GU_REPEAT, clamp_v ? GU_CLAMP : GU_REPEAT);
}

/* 0 opaque, 1 alpha test, 2 blended */
static int batch_pass(uint16_t flags, const DTexHeader *tex)
{
    if (flags & DMESH_BATCH_F_ALPHA_BLEND)
        return 2;
    if ((flags & DMESH_BATCH_F_ALPHA_TEST) || (tex && (tex->flags & DTEX_F_ALPHA_TEST)))
        return 1;
    return 0;
}

static void apply_material(const DMeshBatch *b, const DTexHeader *tex, int lit)
{
    int pass = batch_pass(b->flags, tex);
    bind_texture(tex);
    set_wrap(b->flags & DMESH_BATCH_F_CLAMP_U, b->flags & DMESH_BATCH_F_CLAMP_V);
    set_flag(GU_LIGHTING, lit && !(b->flags & DMESH_BATCH_F_EMISSIVE), &g_lighting);
    if (pass >= 1 && (pass == 1 || (tex && (tex->flags & DTEX_F_ALPHA_TEST)))) {
        int ref = b->alpha_ref ? b->alpha_ref : (tex ? tex->alpha_ref : 0x80);
        set_flag(GU_ALPHA_TEST, 1, &g_alpha_test);
        sceGuAlphaFunc(GU_GREATER, ref, 0xFF);
    } else {
        set_flag(GU_ALPHA_TEST, 0, &g_alpha_test);
    }
    if (pass == 2) {
        set_flag(GU_BLEND, 1, &g_blend);
        if (b->flags & DMESH_BATCH_F_ADDITIVE)
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_FIX, 0, 0xFFFFFFFFu);
        else
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuDepthMask(GU_TRUE); /* no depth writes for translucent surfaces */
    } else {
        set_flag(GU_BLEND, 0, &g_blend);
        sceGuDepthMask(GU_FALSE);
    }
    set_flag(GU_CULL_FACE, g_cull_base && !(b->flags & DMESH_BATCH_F_DOUBLE_SIDED), &g_cull);
}

static void draw_batch(const DMeshBatch *b, const uint8_t *base, const Mat4 *model,
                       const DTexHeader *tex, int lit)
{
    apply_material(b, tex, lit);
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)model);
    sceGuTexScale(b->uv_scale[0], b->uv_scale[1]);
    sceGuTexOffset(b->uv_offset[0], b->uv_offset[1]);
    sceGuDrawArray(GU_TRIANGLES, MESH_FORMAT, (int)b->index_count, base + b->index_ofs,
                   base + b->vertex_ofs);
    g_stats->draws++;
    g_stats->triangles += b->index_count / 3u;
}

static void queue_blend(float dist, const DMeshBatch *b, const uint8_t *base, const Mat4 *world,
                        const Mat4 *model, const DTexHeader *tex, int lit)
{
    BlendItem *it;
    if (g_blend_count >= MAX_BLEND)
        return;
    it = &g_blend_items[g_blend_count++];
    it->dist = dist;
    it->batch = b;
    it->base = base;
    it->world = world;
    it->model = model;
    it->tex = tex;
    it->lit = (uint8_t)lit;
}

static int cmp_blend(const void *a, const void *b)
{
    float da = ((const BlendItem *)a)->dist, db = ((const BlendItem *)b)->dist;
    return (da < db) - (da > db); /* far to near */
}

/* ------------------------------------------------------------------------ */
/* runtime-tessellated geometry                                             */
/* ------------------------------------------------------------------------ */
static uint32_t lerp_color(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0, sh;
    for (sh = 0; sh < 32; sh += 8) {
        float ca = (float)((a >> sh) & 0xFF), cb = (float)((b >> sh) & 0xFF);
        out |= ((uint32_t)(ca + (cb - ca) * t + 0.5f) & 0xFFu) << sh;
    }
    return out;
}

static int subdiv_for(float dist, float radius)
{
    if (dist < radius * 0.45f)
        return 4;
    if (dist < radius)
        return 2;
    return 1;
}

/* Emits n x n sub-quads of a bilinear patch (corners a=(0,0) b=(1,0)
 * c=(1,1) d=(0,1)) as a CCW triangle list. */
static DynVertex *emit_patch(DynVertex *v, int n, const float px[4], const float py[4],
                             const float pz[4], const uint32_t col[4], float u0, float v0,
                             float uv_step)
{
    int i, j, k;
    static const uint8_t corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
    float inv = 1.0f / (float)n;
    for (j = 0; j < n; ++j) {
        for (i = 0; i < n; ++i) {
            for (k = 0; k < 6; ++k) {
                float s = (float)(i + corner[k][0]) * inv, t = (float)(j + corner[k][1]) * inv;
                float w0 = (1 - s) * (1 - t), w1 = s * (1 - t), w2 = s * t, w3 = (1 - s) * t;
                v->x = px[0] * w0 + px[1] * w1 + px[2] * w2 + px[3] * w3;
                v->y = py[0] * w0 + py[1] * w1 + py[2] * w2 + py[3] * w3;
                v->z = pz[0] * w0 + pz[1] * w1 + pz[2] * w2 + pz[3] * w3;
                v->u = u0 + s * uv_step;
                v->v = v0 + t * uv_step;
                v->color = lerp_color(lerp_color(col[0], col[1], s), lerp_color(col[3], col[2], s), t);
                ++v;
            }
        }
    }
    return v;
}

static void draw_dynamic(DynVertex *start, DynVertex *end)
{
    int count = (int)(end - start);
    if (count <= 0)
        return;
    sceGuDrawArray(GU_TRIANGLES, DYN_FORMAT, count, NULL, start);
    g_stats->draws++;
    g_stats->triangles += (uint32_t)count / 3u;
}

static void draw_near_terrain(const Zone *z, const DZoneTerrain *t, const Camera *cam,
                              float radius, const Mat4 *identity)
{
    const float sp = 8192.0f / (DZONE_TERRAIN_GRID - 1);
    const float *hg = (const float *)(z->base + t->heights_ofs);
    const uint32_t *cl = (const uint32_t *)(z->base + t->colors_ofs);
    const uint16_t *qt = (const uint16_t *)(z->base + t->quad_tex_ofs);
    uint16_t texs[MAX_PATCH_TEX];
    uint32_t ntex = 0, q, k;
    const int G = DZONE_TERRAIN_GRID, Q = DZONE_TERRAIN_GRID - 1;

    for (q = 0; q < (uint32_t)(Q * Q); ++q) {
        for (k = 0; k < ntex && texs[k] != qt[q]; ++k)
            ;
        if (k == ntex && ntex < MAX_PATCH_TEX)
            texs[ntex++] = qt[q];
    }
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)identity);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    set_wrap(0, 0);
    set_flag(GU_LIGHTING, 0, &g_lighting);
    set_flag(GU_ALPHA_TEST, 0, &g_alpha_test);
    set_flag(GU_BLEND, 0, &g_blend);
    set_flag(GU_CULL_FACE, g_cull_base, &g_cull);
    sceGuDepthMask(GU_FALSE);
    for (k = 0; k < ntex; ++k) {
        uint32_t verts = 0;
        DynVertex *buf, *v;
        int i, j;
        for (j = 0; j < Q; ++j) {
            for (i = 0; i < Q; ++i) {
                float cx, cy;
                if (qt[j * Q + i] != texs[k])
                    continue;
                cx = t->origin[0] + ((float)i + 0.5f) * sp - cam->pos.x;
                cy = t->origin[1] + ((float)j + 0.5f) * sp - cam->pos.y;
                verts += (uint32_t)(subdiv_for(sqrtf(cx * cx + cy * cy), radius) *
                                    subdiv_for(sqrtf(cx * cx + cy * cy), radius) * 6);
            }
        }
        if (!verts)
            continue;
        buf = (DynVertex *)sceGuGetMemory((int)(verts * sizeof(DynVertex)));
        v = buf;
        for (j = 0; j < Q; ++j) {
            for (i = 0; i < Q; ++i) {
                float px[4], py[4], pz[4], cx, cy;
                uint32_t col[4];
                int a = j * G + i;
                if (qt[j * Q + i] != texs[k])
                    continue;
                px[0] = px[3] = t->origin[0] + (float)i * sp;
                px[1] = px[2] = px[0] + sp;
                py[0] = py[1] = t->origin[1] + (float)j * sp;
                py[2] = py[3] = py[0] + sp;
                pz[0] = hg[a];
                pz[1] = hg[a + 1];
                pz[2] = hg[a + G + 1];
                pz[3] = hg[a + G];
                col[0] = cl[a];
                col[1] = cl[a + 1];
                col[2] = cl[a + G + 1];
                col[3] = cl[a + G];
                cx = px[0] + sp * 0.5f - cam->pos.x;
                cy = py[0] + sp * 0.5f - cam->pos.y;
                v = emit_patch(v, subdiv_for(sqrtf(cx * cx + cy * cy), radius), px, py, pz, col,
                               (float)i, (float)j, 1.0f);
                g_stats->near_quads++;
            }
        }
        bind_texture(texs[k] != DZONE_NONE ? z->tex[texs[k]] : NULL);
        draw_dynamic(buf, v);
    }
}

static void draw_water(const Zone *z, const Camera *cam, float fog_far, const Mat4 *identity)
{
    const float tile = 512.0f;
    int half = (int)(fog_far / tile) + 2, i, j;
    float ox = floorf(cam->pos.x / tile) * tile, oy = floorf(cam->pos.y / tile) * tile;
    float wz = z->hdr->water_height;
    uint32_t verts = 0;
    DynVertex *buf, *v;
    const uint32_t col = z->water_tex ? 0x99FFFFFFu : 0x99806040u; /* ABGR */
    if (cam->pos.z < wz - 8.0f)
        return; /* under water: skip the surface (no underwater pass yet) */
    for (j = -half; j < half; ++j)
        for (i = -half; i < half; ++i) {
            float cx = ox + ((float)i + 0.5f) * tile - cam->pos.x;
            float cy = oy + ((float)j + 0.5f) * tile - cam->pos.y;
            int n = subdiv_for(sqrtf(cx * cx + cy * cy), 1100.0f);
            verts += (uint32_t)(n * n * 6);
        }
    buf = (DynVertex *)sceGuGetMemory((int)(verts * sizeof(DynVertex)));
    v = buf;
    for (j = -half; j < half; ++j) {
        for (i = -half; i < half; ++i) {
            float px[4], py[4], pz[4] = { wz, wz, wz, wz };
            uint32_t c4[4] = { col, col, col, col };
            float x0 = ox + (float)i * tile, y0 = oy + (float)j * tile;
            float cx = x0 + tile * 0.5f - cam->pos.x, cy = y0 + tile * 0.5f - cam->pos.y;
            px[0] = px[3] = x0;
            px[1] = px[2] = x0 + tile;
            py[0] = py[1] = y0;
            py[2] = py[3] = y0 + tile;
            v = emit_patch(v, subdiv_for(sqrtf(cx * cx + cy * cy), 1100.0f), px, py, pz, c4,
                           x0 / tile, y0 / tile, 1.0f);
        }
    }
    sceGuSetMatrix(GU_MODEL, (const ScePspFMatrix4 *)identity);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    set_wrap(0, 0);
    set_flag(GU_LIGHTING, 0, &g_lighting);
    set_flag(GU_ALPHA_TEST, 0, &g_alpha_test);
    set_flag(GU_BLEND, 1, &g_blend);
    set_flag(GU_CULL_FACE, 0, &g_cull); /* visible from below too */
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDepthMask(GU_TRUE);
    bind_texture(z->water_tex);
    draw_dynamic(buf, v);
    sceGuDepthMask(GU_FALSE);
}

/* ------------------------------------------------------------------------ */
/* lights                                                                   */
/* ------------------------------------------------------------------------ */
static void setup_lights(const Zone *z, const Camera *cam)
{
    const DZoneHeader *h = z->hdr;
    ScePspFVector3 dir;
    int best[3] = { -1, -1, -1 };
    float bestd[3] = { 1e30f, 1e30f, 1e30f };
    uint32_t i;
    int k;

    sceGuAmbient(h->ambient | 0xFF000000u);
    sceGuColorMaterial(GU_AMBIENT | GU_DIFFUSE);
    sceGuLightMode(GU_SINGLE_COLOR);
    dir.x = -h->sun_dir[0];
    dir.y = -h->sun_dir[1];
    dir.z = -h->sun_dir[2];
    sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE, &dir);
    sceGuLightColor(0, GU_DIFFUSE, h->sun_color | 0xFF000000u);
    sceGuEnable(GU_LIGHT0);

    for (i = 0; i < h->light_count; ++i) {
        const DZoneLight *l = &z->lights[i];
        float dx = l->pos[0] - cam->pos.x, dy = l->pos[1] - cam->pos.y, dz = l->pos[2] - cam->pos.z;
        float d = dx * dx + dy * dy + dz * dz;
        for (k = 0; k < 3; ++k) {
            if (d < bestd[k]) {
                int m;
                for (m = 2; m > k; --m) {
                    bestd[m] = bestd[m - 1];
                    best[m] = best[m - 1];
                }
                bestd[k] = d;
                best[k] = (int)i;
                break;
            }
        }
    }
    for (k = 0; k < 3; ++k) {
        if (best[k] >= 0) {
            const DZoneLight *l = &z->lights[best[k]];
            ScePspFVector3 p;
            float r = l->radius > 16.0f ? l->radius : 16.0f;
            p.x = l->pos[0];
            p.y = l->pos[1];
            p.z = l->pos[2];
            sceGuLight(k + 1, GU_POINTLIGHT, GU_DIFFUSE, &p);
            sceGuLightColor(k + 1, GU_DIFFUSE, l->color | 0xFF000000u);
            sceGuLightAtt(k + 1, 1.0f, 0.0f, 4.0f / (r * r));
            sceGuEnable(GU_LIGHT1 + k);
        } else {
            sceGuDisable(GU_LIGHT1 + k);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* zone                                                                     */
/* ------------------------------------------------------------------------ */
static float sphere_dist(const Camera *cam, const float b[4])
{
    float dx = b[0] - cam->pos.x, dy = b[1] - cam->pos.y, dz = b[2] - cam->pos.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int visible(const Frustum *f, const Camera *cam, const float b[4], float fog_far, float *dist)
{
    Vec4 c = vec4(b[0], b[1], b[2], 1.0f);
    *dist = sphere_dist(cam, b);
    if (*dist - b[3] > fog_far)
        return 0;
    return frustum_test_sphere(f, &c, b[3]) != CULL_OUTSIDE;
}

void gfx_draw_zone(const Zone *z, const Camera *cam, const GfxOptions *opt, GfxStats *stats)
{
    const DZoneHeader *h = z->hdr;
    static Mat4 proj, view, vp, identity, model;
    static Frustum frustum;
    uint8_t *near_patch = z->terrain_scratch;
    uint8_t *inst_visible = z->inst_scratch;
    Vec4 fwd, at, up = vec4(0.0f, 0.0f, 1.0f, 0.0f);
    float fog_far = h->fog_far;
    uint32_t c, i, pass;

    memset(stats, 0, sizeof(*stats));
    g_stats = stats;
    g_blend_count = 0;
    mat4_identity(&identity);

    gfx_camera_forward(cam, &fwd);
    vec4_add(&at, &cam->pos, &fwd);
    at.w = 1.0f;
    mat4_perspective(&proj, cam->fov_y, (float)GFX_SCREEN_W / (float)GFX_SCREEN_H, NEAR_Z,
                     fog_far + 256.0f);
    mat4_look_at(&view, &cam->pos, &at, &up);
    mat4_mul(&vp, &proj, &view);
    frustum_from_matrix(&frustum, &vp);
    sceGuSetMatrix(GU_PROJECTION, (const ScePspFMatrix4 *)&proj);
    sceGuSetMatrix(GU_VIEW, (const ScePspFMatrix4 *)&view);

    sceGuFog(h->fog_near, fog_far, h->fog_color);
    sceGuEnable(GU_FOG);
    g_cull_base = opt->cull_mode != GFX_CULL_OFF;
    sceGuFrontFace(opt->cull_mode == GFX_CULL_CCW ? GU_CCW : GU_CW);
    setup_lights(z, cam);

    /* terrain patches close to the camera are re-tessellated instead */
    memset(near_patch, 0, h->terrain_count);
    if (opt->near_terrain) {
        for (i = 0; i < h->terrain_count; ++i) {
            const DZoneTerrain *t = &z->terrain[i];
            float dx = fmaxf(fmaxf(t->origin[0] - cam->pos.x, 0.0f), cam->pos.x - (t->origin[0] + 8192.0f));
            float dy = fmaxf(fmaxf(t->origin[1] - cam->pos.y, 0.0f), cam->pos.y - (t->origin[1] + 8192.0f));
            near_patch[i] = (uint8_t)(dx * dx + dy * dy < opt->near_radius * opt->near_radius);
        }
    }

    /* instance visibility once per frame */
    for (i = 0; i < h->instance_count; ++i) {
        float d;
        inst_visible[i] = (uint8_t)(zone_instance_mesh(z, i) &&
                                    visible(&frustum, cam, z->instances[i].bound, fog_far, &d));
        if (!inst_visible[i])
            stats->culled++;
    }

    for (pass = 0; pass < 2; ++pass) {
        /* cell batches: terrain (baked light) and merged statics */
        for (c = 0; c < h->cell_count; ++c) {
            const DZoneCell *cell = &z->cells[c];
            uint32_t t0 = 0, t1 = 0;
            if (cell->terrain != DZONE_NONE) {
                t0 = z->terrain[cell->terrain].first_batch;
                t1 = t0 + z->terrain[cell->terrain].batch_count;
                if (pass == 0 && near_patch[cell->terrain])
                    draw_near_terrain(z, &z->terrain[cell->terrain], cam, opt->near_radius, &identity);
            }
            for (i = cell->first_batch; i < cell->first_batch + cell->batch_count; ++i) {
                const DMeshBatch *b = &z->batches[i];
                const DTexHeader *tex = b->texture != DMESH_NO_TEXTURE ? z->tex[b->texture] : NULL;
                int is_terrain = i >= t0 && i < t1;
                float d;
                if (is_terrain && near_patch[cell->terrain])
                    continue;
                if (!visible(&frustum, cam, b->bound, fog_far, &d)) {
                    if (pass == 0)
                        stats->culled++;
                    continue;
                }
                if (batch_pass(b->flags, tex) == 2) {
                    if (pass == 0)
                        queue_blend(d, b, z->base, NULL, &z->batch_model[i], tex, !is_terrain);
                    continue;
                }
                if ((int)pass != batch_pass(b->flags, tex))
                    continue;
                draw_batch(b, z->base, &z->batch_model[i], tex, !is_terrain);
            }
        }
        /* instanced models */
        for (i = 0; i < h->instance_count; ++i) {
            const ZoneMesh *m;
            uint32_t k;
            if (!inst_visible[i])
                continue;
            m = zone_instance_mesh(z, i);
            for (k = 0; k < m->hdr->batch_count; ++k) {
                const DMeshBatch *b = &m->batches[k];
                const DTexHeader *tex = b->texture != DMESH_NO_TEXTURE ? m->tex[b->texture] : NULL;
                int bp = batch_pass(b->flags, tex);
                if (bp == 2) {
                    if (pass == 0)
                        queue_blend(sphere_dist(cam, z->instances[i].bound), b, m->base,
                                    &z->inst_world[i], &m->batch_model[k], tex, 1);
                    continue;
                }
                if ((int)pass != bp)
                    continue;
                mat4_mul(&model, &z->inst_world[i], &m->batch_model[k]);
                draw_batch(b, m->base, &model, tex, 1);
            }
        }
    }

    if (opt->draw_water && (h->flags & DZONE_F_WATER))
        draw_water(z, cam, fog_far, &identity);

    qsort(g_blend_items, g_blend_count, sizeof(BlendItem), cmp_blend);
    for (i = 0; i < g_blend_count; ++i) {
        const BlendItem *it = &g_blend_items[i];
        if (it->world) {
            mat4_mul(&model, it->world, it->model);
            draw_batch(it->batch, it->base, &model, it->tex, it->lit);
        } else {
            draw_batch(it->batch, it->base, it->model, it->tex, it->lit);
        }
    }
    sceGuDepthMask(GU_FALSE);
    stats->list_bytes = (uint32_t)((uintptr_t)sceGuGetMemory(0) - (uintptr_t)g_list);
}
