/*
 * ui_gu.c - draws a UiCanvas with the GE (PSP only).
 *
 * The 8x8 font lives in a 128x128 T4 texture (16x16 glyph grid, CLUT
 * index 0 transparent, 1 white) built once from font8x8.h; each glyph is
 * one 2D sprite tinted by its vertex colour, so a whole run of text is a
 * single sceGuDrawArray. Panels are untextured alpha-blended sprites.
 * Called after the 3D scene, before gfx_end_frame(); the GE states the
 * world renderer relies on are restored at the end (gfx re-derives its
 * cached state every frame).
 */
#include "ui.h"

#include <pspgu.h>
#include <pspkernel.h>
#include <string.h>

#include "font8x8.h"

#define FONT_TEX 128

typedef struct {
    uint16_t u, v;
    uint32_t color;
    int16_t x, y, z;
    int16_t pad;
} GlyphVertex;

typedef struct {
    uint32_t color;
    int16_t x, y, z;
    int16_t pad;
} RectVertex;

static uint8_t g_font_tex[FONT_TEX * FONT_TEX / 2] __attribute__((aligned(16)));
static uint32_t g_font_clut[16] __attribute__((aligned(16)));
static int g_font_ready;

static void build_font(void)
{
    int ch, x, y;
    memset(g_font_tex, 0, sizeof(g_font_tex));
    for (ch = 0; ch < 256; ++ch) {
        int gx = (ch & 15) * 8, gy = (ch >> 4) * 8;
        for (y = 0; y < 8; ++y) {
            uint8_t bits = font8x8[ch * 8 + y];
            for (x = 0; x < 8; ++x) {
                if (bits & (0x80 >> x)) {
                    int px = gx + x, py = gy + y;
                    uint8_t *b = &g_font_tex[(py * FONT_TEX + px) >> 1];
                    *b = (uint8_t)(*b | ((px & 1) ? 0x10 : 0x01)); /* left pixel = low nibble */
                }
            }
        }
    }
    memset(g_font_clut, 0, sizeof(g_font_clut));
    g_font_clut[1] = 0xFFFFFFFFu;
    sceKernelDcacheWritebackRange(g_font_tex, sizeof(g_font_tex));
    sceKernelDcacheWritebackRange(g_font_clut, sizeof(g_font_clut));
    g_font_ready = 1;
}

static void bind_font(void)
{
    sceGuEnable(GU_TEXTURE_2D);
    sceGuClutMode(GU_PSM_8888, 0, 0x0F, 0);
    sceGuClutLoad(2, g_font_clut); /* 2 blocks of 8 entries */
    sceGuTexMode(GU_PSM_T4, 0, 0, GU_FALSE);
    sceGuTexImage(0, FONT_TEX, FONT_TEX, FONT_TEX, g_font_tex);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
}

static void draw_text_run(const UiCanvas *c, uint32_t first, uint32_t last)
{
    uint32_t glyphs = 0, i, n = 0;
    GlyphVertex *v;
    for (i = first; i < last; ++i)
        glyphs += (uint32_t)c->cmds[i].w;
    if (!glyphs)
        return;
    v = (GlyphVertex *)sceGuGetMemory((int)(glyphs * 2u * sizeof(GlyphVertex)));
    for (i = first; i < last; ++i) {
        const UiCmd *cmd = &c->cmds[i];
        int k;
        for (k = 0; k < cmd->w; ++k) {
            unsigned ch = (unsigned char)c->pool[cmd->ofs + k];
            int x = cmd->x + k * UI_CHAR_W;
            GlyphVertex *a = &v[n++], *b = &v[n++];
            a->u = (uint16_t)((ch & 15) * 8);
            a->v = (uint16_t)((ch >> 4) * 8);
            a->color = cmd->color;
            a->x = (int16_t)x;
            a->y = cmd->y;
            a->z = 0;
            a->pad = 0;
            b->u = (uint16_t)(a->u + 8);
            b->v = (uint16_t)(a->v + 8);
            b->color = cmd->color;
            b->x = (int16_t)(x + 8);
            b->y = (int16_t)(cmd->y + 8);
            b->z = 0;
            b->pad = 0;
        }
    }
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   (int)n, NULL, v);
}

static void draw_rect(const UiCmd *cmd)
{
    RectVertex *v = (RectVertex *)sceGuGetMemory(2 * sizeof(RectVertex));
    v[0].color = v[1].color = cmd->color;
    v[0].x = cmd->x;
    v[0].y = cmd->y;
    v[1].x = (int16_t)(cmd->x + cmd->w);
    v[1].y = (int16_t)(cmd->y + cmd->h);
    v[0].z = v[1].z = 0;
    v[0].pad = v[1].pad = 0;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
}

void ui_render_gu(const UiCanvas *c)
{
    uint32_t i = 0;
    int textured = -1;
    if (!c->count)
        return;
    if (!g_font_ready)
        build_font();
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDepthMask(GU_TRUE);
    sceGuShadeModel(GU_FLAT);
    while (i < c->count) {
        if (c->cmds[i].type == UI_CMD_TEXT) {
            uint32_t j = i;
            while (j < c->count && c->cmds[j].type == UI_CMD_TEXT)
                j++;
            if (textured != 1) {
                bind_font();
                textured = 1;
            }
            draw_text_run(c, i, j);
            i = j;
        } else {
            if (textured != 0) {
                sceGuDisable(GU_TEXTURE_2D);
                textured = 0;
            }
            draw_rect(&c->cmds[i]);
            i++;
        }
    }
    /* hand the GE back to the world renderer */
    sceGuShadeModel(GU_SMOOTH);
    sceGuDepthMask(GU_FALSE);
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexWrap(GU_REPEAT, GU_REPEAT);
}
