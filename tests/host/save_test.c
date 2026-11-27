/*
 * save_test: saved games on the cooked test world - a played state round
 * trips exactly (position, zone, inventory stacks and equipment, journal,
 * globals, stats, effects, stolen goods, a summoned creature, actor
 * positions); damaged, foreign and fuzzed files are rejected without
 * touching the running game.
 *
 *   save_test <test world data dir> <scratch dir>
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
static char g_dir[300];

static void step(float seconds)
{
    const float dt = 1.0f / 30.0f;
    int frames = (int)(seconds / dt + 0.5f), i;
    for (i = 0; i < frames; ++i) {
        InputState in;
        memset(&in, 0, sizeof(in));
        g_game.ui_event = GAME_UI_NONE;
        game_update(&g_game, &in, dt);
    }
}

typedef struct {
    float pos[3], yaw;
    uint32_t zone;
    int32_t gold, potions, goblets;
    int journal;
    float global, crime, longblade, health;
    int equipped;
    uint32_t fx, actors, stolen, visited;
    float actor_pos[8][2];
    int hidden_ring;
} Snapshot;

static void snapshot(Snapshot *s)
{
    Vm *vm = &g_rpg.vm;
    Rpg *r = &g_rpg.rpg;
    VmRef pc = vm_player_ref(vm);
    uint32_t i;
    memset(s, 0, sizeof(*s));
    s->pos[0] = g_game.body.pos.x;
    s->pos[1] = g_game.body.pos.y;
    s->pos[2] = g_game.body.pos.z;
    s->yaw = g_game.yaw;
    s->zone = g_game.zone.index;
    s->gold = rpg_gold(r, &pc);
    s->potions = vm_item_count(vm, vm->player_hash, pw_hash_name("tw_potion_heal"));
    s->goblets = vm_item_count(vm, vm->player_hash, pw_hash_name("tw_goblet"));
    s->journal = vm_journal_index(vm, pw_hash_name("tw_lostring"));
    s->global = vm_global_float(vm, vm_global_index(vm, "tw_ringfound"));
    s->crime = vm_prop(vm, &pc, MWP_CRIME);
    s->longblade = rpg_skill(r, &pc, SKILL_LONGBLADE);
    s->health = rpg_dyn(r, &pc, 0);
    s->equipped = rpg_equipped(r, &pc, RPG_SLOT_CUIRASS);
    s->fx = r->fx_count;
    s->actors = g_game.actors.count;
    s->stolen = g_game.ai.stolen_used;
    s->visited = r->visited_count;
    for (i = 0; i < g_game.actors.count && i < 8; ++i) {
        s->actor_pos[i][0] = g_game.actors.list[i].pos.x;
        s->actor_pos[i][1] = g_game.actors.list[i].pos.y;
    }
}

static int same(const Snapshot *a, const Snapshot *b, char *why, size_t n)
{
    uint32_t i;
#define DIFF(cond, name)                                                       \
    if (cond) {                                                                \
        snprintf(why, n, "%s", name);                                          \
        return 0;                                                              \
    }
    DIFF(fabsf(a->pos[0] - b->pos[0]) > 0.5f || fabsf(a->pos[1] - b->pos[1]) > 0.5f, "position");
    DIFF(a->zone != b->zone, "zone");
    DIFF(fabsf(a->yaw - b->yaw) > 1e-4f, "yaw");
    DIFF(a->gold != b->gold || a->potions != b->potions || a->goblets != b->goblets, "inventory");
    DIFF(a->journal != b->journal || a->global != b->global, "journal / globals");
    DIFF(a->crime != b->crime || a->longblade != b->longblade || fabsf(a->health - b->health) > 0.01f, "stats");
    DIFF(a->equipped != b->equipped, "equipment");
    DIFF(a->fx != b->fx || a->visited != b->visited, "mechanics state");
    DIFF(a->actors != b->actors, "actor count");
    DIFF(a->stolen != b->stolen, "stolen goods");
    for (i = 0; i < a->actors && i < 8; ++i)
        DIFF(fabsf(a->actor_pos[i][0] - b->actor_pos[i][0]) > 0.5f ||
                 fabsf(a->actor_pos[i][1] - b->actor_pos[i][1]) > 0.5f, "actor positions");
#undef DIFF
    return 1;
}

static long file_bytes(const char *path, unsigned char *buf, long cap)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f)
        return -1;
    n = (long)fread(buf, 1, (size_t)cap, f);
    fclose(f);
    return n;
}

static void write_bytes(const char *path, const unsigned char *buf, long n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(buf, 1, (size_t)n, f);
    fclose(f);
}

static void refresh_crc(unsigned char *buf, long n)
{
    DSaveHeader h;
    memcpy(&h, buf, sizeof(h));
    h.crc = save_crc32(0, buf + sizeof(h), (uint32_t)n - (uint32_t)sizeof(h));
    memcpy(buf, &h, sizeof(h));
}

int main(int argc, char **argv)
{
    static unsigned char file[PW_MB(2)], work[PW_MB(2)];
    char err[200], path[400], why[64];
    Game *g = &g_game;
    Vm *vm = &g_rpg.vm;
    Rpg *r = &g_rpg.rpg;
    VmRef pc;
    Snapshot before, after;
    float playtime = 0.0f;
    long len;
    int hamlet;
    if (argc < 3) {
        fprintf(stderr, "usage: save_test <data dir> <scratch dir>\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    snprintf(g_dir, sizeof(g_dir), "%s/saves", argv[2]);
    mem_init();
    if (world_open(&g_world, argv[1], err, sizeof(err)) != 0) {
        printf("world_open failed: %s\n", err);
        return 1;
    }
    game_init(g, &g_world);
    CHECK(game_attach_rpg(g, &g_rpg, argv[1], err, sizeof(err)) == 0, "rpg (%s)", err);
    pc = vm_player_ref(vm);
    rpg_chargen(r, "Saver", pw_hash_name("tw_folk"), 1, pw_hash_name("tw_warrior"), pw_hash_name("tw_sign_blade"));
    hamlet = world_find_exterior(&g_world, 0, 0);
    CHECK(game_enter_zone(g, (uint32_t)hamlet, NULL, NULL, err, sizeof(err)) == 0, "hamlet");
    step(0.5f);
    while (vm_message_front(vm))
        vm_message_answer(vm, 0); /* the start script's welcome box */
    {
        SaveSlotInfo info;
        int k;
        for (k = 0; k < SAVE_SLOTS; ++k) {
            snprintf(path, sizeof(path), "%s/slot%d.sav", g_dir, k);
            remove(path);
        }
        save_slot_info(g_dir, 1, &info);
        CHECK(!info.used && save_latest(g_dir) < 0, "no saves yet");
    }

    /* ---- play a little: inventory, equipment, journal, crime, a summon -- */
    {
        uint32_t cursor = 0, item;
        int32_t count;
        int slot, cuirass = -1;
        while ((slot = vm_item_next(vm, pc.key, &cursor, &item, &count)) >= 0)
            if (item == pw_hash_name("tw_cuirass"))
                cuirass = slot;
        CHECK(cuirass >= 0 && rpg_equip(r, &pc, cuirass) == 1, "cuirass worn");
        vm_item_add(vm, pc.key, pw_hash_name("tw_potion_heal"), 3);
        vm_item_add(vm, pc.key, pw_hash_name(RPG_GOLD), 123);
        vm_journal(vm, pw_hash_name("tw_lostring"), 10);
        vm->globals[vm_global_index(vm, "tw_ringfound")].f = 1.0f;
        rpg_skill_raise(r, SKILL_LONGBLADE, 1);
        rpg_dyn_add(r, &pc, 0, -7.0f);
        {
            int goblet = -1;
            uint32_t i;
            for (i = 0; i < g->zone.instance_count; ++i)
                if (g->zone.instances[i].object_hash == pw_hash_name("tw_goblet"))
                    goblet = (int)i;
            g->body.pos.x = g->zone.instances[goblet].pos[0] - 50.0f;
            g->body.pos.y = g->zone.instances[goblet].pos[1] - 50.0f;
            step(0.3f);
            game_take(g, (uint32_t)goblet);
        }
        vm_item_add(vm, pc.key ^ VM_SPELL_SALT, pw_hash_name("tw_summon"), 1);
        {
            int n = 0;
            const DRpgSpell *sp = rpg_spell(r, pw_hash_name("tw_summon"));
            const DRpgEffect *e = rpg_spell_effects(r, sp, &n);
            rpg_apply(r, &pc, &pc, e, n, RPG_RANGE_SELF, sp->hash, 100.0f, 1);
        }
        CHECK(g->actors.count == 4, "summoned critter present (%u)", (unsigned)g->actors.count);
        g->body.pos.x += 300.0f;
        step(2.0f);
        g->yaw = 1.25f;
    }
    CHECK(save_game(g, g_dir, 1, 321.0f, err, sizeof(err)) == 0, "saved to slot 1 (%s)", err);
    snapshot(&before);
    {
        SaveSlotInfo info;
        save_slot_info(g_dir, 1, &info);
        CHECK(info.used && strcmp(info.hdr.name, "Saver") == 0 && strcmp(info.hdr.cell, "Test Hamlet") == 0 &&
                  info.hdr.level == 1 && info.hdr.playtime == 321.0f,
              "slot summary (%s, %s, level %d)", info.hdr.name, info.hdr.cell, info.hdr.level);
        CHECK(save_latest(g_dir) == 1, "newest slot");
    }

    /* ---- change everything, then load ----------------------------------- */
    {
        uint32_t cellar = (uint32_t)world_find_interior(&g_world, pw_hash_name("test hamlet, cellar"));
        vm_item_add(vm, pc.key, pw_hash_name(RPG_GOLD), -50);
        vm_item_add(vm, pc.key, pw_hash_name("tw_potion_heal"), -2);
        vm_journal(vm, pw_hash_name("tw_lostring"), 100);
        rpg_unequip_slot(r, &pc, RPG_SLOT_CUIRASS);
        vm_set_prop(vm, &pc, MWP_CRIME, 999.0f);
        g->ai.stolen_used = 0;
        CHECK(game_enter_zone(g, cellar, NULL, NULL, err, sizeof(err)) == 0, "went to the cellar");
        step(1.0f);
    }
    snapshot(&after);
    CHECK(!same(&before, &after, why, sizeof(why)), "state really changed");
    CHECK(load_game(g, g_dir, 1, &playtime, err, sizeof(err)) == 0, "loaded slot 1 (%s)", err);
    snapshot(&after);
    CHECK(same(&before, &after, why, sizeof(why)), "state restored (%s differs)", why);
    CHECK(playtime == 321.0f, "play time restored");
    CHECK(strcmp(vm->pc_name, "Saver") == 0 && r->female && r->race == pw_hash_name("tw_folk"), "character restored");
    {
        int summon = (int)g->actors.count - 1;
        CHECK(summon >= 0 && g->ai.actors[summon].ally && g->ai.actors[summon].life > 0.0f &&
                  g->zone.instances[g->actors.list[summon].instance].object_hash == pw_hash_name("tw_critter"),
              "the summon came back as an ally");
    }
    step(1.0f);
    CHECK(vm->errors == 0, "game runs on after loading (%s)", vm->last_error);

    /* ---- damaged and foreign files ---------------------------------------- */
    snprintf(path, sizeof(path), "%s/slot1.sav", g_dir);
    len = file_bytes(path, file, (long)sizeof(file));
    CHECK(len > (long)sizeof(DSaveHeader), "save file read (%ld bytes)", len);
    if (len <= (long)sizeof(DSaveHeader)) {
        printf("save_test: %d/%d checks passed\n", g_checks - g_fail, g_checks);
        return 1;
    }
    snapshot(&before);
    {
        char p2[400];
        snprintf(p2, sizeof(p2), "%s/slot2.sav", g_dir);
        memcpy(work, file, (size_t)len);
        work[len / 2] ^= 0x5A;
        write_bytes(p2, work, len);
        CHECK(load_game(g, g_dir, 2, NULL, err, sizeof(err)) != 0 && strstr(err, "damaged"), "flipped byte -> damaged (%s)",
              err);
        memcpy(work, file, (size_t)len);
        write_bytes(p2, work, len - 100);
        CHECK(load_game(g, g_dir, 2, NULL, err, sizeof(err)) != 0, "truncated file rejected (%s)", err);
        memcpy(work, file, (size_t)len);
        ((DSaveHeader *)work)->db_objects += 1;
        refresh_crc(work, len);
        write_bytes(p2, work, len);
        CHECK(load_game(g, g_dir, 2, NULL, err, sizeof(err)) != 0 && strstr(err, "different game data"),
              "other game data rejected (%s)", err);
        CHECK(load_game(g, g_dir, 5, NULL, err, sizeof(err)) != 0 && strstr(err, "empty"), "missing slot (%s)", err);
        /* structural fuzzing behind a valid CRC: never crashes, either loads or refuses */
        {
            int k, refused = 0, loaded = 0;
            unsigned seed = 1234;
            for (k = 0; k < 150; ++k) {
                int m;
                memcpy(work, file, (size_t)len);
                for (m = 0; m < 4; ++m) {
                    long at;
                    seed = seed * 1103515245u + 12345u;
                    at = (long)sizeof(DSaveHeader) + (long)((seed >> 8) % (unsigned)(len - (long)sizeof(DSaveHeader)));
                    seed = seed * 1103515245u + 12345u;
                    work[at] = (unsigned char)(seed >> 16);
                }
                refresh_crc(work, len);
                write_bytes(p2, work, len);
                if (load_game(g, g_dir, 2, NULL, err, sizeof(err)) == 0)
                    loaded++;
                else
                    refused++;
            }
            CHECK(refused > 0 && refused + loaded == 150, "fuzzed saves: %d refused, %d loaded, no crash", refused,
                  loaded);
        }
        /* a refused load leaves the game as it was */
        CHECK(load_game(g, g_dir, 1, NULL, err, sizeof(err)) == 0, "reload slot 1");
        snapshot(&before);
        memcpy(work, file, (size_t)len);
        work[len - 3] ^= 0xFF;
        write_bytes(p2, work, len);
        load_game(g, g_dir, 2, NULL, err, sizeof(err));
        snapshot(&after);
        CHECK(same(&before, &after, why, sizeof(why)), "failed load had no side effects (%s)", why);
        remove(p2);
    }

    /* ---- refusals ----------------------------------------------------------- */
    vm_message_answer(vm, 0);
    rpg_damage(r, &pc, 0, 10000.0f, NULL);
    step(0.1f);
    CHECK(save_game(g, g_dir, 3, 0.0f, err, sizeof(err)) != 0, "no saving while dead (%s)", err);
    CHECK(load_game(g, g_dir, save_latest(g_dir), NULL, err, sizeof(err)) == 0 && !g->player_dead,
          "loading the latest save revives (%s)", err);
    printf("save_test: %d/%d checks passed (%ld-byte save)\n", g_checks - g_fail, g_checks, len);
    return g_fail ? 1 : 0;
}
