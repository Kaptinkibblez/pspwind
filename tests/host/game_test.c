/*
 * game_test: drives the real gameplay layer (game.c + phys.c + actor.c +
 * anim.c on top of the zone loader) with scripted input on the cooked test
 * world, the way a player would on the PSP.
 *
 *   game_test <test world data dir>
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "mem.h"

static int g_fail, g_checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            printf("  FAIL: ");                                               \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static World g_world;
static Game g_game;

static void step(Game *g, uint32_t held, uint32_t pressed, float ax, float ay, float seconds)
{
    const float dt = 1.0f / 30.0f;
    int frames = (int)(seconds / dt + 0.5f), i;
    for (i = 0; i < frames; ++i) {
        InputState in;
        char err[160];
        in.held = held;
        in.pressed = i == 0 ? pressed : 0;
        in.ax = ax;
        in.ay = ay;
        game_update(g, &in, dt);
        if (g->request.pending) {
            ZoneRequest r = g->request;
            if (game_enter_zone(g, r.zone, r.has_pos ? r.pos : NULL, r.has_pos ? r.rot : NULL,
                                err, sizeof(err)) != 0)
                printf("  zone load failed: %s\n", err);
            return;
        }
    }
}

static float terrain_at(const Game *g)
{
    float h = -1e9f;
    zone_terrain_height(&g->zone, g->body.pos.x, g->body.pos.y, &h);
    return h;
}

static void test_cylinder_triangle(void)
{
    /* vertical wall in the plane x = 50 facing -x */
    float wall[3][3] = { { 50, -100, 0 }, { 50, 100, 0 }, { 50, 0, 300 } };
    const float wall_n[3] = { -1, 0, 0 };
    float step_tri[3][3] = { { -100, -100, 20 }, { 100, -100, 20 }, { 0, 100, 20 } };
    float ledge[3][3] = { { -100, -100, 60 }, { 100, -100, 60 }, { 0, 100, 60 } };
    const float up[3] = { 0, 0, 1 };
    float px = 0, py = 0;
    CHECK(phys_cylinder_triangle(40, 0, 28, 34, 128, wall, wall_n, &px, &py) &&
              fabsf(px + 18.0f) < 1e-3f && fabsf(py) < 1e-3f,
          "wall pushes the cylinder back by the overlap (%.2f, %.2f)", px, py);
    CHECK(!phys_cylinder_triangle(10, 0, 28, 34, 128, wall, wall_n, &px, &py),
          "wall out of reach does not push");
    CHECK(!phys_cylinder_triangle(0, 0, 28, 34, 128, step_tri, up, &px, &py),
          "a 20-unit step stays below the slab (climbable)");
    CHECK(phys_cylinder_triangle(0, -40, 28, 34, 128, ledge, up, &px, &py),
          "a 60-unit ledge enters the slab (blocks)");
    CHECK(!phys_cylinder_triangle(40, 0, 28, 310, 400, wall, wall_n, &px, &py),
          "wall entirely below the slab is ignored");
}

int main(int argc, char **argv)
{
    char err[200];
    Game *g = &g_game;
    int hamlet, cellar;
    float x0, y0, z;
    if (argc < 2) {
        fprintf(stderr, "usage: game_test <data dir>\n");
        return 2;
    }
    mem_init();
    test_cylinder_triangle();
    if (world_open(&g_world, argv[1], err, sizeof(err)) != 0) {
        printf("world_open failed: %s\n", err);
        return 1;
    }
    CHECK(g_world.anims.handle != 0, "anims.dpak mounted");
    game_init(g, &g_world);
    hamlet = world_find_exterior(&g_world, 0, 0);
    cellar = world_find_interior(&g_world, pw_hash_name("test hamlet, cellar"));
    CHECK(hamlet >= 0 && cellar >= 0, "test world zones present");
    if (hamlet < 0 || cellar < 0 || game_enter_zone(g, (uint32_t)hamlet, NULL, NULL, err, sizeof(err))) {
        printf("cannot enter the hamlet: %s\n", err);
        return 1;
    }

    /* ---- standing on terrain */
    step(g, 0, 0, 0, 0, 1.0f);
    CHECK(g->body.on_ground && fabsf(g->body.pos.z - terrain_at(g)) < 2.0f,
          "player rests on the terrain (z %.1f, ground %.1f)", g->body.pos.z, terrain_at(g));
    CHECK(g->phys.tri_count > 0, "static collision built (%u tris)", (unsigned)g->phys.tri_count);
    CHECK(g->actors.count == 3, "elder, weaver and guard in the hamlet (%u)", (unsigned)g->actors.count);

    /* ---- walking north into the door: the door blocks, R takes us inside */
    x0 = g->body.pos.x;
    y0 = g->body.pos.y;
    step(g, 0, 0, 0.0f, 1.0f, 4.0f);
    {
        const DZoneDoor *d = &g->zone.doors[0];
        const DZoneInstance *di = &g->zone.instances[d->instance];
        CHECK(g->body.pos.y > y0 + 200.0f, "walked north (%.0f -> %.0f)", y0, g->body.pos.y);
        CHECK(g->body.pos.y < di->pos[1] - 20.0f,
              "stopped in front of the door (y %.1f, door %.1f)", g->body.pos.y, di->pos[1]);
        CHECK(fabsf(g->body.pos.x - x0) < 40.0f, "no sideways drift (%.1f)", g->body.pos.x - x0);
        CHECK(g->target == d->instance, "the door is under the crosshair (%u)", (unsigned)g->target);
        CHECK(game_target_label(g) && strstr(game_target_label(g), "Cellar") != NULL,
              "prompt names the destination (%s)", game_target_label(g) ? game_target_label(g) : "-");
    }
    step(g, GB_R, GB_R, 0, 0, 0.1f);
    CHECK(g->zone.index == (uint32_t)cellar, "activating the door loads the cellar");

    /* ---- cellar: floor, walls, jumping */
    step(g, 0, 0, 0, 0, 0.5f);
    CHECK(g->body.on_ground && fabsf(g->body.pos.z) < 1.0f, "standing on the cellar floor (z %.2f)",
          g->body.pos.z);
    step(g, 0, 0, 0, 1.0f, 6.0f); /* walk north into the wall at y = +600 */
    CHECK(g->body.pos.y > 400.0f && g->body.pos.y < 600.0f - 27.0f,
          "cellar wall stops the player (y %.1f)", g->body.pos.y);
    {
        float top = 0.0f;
        int i;
        InputState in;
        memset(&in, 0, sizeof(in));
        in.held = in.pressed = GB_L;
        game_update(g, &in, 1.0f / 30.0f);
        in.held = in.pressed = 0;
        for (i = 0; i < 60; ++i) {
            game_update(g, &in, 1.0f / 30.0f);
            if (g->body.pos.z > top)
                top = g->body.pos.z;
        }
        CHECK(top > 80.0f && top < 120.0f, "jump apex ~103 units (%.1f)", top);
        CHECK(g->body.on_ground && fabsf(g->body.pos.z) < 1.0f, "landed again (z %.2f)", g->body.pos.z);
    }
    CHECK(g->actors.count == 1 && (g->actors.list[0].rec->flags & DZONE_ACTOR_F_CREATURE),
          "cellar critter is a creature actor");
    if (g->actors.count == 1) {
        Actor *a = &g->actors.list[0];
        uint32_t tail = 0xFFFFFFFFu, k;
        Vec4 before, after;
        for (k = 0; k < a->skel->hdr->node_count; ++k)
            if (a->skel->nodes[k].name_hash == pw_hash_name("Bip01 Tail"))
                tail = k;
        CHECK(tail != 0xFFFFFFFFu && a->bone_track[tail] >= 0, "creature tail bound to its track");
        if (tail != 0xFFFFFFFFu) {
            a->time = 0.0f;
            actor_pose(a);
            before = a->pose[tail].y;
            a->time = 0.4f;
            actor_pose(a);
            after = a->pose[tail].y;
            CHECK(fabsf(before.x - after.x) > 0.3f, "tail swings over time (%.2f -> %.2f)", before.x,
                  after.x);
        }
    }

    /* ---- back outside through the cellar door */
    {
        const DZoneInstance *di = &g->zone.instances[g->zone.doors[0].instance];
        g->body.pos.x = di->pos[0];
        g->body.pos.y = di->pos[1] + 120.0f;
        g->body.pos.z = 0.0f;
        g->yaw = 3.14159265f; /* face south, towards the door */
        g->pitch = -0.1f;
        step(g, 0, 0, 0, 0, 0.1f);
        CHECK(g->target == g->zone.doors[0].instance, "cellar door targeted");
        step(g, GB_R, GB_R, 0, 0, 0.1f);
        CHECK(g->zone.index == (uint32_t)hamlet, "back in the hamlet");
    }

    /* ---- villagers: bind-pose bones, walking animation, facing the player */
    if (g->actors.count >= 1) {
        Actor *a = &g->actors.list[0];
        uint32_t head = 0xFFFFFFFFu, hand = 0xFFFFFFFFu, k;
        float yaw0;
        for (k = 0; k < a->skel->hdr->node_count; ++k) {
            if (a->skel->nodes[k].name_hash == pw_hash_name("Bip01 Head"))
                head = k;
            if (a->skel->nodes[k].name_hash == pw_hash_name("Bip01 R Hand"))
                hand = k;
        }
        CHECK(head != 0xFFFFFFFFu && hand != 0xFFFFFFFFu, "skeleton bones found by name");
        if (head != 0xFFFFFFFFu && hand != 0xFFFFFFFFu) {
            actor_play(a, pw_hash_name("idle"), 1);
            actor_pose(a);
            CHECK(fabsf(a->pose[head].w.z - 136.0f) < 0.5f && fabsf(a->pose[head].w.y) < 0.5f,
                  "idle frame 0 head at 136 (%.2f, %.2f)", a->pose[head].w.y, a->pose[head].w.z);
            actor_play(a, pw_hash_name("walkforward"), 1);
            a->time = 0.25f;
            actor_pose(a);
            CHECK(a->group_rec && fabsf(a->group_rec->speed - 120.0f) < 1.0f,
                  "walk group root speed 120 u/s");
            CHECK(fabsf(a->pose[hand].w.y) > 15.0f, "arm swings forward/back in the walk (%.1f)",
                  a->pose[hand].w.y);
            CHECK(fabsf(a->pose[head].w.y) < 0.5f, "root motion stripped (head y %.2f)",
                  a->pose[head].w.y);
            {
                Mat4 m;
                int mirrored, any = 0, any_mirror = 0;
                uint32_t p;
                for (p = 0; p < a->rec->part_count; ++p) {
                    if (!a->part_mesh[p])
                        continue;
                    for (k = 0; k < a->part_mesh[p]->hdr->batch_count; ++k) {
                        any += actor_batch_matrix(a, (int)p, k, &m, &mirrored);
                        any_mirror |= mirrored;
                    }
                }
                CHECK(any >= 13, "every body part binds to a bone (%d batches)", any);
                CHECK(any_mirror, "left-side parts are mirrored");
            }
        }
        /* stand 150 units in front of (south of) the villager, facing it */
        g->body.pos.x = a->pos.x + 150.0f;
        g->body.pos.y = a->pos.y - 150.0f;
        yaw0 = a->yaw;
        step(g, 0, 0, 0, 0, 2.0f);
        {
            float want = atan2f(g->body.pos.x - a->pos.x, g->body.pos.y - a->pos.y);
            CHECK(fabsf(pw_wrap_angle(a->yaw - want)) < 0.05f && fabsf(pw_wrap_angle(a->yaw - yaw0)) > 0.1f,
                  "villager turned to face the player (%.2f -> %.2f, want %.2f)", yaw0, a->yaw, want);
        }
        /* NPCs are solid */
        g->body.pos.x = a->pos.x;
        g->body.pos.y = a->pos.y - 100.0f;
        g->yaw = 0.0f;
        step(g, 0, 0, 0, 1.0f, 1.5f);
        CHECK(a->pos.y - g->body.pos.y >= 55.0f, "the villager blocks the player (gap %.1f)",
              a->pos.y - g->body.pos.y);
        game_debug_cycle_animation(g, 1);
        CHECK(strstr(g->message, "plays") != NULL, "debug cycles the nearest actor's animation (%s)",
              g->message);
    }

    /* ---- walking off the zone edge requests the neighbouring zone */
    g->body.pos.x = 40.0f;
    g->body.pos.y = 4000.0f;
    g->body.pos.z = terrain_at(g);
    g->yaw = -1.5708f; /* west */
    step(g, GB_UP, GB_UP, 0, 1.0f, 2.0f); /* run */
    CHECK(g->zone.index != (uint32_t)hamlet && g->body.pos.x < 0.0f,
          "crossing x = 0 loads the western zone (zone %u, x %.0f)", (unsigned)g->zone.index,
          g->body.pos.x);
    CHECK(strstr(world_zone_name(&g_world, g->zone.index), "Coast") != NULL, "western zone is the coast");

    /* ---- swimming in the sea to the south-west */
    {
        int zi = world_find_exterior(&g_world, -3, -3), found = 0, i, j;
        if (zi >= 0 && game_enter_zone(g, (uint32_t)zi, NULL, NULL, err, sizeof(err)) == 0) {
            for (j = 0; j < 40 && !found; ++j) {
                for (i = 0; i < 40 && !found; ++i) {
                    float h, x = -3 * 8192.0f + (float)i * 600.0f, y = -3 * 8192.0f + (float)j * 600.0f;
                    if (zone_terrain_height(&g->zone, x, y, &h) && h < -300.0f) {
                        g->body.pos = vec4(x, y, -150.0f, 1.0f);
                        found = 1;
                    }
                }
            }
            CHECK(found, "found deep water");
            g->body.on_ground = 0;
            step(g, 0, 0, 0, 0, 3.0f);
            z = g->body.pos.z;
            CHECK(g->body.swimming, "player swims in deep water");
            CHECK(z > g->zone.hdr->water_height - 128.0f && z < g->zone.hdr->water_height,
                  "floats near the surface (z %.1f)", z);
        }
    }

    printf("game tests: %d/%d checks passed\n", g_checks - g_fail, g_checks);
    world_close(&g_world);
    printf(g_fail ? "GAME TESTS FAILED (%d)\n" : "GAME TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
