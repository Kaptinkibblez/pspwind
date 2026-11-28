/*
 * persist_test: Milestone 7 on the cooked test world through the real game
 * loop - voice lines, spawning actors and items from templates, arrows and
 * spell bolts in flight (player and NPC archers), stolen goods per owner and
 * merchants refusing them, rest interrupted by sleep creatures, actors that
 * keep their place across zones, followers and summons going through doors,
 * and the roaming table surviving a save and load.
 *
 *   persist_test <test world data dir> <scratch dir>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "mem.h"
#include "save.h"

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
static char g_dir[300];

static void step(float seconds)
{
    const float dt = 1.0f / 30.0f;
    int frames = (int)(seconds / dt + 0.5f), i;
    Game *g = &g_game;
    for (i = 0; i < frames; ++i) {
        InputState in;
        memset(&in, 0, sizeof(in));
        g->ui_event = GAME_UI_NONE;
        while (vm_message_front(&g_rpg.vm))
            vm_message_answer(&g_rpg.vm, 0); /* the UI would show and close it */
        game_update(g, &in, dt);
        if (g->request.pending) {
            ZoneRequest rq = g->request;
            char err[160];
            printf("  (zone change to %u during a step)\n", (unsigned)rq.zone);
            if (game_enter_zone(g, rq.zone, rq.has_pos ? rq.pos : NULL, rq.has_pos ? rq.rot : NULL, err, sizeof(err)))
                printf("  zone load failed: %s\n", err);
        }
        audio_mix(&g_audio, g_mix, 1470 / 2);
    }
}

static int actor_of(const char *id)
{
    uint32_t i, h = pw_hash_name(id);
    for (i = 0; i < g_game.actors.count; ++i)
        if (g_game.zone.instances[g_game.actors.list[i].instance].object_hash == h &&
            !g_game.zone.inst_hidden[g_game.actors.list[i].instance])
            return (int)i;
    return -1;
}

static int actor_by_key(uint32_t key, int *hidden)
{
    uint32_t i;
    for (i = 0; i < g_game.actors.count; ++i)
        if (game_actor_ref(&g_game, i).key == key) {
            if (hidden)
                *hidden = g_game.zone.inst_hidden[g_game.actors.list[i].instance];
            return (int)i;
        }
    return -1;
}

static int visible_count(const char *id)
{
    uint32_t i, h = pw_hash_name(id);
    int n = 0;
    for (i = 0; i < g_game.actors.count; ++i)
        n += g_game.zone.instances[g_game.actors.list[i].instance].object_hash == h &&
             !g_game.zone.inst_hidden[g_game.actors.list[i].instance];
    return n;
}

static float dist_xy(float ax, float ay, float bx, float by)
{
    return sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by));
}

static float actor_dist(int a)
{
    return dist_xy(g_game.actors.list[a].pos.x, g_game.actors.list[a].pos.y, g_game.body.pos.x, g_game.body.pos.y);
}

static void place_player(float x, float y, float lx, float ly)
{
    float ground;
    g_game.body.pos.x = x;
    g_game.body.pos.y = y;
    if (phys_ground(&g_game.phys, x, y, g_game.body.pos.z + 2000.0f, &ground))
        g_game.body.pos.z = ground;
    g_game.body.vel.x = g_game.body.vel.y = g_game.body.vel.z = 0.0f;
    g_game.yaw = atan2f(lx - x, ly - y);
    g_game.pitch = 0.0f;
}

static void enter(int zone)
{
    char err[160];
    if (zone < 0 || game_enter_zone(&g_game, (uint32_t)zone, NULL, NULL, err, sizeof(err)) != 0)
        printf("  cannot enter zone %d: %s\n", zone, err);
}

static int stack_of(uint32_t owner, const char *id)
{
    uint32_t cursor = 0, item, h = pw_hash_name(id);
    int32_t count;
    int s;
    while ((s = vm_item_next(&g_rpg.vm, owner, &cursor, &item, &count)) >= 0) {
        if (item == h)
            return s;
        cursor = (uint32_t)s + 1;
    }
    return -1;
}

static void heal_player(void)
{
    VmRef pc = vm_player_ref(&g_rpg.vm);
    vm_set_prop(&g_rpg.vm, &pc, MWP_DYN_HEALTH, rpg_dyn_max(&g_rpg.rpg, &pc, 0));
    vm_set_prop(&g_rpg.vm, &pc, MWP_DYN_FATIGUE, rpg_dyn_max(&g_rpg.rpg, &pc, 2));
}

static void kill_actor(int a)
{
    VmRef ref = game_actor_ref(&g_game, (uint32_t)a), pc = vm_player_ref(&g_rpg.vm);
    rpg_kill(&g_rpg.rpg, &ref, &pc);
}

/* chest-high point of an actor, as an aim */
static void aim_at(int a, const float from[3], float dir[3])
{
    const Actor *ac = &g_game.actors.list[a];
    dir[0] = ac->pos.x - from[0];
    dir[1] = ac->pos.y - from[1];
    dir[2] = ac->pos.z + 64.0f * ac->scale - from[2];
}

static void eye(float out[3])
{
    out[0] = g_game.body.pos.x;
    out[1] = g_game.body.pos.y;
    out[2] = g_game.body.pos.z + GAME_EYE_HEIGHT - 12.0f;
}

static int shots_active(void)
{
    int k, n = 0;
    for (k = 0; k < GAME_SHOTS; ++k)
        n += g_game.shots[k].active;
    return n;
}

int main(int argc, char **argv)
{
    char err[200], msg[96];
    Game *g = &g_game;
    Vm *vm = &g_rpg.vm;
    Rpg *r = &g_rpg.rpg;
    VmRef pc;
    int hamlet, cellar, elder, weaver, guard;
    float hx = 1200.0f, hy = 900.0f; /* the house */
    if (argc < 3) {
        fprintf(stderr, "usage: persist_test <data dir> <scratch dir>\n");
        return 2;
    }
    snprintf(g_dir, sizeof(g_dir), "%s/m7saves", argv[2]);
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
    cellar = world_find_interior(&g_world, pw_hash_name("test hamlet, cellar"));
    CHECK(hamlet >= 0 && cellar >= 0, "zones");
    enter(hamlet);
    vm_message_answer(vm, 0);
    elder = actor_of("tw_villager");
    weaver = actor_of("tw_villager_f");
    guard = actor_of("tw_guard");
    CHECK(elder >= 0 && weaver >= 0 && guard >= 0, "hamlet actors");

    /* ---- voices ---------------------------------------------------------- */
    {
        VmRef gr = game_actor_ref(g, (uint32_t)guard), er = game_actor_ref(g, (uint32_t)elder);
        float gx = g->actors.list[guard].pos.x, gy = g->actors.list[guard].pos.y;
        int k, said = 0;
        place_player(gx, gy - 160.0f, gx, gy);
        for (k = 0; k < 60 && !said; ++k) {
            step(1.0f / 30.0f);
            said = audio_saying(&g_audio, gr.key);
        }
        CHECK(said, "the guard says hello (voice playing)");
        CHECK(strstr(g->message, "Citizen.") != NULL, "with a subtitle (%s)", g->message);
        CHECK(dlg_start(&g_rpg.dlg, &er, "Hamlet Elder") == 1, "elder greets");
        CHECK(audio_saying(&g_audio, er.key), "the greeting's recorded voice plays");
        dlg_end(&g_rpg.dlg);
        {
            uint32_t sound = 0;
            char text[96];
            CHECK(dlg_voice_line(&g_rpg.dlg, &er, 0, "attack", &sound, text, sizeof(text)) == 1 && sound &&
                      strcmp(text, "Die!") == 0,
                  "attack battle cry (%s)", text);
            CHECK(dlg_voice_line(&g_rpg.dlg, &er, 0, "hello", &sound, text, sizeof(text)) == 0,
                  "hello lines are the guard's only");
        }
    }

    if (getenv("M7_DEBUG"))
        printf("after voices: menu %d dlg %d msg '%s' dead %d\n", g->menu_mode, g_rpg.dlg.active,
               vm_message_front(vm) ? vm_message_front(vm)->text : "", g->player_dead);
    /* ---- spawning from templates ------------------------------------------ */
    {
        float p[3] = { hx + 800.0f, hy + 1500.0f, 0.0f };
        uint32_t before = g->zone.instance_count;
        int item = game_spawn_item(g, pw_hash_name("tw_coin"), p, 0.0f);
        int npc;
        CHECK(item >= 0 && g->zone.instance_count == before + 1, "coin placed in the world (%d)", item);
        CHECK(item >= 0 && zone_instance_mesh(&g->zone, (uint32_t)item) != NULL, "with its model");
        npc = ai_spawn(g, pw_hash_name("tw_villager"), p, 0.0f, 0, -1.0f, 0);
        CHECK(npc >= 0 && g->actors.list[npc].rec->part_count > 0, "an NPC assembled from its template (%d parts)",
              npc >= 0 ? (int)g->actors.list[npc].rec->part_count : -1);
        CHECK(npc >= 0 && game_actor_ref(g, (uint32_t)npc).key != game_actor_ref(g, (uint32_t)elder).key,
              "a copy of a unique NPC has its own identity");
        if (npc >= 0)
            kill_actor(npc);
    }

    /* ---- projectiles: the player's bow ----------------------------------- */
    {
        float from[3], dir[3], p[3] = { hx + 2300.0f, hy + 2600.0f, 0.0f };
        int critter, bow, ammo, k, shot, arrows;
        uint32_t fired = g->shots_fired;
        VmRef cr;
        vm_item_add(vm, pc.key, pw_hash_name("tw_bow"), 1);
        vm_item_add(vm, pc.key, pw_hash_name("tw_arrow"), 10);
        bow = stack_of(pc.key, "tw_bow");
        ammo = stack_of(pc.key, "tw_arrow");
        CHECK(bow >= 0 && ammo >= 0 && rpg_equip(r, &pc, bow) == 1 && rpg_equip(r, &pc, ammo) == 1, "bow and arrows ready");
        CHECK(rpg_weapon_ranged(r, &pc), "a ranged weapon");
        place_player(hx + 2300.0f, hy + 2000.0f, hx + 2300.0f, hy + 2600.0f);
        critter = ai_spawn(g, pw_hash_name("tw_critter"), p, 3.14159265f, 0, -1.0f, 0);
        CHECK(critter >= 0, "target critter spawned");
        cr = game_actor_ref(g, (uint32_t)critter);
        eye(from);
        aim_at(critter, from, dir);
        shot = game_fire_weapon(g, AI_TARGET_PLAYER, from, dir, 1.0f);
        arrows = vm_item_count(vm, pc.key, pw_hash_name("tw_arrow"));
        CHECK(shot >= 0 && g->shots_fired == fired + 1 && arrows == 9, "an arrow leaves the bow (%d left)", arrows);
        CHECK(shot >= 0 && g->shots[shot].mesh >= 0, "drawn with the arrow's model");
        step(1.0f / 30.0f);
        CHECK(shot >= 0 && g->shots[shot].active && g->shots[shot].pos[1] > from[1] + 60.0f,
              "it is in flight (y %.0f)", shot >= 0 ? g->shots[shot].pos[1] : 0.0f);
        CHECK(vm_prop(vm, &cr, MWP_ATTACKED) == 0.0f, "not there yet");
        for (k = 0; k < 30 && shots_active(); ++k) {
            step(1.0f / 30.0f);
            if (getenv("M7_DEBUG"))
                printf("shot %d active %d pos %.0f %.0f %.0f vel %.0f %.0f %.0f life %.2f critter %.0f %.0f %.0f\n", shot,
                       g->shots[shot].active, g->shots[shot].pos[0], g->shots[shot].pos[1], g->shots[shot].pos[2],
                       g->shots[shot].vel[0], g->shots[shot].vel[1], g->shots[shot].vel[2], g->shots[shot].life,
                       g->actors.list[critter].pos.x, g->actors.list[critter].pos.y, g->actors.list[critter].pos.z);
            if (getenv("M7_DEBUG"))
                printf("  req %d menu %d ctl %d dead %d ui %d\n", g->request.pending, g->menu_mode, g->controls_disabled, g->player_dead, g->ui_event);
        }
        CHECK(!shots_active(), "the arrow landed");
        CHECK(vm_prop(vm, &cr, MWP_ATTACKED) != 0.0f, "on the critter");
        /* misses still alert it; a second shot after it charges */
        CHECK(g->ai.actors[critter].state == AI_COMBAT, "which turns on the archer");
        kill_actor(critter);
        /* into the house wall: stops there */
        place_player(hx, hy - 700.0f, hx, hy);
        eye(from);
        dir[0] = 0.0f;
        dir[1] = 1.0f;
        dir[2] = 0.0f;
        shot = game_fire_weapon(g, AI_TARGET_PLAYER, from, dir, 1.0f);
        for (k = 0; k < 30 && shots_active(); ++k)
            step(1.0f / 30.0f);
        CHECK(shot >= 0 && !shots_active() && k < 10, "an arrow at a wall stops (%d frames)", k);
        CHECK(shot >= 0 && g->shots[shot].pos[1] < hy - 150.0f, "short of the wall (y %.0f)", g->shots[shot].pos[1]);
        /* no ammunition */
        vm_item_add(vm, pc.key, pw_hash_name("tw_arrow"), -vm_item_count(vm, pc.key, pw_hash_name("tw_arrow")));
        CHECK(game_fire_weapon(g, AI_TARGET_PLAYER, from, dir, 1.0f) < 0, "no arrows, no shot");
        rpg_unequip_slot(r, &pc, RPG_SLOT_WEAPON);
    }

    /* ---- projectiles: a spell bolt with an area ---------------------------- */
    {
        const DRpgSpell *sp = rpg_spell(r, pw_hash_name("tw_bolt"));
        float from[3], dir[3], p[3] = { hx + 2300.0f, hy + 2600.0f, 0.0f }, q[3] = { hx + 2360.0f, hy + 2620.0f, 0.0f };
        int n = 0, a, b, k;
        const DRpgEffect *e = sp ? rpg_spell_effects(r, sp, &n) : NULL;
        VmRef ar, br;
        float ha, hb;
        place_player(hx + 2300.0f, hy + 2000.0f, hx + 2300.0f, hy + 2600.0f);
        a = ai_spawn(g, pw_hash_name("tw_critter"), p, 3.14159265f, 0, -1.0f, 0);
        b = ai_spawn(g, pw_hash_name("tw_critter"), q, 3.14159265f, 0, -1.0f, 0);
        CHECK(sp && n > 0 && a >= 0 && b >= 0, "frost bolt and two critters");
        if (sp && a >= 0 && b >= 0) {
            ar = game_actor_ref(g, (uint32_t)a);
            br = game_actor_ref(g, (uint32_t)b);
            ha = rpg_dyn(r, &ar, 0);
            hb = rpg_dyn(r, &br, 0);
            eye(from);
            aim_at(a, from, dir);
            k = game_fire_spell(g, AI_TARGET_PLAYER, from, dir, sp->hash, e, n, 100.0f);
            CHECK(k >= 0 && g->shots[k].mesh < 0 && g->shots[k].area > 0.0f && g->shots[k].harmful,
                  "a glowing, harmful bolt with an area");
            step(1.0f / 30.0f);
            CHECK(rpg_dyn(r, &ar, 0) == ha, "the bolt takes time to arrive");
            for (k = 0; k < 60 && shots_active(); ++k)
                step(1.0f / 30.0f);
            CHECK(rpg_dyn(r, &ar, 0) < ha, "frost on the target (%.0f -> %.0f)", ha, rpg_dyn(r, &ar, 0));
            CHECK(rpg_dyn(r, &br, 0) < hb, "and the one beside it (%.0f -> %.0f)", hb, rpg_dyn(r, &br, 0));
            kill_actor(a);
            kill_actor(b);
        }
    }

    /* ---- projectiles: an NPC archer keeps its distance and shoots --------- */
    {
        float p[3] = { hx + 2300.0f, hy + 3000.0f, 0.0f };
        int archer = ai_spawn(g, pw_hash_name("tw_guard"), p, 3.14159265f, 0, -1.0f, 0), k, shots = 0;
        uint32_t fired0 = g->shots_fired;
        place_player(hx + 2300.0f, hy + 2000.0f, hx + 2300.0f, hy + 3000.0f);
        heal_player();
        vm_set_prop(vm, &pc, MWP_ATTACKED, 0.0f);
        CHECK(archer >= 0, "archer spawned");
        if (archer >= 0) {
            VmRef ar = game_actor_ref(g, (uint32_t)archer);
            float hp0 = rpg_dyn(r, &pc, 0);
            int bs, as;
            rpg_autoequip(r, &ar); /* dressed with its own gear first, then handed a bow */
            vm_item_add(vm, ar.key, pw_hash_name("tw_bow"), 1);
            vm_item_add(vm, ar.key, pw_hash_name("tw_arrow"), 6);
            bs = stack_of(ar.key, "tw_bow");
            as = stack_of(ar.key, "tw_arrow");
            CHECK(rpg_equip(r, &ar, bs) == 1 && rpg_equip(r, &ar, as) == 1, "archer armed");
            ai_start_combat(g, archer, AI_TARGET_PLAYER);
            for (k = 0; k < 8 * 30; ++k) {
                step(1.0f / 30.0f);
                heal_player(); /* survive the test */
                hp0 = PW_MAX(hp0, rpg_dyn(r, &pc, 0));
            }
            shots = (int)(g->shots_fired - fired0);
            CHECK(shots >= 2, "the archer shot %d arrows", shots);
            CHECK(vm_item_count(vm, ar.key, pw_hash_name("tw_arrow")) == 6 - shots, "using its own arrows");
            CHECK(actor_dist(archer) > 600.0f, "from range (%.0f units)", actor_dist(archer));
            CHECK(vm_prop(vm, &pc, MWP_ATTACKED) != 0.0f, "at the player");
            kill_actor(archer);
        }
        step(0.5f);
        heal_player();
    }

    /* ---- stolen goods per owner --------------------------------------------- */
    {
        VmRef er = game_actor_ref(g, (uint32_t)elder), wr = game_actor_ref(g, (uint32_t)weaver);
        int st, gold0;
        /* far from every witness */
        place_player(hx + 3000.0f, hy + 3000.0f, hx, hy);
        vm_item_add(vm, pc.key, pw_hash_name("tw_bow"), 1);
        game_theft(g, er.object, pw_hash_name("tw_bow"), 1); /* a bow from the elder */
        CHECK(ai_stolen_from(g, pw_hash_name("tw_bow"), er.object) >= 1, "the bow is the elder's");
        vm_set_prop(vm, &er, MWP_GOLDPOOL, 500.0f);
        st = stack_of(pc.key, "tw_bow");
        CHECK(rpg_sell(r, &er, st, 1, msg, sizeof(msg)) == 0 && strstr(msg, "stolen"), "the elder won't buy it back (%s)",
              msg);
        /* a goblet "of the weavers" */
        vm_item_add(vm, pc.key, pw_hash_name("tw_goblet"), 2);
        gold0 = vm_item_count(vm, pc.key, pw_hash_name(RPG_GOLD));
        st = stack_of(pc.key, "tw_goblet");
        vm_set_prop(vm, &wr, MWP_GOLDPOOL, 500.0f);
        CHECK(rpg_sell(r, &wr, st, 1, msg, sizeof(msg)) > 0, "an honest goblet sells (%s)", msg);
        CHECK(vm_item_count(vm, pc.key, pw_hash_name(RPG_GOLD)) > gold0, "for gold");
        ai_mark_stolen(g, pw_hash_name("tw_goblet"), pw_hash_name("tw_weavers"), 1);
        st = stack_of(pc.key, "tw_goblet");
        CHECK(rpg_sell(r, &wr, st, 1, msg, sizeof(msg)) == 0 && strstr(msg, "stolen"),
              "a weaver won't buy her guild's goblet (%s)", msg);
        CHECK(ai_stolen_from(g, pw_hash_name("tw_goblet"), er.object) == 0, "nothing of the elder's goblets");
        vm_item_add(vm, pc.key, pw_hash_name("tw_goblet"), -vm_item_count(vm, pc.key, pw_hash_name("tw_goblet")));
        vm_item_add(vm, pc.key, pw_hash_name("tw_bow"), -vm_item_count(vm, pc.key, pw_hash_name("tw_bow")));
        vm_set_prop(vm, &pc, MWP_CRIME, 0.0f);
    }

    /* ---- rest interrupted by the region's sleep creatures ------------------ */
    {
        uint32_t lv = pw_hash_name("tw_sleep_creatures"), crit = pw_hash_name("tw_critter"),
                 snap = pw_hash_name("tw_snapper");
        int k, low_ok = 1, high_snap = 0, tries, done = 0, woke = 0;
        for (k = 0; k < 40; ++k) {
            uint32_t c = rpg_leveled(r, lv, 1), d = rpg_leveled(r, lv, 25);
            low_ok &= c == crit;
            high_snap |= d == snap;
        }
        CHECK(low_ok, "a level 1 sleeper meets only critters");
        CHECK(high_snap, "a level 25 one may meet a snapper");
        place_player(hx + 3500.0f, hy + 4000.0f, hx, hy);
        heal_player();
        step(0.5f);
        for (tries = 0; tries < 60 && !woke; ++tries) {
            done = game_rest(g, 8, 1, msg, sizeof(msg));
            woke = msg[0] != '\0';
        }
        CHECK(woke && strstr(msg, "interrupted"), "rest interrupted after %d nights (%s)", tries, msg);
        CHECK(done >= 1 && done <= 8, "after %d hours", done);
        k = actor_of("tw_critter");
        {
            uint32_t i;
            int found = -1;
            for (i = 0; i < g->actors.count; ++i)
                if (g->zone.instances[g->actors.list[i].instance].object_hash == crit &&
                    g->ai.actors[i].state == AI_COMBAT) {
                    VmRef cr = game_actor_ref(g, i);
                    if (!rpg_is_dead(r, &cr))
                        found = (int)i;
                }
            CHECK(found >= 0 && actor_dist(found) < 600.0f, "a critter stands over the sleeper");
            CHECK(game_rest(g, 8, 1, msg, sizeof(msg)) == 0, "no more sleep with it about");
            if (found >= 0)
                kill_actor(found);
        }
        PW_UNUSED(k);
        CHECK(game_rest(g, 2, 0, msg, sizeof(msg)) == 2 && !msg[0], "waiting is never interrupted");
        heal_player();
    }

    /* ---- persistence, followers and summons across doors ------------------ */
    {
        VmRef er = game_actor_ref(g, (uint32_t)elder), wr = game_actor_ref(g, (uint32_t)weaver);
        DRpgAiPackage follow, wander;
        float ex = hx + 700.0f, ey = hy + 2600.0f, ez, sp[3];
        Vec4 ep;
        int a, hidden = 0, summon;
        uint32_t summon_key;
        step(0.5f);
        /* the elder walks off somewhere (a script would MoveTo); he stays there */
        ep = g->actors.list[elder].pos;
        ep.x = ex;
        ep.y = ey;
        if (phys_ground(&g->phys, ex, ey, ep.z + 2000.0f, &ez))
            ep.z = ez;
        actor_set_placement(&g->actors.list[elder], &ep, 1.0f);
        /* the weaver follows the player; a summoned critter comes along too */
        memset(&follow, 0, sizeof(follow));
        follow.type = DRPG_AI_FOLLOW;
        follow.target = pw_hash_name("player");
        ai_set_package(g, weaver, &follow);
        place_player(g->actors.list[weaver].pos.x + 200.0f, g->actors.list[weaver].pos.y, hx, hy);
        sp[0] = g->body.pos.x - 100.0f;
        sp[1] = g->body.pos.y;
        sp[2] = g->body.pos.z;
        summon = ai_spawn(g, pw_hash_name("tw_critter"), sp, 0.0f, 1, 60.0f, 0);
        CHECK(summon >= 0, "summoned critter");
        summon_key = summon >= 0 ? game_actor_ref(g, (uint32_t)summon).key : 0;
        step(0.2f);
        ex = g->actors.list[elder].pos.x;
        ey = g->actors.list[elder].pos.y;
        enter(cellar);
        a = actor_by_key(wr.key, &hidden);
        CHECK(a >= 0 && !hidden, "the weaver came down the cellar stairs");
        CHECK(a >= 0 && actor_dist(a) < 300.0f, "right behind the player (%.0f)", a >= 0 ? actor_dist(a) : -1.0f);
        CHECK(a >= 0 && ai_current_package(g, a) == DRPG_AI_FOLLOW, "still following");
        CHECK(actor_by_key(summon_key, &hidden) >= 0 && !hidden, "the summon too");
        CHECK(actor_by_key(er.key, NULL) < 0, "the elder stayed upstairs");
        step(1.0f);
        /* she stays in the cellar */
        memset(&wander, 0, sizeof(wander));
        wander.type = DRPG_AI_WANDER;
        if (a >= 0)
            ai_set_package(g, a, &wander);
        enter(hamlet);
        a = actor_by_key(er.key, NULL);
        CHECK(a >= 0 && dist_xy(g->actors.list[a].pos.x, g->actors.list[a].pos.y, ex, ey) < 2.0f,
              "the elder is where he was left (%.0f, %.0f)", a >= 0 ? g->actors.list[a].pos.x : 0.0f,
              a >= 0 ? g->actors.list[a].pos.y : 0.0f);
        a = actor_by_key(wr.key, &hidden);
        CHECK(a >= 0 && hidden, "the weaver's house place is empty");
        CHECK(visible_count("tw_villager_f") == 0, "no second weaver");
        CHECK(g->roam_count >= 1, "she is on the roaming table (%u)", (unsigned)g->roam_count);
        CHECK(actor_by_key(summon_key, &hidden) >= 0 && !hidden, "the summon followed back up");
        /* save, reload: the table and her absence survive */
        CHECK(save_game(g, g_dir, 3, 10.0f, err, sizeof(err)) == 0, "saved (%s)", err);
        g->roam_count = 0;
        CHECK(load_game(g, g_dir, 3, NULL, err, sizeof(err)) == 0, "loaded (%s)", err);
        CHECK(g->roam_count >= 1, "roaming table restored (%u)", (unsigned)g->roam_count);
        a = actor_by_key(wr.key, &hidden);
        CHECK(a >= 0 && hidden, "and the weaver is still away");
        a = actor_by_key(er.key, NULL);
        CHECK(a >= 0 && dist_xy(g->actors.list[a].pos.x, g->actors.list[a].pos.y, ex, ey) < 2.0f,
              "the elder kept his place through the save");
        enter(cellar);
        a = actor_by_key(wr.key, &hidden);
        CHECK(a >= 0 && !hidden, "the weaver is found in the cellar");
        CHECK(visible_count("tw_villager_f") == 1, "exactly once");
        /* she follows again and goes home with the player */
        if (a >= 0) {
            place_player(g->actors.list[a].pos.x + 150.0f, g->actors.list[a].pos.y, 0.0f, 0.0f);
            ai_set_package(g, a, &follow);
        }
        enter(hamlet);
        a = actor_by_key(wr.key, &hidden);
        CHECK(a >= 0 && !hidden && actor_dist(a) < 300.0f, "home again, beside the player");
        CHECK(visible_count("tw_villager_f") == 1, "one weaver in the hamlet");
        CHECK(g->roam_count == 0 || actor_by_key(wr.key, NULL) >= 0, "nobody left behind");
    }

    CHECK(vm->errors == 0, "no script errors (%s)", vm->last_error);
    audio_shutdown(&g_audio);
    printf("persist_test: %d/%d checks passed (shots %u, hits %u, roamers %u)\n", g_checks - g_fail, g_checks,
           (unsigned)g->shots_fired, (unsigned)g->shots_hit, (unsigned)g->roam_count);
    return g_fail ? 1 : 0;
}
