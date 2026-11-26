/*
 * zone.c - world directory + zone loader (see zone.h).
 */
#include "zone.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mem.h"

#if PW_PSP
#include <psputils.h>
#endif

/* ------------------------------------------------------------------------ */
/* validation helpers                                                       */
/* ------------------------------------------------------------------------ */
static int range_ok(uint32_t ofs, uint32_t count, uint32_t elem, uint32_t size, uint32_t align)
{
    if (count == 0)
        return ofs <= size;
    if (align > 1 && (ofs % align) != 0)
        return 0;
    if (ofs > size || elem == 0)
        return 0;
    return count <= (size - ofs) / elem;
}

static const char *safe_str(const uint8_t *base, uint32_t size, uint32_t strings_ofs, uint32_t ofs)
{
    uint32_t i;
    if (strings_ofs > size || ofs >= size - strings_ofs)
        return "";
    for (i = strings_ofs + ofs; i < size; ++i)
        if (base[i] == 0)
            return (const char *)base + strings_ofs + ofs;
    return "";
}

static int batches_valid(const uint8_t *base, uint32_t size, const DMeshBatch *b, uint32_t count,
                         uint32_t node_limit, uint32_t tex_limit)
{
    uint32_t i, k;
    for (i = 0; i < count; ++i) {
        const uint16_t *idx;
        if (b[i].node >= node_limit)
            return 0;
        if (b[i].texture != DMESH_NO_TEXTURE && b[i].texture >= tex_limit)
            return 0;
        if (b[i].index_count % 3u)
            return 0;
        if (!range_ok(b[i].vertex_ofs, b[i].vertex_count, sizeof(DMeshVertex), size, 16))
            return 0;
        if (!range_ok(b[i].index_ofs, b[i].index_count, 2, size, 2))
            return 0;
        idx = (const uint16_t *)(base + b[i].index_ofs);
        for (k = 0; k < b[i].index_count; ++k)
            if (idx[k] >= b[i].vertex_count)
                return 0;
    }
    return 1;
}

int dmesh_validate(const uint8_t *base, uint32_t size)
{
    const DMeshHeader *h = (const DMeshHeader *)base;
    const DMeshNode *nodes;
    uint32_t i;
    if (size < sizeof(*h) || h->magic != DMESH_MAGIC || h->version != DMESH_VERSION ||
        h->file_size != size)
        return 1;
    if (h->node_count == 0 || !range_ok(h->nodes_ofs, h->node_count, sizeof(DMeshNode), size, 16) ||
        !range_ok(h->batches_ofs, h->batch_count, sizeof(DMeshBatch), size, 16) ||
        !range_ok(h->textures_ofs, h->texture_count, sizeof(DMeshTexture), size, 4) ||
        !range_ok(h->emitters_ofs, h->emitter_count, sizeof(DMeshEmitter), size, 4))
        return 2;
    nodes = (const DMeshNode *)(base + h->nodes_ofs);
    for (i = 0; i < h->node_count; ++i)
        if (nodes[i].parent >= (int)i || nodes[i].parent < DMESH_NO_PARENT)
            return 3;
    if (!batches_valid(base, size, (const DMeshBatch *)(base + h->batches_ofs), h->batch_count,
                       h->node_count, h->texture_count))
        return 4;
    if (h->collision_ofs) {
        const DMeshCollision *c;
        if (!range_ok(h->collision_ofs, 1, sizeof(DMeshCollision), size, 16))
            return 5;
        c = (const DMeshCollision *)(base + h->collision_ofs);
        if (!range_ok(c->vertex_ofs, c->vertex_count, 12, size, 4) ||
            !range_ok(c->index_ofs, c->tri_count * 3u,
                      (c->flags & DMESH_COLL_F_INDEX32) ? 4u : 2u, size, 2))
            return 6;
    }
    return 0;
}

int dzone_validate(const uint8_t *base, uint32_t size)
{
    const DZoneHeader *h = (const DZoneHeader *)base;
    const DZoneCell *cells;
    const DZoneTerrain *ter;
    const DZoneInstance *ins;
    const DZoneDoor *doors;
    const DZoneLight *lights;
    uint32_t i, k;
    if (size < sizeof(*h) || h->magic != DZONE_MAGIC || h->version != DZONE_VERSION ||
        h->file_size != size)
        return 1;
    if (!range_ok(h->cells_ofs, h->cell_count, sizeof(DZoneCell), size, 16) ||
        !range_ok(h->meshes_ofs, h->mesh_count, 4, size, 4) ||
        !range_ok(h->textures_ofs, h->texture_count, sizeof(DMeshTexture), size, 4) ||
        !range_ok(h->terrain_ofs, h->terrain_count, sizeof(DZoneTerrain), size, 16) ||
        !range_ok(h->batches_ofs, h->batch_count, sizeof(DMeshBatch), size, 16) ||
        !range_ok(h->instances_ofs, h->instance_count, sizeof(DZoneInstance), size, 16) ||
        !range_ok(h->doors_ofs, h->door_count, sizeof(DZoneDoor), size, 16) ||
        !range_ok(h->lights_ofs, h->light_count, sizeof(DZoneLight), size, 16) ||
        h->strings_ofs > size)
        return 2;
    if (!batches_valid(base, size, (const DMeshBatch *)(base + h->batches_ofs), h->batch_count,
                       h->cell_count, h->texture_count))
        return 3;
    cells = (const DZoneCell *)(base + h->cells_ofs);
    for (i = 0; i < h->cell_count; ++i) {
        if (cells[i].first_batch > h->batch_count ||
            cells[i].batch_count > h->batch_count - cells[i].first_batch ||
            cells[i].first_instance > h->instance_count ||
            cells[i].instance_count > h->instance_count - cells[i].first_instance ||
            (cells[i].terrain != DZONE_NONE && cells[i].terrain >= h->terrain_count))
            return 4;
    }
    ter = (const DZoneTerrain *)(base + h->terrain_ofs);
    for (i = 0; i < h->terrain_count; ++i) {
        const uint16_t *q;
        const uint32_t n = DZONE_TERRAIN_GRID * DZONE_TERRAIN_GRID;
        const uint32_t nq = (DZONE_TERRAIN_GRID - 1) * (DZONE_TERRAIN_GRID - 1);
        if (!range_ok(ter[i].heights_ofs, n, 4, size, 4) ||
            !range_ok(ter[i].colors_ofs, n, 4, size, 4) ||
            !range_ok(ter[i].quad_tex_ofs, nq, 2, size, 2) ||
            ter[i].first_batch > h->batch_count ||
            ter[i].batch_count > h->batch_count - ter[i].first_batch)
            return 5;
        q = (const uint16_t *)(base + ter[i].quad_tex_ofs);
        for (k = 0; k < nq; ++k)
            if (q[k] != DZONE_NONE && q[k] >= h->texture_count)
                return 5;
    }
    ins = (const DZoneInstance *)(base + h->instances_ofs);
    for (i = 0; i < h->instance_count; ++i) {
        if ((ins[i].mesh != DZONE_NONE && ins[i].mesh >= h->mesh_count) ||
            ins[i].cell >= h->cell_count ||
            (ins[i].door != DZONE_NONE && ins[i].door >= h->door_count))
            return 6;
    }
    doors = (const DZoneDoor *)(base + h->doors_ofs);
    for (i = 0; i < h->door_count; ++i)
        if (doors[i].instance >= h->instance_count)
            return 7;
    lights = (const DZoneLight *)(base + h->lights_ofs);
    for (i = 0; i < h->light_count; ++i)
        if (lights[i].instance >= h->instance_count)
            return 8;
    if (!range_ok(h->anims_ofs, h->anim_count, 4, size, 4) ||
        !range_ok(h->actors_ofs, h->actor_count, sizeof(DZoneActor), size, 16))
        return 9;
    {
        const DZoneActor *act = (const DZoneActor *)(base + h->actors_ofs);
        for (i = 0; i < h->actor_count; ++i) {
            if (act[i].instance >= h->instance_count || act[i].skeleton >= h->mesh_count ||
                (act[i].anim != DZONE_NONE && act[i].anim >= h->anim_count) ||
                act[i].part_count > DZONE_ACTOR_MAX_PARTS ||
                !range_ok(act[i].part_bone_ofs, act[i].part_count, 4, size, 4))
                return 10;
            for (k = 0; k < act[i].part_count; ++k)
                if (act[i].parts[k] >= h->mesh_count)
                    return 10;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* world                                                                    */
/* ------------------------------------------------------------------------ */
static int open_pak(Pak *p, const char *dir, const char *name, char *err, uint32_t errlen)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return pak_open(p, path, MEM_REGION_DB, err, errlen);
}

int world_open(World *w, const char *data_dir, char *err, uint32_t errlen)
{
    const DWorldHeader *h;
    uint32_t i;
    memset(w, 0, sizeof(*w));
    if (open_pak(&w->zones, data_dir, "zones.dpak", err, errlen) ||
        open_pak(&w->meshes, data_dir, "meshes.dpak", err, errlen) ||
        open_pak(&w->textures, data_dir, "textures.dpak", err, errlen)) {
        world_close(w);
        return -1;
    }
    {
        char ignored[8];
        /* animations are optional: actors stay in their bind pose without them */
        if (open_pak(&w->anims, data_dir, "anims.dpak", ignored, sizeof(ignored)) != 0)
            memset(&w->anims, 0, sizeof(w->anims));
    }
    w->base = (const uint8_t *)pak_load(&w->zones, pw_hash_name("world"), MEM_REGION_DB, 16,
                                        &w->size);
    if (!w->base) {
        snprintf(err, errlen, "zones.dpak has no world index");
        world_close(w);
        return -1;
    }
    h = (const DWorldHeader *)w->base;
    if (w->size < sizeof(*h) || h->magic != DWORLD_MAGIC || h->version != DWORLD_VERSION ||
        h->file_size != w->size ||
        !range_ok(h->zones_ofs, h->zone_count, sizeof(DWorldZone), w->size, 4) ||
        !range_ok(h->ext_ofs, h->ext_count, sizeof(DWorldExt), w->size, 4) ||
        !range_ok(h->int_ofs, h->int_count, sizeof(DWorldInt), w->size, 4) ||
        h->strings_ofs > w->size) {
        snprintf(err, errlen, "corrupt world index");
        world_close(w);
        return -1;
    }
    w->hdr = h;
    w->zone_list = (const DWorldZone *)(w->base + h->zones_ofs);
    w->ext = (const DWorldExt *)(w->base + h->ext_ofs);
    w->ints = (const DWorldInt *)(w->base + h->int_ofs);
    w->strings = (const char *)w->base + h->strings_ofs;
    for (i = 0; i < h->ext_count; ++i) {
        if (w->ext[i].zone >= h->zone_count) {
            snprintf(err, errlen, "corrupt world exterior map");
            world_close(w);
            return -1;
        }
    }
    for (i = 0; i < h->int_count; ++i) {
        if (w->ints[i].zone >= h->zone_count) {
            snprintf(err, errlen, "corrupt world interior map");
            world_close(w);
            return -1;
        }
    }
    texcache_init(&w->tex, &w->textures);
    return 0;
}

void world_close(World *w)
{
    pak_close(&w->zones);
    pak_close(&w->meshes);
    pak_close(&w->textures);
    pak_close(&w->anims);
    w->hdr = NULL;
}

uint32_t world_zone_count(const World *w)
{
    return w->hdr ? w->hdr->zone_count : 0;
}

const char *world_zone_name(const World *w, uint32_t zi)
{
    if (!w->hdr || zi >= w->hdr->zone_count)
        return "?";
    return safe_str(w->base, w->size, w->hdr->strings_ofs, w->zone_list[zi].name_ofs);
}

int world_find_exterior(const World *w, int cx, int cy)
{
    uint32_t lo = 0, hi = w->hdr ? w->hdr->ext_count : 0;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        const DWorldExt *e = &w->ext[mid];
        if (e->x == cx && e->y == cy)
            return (int)e->zone;
        if (e->x < cx || (e->x == cx && e->y < cy))
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

int world_find_interior(const World *w, uint32_t name_hash)
{
    uint32_t lo = 0, hi = w->hdr ? w->hdr->int_count : 0;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (w->ints[mid].hash == name_hash)
            return (int)w->ints[mid].zone;
        if (w->ints[mid].hash < name_hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/* zone loading                                                             */
/* ------------------------------------------------------------------------ */
void zone_placement_matrix(Mat4 *out, const float pos[3], const float rot[3], float scale)
{
    mat4_translation(out, pos[0], pos[1], pos[2]);
    mat4_rotate_x(out, -rot[0]);
    mat4_rotate_y(out, -rot[1]);
    mat4_rotate_z(out, -rot[2]);
    mat4_scale(out, scale, scale, scale);
}

static void dequant_matrix(Mat4 *out, const DMeshBatch *b)
{
    mat4_identity(out);
    out->x.x = b->pos_scale[0];
    out->y.y = b->pos_scale[1];
    out->z.z = b->pos_scale[2];
    out->w = vec4(b->pos_offset[0], b->pos_offset[1], b->pos_offset[2], 1.0f);
}

static int load_mesh(World *w, ZoneMesh *m, uint32_t hash)
{
    uint32_t i;
    Mat4 *worlds;
    MemMark scratch;
    memset(m, 0, sizeof(*m));
    m->hash = hash;
    m->base = (const uint8_t *)pak_load(&w->meshes, hash, MEM_REGION_ZONE, 64, &m->size);
    if (!m->base)
        return -1;
    if (dmesh_validate(m->base, m->size) != 0) {
        m->base = NULL;
        return -2;
    }
    m->hdr = (const DMeshHeader *)m->base;
    m->nodes = (const DMeshNode *)(m->base + m->hdr->nodes_ofs);
    m->batches = (const DMeshBatch *)(m->base + m->hdr->batches_ofs);
    m->textures = (const DMeshTexture *)(m->base + m->hdr->textures_ofs);
    m->collision = m->hdr->collision_ofs
                       ? (const DMeshCollision *)(m->base + m->hdr->collision_ofs) : NULL;
    m->tex = (const DTexHeader **)mem_alloc(MEM_REGION_ZONE,
                                            (m->hdr->texture_count + 1u) * sizeof(void *), 4);
    for (i = 0; i < m->hdr->texture_count; ++i)
        m->tex[i] = texcache_get(&w->tex, m->textures[i].hash);
    m->batch_model = (Mat4 *)mem_alloc(MEM_REGION_ZONE,
                                       (m->hdr->batch_count + 1u) * sizeof(Mat4), 64);
    scratch = mem_mark_hi(MEM_REGION_ZONE);
    worlds = (Mat4 *)mem_alloc_hi(MEM_REGION_ZONE, m->hdr->node_count * sizeof(Mat4), 64);
    for (i = 0; i < m->hdr->node_count; ++i) {
        const DMeshNode *n = &m->nodes[i];
        Vec4 pos = vec4(n->pos[0], n->pos[1], n->pos[2], 1.0f);
        Quat q = vec4(n->rot[0], n->rot[1], n->rot[2], n->rot[3]);
        Mat4 local;
        mat4_from_trs(&local, &pos, &q, n->scale);
        if (n->parent < 0)
            worlds[i] = local;
        else
            mat4_mul(&worlds[i], &worlds[n->parent], &local);
    }
    for (i = 0; i < m->hdr->batch_count; ++i) {
        Mat4 dq;
        dequant_matrix(&dq, &m->batches[i]);
        mat4_mul(&m->batch_model[i], &worlds[m->batches[i].node], &dq);
    }
    mem_release_hi(MEM_REGION_ZONE, scratch);
    return 0;
}

static void resolve_water(World *w, Zone *z)
{
    const DTexHeader *t;
    if (!(z->hdr->flags & DZONE_F_WATER))
        return;
    /* geometry is generated per frame around the camera by the renderer */
    t = texcache_get(&w->tex, pw_hash_name("water/water00"));
    z->water_tex = (t == w->tex.fallback) ? NULL : t;
}

#define ZONE_FAIL(...)                                                        \
    do {                                                                      \
        if (err && errlen)                                                    \
            snprintf(err, errlen, __VA_ARGS__);                               \
        memset(z, 0, sizeof(*z));                                             \
        mem_reset(MEM_REGION_ZONE);                                           \
        return -1;                                                            \
    } while (0)

int zone_load(World *w, Zone *z, uint32_t zi, char *err, uint32_t errlen)
{
    const DZoneHeader *h;
    uint32_t i;
    int rc;

    memset(z, 0, sizeof(*z));
    if (!w->hdr || zi >= w->hdr->zone_count)
        ZONE_FAIL("zone %u out of range", (unsigned)zi);
    mem_reset(MEM_REGION_ZONE);
    z->index = zi;
    z->base = (const uint8_t *)pak_load(&w->zones, w->zone_list[zi].entry_hash, MEM_REGION_ZONE,
                                        64, &z->size);
    if (!z->base)
        ZONE_FAIL("zone %u missing or too large", (unsigned)zi);
    rc = dzone_validate(z->base, z->size);
    if (rc)
        ZONE_FAIL("zone %u corrupt (check %d)", (unsigned)zi, rc);
    h = z->hdr = (const DZoneHeader *)z->base;
    z->cells = (const DZoneCell *)(z->base + h->cells_ofs);
    z->mesh_hashes = (const uint32_t *)(z->base + h->meshes_ofs);
    z->textures = (const DMeshTexture *)(z->base + h->textures_ofs);
    z->terrain = (const DZoneTerrain *)(z->base + h->terrain_ofs);
    z->batches = (const DMeshBatch *)(z->base + h->batches_ofs);
    z->instances = (const DZoneInstance *)(z->base + h->instances_ofs);
    z->doors = (const DZoneDoor *)(z->base + h->doors_ofs);
    z->lights = (const DZoneLight *)(z->base + h->lights_ofs);
    z->strings = (const char *)z->base + h->strings_ofs;
    z->name = safe_str(z->base, z->size, h->strings_ofs, h->name_ofs);

    texcache_begin(&w->tex);
    z->tex = (const DTexHeader **)mem_try_alloc(MEM_REGION_ZONE,
                                                (h->texture_count + 1u) * sizeof(void *), 4);
    z->batch_model = (Mat4 *)mem_try_alloc(MEM_REGION_ZONE, (h->batch_count + 1u) * sizeof(Mat4), 64);
    z->meshes = (ZoneMesh *)mem_try_alloc(MEM_REGION_ZONE, (h->mesh_count + 1u) * sizeof(ZoneMesh), 16);
    z->inst_world = (Mat4 *)mem_try_alloc(MEM_REGION_ZONE,
                                          (h->instance_count + 1u) * sizeof(Mat4), 64);
    z->inst_scratch = (uint8_t *)mem_try_alloc(MEM_REGION_ZONE, h->instance_count + 1u, 4);
    z->inst_hidden = (uint8_t *)mem_try_alloc(MEM_REGION_ZONE, h->instance_count + 1u, 4);
    if (z->inst_hidden)
        memset(z->inst_hidden, 0, h->instance_count + 1u);
    z->terrain_scratch = (uint8_t *)mem_try_alloc(MEM_REGION_ZONE, h->terrain_count + 1u, 4);
    if (!z->tex || !z->batch_model || !z->meshes || !z->inst_world || !z->inst_scratch || !z->inst_hidden ||
        !z->terrain_scratch)
        ZONE_FAIL("zone %u: out of zone memory for tables", (unsigned)zi);
    for (i = 0; i < h->texture_count; ++i)
        z->tex[i] = texcache_get(&w->tex, z->textures[i].hash);
    for (i = 0; i < h->batch_count; ++i)
        dequant_matrix(&z->batch_model[i], &z->batches[i]);
    for (i = 0; i < h->mesh_count; ++i) {
        rc = load_mesh(w, &z->meshes[i], z->mesh_hashes[i]);
        if (rc == -2)
            ZONE_FAIL("zone %u: mesh %08x corrupt", (unsigned)zi, (unsigned)z->mesh_hashes[i]);
        if (rc != 0)
            z->missing_meshes++;
        else
            z->mesh_bytes += z->meshes[i].size;
    }
    for (i = 0; i < h->instance_count; ++i) {
        const DZoneInstance *in = &z->instances[i];
        zone_placement_matrix(&z->inst_world[i], in->pos, in->rot, in->scale);
    }
    z->actor_recs = (const DZoneActor *)(z->base + h->actors_ofs);
    z->anim_hashes = (const uint32_t *)(z->base + h->anims_ofs);
    z->anims = (Anim *)mem_try_alloc(MEM_REGION_ZONE, (h->anim_count + 1u) * sizeof(Anim), 16);
    if (!z->anims)
        ZONE_FAIL("zone %u: out of zone memory for animations", (unsigned)zi);
    memset(z->anims, 0, (h->anim_count + 1u) * sizeof(Anim));
    for (i = 0; i < h->anim_count; ++i) {
        uint32_t size = 0;
        const uint8_t *blob = w->anims.handle
            ? (const uint8_t *)pak_load(&w->anims, z->anim_hashes[i], MEM_REGION_ZONE, 16, &size)
            : NULL;
        if (!blob || anim_bind(&z->anims[i], blob, size) != 0)
            z->missing_anims++;
    }
    resolve_water(w, z);
#if PW_PSP
    /* everything above was written through the data cache; the GE reads RAM */
    sceKernelDcacheWritebackAll();
#endif
    return 0;
}

const ZoneMesh *zone_instance_mesh(const Zone *z, uint32_t inst)
{
    const DZoneInstance *in = &z->instances[inst];
    if (in->mesh == DZONE_NONE || (in->flags & (DZONE_IF_MERGED | DZONE_IF_ACTOR)))
        return NULL;
    return z->meshes[in->mesh].hdr ? &z->meshes[in->mesh] : NULL;
}

int zone_terrain_height(const Zone *z, float x, float y, float *out)
{
    const float spacing = 8192.0f / (DZONE_TERRAIN_GRID - 1);
    uint32_t t;
    for (t = 0; t < z->hdr->terrain_count; ++t) {
        const DZoneTerrain *p = &z->terrain[t];
        const float *hgt = (const float *)(z->base + p->heights_ofs);
        float fx = (x - p->origin[0]) / spacing, fy = (y - p->origin[1]) / spacing;
        int i, j;
        float u, v, ha, hb, hc, hd;
        if (fx < 0.0f || fy < 0.0f || fx > DZONE_TERRAIN_GRID - 1 || fy > DZONE_TERRAIN_GRID - 1)
            continue;
        i = (int)fx;
        j = (int)fy;
        if (i > DZONE_TERRAIN_GRID - 2)
            i = DZONE_TERRAIN_GRID - 2;
        if (j > DZONE_TERRAIN_GRID - 2)
            j = DZONE_TERRAIN_GRID - 2;
        u = fx - (float)i;
        v = fy - (float)j;
        ha = hgt[j * DZONE_TERRAIN_GRID + i];
        hb = hgt[j * DZONE_TERRAIN_GRID + i + 1];
        hc = hgt[(j + 1) * DZONE_TERRAIN_GRID + i + 1];
        hd = hgt[(j + 1) * DZONE_TERRAIN_GRID + i];
        /* quads split along a-c: triangles (a,b,c) and (a,c,d) */
        if (u >= v)
            *out = ha + u * (hb - ha) + v * (hc - hb);
        else
            *out = ha + v * (hd - ha) + u * (hc - hd);
        return 1;
    }
    return 0;
}

void zone_spawn_point(const Zone *z, Vec4 *pos, float *yaw)
{
    const DZoneHeader *h = z->hdr;
    float ground;
    *yaw = 0.0f;
    if (h->door_count > 0) {
        const DZoneInstance *d = &z->instances[z->doors[0].instance];
        if (h->kind == DZONE_KIND_EXTERIOR) {
            *pos = vec4(d->pos[0], d->pos[1] - 400.0f, d->pos[2], 1.0f);
            if (zone_terrain_height(z, pos->x, pos->y, &ground))
                pos->z = ground;
        } else {
            float cx = (h->bbox_min[0] + h->bbox_max[0]) * 0.5f;
            float cy = (h->bbox_min[1] + h->bbox_max[1]) * 0.5f;
            *pos = vec4(d->pos[0] + (cx - d->pos[0]) * 0.3f, d->pos[1] + (cy - d->pos[1]) * 0.3f,
                        d->pos[2], 1.0f);
            *yaw = atan2f(cx - d->pos[0], cy - d->pos[1]);
        }
    } else if (h->kind == DZONE_KIND_EXTERIOR) {
        *pos = vec4(((float)h->cell_x0 + (float)(h->cell_x1 + 1)) * 0.5f * 8192.0f,
                    ((float)h->cell_y0 + (float)(h->cell_y1 + 1)) * 0.5f * 8192.0f, 0.0f, 1.0f);
        if (zone_terrain_height(z, pos->x, pos->y, &ground))
            pos->z = ground;
    } else {
        *pos = vec4((h->bbox_min[0] + h->bbox_max[0]) * 0.5f,
                    (h->bbox_min[1] + h->bbox_max[1]) * 0.5f, h->bbox_min[2], 1.0f);
    }
    if (h->kind == DZONE_KIND_EXTERIOR && pos->z < h->water_height)
        pos->z = h->water_height;
    pos->z += 128.0f; /* eye height */
}
