/*
 * ui_test: plays "The Weaver's Lost Ring" end to end through the real game
 * loop and UI with button presses, the way main.c drives them on the PSP:
 * walk up to NPCs, talk, follow a hyperlink, answer a choice, open the
 * journal, read and take a book, pick up the ring through its OnActivate
 * script (message box), and claim the reward. Screens are rendered by the
 * host UI backend into PPM images for inspection.
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
static void frame(uint32_t pressed)
{
    InputState in, none;
    char err[160];
    memset(&in, 0, sizeof(in));
    memset(&none, 0, sizeof(none));
    in.pressed = pressed;
    in.held = pressed;
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

/* moves the topic cursor to a topic by name and asks it */
static int ask(const char *topic)
{
    int t = rpgdb_topic(&g_rpg.db, pw_hash_name(topic)), i, guard = 0;
    for (i = 0; i < (int)g_rpg.dlg.list_count; ++i)
        if ((int)g_rpg.dlg.list[i] == t)
            break;
    if (i == (int)g_rpg.dlg.list_count)
        return 0;
    while (g_ui.sel_topic != i && guard++ < 64)
        press(g_ui.sel_topic < i ? GB_DOWN : GB_UP);
    press(GB_CROSS);
    return 1;
}

static void close_dialogue(void)
{
    press(GB_CIRCLE);
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
    CHECK(g_ui.mode == UI_MENU && g_ui.tab == UI_TAB_JOURNAL, "journal open");
    screenshot("07_journal");
    press(GB_R);
    CHECK(g_ui.tab == UI_TAB_QUESTS && g_ui.item_count == 1, "one quest listed");
    press(GB_CROSS);
    CHECK(g_ui.detail >= 0, "quest details");
    screenshot("08_quest");
    press(GB_CIRCLE);
    press(GB_R);
    CHECK(g_ui.tab == UI_TAB_TOPICS && g_ui.item_count >= 2, "heard topics (%u)", (unsigned)g_ui.item_count);
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
    CHECK(strstr(last_response(), "Hello again, Outlander") != NULL, "second greeting");
    CHECK(ask("lost ring"), "lost ring still listed");
    CHECK(strcmp(last_response(), "You found it! Here, take this gold.") == 0, "reward ('%s')", last_response());
    screenshot("11_reward");
    CHECK(vm_item_count(vm, vm->player_hash, pw_hash_name("gold_001")) == 50, "50 gold");
    CHECK(vm_journal_index(vm, pw_hash_name("tw_lostring")) == 100, "quest finished");
    close_dialogue();
    frames(130); /* toasts expire */
    press(GB_SELECT);
    screenshot("12_journal_done");
    press(GB_SELECT);

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
