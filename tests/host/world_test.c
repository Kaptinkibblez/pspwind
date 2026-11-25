/*
 * world_test: loads every zone of a cooked world on the host and checks the
 * loader end to end (pak directory, world index, zone/mesh validation,
 * texture cache, placement matrices, terrain sampling), then fuzzes the
 * validators with corrupted copies under ASan.
 *
 *   world_test <data dir> [--expect-testworld]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "zone.h"

static int g_fail;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            printf("  FAIL: ");                                               \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static World g_world;
static Zone g_zone;

static void check_zone(World *w, Zone *z, uint32_t zi)
{
    const DZoneHeader *h = z->hdr;
    uint32_t i, placement_ok = 1, bounds_checked = 0;
    for (i = 0; i < h->instance_count; ++i) {
        const DZoneInstance *in = &z->instances[i];
        const ZoneMesh *m;
        Vec4 c, wc;
        if (in->mesh == DZONE_NONE || !z->meshes[in->mesh].hdr)
            continue;
        if ((in->flags & DZONE_IF_ACTOR) && in->kind == DZONE_INST_NPC)
            continue; /* NPC bounds are a standing figure, not the bare skeleton's */
        m = &z->meshes[in->mesh];
        c = vec4(m->hdr->bound_center[0], m->hdr->bound_center[1], m->hdr->bound_center[2], 1.0f);
        mat4_transform_point(&wc, &z->inst_world[i], &c);
        /* the cooker computed the same bound with its own placement code */
        if (fabsf(wc.x - in->bound[0]) > 0.05f || fabsf(wc.y - in->bound[1]) > 0.05f ||
            fabsf(wc.z - in->bound[2]) > 0.05f)
            placement_ok = 0;
        bounds_checked++;
    }
    CHECK(placement_ok, "zone %u: C placement matrix disagrees with cooker bounds", (unsigned)zi);
    for (i = 0; i < h->texture_count; ++i)
        CHECK(z->tex[i] != NULL, "zone %u texture %u unresolved", (unsigned)zi, (unsigned)i);
    if (h->terrain_count > 0) {
        const DZoneTerrain *t = &z->terrain[0];
        const float *hg = (const float *)(z->base + t->heights_ofs);
        float hh = 0.0f;
        int ok = zone_terrain_height(z, t->origin[0] + 512.0f, t->origin[1] + 1024.0f, &hh);
        CHECK(ok && fabsf(hh - hg[2 * DZONE_TERRAIN_GRID + 1]) < 0.01f,
              "zone %u terrain sample at a grid vertex", (unsigned)zi);
        ok = zone_terrain_height(z, t->origin[0] + 256.0f, t->origin[1] + 128.0f, &hh);
        CHECK(ok && hh >= fminf(fminf(hg[0], hg[1]), hg[DZONE_TERRAIN_GRID + 1]) - 0.01f &&
                  hh <= fmaxf(fmaxf(hg[0], hg[1]), hg[DZONE_TERRAIN_GRID + 1]) + 0.01f,
              "zone %u terrain sample inside triangle", (unsigned)zi);
    }
    printf("  zone %2u %-28.28s cells %3u batches %4u inst %4u (%u checked) meshes %3u "
           "missing %u water %s | zone %6uB meshes %6uB\n",
           (unsigned)zi, z->name, (unsigned)h->cell_count, (unsigned)h->batch_count,
           (unsigned)h->instance_count, (unsigned)bounds_checked, (unsigned)h->mesh_count,
           (unsigned)z->missing_meshes, (h->flags & DZONE_F_WATER) ? (z->water_tex ? "tex" : "flat") : "-", (unsigned)z->size,
           (unsigned)z->mesh_bytes);
    (void)w;
}

static uint32_t rng = 12345u;
static uint32_t xr(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void fuzz(const uint8_t *blob, uint32_t size, int (*validate)(const uint8_t *, uint32_t),
                 const char *what)
{
    uint8_t *copy = (uint8_t *)malloc(size);
    uint32_t iter, rejected = 0;
    for (iter = 0; iter < 2000; ++iter) {
        uint32_t k, flips = 1 + xr() % 8;
        memcpy(copy, blob, size);
        for (k = 0; k < flips; ++k) {
            /* bias corruption towards headers and tables */
            uint32_t pos = (xr() & 1) ? xr() % (size < 512 ? size : 512) : xr() % size;
            copy[pos] = (uint8_t)xr();
        }
        rejected += validate(copy, size) != 0;
    }
    CHECK(validate(blob, size) == 0, "%s: pristine blob rejected", what);
    printf("  fuzz %-6s: 2000 corrupted copies, %u rejected, no crash\n", what, (unsigned)rejected);
    free(copy);
}

int main(int argc, char **argv)
{
    char err[200];
    uint32_t zi, n;
    int expect_tw = argc > 2 && strcmp(argv[2], "--expect-testworld") == 0;
    if (argc < 2) {
        fprintf(stderr, "usage: world_test <data dir> [--expect-testworld]\n");
        return 2;
    }
    mem_init();
    if (world_open(&g_world, argv[1], err, sizeof(err)) != 0) {
        printf("world_open failed: %s\n", err);
        return 1;
    }
    n = world_zone_count(&g_world);
    printf("world: %u zones, %u exterior cells, %u interiors\n", (unsigned)n,
           (unsigned)g_world.hdr->ext_count, (unsigned)g_world.hdr->int_count);
    for (zi = 0; zi < n; ++zi) {
        if (zone_load(&g_world, &g_zone, zi, err, sizeof(err)) != 0) {
            CHECK(0, "zone %u failed to load: %s", (unsigned)zi, err);
            continue;
        }
        check_zone(&g_world, &g_zone, zi);
    }
    if (expect_tw) {
        int hamlet = world_find_exterior(&g_world, 0, 0);
        int cellar = world_find_interior(&g_world, pw_hash_name("test hamlet, cellar"));
        CHECK(hamlet >= 0 && cellar >= 0, "test world lookups");
        CHECK(world_find_exterior(&g_world, 40, 40) < 0, "missing exterior cell");
        if (hamlet >= 0 && cellar >= 0 &&
            zone_load(&g_world, &g_zone, (uint32_t)hamlet, err, sizeof(err)) == 0) {
            Vec4 pos;
            float yaw;
            CHECK(g_zone.hdr->door_count == 1 && g_zone.doors[0].dest_zone == (uint32_t)cellar,
                  "hamlet door leads to the cellar");
            CHECK(g_zone.missing_meshes == 0, "all hamlet meshes present");
            CHECK(g_world.tex.misses == 0 && g_world.tex.corrupt == 0,
                  "texture cache: %u misses, %u corrupt", (unsigned)g_world.tex.misses,
                  (unsigned)g_world.tex.corrupt);
            CHECK(g_zone.hdr->light_count == 1, "hamlet lantern light");
            zone_spawn_point(&g_zone, &pos, &yaw);
            CHECK(pos.z > g_zone.hdr->water_height, "spawn above water");
            fuzz(g_zone.base, g_zone.size, dzone_validate, "zone");
            if (g_zone.hdr->mesh_count && g_zone.meshes[0].hdr)
                fuzz(g_zone.meshes[0].base, g_zone.meshes[0].size, dmesh_validate, "mesh");
        }
        if (cellar >= 0 && zone_load(&g_world, &g_zone, (uint32_t)cellar, err, sizeof(err)) == 0)
            CHECK(g_zone.doors[0].dest_zone == (uint32_t)hamlet, "cellar door leads back");
    }
    printf("texture cache: %u loaded, %u hits, %u misses, %u evictions, %uK used\n",
           (unsigned)g_world.tex.loads, (unsigned)g_world.tex.hits, (unsigned)g_world.tex.misses,
           (unsigned)g_world.tex.evictions, (unsigned)(texcache_bytes_used(&g_world.tex) >> 10));
    world_close(&g_world);
    printf(g_fail ? "WORLD TESTS FAILED (%d)\n" : "WORLD TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
