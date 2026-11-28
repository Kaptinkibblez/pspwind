/*
 * gfx.h - PS1/PSP-style sceGu renderer (PSP only).
 *
 * 16-bit 5650 colour + 16-bit depth, nearest-filtered paletted textures,
 * Gouraud vertex colours, thick per-zone distance fog, hardware sun + up to
 * three point lights for instanced models, baked lighting for terrain and
 * merged statics, alpha-tested foliage, sorted alpha blending, translucent
 * water plane, VFPU frustum + fog-distance culling, and on-the-fly
 * re-tessellation of terrain next to the camera (keeps big ground triangles
 * inside the GE guard band so they do not pop out).
 */
#ifndef PSPWIND_GFX_H
#define PSPWIND_GFX_H

#include "actor.h"
#include "common.h"
#include "vfpu.h"
#include "zone.h"

#define GFX_SCREEN_W 480
#define GFX_SCREEN_H 272

typedef struct {
    Vec4 pos;     /* eye position, world units */
    float yaw;    /* radians, 0 = north (+Y), positive turns east */
    float pitch;  /* radians, positive looks up */
    float fov_y;  /* radians */
} Camera;

enum { GFX_CULL_CW = 0, GFX_CULL_CCW = 1, GFX_CULL_OFF = 2 };

/* View direction: yaw 0 looks north (+Y), positive yaw turns east. */
static inline void gfx_camera_forward(const Camera *cam, Vec4 *fwd)
{
    float sy, cy, sp, cp;
    pw_sincosf(cam->yaw, &sy, &cy);
    pw_sincosf(cam->pitch, &sp, &cp);
    *fwd = vec4(sy * cp, cy * cp, sp, 0.0f);
}

typedef struct {
    int cull_mode;       /* GFX_CULL_*: which winding the GE treats as front */
    int near_terrain;    /* re-tessellate terrain near the camera */
    int draw_water;
    float near_radius;   /* re-tessellation radius in world units */
} GfxOptions;

typedef struct {
    uint32_t draws;
    uint32_t triangles;
    uint32_t culled;
    uint32_t tex_binds;
    uint32_t near_quads;
    uint32_t actors;
    uint32_t list_bytes;
} GfxStats;

int gfx_init(void);
void gfx_shutdown(void);
void gfx_default_options(GfxOptions *o);

void gfx_begin_frame(uint32_t clear_abgr);
/* actors may be NULL */
void gfx_draw_zone(const Zone *z, const ActorSet *actors, const Camera *cam,
                   const GfxOptions *opt, GfxStats *stats);
/* Small moving things drawn after the zone (projectiles): a model with
 * its world matrix, or (mesh NULL) a glowing diamond of size units at pos.
 * color is ABGR. */
typedef struct {
    const ZoneMesh *mesh;
    Mat4 world;
    float pos[3];
    float size;
    uint32_t color;
} GfxProp;
void gfx_draw_props(const GfxProp *props, uint32_t count, const Camera *cam, GfxStats *stats);
/* Finishes the display list, waits for the GE, lets the caller draw debug
 * text into the finished frame (overlay may be NULL) and flips at vblank. */
void gfx_end_frame(void (*overlay)(void *user), void *user);
/* frame pacing: present every n vblanks (1 = 60 Hz, 2 = 30 Hz) */
void gfx_set_frame_interval(int vblanks);
int gfx_frame_interval(void);
/* microseconds from gfx_begin_frame to the GPU finishing the last frame */
uint32_t gfx_last_work_us(void);
/* Full-screen loading card drawn immediately (used during zone loads). */
void gfx_loading_screen(const char *title, const char *subtitle, uint32_t bg_abgr);

#endif /* PSPWIND_GFX_H */
