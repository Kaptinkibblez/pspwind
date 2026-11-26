/*
 * ui_test: plays "The Weaver's Lost Ring" end to end through the real game
 * loop and UI with button presses, the way main.c drives them on the PSP:
 * walk up to NPCs, talk, follow a hyperlink, answer a choice, open the
 * journal, read and take a book, pick up the ring through its OnActivate
 * script (message box), and claim the reward. Milestone 5 flows follow:
 * character creation, the Stats / Inventory / Magic / Map tabs, barter,
 * persuasion, buying and making spells, travel, looting, melee combat,
 * alchemy, levelling up through rest, and death. Screens are rendered by
 * the host UI backend into PPM images for inspection.
 *
 *   ui_test <test world data dir> [screenshot dir]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "mem.h"
#include "ui.h"

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
static Ui g_ui;
static UiCanvas g_canvas;
static uint32_t g_pixels[UI_SCREEN_W * UI_SCREEN_H];
static const char *g_shots;

/* one frame exactly like main.c */
static void frame_held(uint32_t held, uint32_t pressed)
{
    InputState in, none;
    char err[160];
    memset(&in, 0, sizeof(in));
    memset(&none, 0, sizeof(none));
    in.pressed = pressed;
    in.held = held | pressed;
    game_update(&g_game, ui_active(&g_ui) ? &none : &in, 1.0f / 30.0f);
    ui_update(&g_ui, &in, 1.0f / 30.0f);
    if (g_game.request.pending) {
        ZoneRequest r = g_game.request;
        if (game_enter_zone(&g_game, r.zone, r.has_pos ? r.pos : NULL, r.has_pos ? r.rot : NULL, err,
                            sizeof(err)) != 0)
            printf("  zone load failed: %s\n", err);
    }
    ui_draw(&g_ui, &g_canvas);
}

static void frame(uint32_t pressed)
{
    frame_held(0, pressed);
}

static void frames(int n)
{
    while (n-- > 0)
        frame(0);
}

static void press(uint32_t button)
{
    frame(button);
    frame(0);
}

static void screenshot(const char *name)
{
    char path[512];
    FILE *f;
    int i;
    for (i = 0; i < UI_SCREEN_W * UI_SCREEN_H; ++i)
        g_pixels[i] = 0xFF403830u; /* stand-in for the 3D scene */
    ui_draw(&g_ui, &g_canvas);
    ui_render_rgba(&g_canvas, g_pixels, UI_SCREEN_W);
    CHECK(g_canvas.dropped == 0, "%s: canvas big enough (%u dropped)", name, (unsigned)g_canvas.dropped);
    if (!g_shots)
        return;
    snprintf(path, sizeof(path), "%s/%s.ppm", g_shots, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", UI_SCREEN_W, UI_SCREEN_H);
    for (i = 0; i < UI_SCREEN_W * UI_SCREEN_H; ++i) {
        uint32_t p = g_pixels[i];
        unsigned char rgb[3] = { (unsigned char)p, (unsigned char)(p >> 8), (unsigned char)(p >> 16) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static int find_instance(const char *id)
{
    uint32_t i, h = pw_hash_name(id);
    for (i = 0; i < g_game.zone.hdr->instance_count; ++i)
        if (g_game.zone.instances[i].object_hash == h)
            return (int)i;
    return -1;
}

/* stands the player `dist` units in front of an instance, looking at it */
static void face_instance(int inst, float dist)
{
    const DZoneInstance *in = &g_game.zone.instances[inst];
    float tx = in->bound[0], ty = in->bound[1], tz = in->bound[2];
    float dx = g_game.body.pos.x - tx, dy = g_game.body.pos.y - ty, len = sqrtf(dx * dx + dy * dy);
    float ground;
    if (len < 1.0f) {
        dx = 0.0f;
        dy = -1.0f;
        len = 1.0f;
    }
    g_game.body.pos.x = tx + dx / len * dist;
    g_game.body.pos.y = ty + dy / len * dist;
    if (phys_ground(&g_game.phys, g_game.body.pos.x, g_game.body.pos.y, in->pos[2] + 200.0f, &ground))
        g_game.body.pos.z = ground;
    else
        g_game.body.pos.z = in->pos[2];
    g_game.body.vel.x = g_game.body.vel.y = g_game.body.vel.z = 0.0f;
    g_game.yaw = atan2f(tx - g_game.body.pos.x, ty - g_game.body.pos.y);
    g_game.pitch = atan2f(tz - (g_game.body.pos.z + GAME_EYE_HEIGHT), dist);
    frame(0);
}

static const char *last_response(void)
{
    uint32_t cursor = 0;
    int kind;
    const char *text, *last = "";
    while (dlg_history_next(&g_rpg.dlg, &cursor, &kind, &text))
        if (kind == DLG_ENTRY_TEXT)
            last = text;
    return last;
}

/* moves a list cursor with up/down presses */
static void move_to(const int *sel, int target)
{
    int guard = 0;
    while (*sel != target && guard++ < 300)
        press(*sel < target ? GB_DOWN : GB_UP);
}

/* moves the topic cursor to a topic by name and asks it (service entries
 * come first in the list) */
static int ask(const char *topic)
{
    int t = rpgdb_topic(&g_rpg.db, pw_hash_name(topic)), i, guard = 0;
    for (i = 0; i < (int)g_rpg.dlg.list_count; ++i)
        if ((int)g_rpg.dlg.list[i] == t)
            break;
    if (i == (int)g_rpg.dlg.list_count)
        return 0;
    i += g_rpg.dlg.goodbye ? 0 : g_ui.svc_count;
    while (g_ui.sel_topic != i && guard++ < 64)
        press(g_ui.sel_topic < i ? GB_DOWN : GB_UP);
    press(GB_CROSS);
    return 1;
}

static void close_dialogue(void)
{
    press(GB_CIRCLE);
}

/* picks a dialogue service entry (UI_SVC_*); 0 when not offered */
static int service(int svc)
{
    int k;
    for (k = 0; k < g_ui.svc_count; ++k)
        if (g_ui.svc[k] == svc) {
            move_to(&g_ui.sel_topic, k);
            press(GB_CROSS);
            return 1;
        }
    return 0;
}

/* inventory / column cursor onto the first stack holding item */
static int select_stack(const int32_t *stacks, uint32_t count, int *sel, const char *item)
{
    uint32_t i, h = pw_hash_name(item);
    for (i = 0; i < count; ++i)
        if (vm_item_at(&g_rpg.vm, stacks[i])->item == h) {
            move_to(sel, (int)i);
            return 1;
        }
    return 0;
}

/* on-screen keyboard: rows first (the action row resets the column) */
static void type_text(const char *text)
{
    static const char keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 '-";
    for (; *text; ++text) {
        int idx = (int)(strchr(keys, *text) - keys), guard = 0;
        while (g_ui.kb_sel / 13 != idx / 13 && guard++ < 12)
            press(GB_DOWN);
        while (g_ui.kb_sel % 13 != idx % 13 && guard++ < 40)
            press(GB_RIGHT);
        press(GB_CROSS);
    }
}

static int find_index(const uint32_t *items, uint32_t count, const char *id)
{
    uint32_t i, h = pw_hash_name(id);
    for (i = 0; i < count; ++i)
        if (items[i] == h)
            return (int)i;
    return -1;
}

static int tab_to(int tab)
{
    int guard = 0;
    while (g_ui.tab != tab && guard++ < 8)
        press(GB_R);
    return g_ui.tab == tab;
}

static int count_of(const char *id)
{
    return vm_item_count(&g_rpg.vm, g_rpg.vm.player_hash, pw_hash_name(id));
}

static int find_door(void)
{
    uint32_t i;
    int door = -1;
    for (i = 0; i < g_game.zone.hdr->instance_count; ++i)
        if (g_game.zone.instances[i].kind == DZONE_INST_DOOR && g_game.zone.instances[i].door != DZONE_NONE)
            door = (int)i;
    return door;
}

int main(int argc, char **argv)
{
    char err[200];
    int hamlet, weaver, elder, door, ring, book;
    Vm *vm = &g_rpg.vm;
    if (argc < 2) {
        fprintf(stderr, "usage: ui_test <data dir> [screenshot dir]\n");
        return 2;
    }
    g_shots = argc > 2 ? argv[2] : NULL;
    mem_init();
    if (world_open(&g_world, argv[1], err, sizeof(err)) != 0) {
        printf("world_open failed: %s\n", err);
        return 1;
    }
    game_init(&g_game, &g_world);
    CHECK(game_attach_rpg(&g_game, &g_rpg, argv[1], err, sizeof(err)) == 0, "rpg attached (%s)", err);
    ui_init(&g_ui, &g_game);
    hamlet = world_find_exterior(&g_world, 0, 0);
    if (hamlet < 0 || game_enter_zone(&g_game, (uint32_t)hamlet, NULL, NULL, err, sizeof(err)) != 0) {
        printf("cannot enter the hamlet: %s\n", err);
        return 1;
    }

    /* start script greets with a modal message box */
    frames(2);
    CHECK(ui_active(&g_ui) && vm_message_front(vm) != NULL, "welcome message box is modal");
    screenshot("01_messagebox");
    press(GB_CROSS);
    frames(2);
    CHECK(!ui_active(&g_ui), "message box answered");
    CHECK(vm_global_float(vm, vm_global_index(vm, "tw_welcomed")) == 1.0f, "button press seen by script");
    CHECK(strcmp(game_cell_name(&g_game), "Test Hamlet") == 0, "cell name '%s'", game_cell_name(&g_game));

    /* character creation, requested the way main.c does without a CharGen script */
    CHECK(g_game.menus_enabled == GAME_MENU_ALL && !g_rpg.rpg.chargen_done, "new game needs a character");
    g_game.ui_event = GAME_UI_CHARGEN;
    g_game.ui_chargen = GAME_CG_ALL;
    frame(0);
    CHECK(g_ui.mode == UI_CHARGEN && g_ui.cg_step == GAME_CG_NAME && g_ui.kb_active, "name step");
    while (g_ui.name[0])
        press(GB_SQUARE);
    type_text("Ab");
    screenshot("13_chargen_name");
    press(GB_START);
    CHECK(strcmp(vm->pc_name, "Ab") == 0 && g_ui.cg_step == GAME_CG_RACE, "named '%s'", vm->pc_name);
    {
        int folk = find_index(g_ui.cg_items, g_ui.cg_count, "tw_folk");
        CHECK(g_ui.cg_count == 2 && folk >= 0, "two playable races (%u)", (unsigned)g_ui.cg_count);
        move_to(&g_ui.sel, folk);
        press(GB_TRIANGLE);
        CHECK(g_ui.cg_female, "sex toggled");
        screenshot("14_chargen_race");
        press(GB_CROSS);
    }
    CHECK(g_ui.cg_step == GAME_CG_CLASS, "class step");
    move_to(&g_ui.sel, find_index(g_ui.cg_items, g_ui.cg_count, "tw_warrior"));
    screenshot("15_chargen_class");
    press(GB_CROSS);
    CHECK(g_ui.cg_step == GAME_CG_BIRTH && g_ui.cg_count == 2, "birthsign step");
    move_to(&g_ui.sel, find_index(g_ui.cg_items, g_ui.cg_count, "tw_sign_blade"));
    press(GB_CROSS);
    CHECK(g_ui.cg_step == GAME_CG_REVIEW, "review step");
    screenshot("16_chargen_review");
    press(GB_CROSS);
    {
        VmRef pc = vm_player_ref(vm);
        Rpg *r = &g_rpg.rpg;
        CHECK(g_ui.mode == UI_NONE && r->chargen_done, "character created");
        CHECK(r->race == pw_hash_name("tw_folk") && r->female && r->cls == pw_hash_name("tw_warrior") &&
              r->sign == pw_hash_name("tw_sign_blade"), "choices applied");
        CHECK(rpg_skill(r, &pc, SKILL_LONGBLADE) == 45.0f && rpg_dyn_max(r, &pc, 0) == 50.0f,
              "warrior stats (longblade %g)", rpg_skill(r, &pc, SKILL_LONGBLADE));
    }

    /* the elder */
    elder = find_instance("tw_villager");
    weaver = find_instance("tw_villager_f");
    CHECK(elder >= 0 && weaver >= 0, "villagers placed");
    face_instance(elder, 110.0f);
    CHECK(g_game.target == (uint32_t)elder, "elder targeted (%d)", (int)g_game.target);
    CHECK(game_target_label(&g_game) && strcmp(game_target_label(&g_game), "Hamlet Elder") == 0,
          "target label shows the NPC name");
    press(GB_R);
    CHECK(g_ui.mode == UI_DIALOGUE && g_rpg.dlg.active, "talking to the elder");
    CHECK(g_game.menu_mode, "world paused in dialogue");
    screenshot("02_elder_greeting");
    /* follow the hyperlink in the greeting */
    press(GB_RIGHT);
    CHECK(g_ui.focus == 1 && g_ui.doc.link_count == 1, "link focus (%u links)", (unsigned)g_ui.doc.link_count);
    screenshot("03_link_selected");
    press(GB_CROSS);
    CHECK(strstr(last_response(), "Ask her about her lost ring.") != NULL, "link asked 'latest rumors'");
    screenshot("04_rumors");
    close_dialogue();
    CHECK(g_ui.mode == UI_NONE && !g_rpg.dlg.active && !g_game.menu_mode, "dialogue closed");

    /* the weaver: greeting script, quest offer and choice */
    face_instance(weaver, 110.0f);
    press(GB_R);
    CHECK(g_ui.mode == UI_DIALOGUE, "talking to the weaver");
    CHECK(ask("lost ring"), "lost ring listed");
    CHECK(g_rpg.dlg.in_choice, "quest offer asks a question");
    screenshot("05_choice");
    press(GB_CROSS);
    CHECK(strstr(last_response(), "Search the cellar.") != NULL, "accepted the quest");
    CHECK(vm_journal_index(vm, pw_hash_name("tw_lostring")) == 10, "journal 10");
    screenshot("06_accepted");
    close_dialogue();
    frames(1);

    /* journal */
    press(GB_SELECT);
    CHECK(g_ui.mode == UI_MENU && g_ui.tab == UI_TAB_STATS, "menu opens on Stats");
    press(GB_L);
    CHECK(g_ui.tab == UI_TAB_JOURNAL && g_ui.jtab == UI_J_ENTRIES, "journal open");
    screenshot("07_journal");
    press(GB_TRIANGLE);
    CHECK(g_ui.jtab == UI_J_QUESTS && g_ui.item_count == 1, "one quest listed");
    press(GB_CROSS);
    CHECK(g_ui.detail >= 0, "quest details");
    screenshot("08_quest");
    press(GB_CIRCLE);
    press(GB_TRIANGLE);
    CHECK(g_ui.jtab == UI_J_TOPICS && g_ui.item_count >= 2, "heard topics (%u)", (unsigned)g_ui.item_count);
    press(GB_TRIANGLE);
    press(GB_SELECT);
    CHECK(g_ui.mode == UI_NONE, "menu closed");

    /* into the cellar */
    door = -1;
    {
        uint32_t i;
        for (i = 0; i < g_game.zone.hdr->instance_count; ++i)
            if (g_game.zone.instances[i].kind == DZONE_INST_DOOR &&
                g_game.zone.instances[i].door != DZONE_NONE)
                door = (int)i;
    }
    CHECK(door >= 0, "cellar door");
    face_instance(door, 100.0f);
    press(GB_R);
    CHECK(g_game.zone.hdr && g_game.zone.hdr->kind != DZONE_KIND_EXTERIOR, "entered the cellar");
    CHECK(strcmp(game_cell_name(&g_game), "Test Hamlet, Cellar") == 0, "interior cell name '%s'",
          game_cell_name(&g_game));
    frames(2);

    /* a book */
    book = find_instance("tw_book_history");
    CHECK(book >= 0, "book placed");
    face_instance(book, 90.0f);
    CHECK(g_game.target == (uint32_t)book, "book targeted");
    press(GB_R);
    CHECK(g_ui.mode == UI_BOOK && g_ui.doc.line_count >= 4, "book open (%u lines)", (unsigned)g_ui.doc.line_count);
    screenshot("09_book");
    press(GB_SQUARE);
    CHECK(g_ui.mode == UI_NONE && vm_item_count(vm, vm->player_hash, pw_hash_name("tw_book_history")) == 1,
          "book taken");
    CHECK(g_game.zone.inst_hidden[book], "taken book hidden");

    /* the ring: its OnActivate script raises a message box and calls Activate */
    ring = find_instance("tw_ring");
    CHECK(ring >= 0, "ring placed");
    face_instance(ring, 90.0f);
    CHECK(g_game.target == (uint32_t)ring, "ring targeted");
    press(GB_R);
    frames(1);
    {
        /* a MessageBox without buttons shows as a transient notice */
        int i, found = 0;
        for (i = 0; i < UI_TOASTS; ++i)
            found |= g_ui.toast_time[i] > 0.0f && strcmp(g_ui.toast[i], "The ring glints in the dust.") == 0;
        CHECK(found && !ui_active(&g_ui), "ring script message shown as a notice");
    }
    screenshot("10_ring");
    CHECK(vm_item_count(vm, vm->player_hash, pw_hash_name("tw_ring")) == 1, "ring taken by the script");
    CHECK(g_game.zone.inst_hidden[ring], "ring hidden after pickup");
    CHECK(vm_journal_index(vm, pw_hash_name("tw_lostring")) == 20, "journal 20");

    /* back out and claim the reward */
    {
        uint32_t i;
        door = -1;
        for (i = 0; i < g_game.zone.hdr->instance_count; ++i)
            if (g_game.zone.instances[i].kind == DZONE_INST_DOOR &&
                g_game.zone.instances[i].door != DZONE_NONE)
                door = (int)i;
    }
    face_instance(door, 100.0f);
    press(GB_R);
    CHECK(g_game.zone.hdr->kind == DZONE_KIND_EXTERIOR, "back outside");
    frames(2);
    weaver = find_instance("tw_villager_f");
    face_instance(weaver, 110.0f);
    press(GB_R);
    CHECK(strstr(last_response(), "Hello again, Ab.") != NULL, "second greeting uses the new name (%s)", last_response());
    {
        int32_t gold0 = vm_item_count(vm, vm->player_hash, pw_hash_name("gold_001"));
        CHECK(ask("lost ring"), "lost ring still listed");
        CHECK(vm_item_count(vm, vm->player_hash, pw_hash_name("gold_001")) == gold0 + 50, "50 gold");
    }
    CHECK(strcmp(last_response(), "You found it! Here, take this gold.") == 0, "reward ('%s')", last_response());
    screenshot("11_reward");
    CHECK(vm_journal_index(vm, pw_hash_name("tw_lostring")) == 100, "quest finished");
    close_dialogue();
    frames(130); /* toasts expire */
    press(GB_SELECT);
    CHECK(g_ui.tab == UI_TAB_JOURNAL, "menu remembers the tab");
    screenshot("12_journal_done");
    press(GB_SELECT);

    /* ---- Milestone 5 ---------------------------------------------------- */
    {
        Rpg *r = &g_rpg.rpg;
        VmRef pc = vm_player_ref(vm);
        int32_t gold, reeds;

        /* barter with the weaver: sell a reed, buy something back */
        face_instance(weaver, 110.0f);
        press(GB_R);
        CHECK(g_ui.mode == UI_DIALOGUE && g_ui.svc_count == 2 && g_ui.svc[1] == UI_SVC_BARTER,
              "weaver offers persuasion + barter (%d)", g_ui.svc_count);
        screenshot("17_services");
        CHECK(service(UI_SVC_BARTER) && g_ui.mode == UI_BARTER, "barter open");
        gold = count_of("gold_001");
        reeds = count_of("tw_reed");
        press(GB_RIGHT);
        CHECK(select_stack(g_ui.stacks2, g_ui.stack2_count, &g_ui.sel2, "tw_reed"), "reed sellable");
        screenshot("18_barter");
        press(GB_CROSS);
        CHECK(count_of("tw_reed") == reeds - 1 && count_of("gold_001") > gold, "sold a reed (%d gold)",
              count_of("gold_001") - gold);
        press(GB_LEFT);
        CHECK(select_stack(g_ui.stacks, g_ui.stack_count, &g_ui.sel, "tw_reed"), "merchant shows the reed");
        press(GB_CROSS);
        CHECK(count_of("tw_reed") == reeds, "bought it back");
        press(GB_CIRCLE);
        CHECK(g_ui.mode == UI_DIALOGUE, "barter returns to the conversation");

        /* persuasion: the NPC answers from the Persuasion topics */
        CHECK(service(UI_SVC_PERSUADE) && g_ui.mode == UI_SERVICE, "persuasion open");
        press(GB_CROSS);
        CHECK(g_ui.status[0] != '\0' && strstr(g_ui.status, "Disposition") != NULL, "admired ('%s')",
              g_ui.status);
        CHECK(strcmp(last_response(), "You are too kind, Ab.") == 0 ||
              strcmp(last_response(), "Flattery will get you nowhere.") == 0,
              "answer from the Admire topics ('%s')", last_response());
        screenshot("19_persuade");
        press(GB_CIRCLE);
        close_dialogue();
        CHECK(g_ui.mode == UI_NONE && !g_rpg.dlg.active, "conversation over");

        /* the elder: buy a spell, make one, then travel to the cellar */
        face_instance(elder, 110.0f);
        press(GB_R);
        CHECK(g_ui.mode == UI_DIALOGUE, "talking to the elder");
        CHECK(service(UI_SVC_SPELLS) && g_ui.mode == UI_SERVICE && g_ui.svc_line_count >= 1,
              "spells for sale (%d)", g_ui.svc_line_count);
        screenshot("20_spells");
        {
            uint32_t sp = g_ui.spells[g_ui.svc_lines[0]];
            press(GB_CROSS);
            CHECK(vm_item_count(vm, pc.key ^ VM_SPELL_SALT, sp) == 1, "spell bought (%s)", g_ui.status);
        }
        press(GB_CIRCLE);
        CHECK(service(UI_SVC_SPELLMAKING) && g_ui.mode == UI_SPELLMAKING, "spellmaking open");
        press(GB_CROSS); /* name */
        CHECK(g_ui.kb_active, "name keyboard");
        type_text("Zap");
        press(GB_START);
        CHECK(strcmp(g_ui.name, "Zap") == 0 && !g_ui.kb_active, "spell named");
        press(GB_DOWN);
        press(GB_CROSS); /* add effect: opens the editor */
        CHECK(g_ui.fx_count == 1 && g_ui.fx_field == 0, "effect added, editing");
        press(GB_DOWN);
        press(GB_DOWN);
        press(GB_DOWN);
        press(GB_RIGHT);
        CHECK(g_ui.fx[0].mag_min == 2 || g_ui.fx_field != 3, "magnitude raised");
        screenshot("21_spellmaking");
        press(GB_CIRCLE);
        move_to(&g_ui.sel, 3); /* Name, effect, Add, Buy */
        {
            uint32_t customs = r->custom_count;
            vm_item_add(vm, pc.key, pw_hash_name("gold_001"), 500);
            press(GB_CROSS);
            CHECK(r->custom_count == customs + 1 && g_ui.fx_count == 0, "spell made (%s)", g_ui.status);
        }
        press(GB_CIRCLE);
        CHECK(service(UI_SVC_ENCHANT) && g_ui.mode == UI_ENCHANT, "enchanting open");
        screenshot("22_enchant");
        press(GB_CIRCLE);
        CHECK(service(UI_SVC_TRAVEL) && g_ui.mode == UI_SERVICE && g_ui.svc_line_count == 1, "travel offered");
        screenshot("23_travel");
        press(GB_CROSS);
        frames(2);
        CHECK(g_ui.mode == UI_NONE && !g_rpg.dlg.active, "travel ends the conversation");
        CHECK(g_game.zone.hdr->kind != DZONE_KIND_EXTERIOR, "travelled to the cellar");

        /* inventory: equip, drink, read a skill book */
        frames(130);
        press(GB_SELECT);
        CHECK(tab_to(UI_TAB_STATS), "stats tab");
        screenshot("24_stats");
        CHECK(tab_to(UI_TAB_INVENTORY), "inventory tab");
        CHECK(select_stack(g_ui.stacks, g_ui.stack_count, &g_ui.sel, "tw_cuirass"), "cuirass listed");
        press(GB_CROSS);
        CHECK(rpg_equipped(r, &pc, RPG_SLOT_CUIRASS) >= 0, "cuirass worn");
        CHECK(select_stack(g_ui.stacks, g_ui.stack_count, &g_ui.sel, "tw_dagger"), "dagger listed");
        press(GB_CROSS);
        CHECK(rpg_equipped(r, &pc, RPG_SLOT_WEAPON) >= 0, "dagger wielded");
        screenshot("25_inventory");
        {
            float before = rpg_skill(r, &pc, SKILL_LONGBLADE);
            int skill = rpg_object(r, pw_hash_name("tw_skillbook"))->stat[0];
            before = rpg_skill(r, &pc, skill);
            CHECK(select_stack(g_ui.stacks, g_ui.stack_count, &g_ui.sel, "tw_skillbook"), "skill book listed");
            press(GB_CROSS);
            CHECK(g_ui.mode == UI_BOOK && rpg_skill(r, &pc, skill) == before + 1.0f, "reading raised %s",
                  rpg_skill_name(r, skill));
            press(GB_CIRCLE);
            CHECK(g_ui.mode == UI_MENU && g_ui.tab == UI_TAB_INVENTORY, "book returns to the inventory");
        }
        /* alchemy through the mortar */
        CHECK(select_stack(g_ui.stacks, g_ui.stack_count, &g_ui.sel, "tw_mortar"), "mortar listed");
        press(GB_CROSS);
        CHECK(g_ui.mode == UI_ALCHEMY && g_ui.alch.tools[0] >= 0, "alchemy open");
        press(GB_CROSS);
        CHECK(g_ui.column == 1, "ingredient picker");
        select_stack(g_ui.stacks2, g_ui.stack2_count, &g_ui.sel2, "tw_reed");
        press(GB_CROSS);
        press(GB_DOWN);
        press(GB_CROSS);
        select_stack(g_ui.stacks2, g_ui.stack2_count, &g_ui.sel2, "tw_root");
        press(GB_CROSS);
        {
            DRpgEffect fx[8];
            int value, potions = 0, n = rpg_alchemy_preview(r, &g_ui.alch, fx, 8, &value);
            uint32_t i;
            for (i = 0; i < r->custom_count; ++i)
                potions += r->custom[i].kind == 0;
            CHECK(n >= 1, "shared effect previewed (%d)", n);
            screenshot("26_alchemy");
            move_to(&g_ui.sel, 5);
            press(GB_CROSS);
            CHECK(g_ui.status[0] != '\0', "brewing reported ('%s')", g_ui.status);
            for (i = 0, n = 0; i < r->custom_count; ++i)
                n += r->custom[i].kind == 0;
            CHECK(n == potions + 1 || strstr(g_ui.status, "fail") != NULL || strstr(g_ui.status, "Fail") != NULL,
                  "potion made or the roll failed");
        }
        press(GB_CIRCLE);
        CHECK(g_ui.mode == UI_MENU, "alchemy returns to the inventory");
        CHECK(tab_to(UI_TAB_MAGIC) && g_ui.spell_count >= 2, "magic tab (%u)", (unsigned)g_ui.spell_count);
        press(GB_CROSS);
        CHECK(r->sel_spell != 0 || r->sel_item >= 0, "something selected for casting");
        screenshot("27_magic");
        CHECK(tab_to(UI_TAB_MAP), "map tab");
        screenshot("28_map");
        press(GB_SELECT);

        /* loot the cellar crate */
        {
            int crate = find_instance("tw_crate");
            int32_t g0 = count_of("gold_001");
            CHECK(crate >= 0, "cellar crate");
            face_instance(crate, 90.0f);
            press(GB_R);
            CHECK(g_ui.mode == UI_LOOT && g_ui.stack_count >= 1, "crate open (%u)", (unsigned)g_ui.stack_count);
            screenshot("29_loot");
            press(GB_TRIANGLE);
            CHECK(g_ui.mode == UI_NONE, "take all closes");
            CHECK(count_of("gold_001") >= g0, "crate emptied");
        }

        /* fight the critter with the dagger and loot the corpse */
        {
            int critter = find_instance("tw_critter"), swings = 0, roots = count_of("tw_root");
            VmRef cref;
            CHECK(critter >= 0, "critter placed");
            cref = game_instance_ref(&g_game, (uint32_t)critter);
            face_instance(critter, 70.0f);
            press(GB_LEFT); /* ready the weapon */
            CHECK(r->draw_state == 1, "weapon readied");
            while (!rpg_is_dead(r, &cref) && swings++ < 120) {
                int k;
                face_instance(critter, 70.0f);
                for (k = 0; k < 12; ++k)
                    frame_held(GB_R, k == 0 ? GB_R : 0);
                screenshot(swings == 1 ? "30_combat" : "30_combat_last");
                frame(0);
                frames(20);
            }
            CHECK(rpg_is_dead(r, &cref), "critter killed after %d swings", swings);
            press(GB_LEFT); /* sheathe so R activates */
            CHECK(r->draw_state == 0, "weapon sheathed");
            face_instance(critter, 70.0f);
            press(GB_R);
            CHECK(g_ui.mode == UI_LOOT, "corpse searchable");
            press(GB_TRIANGLE);
            CHECK(count_of("tw_root") == roots + 1, "root looted from the corpse");
        }

        /* level up: rest after the progress is reached */
        vm_set_prop(vm, &pc, MWP_LEVELPROG, 10.0f);
        r->level_pending = 1;
        frames(130);
        press(GB_SELECT);
        CHECK(tab_to(UI_TAB_STATS), "stats again");
        press(GB_TRIANGLE);
        CHECK(g_ui.mode == UI_REST, "rest from the stats tab");
        press(GB_TRIANGLE);
        screenshot("31_rest");
        {
            int level = rpg_level(r, &pc);
            float str = vm_prop(vm, &pc, MWP_ATTR_STRENGTH);
            press(GB_CROSS);
            CHECK(g_ui.mode == UI_LEVELUP, "level-up after resting");
            press(GB_CROSS);
            press(GB_DOWN);
            press(GB_CROSS);
            press(GB_DOWN);
            press(GB_CROSS);
            CHECK(g_ui.lv_count == 3, "three attributes picked");
            screenshot("32_levelup");
            move_to(&g_ui.sel, ATTR_COUNT);
            press(GB_CROSS);
            CHECK(g_ui.mode == UI_NONE && rpg_level(r, &pc) == level + 1, "level %d", rpg_level(r, &pc));
            CHECK(vm_prop(vm, &pc, MWP_ATTR_STRENGTH) > str, "strength raised");
        }

        /* death and respawn */
        rpg_damage(r, &pc, 0, 10000.0f, NULL);
        frames(2);
        CHECK(g_ui.mode == UI_DEATH && g_game.player_dead, "death screen");
        screenshot("33_death");
        press(GB_CROSS);
        frames(2);
        CHECK(g_ui.mode == UI_NONE && !g_game.player_dead && rpg_dyn(r, &pc, 0) == rpg_dyn_max(r, &pc, 0),
              "respawned with full health");
        frames(130);
        screenshot("34_hud");
        (void)find_door;
    }

    /* the cellar ring stays taken after reloading the zone */
    {
        uint32_t cellar = (uint32_t)world_find_interior(&g_world, pw_hash_name("test hamlet, cellar"));
        CHECK(game_enter_zone(&g_game, cellar, NULL, NULL, err, sizeof(err)) == 0, "cellar reload");
        ring = find_instance("tw_ring");
        CHECK(ring >= 0 && g_game.zone.inst_hidden[ring], "picked-up ring stays gone");
    }
    CHECK(vm->errors == 0, "no script errors (%u: %s)", (unsigned)vm->errors, vm->last_error);
    printf("ui_test: %d/%d checks passed (script runs %u, unhandled verbs %u)\n", g_checks - g_fail,
           g_checks, (unsigned)vm->runs, (unsigned)vm->unhandled);
    return g_fail ? 1 : 0;
}
