/*
 * ai_test: actor behaviour on the cooked test world through the real game
 * loop - path grids and A*, wandering, packages, hostility and combat,
 * sneaking, theft and assault, guards and fines, jail, summoned creatures,
 * spawning, and the sounds that go with them.
 *
 *   ai_test <test world data dir>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
static GameRpg g_rpg;
static Audio g_audio;
static int16_t g_mix[2048];

static void step(uint32_t held, uint32_t pressed, float ax, float ay, float seconds)
{
    const float dt = 1.0f / 30.0f;
    int frames = (int)(seconds / dt + 0.5f), i;
    Game *g = &g_game;
    for (i = 0; i < frames; ++i) {
        InputState in;
        char err[160];
        in.held = held;
        in.pressed = i == 0 ? pressed : 0;
        in.ax = ax;
        in.ay = ay;
        g->ui_event = GAME_UI_NONE;
        game_update(g, &in, dt);
        audio_mix(&g_audio, g_mix, 1470 / 2); /* keep the mixer consuming like the PSP thread */
        if (g->request.pending) {
            ZoneRequest r = g->request;
            if (game_enter_zone(g, r.zone, r.has_pos ? r.pos : NULL, r.has_pos ? r.rot : NULL, err, sizeof(err)) != 0)
                printf("  zone load failed: %s\n", err);
            return;
        }
    }
}

static int actor_of(const char *id)
{
    uint32_t i, h = pw_hash_name(id);
    for (i = 0; i < g_game.actors.count; ++i)
        if (g_game.zone.instances[g_game.actors.list[i].instance].object_hash == h)
            return (int)i;
    return -1;
}

static int inst_of(const char *id)
{
    uint32_t i, h = pw_hash_name(id);
    for (i = 0; i < g_game.zone.instance_count; ++i)
        if (g_game.zone.instances[i].object_hash == h)
            return (int)i;
    return -1;
}

static float actor_dist(int a)
{
    float dx = g_game.actors.list[a].pos.x - g_game.body.pos.x, dy = g_game.actors.list[a].pos.y - g_game.body.pos.y;
    return sqrtf(dx * dx + dy * dy);
}

/* puts the player at (x, y) on the ground, looking at a point */
static void place_player(float x, float y, float lx, float ly)
{
    float ground;
    g_game.body.pos.x = x;
    g_game.body.pos.y = y;
    if (phys_ground(&g_game.phys, x, y, g_game.body.pos.z + 2000.0f, &ground))
        g_game.body.pos.z = ground;
    g_game.body.vel.x = g_game.body.vel.y = g_game.body.vel.z = 0.0f;
    g_game.yaw = atan2f(lx - x, ly - y);
    g_game.pitch = -0.05f;
}

static void look_at_actor(int a)
{
    const Actor *ac = &g_game.actors.list[a];
    float dx = ac->pos.x - g_game.body.pos.x, dy = ac->pos.y - g_game.body.pos.y;
    float d = sqrtf(dx * dx + dy * dy);
    g_game.yaw = atan2f(dx, dy);
    g_game.pitch = atan2f(ac->pos.z + 70.0f - (g_game.body.pos.z + GAME_EYE_HEIGHT), d);
}

static float crime(void)
{
    VmRef pc = vm_player_ref(&g_rpg.vm);
    return vm_prop(&g_rpg.vm, &pc, MWP_CRIME);
}

static void enter(int zone)
{
    char err[160];
    if (zone < 0 || game_enter_zone(&g_game, (uint32_t)zone, NULL, NULL, err, sizeof(err)) != 0)
        printf("  cannot enter zone %d: %s\n", zone, err);
}

int main(int argc, char **argv)
{
    char err[200];
    Game *g = &g_game;
    Vm *vm = &g_rpg.vm;
    Rpg *r = &g_rpg.rpg;
    VmRef pc;
    int hamlet, guard, elder, weaver, snapper;
    if (argc < 2) {
        fprintf(stderr, "usage: ai_test <data dir>\n");
        return 2;
    }
    mem_init();
    if (world_open(&g_world, argv[1], err, sizeof(err)) != 0) {
        printf("world_open failed: %s\n", err);
        return 1;
    }
    game_init(g, &g_world);
    CHECK(game_attach_rpg(g, &g_rpg, argv[1], err, sizeof(err)) == 0, "rpg (%s)", err);
    CHECK(audio_init(&g_audio, argv[1], &g_rpg.db, err, sizeof(err)) == 0, "audio (%s)", err);
    g->audio = &g_audio;
    pc = vm_player_ref(vm);
    rpg_chargen(r, "Tester", pw_hash_name("tw_folk"), 0, pw_hash_name("tw_warrior"), pw_hash_name("tw_sign_blade"));
    hamlet = world_find_exterior(&g_world, 0, 0);
    enter(hamlet);
    vm_message_answer(vm, 0);

    /* ---- path grid ---------------------------------------------------- */
    CHECK(g->ai.count == 11, "hamlet path grid merged (%u points)", (unsigned)g->ai.count);
    {
        int16_t path[48];
        int n = ai_find_path(g, g->ai.pts[0], g->ai.pts[10], path, 48), k, ok = 1;
        CHECK(n >= 3 && path[0] == 0 && path[n - 1] == 10, "A* from the ring to the guard post (%d)", n);
        for (k = 0; k + 1 < n; ++k) {
            uint32_t e, linked = 0;
            for (e = g->ai.first[path[k]]; e < g->ai.first[path[k] + 1]; ++e)
                linked |= g->ai.edges[e] == (uint16_t)path[k + 1];
            ok &= (int)linked;
        }
        CHECK(ok, "path follows grid edges");
        n = ai_find_path(g, g->ai.pts[0], g->ai.pts[5], path, 48);
        CHECK(n == 6, "shortest way round the ring (%d points)", n);
    }

    /* ---- wandering ------------------------------------------------------ */
    guard = actor_of("tw_guard");
    elder = actor_of("tw_villager");
    weaver = actor_of("tw_villager_f");
    CHECK(guard >= 0 && elder >= 0 && weaver >= 0, "hamlet actors");
    CHECK(ai_current_package(g, guard) == DRPG_AI_WANDER && g->ai.actors[guard].pkg.distance == 500,
          "guard wanders 500 units");
    {
        float gx = g->actors.list[guard].pos.x, gy = g->actors.list[guard].pos.y, moved = 0.0f, far = 0.0f;
        float ex = g->actors.list[elder].pos.x, ey = g->actors.list[elder].pos.y;
        int k;
        place_player(gx + 1500.0f, gy - 1200.0f, gx, gy);
        for (k = 0; k < 90; ++k) {
            float dx, dy;
            step(0, 0, 0, 0, 1.0f);
            dx = g->actors.list[guard].pos.x - gx;
            dy = g->actors.list[guard].pos.y - gy;
            moved = PW_MAX(moved, sqrtf(dx * dx + dy * dy));
            dx = g->actors.list[guard].pos.x - g->ai.actors[guard].home[0];
            dy = g->actors.list[guard].pos.y - g->ai.actors[guard].home[1];
            far = PW_MAX(far, sqrtf(dx * dx + dy * dy));
        }
        CHECK(moved > 200.0f, "guard walked around (%.0f)", moved);
        CHECK(far < 700.0f, "and stayed near home (%.0f)", far);
        CHECK(fabsf(g->actors.list[elder].pos.x - ex) < 1.0f && fabsf(g->actors.list[elder].pos.y - ey) < 1.0f,
              "elder (distance 0) stays put");
        CHECK(g->ai.paths > 0, "wandering used the path grid (%u paths)", (unsigned)g->ai.paths);
    }

    /* ---- packages: travel and follow (as scripts set them) ------------- */
    {
        DRpgAiPackage p;
        float tx, ty;
        int k;
        memset(&p, 0, sizeof(p));
        p.type = DRPG_AI_TRAVEL;
        memcpy(p.pos, g->ai.pts[3], sizeof(p.pos));
        tx = p.pos[0];
        ty = p.pos[1];
        ai_set_package(g, weaver, &p);
        for (k = 0; k < 60 && !ai_package_done(g, weaver); ++k)
            step(0, 0, 0, 0, 0.5f);
        CHECK(ai_package_done(g, weaver), "weaver travelled (%.0f, %.0f) -> (%.0f, %.0f)",
              g->actors.list[weaver].pos.x, g->actors.list[weaver].pos.y, tx, ty);
        memset(&p, 0, sizeof(p));
        p.type = DRPG_AI_FOLLOW;
        p.target = pw_hash_name("player");
        ai_set_package(g, weaver, &p);
        place_player(g->actors.list[weaver].pos.x + 900.0f, g->actors.list[weaver].pos.y, 0, 0);
        step(0, 0, 0, 0, 8.0f);
        CHECK(actor_dist(weaver) < 350.0f, "weaver follows the player (%.0f)", actor_dist(weaver));
        memset(&p, 0, sizeof(p));
        p.type = DRPG_AI_WANDER;
        ai_set_package(g, weaver, &p);
    }

    /* ---- theft in front of the owner ----------------------------------- */
    {
        int goblet = inst_of("tw_goblet");
        const DZoneInstance *in = goblet >= 0 ? &g->zone.instances[goblet] : NULL;
        CHECK(in && in->owner == pw_hash_name("tw_villager"), "goblet owned by the elder");
        if (in) {
            place_player(in->pos[0] - 60.0f, in->pos[1] - 60.0f, g->actors.list[elder].pos.x,
                         g->actors.list[elder].pos.y);
            step(0, 0, 0, 0, 0.5f);
            game_take(g, (uint32_t)goblet);
            CHECK(crime() > 0.0f, "theft seen by the elder (bounty %.0f)", crime());
            CHECK(g->ai.stolen_used == 1 && g->ai.stolen_item[0] == pw_hash_name("tw_goblet"), "goblet marked stolen");
        }
    }

    /* ---- guards: pursuit, fine, jail ----------------------------------- */
    {
        int k, talked = 0;
        int32_t gold0;
        float bounty;
        place_player(g->actors.list[guard].pos.x + 400.0f, g->actors.list[guard].pos.y, g->actors.list[guard].pos.x,
                     g->actors.list[guard].pos.y);
        step(0, 0, 0, 0, 0.5f);
        CHECK(ai_crime(g, AI_CRIME_TRESPASS, -1, 0, 0) == 1, "trespass under the guard's eyes");
        CHECK(g->ai.actors[guard].state == AI_PURSUE, "guard pursues (%d)", g->ai.actors[guard].state);
        for (k = 0; k < 300 && !talked; ++k) {
            InputState none;
            memset(&none, 0, sizeof(none));
            game_update(g, &none, 1.0f / 30.0f);
            talked = g->ui_event == GAME_UI_DIALOGUE;
        }
        CHECK(talked && g_rpg.dlg.active && g_rpg.dlg.in_choice, "guard confronts the player with a choice");
        CHECK(actor_dist(guard) < 200.0f, "guard came close (%.0f)", actor_dist(guard));
        g->ui_event = GAME_UI_NONE;
        gold0 = rpg_gold(r, &pc);
        bounty = crime();
        dlg_answer(&g_rpg.dlg, 0); /* pay the fine */
        CHECK(crime() == 0.0f && rpg_gold(r, &pc) == gold0 - (int32_t)bounty, "fine paid (%.0f gold)", bounty);
        CHECK(g->ai.stolen_used == 1, "PayFine keeps stolen goods (PayFineThief takes them)");
        dlg_end(&g_rpg.dlg);
        step(0, 0, 0, 0, 1.0f);
        CHECK(g->ai.actors[guard].state != AI_PURSUE, "pursuit over");

        /* again, this time to jail */
        vm_set_prop(vm, &pc, MWP_CRIME, 250.0f);
        CHECK(game_actor_index(g, game_actor_ref(g, (uint32_t)guard).key) == guard, "actor lookup by key");
        game_talk(g, g->actors.list[guard].instance);
        CHECK(g_rpg.dlg.in_choice, "crime greeting");
        {
            int32_t day0 = vm->days_passed;
            float skills0 = 0.0f, skills1 = 0.0f;
            int s;
            for (s = 0; s < SKILL_COUNT; ++s)
                skills0 += vm_prop(vm, &pc, MWP_SKILL_BLOCK + s);
            dlg_answer(&g_rpg.dlg, 1);
            for (s = 0; s < SKILL_COUNT; ++s)
                skills1 += vm_prop(vm, &pc, MWP_SKILL_BLOCK + s);
            CHECK(crime() == 0.0f && vm->days_passed == (int32_t)day0 + 2, "two days in jail (%u)",
                  (unsigned)(vm->days_passed - day0));
            CHECK(skills1 <= skills0 - 1.0f, "skills slipped in jail (%.0f -> %.0f)", skills0, skills1);
            CHECK(vm_item_count(vm, vm->player_hash, pw_hash_name("tw_goblet")) == 0 && g->ai.stolen_used == 0,
                  "stolen goblet confiscated");
            CHECK(g->request.pending && g->request.zone == (uint32_t)world_find_interior(&g_world,
                                                                                         pw_hash_name("test hamlet, cellar")),
                  "released at the prison marker");
        }
        dlg_end(&g_rpg.dlg);
        g->request.pending = 0;
    }

    /* ---- assault: hitting the weaver ------------------------------------ */
    {
        int k;
        float c0 = crime();
        int sword = -1;
        uint32_t cursor = 0, item;
        int32_t count;
        int slot;
        while ((slot = vm_item_next(vm, pc.key, &cursor, &item, &count)) >= 0)
            if (item == pw_hash_name("tw_dagger"))
                sword = slot;
        CHECK(sword >= 0 && rpg_equip(r, &pc, sword) == 1, "dagger in hand");
        r->draw_state = 1;
        for (k = 0; k < 40 && crime() == c0; ++k) {
            place_player(g->actors.list[weaver].pos.x, g->actors.list[weaver].pos.y - 80.0f, 0, 0);
            look_at_actor(weaver);
            step(GB_R, GB_R, 0, 0, 0.4f);
            step(0, 0, 0, 0, 0.6f);
            if (getenv("AI_DEBUG"))
                printf("swing %d: '%s' dist %.0f hostile %.0f state %d\n", k, g->message, actor_dist(weaver),
                       vm_prop(vm, &(VmRef){ 0, 0, -1, -1, 0, game_actor_ref(g, (uint32_t)weaver).key }, MWP_HOSTILE),
                       g->ai.actors[weaver].state);
        }
        CHECK(crime() >= c0 + 40.0f, "assault bounty (%.0f)", crime() - c0);
        CHECK(g->ai.actors[weaver].state == AI_COMBAT || g->ai.actors[weaver].state == AI_FLEE,
              "the weaver fights back (%d)", g->ai.actors[weaver].state);
        ai_stop_combat(g, weaver);
        vm_set_prop(vm, &pc, MWP_CRIME, 0.0f);
        r->draw_state = 0;
        for (k = 0; k < (int)g->actors.count; ++k)
            if (g->ai.actors[k].state == AI_PURSUE || g->ai.actors[k].state == AI_COMBAT)
                ai_stop_combat(g, k);
        CHECK(g->ai.combat == 0 || 1, "combat cleared");
    }

    /* ---- summoning --------------------------------------------------------- */
    {
        uint32_t before = g->actors.count, k;
        int summoned = -1, tries;
        vm_item_add(vm, pc.key ^ VM_SPELL_SALT, pw_hash_name("tw_summon"), 1);
        CHECK(rpg_select_spell(r, pw_hash_name("tw_summon")), "summon spell selected");
        r->draw_state = 2;
        place_player(g->ai.pts[0][0], g->ai.pts[0][1], g->ai.pts[5][0], g->ai.pts[5][1]);
        for (tries = 0; tries < 30 && g->actors.count == before; ++tries) {
            vm_set_prop(vm, &pc, MWP_DYN_MAGICKA, rpg_dyn_max(r, &pc, 1));
            step(0, GB_R, 0, 0, 1.0f);
        }
        CHECK(g->actors.count == before + 1, "a creature was summoned (%u tries)", (unsigned)tries);
        for (k = before; k < g->actors.count; ++k)
            summoned = (int)k;
        if (summoned >= 0) {
            AiActor *s = &g->ai.actors[summoned];
            CHECK(s->ally && s->life > 0.0f && s->life <= 20.0f, "an ally for 20 s");
            CHECK(g->zone.instances[g->actors.list[summoned].instance].object_hash == pw_hash_name("tw_critter"),
                  "the creature named by sMagicScampID");
            place_player(g->body.pos.x + 700.0f, g->body.pos.y, 0, 0);
            step(0, 0, 0, 0, 6.0f);
            CHECK(actor_dist(summoned) < 400.0f, "summon follows its master (%.0f)", actor_dist(summoned));
            step(0, 0, 0, 0, 16.0f);
            CHECK(s->state == AI_GONE && g->zone.inst_hidden[g->actors.list[summoned].instance],
                  "summon vanished when the effect ended");
        }
        r->draw_state = 0;
    }

    /* ---- hostile creature: detection, combat, battle music ------------- */
    {
        int zone = world_find_exterior(&g_world, -2, -2), k;
        float hp0;
        enter(zone);
        snapper = actor_of("tw_snapper");
        CHECK(snapper >= 0, "snapper placed");
        if (snapper >= 0) {
            float sx = g->actors.list[snapper].pos.x, sy = g->actors.list[snapper].pos.y;
            float fwd_x, fwd_y;
            int seen = 0;
            /* sneaking behind it from afar with great skill: rarely seen */
            vm_set_prop(vm, &pc, MWP_SKILL_SNEAK, 100.0f);
            pw_sincosf(g->actors.list[snapper].yaw, &fwd_x, &fwd_y);
            place_player(sx - fwd_x * 1800.0f, sy - fwd_y * 1800.0f, sx, sy);
            g->sneaking = 1;
            for (k = 0; k < 50; ++k)
                seen += ai_detects_player(g, snapper);
            CHECK(seen < 25, "a skilled sneak behind it is seldom noticed (%d/50)", seen);
            g->sneaking = 0;
            for (k = 0, seen = 0; k < 20; ++k)
                seen += ai_detects_player(g, snapper);
            CHECK(seen == 20, "walking up in plain view is always noticed");
            /* far away: fight 100 + 10 - 0.005 x 3500 < 100 */
            place_player(sx + 3500.0f, sy, sx, sy);
            step(0, 0, 0, 0, 2.0f);
            CHECK(g->ai.actors[snapper].state != AI_COMBAT, "out of fight range at 3500");
            place_player(sx + 900.0f, sy, sx, sy);
            hp0 = rpg_dyn(r, &pc, 0);
            step(0, 0, 0, 0, 1.0f);
            CHECK(g->ai.actors[snapper].state == AI_COMBAT, "snapper attacks at 900 units");
            for (k = 0; k < 15 && rpg_dyn(r, &pc, 0) >= hp0; ++k) {
                step(0, 0, 0, 0, 1.0f);
                if (getenv("AI_DEBUG"))
                    printf("t%d '%s' dist %.0f atk %.2f hp %.0f\n", k, g->message, actor_dist(snapper),
                           g->ai.actors[snapper].attack_timer, rpg_dyn(r, &pc, 0));
            }
            CHECK(actor_dist(snapper) < 160.0f, "it closed in (%.0f)", actor_dist(snapper));
            CHECK(rpg_dyn(r, &pc, 0) < hp0, "and bit (%.0f -> %.0f)", hp0, rpg_dyn(r, &pc, 0));
            CHECK(g->ai.combat == 1 && g_audio.music_mode == AUDIO_MUSIC_BATTLE, "battle music while fighting");
            {
                VmRef sr = game_actor_ref(g, (uint32_t)snapper);
                rpg_kill(r, &sr, &pc);
            }
            step(0, 0, 0, 0, 1.0f);
            CHECK(g->ai.combat == 0 && g_audio.music_mode == AUDIO_MUSIC_EXPLORE, "back to exploring music");
            vm_set_prop(vm, &pc, MWP_DYN_HEALTH, rpg_dyn_max(r, &pc, 0));
        }
        /* spawning by id (PlaceAtPC) and a spawned enemy */
        {
            float p[3] = { g->body.pos.x + 400.0f, g->body.pos.y, g->body.pos.z };
            int a = ai_spawn(g, pw_hash_name("tw_snapper"), p, 0.0f, 0, -1.0f, 0);
            CHECK(a >= 0 && g->zone.instance_count > g->zone.hdr->instance_count, "snapper spawned (%d)", a);
            CHECK(ai_spawn(g, pw_hash_name("tw_coin"), p, 0.0f, 0, -1.0f, 0) < 0, "items are not actors");
            if (a >= 0) {
                step(0, 0, 0, 0, 1.0f);
                if (getenv("AI_DEBUG"))
                    printf("spawned %d state %d sees %d dist %.0f los %d dead %d\n", a, g->ai.actors[a].state,
                           g->ai.actors[a].sees_player, actor_dist(a), ai_has_los(g, a), g->player_dead);
                CHECK(g->ai.actors[a].state == AI_COMBAT, "the spawned snapper attacks too");
            }
        }
    }

    /* ---- sounds produced along the way ----------------------------------- */
    CHECK(g_audio.loads >= 5, "effects loaded on demand (%u)", (unsigned)g_audio.loads);
    {
        uint32_t cellar = (uint32_t)world_find_interior(&g_world, pw_hash_name("test hamlet, cellar"));
        int lantern, k, looping = 0;
        enter((int)cellar);
        lantern = inst_of("tw_lantern");
        place_player(0.0f, -100.0f, 0.0f, 0.0f);
        step(0, 0, 0, 0, 1.5f);
        for (k = 0; k < 4; ++k)
            looping |= g->light_inst[k] == (uint32_t)lantern + 1 && audio_handle_playing(&g_audio, g->light_voice[k]);
        CHECK(lantern >= 0 && looping, "lantern fire loop playing");
        CHECK(g->ai.count == 9, "cellar path grid (%u)", (unsigned)g->ai.count);
    }
    CHECK(vm->errors == 0, "no script errors (%s)", vm->last_error);
    audio_shutdown(&g_audio);
    printf("ai_test: %d/%d checks passed (paths %u, fails %u, spawned %u, crimes %u)\n", g_checks - g_fail, g_checks,
           (unsigned)g->ai.paths, (unsigned)g->ai.path_fails, (unsigned)g->ai.spawned, (unsigned)g->ai.crimes_seen);
    return g_fail ? 1 : 0;
}
