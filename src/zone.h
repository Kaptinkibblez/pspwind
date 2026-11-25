/*
 * zone.h - world directory + zone loader.
 *
 * world_open() mounts zones/meshes/textures.dpak and keeps the DWORLD index
 * in the DB region. zone_load() resets the ZONE region and pulls in one
 * zone: the DZONE blob, every DMESH it instances, and their textures
 * (TEXTURE region). Every offset and count in the cooked files is validated
 * before use, so a corrupt pack fails the load cleanly instead of crashing
 * the GE. Precomputed per load: placement matrices for instances and
 * node-world x dequantisation matrices for every batch, so drawing a batch
 * is one VFPU matrix multiply.
 */
#ifndef PSPWIND_ZONE_H
#define PSPWIND_ZONE_H

#include "common.h"
#include "fmt.h"
#include "anim.h"
#include "pak.h"
#include "texcache.h"
#include "vfpu.h"

typedef struct {
    uint32_t hash;
    const uint8_t *base;
    uint32_t size;
    const DMeshHeader *hdr;
    const DMeshNode *nodes;
    const DMeshBatch *batches;
    const DMeshTexture *textures;
    const DMeshCollision *collision;
    const DTexHeader **tex; /* per mesh texture index */
    Mat4 *batch_model;      /* node world * dequant, per batch */
} ZoneMesh;

typedef struct {
    uint32_t index; /* world zone index */
    const uint8_t *base;
    uint32_t size;
    const DZoneHeader *hdr;
    const DZoneCell *cells;
    const uint32_t *mesh_hashes;
    const DMeshTexture *textures;
    const DZoneTerrain *terrain;
    const DMeshBatch *batches;
    const DZoneInstance *instances;
    const DZoneDoor *doors;
    const DZoneLight *lights;
    const char *strings;
    const char *name;
    const DTexHeader **tex; /* zone texture table */
    Mat4 *batch_model;      /* dequant per zone batch */
    ZoneMesh *meshes;
    Mat4 *inst_world;       /* placement per instance */
    const DTexHeader *water_tex; /* NULL: untextured water */
    uint8_t *inst_scratch;       /* per-instance byte for the renderer (visibility) */
    uint8_t *terrain_scratch;    /* per-terrain-patch byte for the renderer */
    const DZoneActor *actor_recs;
    const uint32_t *anim_hashes;
    Anim *anims;               /* loaded clips, anim_count entries (unbound if missing) */
    uint32_t mesh_bytes;
    uint32_t missing_meshes;
    uint32_t missing_anims;
} Zone;

typedef struct {
    Pak zones, meshes, textures;
    Pak anims;  /* optional: anims.dpak (handle 0 when absent) */
    const uint8_t *base;
    uint32_t size;
    const DWorldHeader *hdr;
    const DWorldZone *zone_list;
    const DWorldExt *ext;
    const DWorldInt *ints;
    const char *strings;
    TexCache tex;
} World;

/* data_dir holds zones.dpak, meshes.dpak, textures.dpak (+ optional anims.dpak). */
int world_open(World *w, const char *data_dir, char *err, uint32_t errlen);
void world_close(World *w);
uint32_t world_zone_count(const World *w);
const char *world_zone_name(const World *w, uint32_t zi);
int world_find_exterior(const World *w, int cx, int cy); /* zone index or -1 */
int world_find_interior(const World *w, uint32_t name_hash);

int zone_load(World *w, Zone *z, uint32_t zi, char *err, uint32_t errlen);
/* Ground height under (x, y) from the terrain patches (same triangulation as
 * the renderer). Returns 0 when no patch covers the point. */
int zone_terrain_height(const Zone *z, float x, float y, float *h);
/* Reasonable viewer start position (eye at pos) and yaw. */
void zone_spawn_point(const Zone *z, Vec4 *pos, float *yaw);
/* Static mesh of an instance (NULL when missing, merged into cell batches,
 * or an actor - actors are drawn through actor.h). */
const ZoneMesh *zone_instance_mesh(const Zone *z, uint32_t inst);
/* Morrowind reference placement: T * Rx(-x) * Ry(-y) * Rz(-z) * S. */
void zone_placement_matrix(Mat4 *out, const float pos[3], const float rot[3], float scale);
/* Structural validation of cooked blobs; 0 = valid, else a check id. */
int dmesh_validate(const uint8_t *base, uint32_t size);
int dzone_validate(const uint8_t *base, uint32_t size);

#endif /* PSPWIND_ZONE_H */
