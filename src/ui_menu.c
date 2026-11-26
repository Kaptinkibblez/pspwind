/*
 * ui_menu.c - SELECT menu tabs: Stats, Inventory, Magic and Map (the
 * Journal tab lives in ui.c).
 *
 *   Stats      attributes, dynamic stats, level progress, skills grouped
 *              major / minor / misc with progress bars, factions;
 *              triangle opens rest / wait
 *   Inventory  filters (All, Weapons, Apparel, Magic, Misc) on left/right,
 *              cross uses the item (equip, drink, eat, read, alchemy,
 *              repair), details of the selected stack on the right
 *   Magic      powers, spells and enchanted items with cost and chance;
 *              cross selects what R casts; active effects on the right
 *   Map        exterior cell grid (visited cells lit), the player's
 *              position; D-pad pans, triangle recentres
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define CONTENT_Y 30
#define FOOTER_Y 256
#define LIST_ROWS 22

static Vm *mvm(Ui *ui)
{
    return &ui->g->rpg->vm;
}

static void footer(UiCanvas *c, const char *text)
{
    ui_rect(c, 10, FOOTER_Y - 3, 460, 1, C_BORDER);
    ui_text(c, 12, FOOTER_Y, C_DIM, text, -1);
}

/* ------------------------------------------------------------------------ */
/* stats                                                                    */
/* ------------------------------------------------------------------------ */
#define ROW_HEADER 0xF000u
#define ROW_FACTION 0x1000u
#define STAT_ROWS 24

static void build_stats(Ui *ui)
{
    Rpg *r = ui_rpg(ui);
    const RpgDb *db = r->db;
    int type, s;
    uint32_t i, n = 0;
    static const int order[3] = { 0, 1, 2 };
    for (type = 0; type < 3; ++type) {
        ui->items[n++] = (uint16_t)(ROW_HEADER | (uint32_t)order[type]);
        for (s = 0; s < SKILL_COUNT; ++s)
            if (rpg_skill_type(r, s) == type)
                ui->items[n++] = (uint16_t)s;
    }
    {
        int any = 0;
        for (i = 0; i < db->hdr->factions.count && n < 500; ++i) {
            const DRpgFaction *f = &db->factions[i];
            if (vm_pc_rank(mvm(ui), f->hash) < 0)
                continue;
            if (!any) {
                ui->items[n++] = (uint16_t)(ROW_HEADER | 3u);
                any = 1;
            }
            ui->items[n++] = (uint16_t)(ROW_FACTION | i);
        }
    }
    ui->item_count = n;
}

static const char *object_name(Rpg *r, uint32_t hash, const char *fallback)
{
    const DRpgObject *o = hash ? rpg_object(r, hash) : NULL;
    const char *name = o ? rpg_name(r, o) : "";
    return *name ? name : fallback;
}

static void draw_stats(Ui *ui, UiCanvas *c)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    char buf[96];
    int i, a, y = CONTENT_Y;
    static const char *const dyn_names[3] = { "Health", "Magicka", "Fatigue" };
    static const uint32_t dyn_cols[3] = { C_HEALTH, C_MAGICKA, C_FATIGUE };
    static const char *const headers[4] = { "Major Skills", "Minor Skills", "Misc Skills", "Factions" };
    float prog = vm_prop(mvm(ui), &pc, MWP_LEVELPROG);
    int total = rpg_gmst_i(r, "iLevelUpTotal", 10);
    ui_text_clipped(c, 12, y, C_TITLE, mvm(ui)->pc_name[0] ? mvm(ui)->pc_name : "Player", 32);
    y += UI_LINE_H;
    snprintf(buf, sizeof(buf), "Level %d  %s", rpg_level(r, &pc), object_name(r, r->race, "-"));
    ui_text_clipped(c, 12, y, C_TEXT, buf, 32);
    y += UI_LINE_H;
    snprintf(buf, sizeof(buf), "%s, %s", object_name(r, r->cls, "-"), object_name(r, r->sign, "-"));
    ui_text_clipped(c, 12, y, C_TEXT, buf, 32);
    y += UI_LINE_H + 3;
    for (i = 0; i < 3; ++i) {
        float cur = rpg_dyn(r, &pc, i), max = rpg_dyn_max(r, &pc, i);
        ui_text(c, 12, y, C_DIM, dyn_names[i], -1);
        ui_bar(c, 62, y + 1, 78, 6, cur, max, dyn_cols[i]);
        snprintf(buf, sizeof(buf), "%d/%d", (int)ceilf(cur), (int)max);
        ui_text(c, 146, y, C_TEXT, buf, -1);
        y += UI_LINE_H + 1;
    }
    ui_text(c, 12, y, C_DIM, "Level", -1);
    ui_bar(c, 62, y + 1, 78, 6, prog, (float)total, C_TITLE);
    snprintf(buf, sizeof(buf), "%d/%d", (int)prog, total);
    ui_text(c, 146, y, C_TEXT, buf, -1);
    y += UI_LINE_H + 4;
    for (a = 0; a < ATTR_COUNT; ++a) {
        float mod = rpg_attr(r, &pc, a), base = vm_prop(mvm(ui), &pc, MWP_ATTR_STRENGTH + a);
        uint32_t col = mod > base + 0.5f ? C_PLAYER : mod < base - 0.5f ? C_BAD : C_TEXT;
        ui_text(c, 12, y, C_DIM, rpg_attr_name(r, a), -1);
        snprintf(buf, sizeof(buf), "%3d", (int)mod);
        ui_text(c, 110, y, col, buf, -1);
        y += UI_LINE_H;
    }
    y += 4;
    snprintf(buf, sizeof(buf), "Encumbrance %d/%d", (int)rpg_encumbrance(r, &pc), (int)rpg_capacity(r, &pc));
    ui_text(c, 12, y, C_TEXT, buf, -1);
    y += UI_LINE_H;
    snprintf(buf, sizeof(buf), "Armor rating %d", (int)rpg_armor_rating(r, &pc));
    ui_text(c, 12, y, C_TEXT, buf, -1);
    y += UI_LINE_H;
    snprintf(buf, sizeof(buf), "Reputation %d", (int)vm_prop(mvm(ui), &pc, MWP_REPUTATION));
    ui_text(c, 12, y, C_TEXT, buf, -1);
    y += UI_LINE_H + 2;
    if (r->level_pending)
        ui_text(c, 12, y, C_NOTICE, "Rest to level up", -1);
    /* right column: skills and factions */
    ui_rect(c, 204, CONTENT_Y - 2, 1, STAT_ROWS * UI_LINE_H, C_BORDER);
    for (i = 0; i < STAT_ROWS && ui->list_top + i < (int)ui->item_count; ++i) {
        int idx = ui->list_top + i, row = ui->items[idx];
        int yy = CONTENT_Y + i * UI_LINE_H;
        if (idx == ui->sel)
            ui_row_select(c, 212, yy, 42);
        if ((row & ROW_HEADER) == ROW_HEADER) {
            ui_text(c, 212, yy, C_TITLE, headers[row & 3], -1);
        } else if (row & ROW_FACTION) {
            const DRpgFaction *f = &r->db->factions[row & 0xFFF];
            int rank = vm_pc_rank(mvm(ui), f->hash);
            const char *rn = rank >= 0 && rank < 10 ? rpgdb_name(r->db, f->rank_name[rank]) : "";
            ui_text_clipped(c, 216, yy, C_TEXT, rpgdb_name(r->db, f->name), 22);
            ui_text_clipped(c, 352, yy, vm_pc_expelled(mvm(ui), f->hash) ? C_BAD : C_DIM, rn, 19);
        } else {
            float mod = rpg_skill(r, &pc, row), base = vm_prop(mvm(ui), &pc, MWP_SKILL_BLOCK + row);
            uint32_t col = mod > base + 0.5f ? C_PLAYER : mod < base - 0.5f ? C_BAD : C_TEXT;
            ui_text_clipped(c, 216, yy, C_TEXT, rpg_skill_name(r, row), 24);
            snprintf(buf, sizeof(buf), "%3d", (int)mod);
            ui_text(c, 372, yy, col, buf, -1);
            ui_bar(c, 396, yy + 2, 60, 4, rpg_skill_progress(r, row), 1.0f, C_FATIGUE);
        }
    }
    ui_scrollbar(c, 464, CONTENT_Y, STAT_ROWS * UI_LINE_H, (int)ui->item_count, ui->list_top, STAT_ROWS);
    buf[0] = '\0';
    if (ui->sel < (int)ui->item_count) {
        int row = ui->items[ui->sel];
        if ((row & (ROW_HEADER | ROW_FACTION)) == 0 && r->db->skills) {
            static const char *const spec[3] = { "Combat", "Magic", "Stealth" };
            const DRpgSkill *sk = &r->db->skills[row];
            snprintf(buf, sizeof(buf), "%s: %s, %s.  ", rpg_skill_name(r, row), rpg_attr_name(r, sk->attribute),
                     spec[sk->specialization % 3]);
        }
    }
    {
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof(buf) - n, "%s", (ui->g->menus_enabled & GAME_MENU_REST) ? "/\\ rest/wait" : "");
    }
    footer(c, buf);
}

static void update_stats(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    if (p & GB_CIRCLE) {
        ui_close(ui);
        return;
    }
    if ((p & GB_TRIANGLE) && (ui->g->menus_enabled & GAME_MENU_REST)) {
        ui->return_mode = UI_MENU;
        ui_open_rest(ui);
        return;
    }
    ui_list_nav(&ui->sel, &ui->list_top, (int)ui->item_count, STAT_ROWS, p);
}

/* ------------------------------------------------------------------------ */
/* inventory                                                                */
/* ------------------------------------------------------------------------ */
enum { F_ALL = 0, F_WEAPON, F_APPAREL, F_MAGIC, F_MISC, F_COUNT };
static const char *const filter_names[F_COUNT] = { "All", "Weapons", "Apparel", "Magic", "Misc" };

static int item_order(int type)
{
    static const int order[DRPG_OT_COUNT] = { 20, 20, 0, 1, 2, 5, 3, 4, 11, 6, 7, 8, 9, 10, 20, 20, 20, 20,
                                              20, 20, 20, 20, 20, 20 };
    return type >= 0 && type < DRPG_OT_COUNT ? order[type] : 20;
}

static int enchanted(const DRpgObject *o)
{
    return o->type >= DRPG_OT_WEAP && o->type <= DRPG_OT_BOOK && o->link[0] != 0 && o->link[0] != DRPG_NONE;
}

static int filter_match(const DRpgObject *o, int filter)
{
    switch (filter) {
    case F_WEAPON:
        return o->type == DRPG_OT_WEAP;
    case F_APPAREL:
        return o->type == DRPG_OT_ARMO || o->type == DRPG_OT_CLOT;
    case F_MAGIC:
        return enchanted(o);
    case F_MISC:
        return o->type != DRPG_OT_WEAP && o->type != DRPG_OT_ARMO && o->type != DRPG_OT_CLOT && !enchanted(o);
    default:
        return 1;
    }
}

static int stack_before(Ui *ui, int a, int b)
{
    Rpg *r = ui_rpg(ui);
    const VmItem *ia = vm_item_at(mvm(ui), a), *ib = vm_item_at(mvm(ui), b);
    const DRpgObject *oa = rpg_object(r, ia->item), *ob = rpg_object(r, ib->item);
    VmRef ra, rb;
    int ea, eb, c;
    memset(&ra, 0, sizeof(ra));
    memset(&rb, 0, sizeof(rb));
    ra.key = ia->owner;
    rb.key = ib->owner;
    ea = rpg_is_equipped(r, &ra, a);
    eb = rpg_is_equipped(r, &rb, b);
    if (ea != eb)
        return ea > eb;
    if (item_order(oa->type) != item_order(ob->type))
        return item_order(oa->type) < item_order(ob->type);
    c = strcmp(rpg_name(r, oa), rpg_name(r, ob));
    return c != 0 ? c < 0 : a < b;
}

uint32_t ui_list_stacks(Ui *ui, uint32_t owner, int filter, int32_t *out, uint32_t max, int with_gold)
{
    Rpg *r = ui_rpg(ui);
    uint32_t cursor = 0, item, n = 0, gold = pw_hash_name(RPG_GOLD), i;
    int32_t count;
    int slot;
    while ((slot = vm_item_next(mvm(ui), owner, &cursor, &item, &count)) >= 0 && n < max) {
        const DRpgObject *o = rpg_object(r, item);
        if (!o || (item == gold && !with_gold) || (filter >= 0 && !filter_match(o, filter)))
            continue;
        out[n++] = slot;
    }
    for (i = 1; i < n; ++i) {
        int32_t v = out[i];
        uint32_t j = i;
        while (j > 0 && stack_before(ui, v, out[j - 1])) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = v;
    }
    return n;
}

static void build_inventory(Ui *ui)
{
    VmRef pc = ui_pc(ui);
    ui->stack_count = ui_list_stacks(ui, pc.key, ui->filter, ui->stacks, 256, 0);
    if (ui->sel >= (int)ui->stack_count)
        ui->sel = PW_MAX(0, (int)ui->stack_count - 1);
    ui_list_nav(&ui->sel, &ui->list_top, (int)ui->stack_count, LIST_ROWS, 0);
}

static void update_inventory(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    if (p & GB_CIRCLE) {
        ui_close(ui);
        return;
    }
    if (p & (GB_LEFT | GB_RIGHT)) {
        ui->filter = (ui->filter + ((p & GB_RIGHT) ? 1 : F_COUNT - 1)) % F_COUNT;
        ui->sel = 0;
        ui->list_top = 0;
        build_inventory(ui);
        return;
    }
    ui_list_nav(&ui->sel, &ui->list_top, (int)ui->stack_count, LIST_ROWS, p);
    if ((p & GB_CROSS) && ui->sel < (int)ui->stack_count) {
        char msg[96];
        int stack = ui->stacks[ui->sel];
        const VmItem *it = vm_item_at(mvm(ui), stack);
        uint32_t item = it ? it->item : 0;
        int res;
        if (it && it->soul && rpg_is_soulgem(ui_rpg(ui), rpg_object(ui_rpg(ui), item))) {
            ui->return_mode = UI_MENU;
            ui_open_enchant(ui, 1, stack);
            return;
        }
        res = rpg_use(ui_rpg(ui), stack, msg, sizeof(msg));
        switch (res) {
        case RPG_USE_BOOK:
            ui->return_mode = UI_MENU;
            ui_open_book(ui, item, GAME_NO_TARGET);
            return;
        case RPG_USE_ALCHEMY:
            ui->return_mode = UI_MENU;
            ui_open_alchemy(ui);
            return;
        case RPG_USE_REPAIR:
            ui->return_mode = UI_MENU;
            ui_open_repair(ui, stack);
            return;
        default:
            break;
        }
        if (msg[0])
            ui_status(ui, msg);
        build_inventory(ui);
    }
}

static void draw_inventory(Ui *ui, UiCanvas *c)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    char buf[96];
    int i, x = 12;
    for (i = 0; i < F_COUNT; ++i) {
        int w = (int)strlen(filter_names[i]) * UI_CHAR_W + 8;
        if (i == ui->filter)
            ui_rect(c, x, CONTENT_Y - 2, w, 11, C_SEL_BG);
        ui_text(c, x + 4, CONTENT_Y, i == ui->filter ? C_TITLE : C_DIM, filter_names[i], -1);
        x += w + 2;
    }
    for (i = 0; i < LIST_ROWS && ui->list_top + i < (int)ui->stack_count; ++i) {
        int idx = ui->list_top + i, stack = ui->stacks[idx], y = CONTENT_Y + 14 + i * UI_LINE_H;
        if (idx == ui->sel)
            ui_row_select(c, 12, y, 41);
        if (rpg_is_equipped(r, &pc, stack))
            ui_text(c, 12, y, C_NOTICE, "*", 1);
        ui_stack_label(ui, stack, buf, sizeof(buf));
        ui_text_clipped(c, 20, y, C_TEXT, buf, 40);
    }
    if (!ui->stack_count)
        ui_text(c, 20, CONTENT_Y + 14, C_DIM, "(nothing)", -1);
    ui_scrollbar(c, 260, CONTENT_Y + 14, LIST_ROWS * UI_LINE_H, (int)ui->stack_count, ui->list_top, LIST_ROWS);
    ui_rect(c, 264, CONTENT_Y + 12, 1, LIST_ROWS * UI_LINE_H, C_BORDER);
    if (ui->sel < (int)ui->stack_count)
        ui_item_details(ui, ui->stacks[ui->sel], 270, CONTENT_Y + 14, 32, 18, c);
    snprintf(buf, sizeof(buf), "Gold %d   Weight %d/%d", rpg_gold(r, &pc), (int)rpg_encumbrance(r, &pc),
             (int)rpg_capacity(r, &pc));
    ui_text(c, 270, FOOTER_Y - 14, C_TEXT, buf, -1);
    footer(c, "X use/equip   <- -> filter   O close   * equipped");
}

/* ------------------------------------------------------------------------ */
/* magic                                                                    */
/* ------------------------------------------------------------------------ */
enum { MK_POWER = 0, MK_SPELL = 1, MK_ITEM = 2, MK_HEADER = 3 };

static void build_magic(Ui *ui)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    uint32_t cursor, item, n = 0, i;
    int32_t count, tmp[256];
    int pass;
    for (pass = 0; pass < 2; ++pass) {
        int header_at = -1;
        cursor = 0;
        while (vm_item_next(mvm(ui), pc.key ^ VM_SPELL_SALT, &cursor, &item, &count) >= 0 && n < 95) {
            const DRpgSpell *s = rpg_spell(r, item);
            if (!s || s->kind != 0 || s->type != (pass == 0 ? 5 : 0))
                continue;
            if (header_at < 0) {
                header_at = (int)n;
                ui->spell_kind[n] = MK_HEADER;
                ui->spells[n++] = (uint32_t)pass;
            }
            ui->spell_kind[n] = (uint8_t)pass;
            ui->spells[n++] = item;
        }
        /* alphabetical within the section */
        if (header_at >= 0)
            for (i = (uint32_t)header_at + 2; i < n; ++i) {
                uint32_t v = ui->spells[i], j = i;
                while (j > (uint32_t)header_at + 1 &&
                       strcmp(rpg_spell_name(r, v), rpg_spell_name(r, ui->spells[j - 1])) < 0) {
                    ui->spells[j] = ui->spells[j - 1];
                    --j;
                }
                ui->spells[j] = v;
            }
    }
    {
        uint32_t k, m = ui_list_stacks(ui, pc.key, F_MAGIC, tmp, 256, 0);
        int header = 0;
        for (k = 0; k < m && n < 95; ++k) {
            const VmItem *it = vm_item_at(mvm(ui), tmp[k]);
            const DRpgObject *o = rpg_object(r, it->item);
            const DRpgSpell *en = o ? rpg_spell(r, o->link[0]) : NULL;
            if (!en || (en->type != 0 && en->type != 2))
                continue;
            if (!header) {
                ui->spell_kind[n] = MK_HEADER;
                ui->spells[n++] = 2;
                header = 1;
            }
            ui->spell_kind[n] = MK_ITEM;
            ui->spells[n++] = (uint32_t)tmp[k];
        }
    }
    ui->spell_count = n;
    if (ui->sel >= (int)n)
        ui->sel = PW_MAX(0, (int)n - 1);
    if (n && ui->spell_kind[ui->sel] == MK_HEADER && ui->sel + 1 < (int)n)
        ++ui->sel;
    ui_list_nav(&ui->sel, &ui->list_top, (int)n, LIST_ROWS, 0);
}

static void update_magic(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    int guard;
    if (p & GB_CIRCLE) {
        ui_close(ui);
        return;
    }
    ui_list_nav(&ui->sel, &ui->list_top, (int)ui->spell_count, LIST_ROWS, p);
    /* skip section headers in the direction of travel */
    for (guard = 0; guard < 4 && ui->spell_count > 0 && ui->spell_kind[ui->sel] == MK_HEADER; ++guard)
        ui_list_nav(&ui->sel, &ui->list_top, (int)ui->spell_count, LIST_ROWS, (p & GB_UP) ? GB_UP : GB_DOWN);
    if ((p & GB_CROSS) && ui->sel < (int)ui->spell_count) {
        Rpg *r = ui_rpg(ui);
        int kind = ui->spell_kind[ui->sel], ok;
        char buf[96];
        if (kind == MK_HEADER)
            return;
        ok = kind == MK_ITEM ? rpg_select_item(r, (int)ui->spells[ui->sel])
                             : rpg_select_spell(r, ui->spells[ui->sel]);
        if (ok) {
            const char *name;
            if (kind == MK_ITEM) {
                const VmItem *it = vm_item_at(mvm(ui), (int)ui->spells[ui->sel]);
                name = rpg_name(r, rpg_object(r, it->item));
            } else {
                name = rpg_spell_name(r, ui->spells[ui->sel]);
            }
            snprintf(buf, sizeof(buf), "%s readied for casting (D-pad right)", name);
            ui_status(ui, buf);
        }
    }
}

static void draw_magic(Ui *ui, UiCanvas *c)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    static const char *const headers[3] = { "Powers", "Spells", "Magic Items" };
    char buf[96];
    int i, y;
    ui_text(c, 12, CONTENT_Y, C_DIM, "Name", -1);
    ui_text(c, 182, CONTENT_Y, C_DIM, "Cost/Chance", -1);
    for (i = 0; i < LIST_ROWS && ui->list_top + i < (int)ui->spell_count; ++i) {
        int idx = ui->list_top + i, kind = ui->spell_kind[idx];
        uint32_t v = ui->spells[idx];
        int selected;
        y = CONTENT_Y + 12 + i * UI_LINE_H;
        if (kind == MK_HEADER) {
            ui_text(c, 12, y, C_TITLE, headers[v % 3], -1);
            continue;
        }
        if (idx == ui->sel)
            ui_row_select(c, 12, y, 41);
        selected = kind == MK_ITEM ? r->sel_item == (int32_t)v : (r->sel_item < 0 && r->sel_spell == v);
        if (selected)
            ui_text(c, 12, y, C_NOTICE, ">", 1);
        if (kind == MK_ITEM) {
            const VmItem *it = vm_item_at(mvm(ui), (int)v);
            const DRpgObject *o = rpg_object(r, it->item);
            float max = rpg_item_charge_max(r, o), cur = it->charge < 0.0f ? max : it->charge;
            const DRpgSpell *en = rpg_spell(r, o->link[0]);
            ui_text_clipped(c, 20, y, C_TEXT, rpg_name(r, o), 26);
            if (en && en->type == 0)
                snprintf(buf, sizeof(buf), "x%d", (int)it->count);
            else
                snprintf(buf, sizeof(buf), "%d/%d", (int)cur, (int)max);
        } else {
            const DRpgSpell *s = rpg_spell(r, v);
            ui_text_clipped(c, 20, y, C_TEXT, rpg_spell_name(r, v), 26);
            if (kind == MK_POWER)
                snprintf(buf, sizeof(buf), "power");
            else
                snprintf(buf, sizeof(buf), "%d/%d%%", rpg_spell_cost(r, s),
                         (int)PW_CLAMP(rpg_spell_chance(r, &pc, s), 0.0f, 100.0f));
        }
        ui_text(c, 182, y, C_DIM, buf, -1);
    }
    if (!ui->spell_count)
        ui_text(c, 20, CONTENT_Y + 12, C_DIM, "(no spells)", -1);
    ui_scrollbar(c, 260, CONTENT_Y + 12, LIST_ROWS * UI_LINE_H, (int)ui->spell_count, ui->list_top, LIST_ROWS);
    ui_rect(c, 264, CONTENT_Y, 1, (LIST_ROWS + 1) * UI_LINE_H, C_BORDER);
    /* right: effects of the selection, then the active effects */
    y = CONTENT_Y;
    if (ui->sel < (int)ui->spell_count && ui->spell_kind[ui->sel] != MK_HEADER) {
        int kind = ui->spell_kind[ui->sel], n = 0, k;
        const DRpgEffect *e = NULL;
        if (kind == MK_ITEM) {
            const VmItem *it = vm_item_at(mvm(ui), (int)ui->spells[ui->sel]);
            const DRpgObject *o = rpg_object(r, it->item);
            const DRpgSpell *en = rpg_spell(r, o->link[0]);
            ui_text_clipped(c, 270, y, C_TITLE, rpg_name(r, o), 32);
            if (en)
                e = rpg_spell_effects(r, en, &n);
        } else {
            const DRpgSpell *s = rpg_spell(r, ui->spells[ui->sel]);
            ui_text_clipped(c, 270, y, C_TITLE, rpg_spell_name(r, ui->spells[ui->sel]), 32);
            if (s)
                e = rpg_spell_effects(r, s, &n);
        }
        y += UI_LINE_H;
        for (k = 0; k < n && k < 8; ++k) {
            rpg_describe_effect(r, &e[k], buf, sizeof(buf));
            ui_doc_reset(&ui->doc, 32);
            ui_doc_add(&ui->doc, K_TEXT, buf, NULL, 0);
            ui_draw_doc(c, &ui->doc, 270, y, (int)PW_MIN(ui->doc.line_count, 2u), 0, -1);
            y += (int)PW_MIN(ui->doc.line_count, 2u) * UI_LINE_H;
        }
    }
    y = PW_MAX(y + 6, CONTENT_Y + 9 * UI_LINE_H);
    ui_text(c, 270, y, C_TITLE, "Active effects", -1);
    y += UI_LINE_H;
    {
        uint32_t f;
        int shown = 0;
        for (f = 0; f < r->fx_count && y < FOOTER_Y - 12; ++f) {
            const RpgFx *fx = &r->fx[f];
            const char *en = rpg_effect_name(r, fx->effect);
            int n;
            if (fx->key != pc.key)
                continue;
            n = snprintf(buf, sizeof(buf), "%s", en);
            if (fx->arg >= 0 && n < (int)sizeof(buf)) {
                int skill_fx = fx->effect == MGEF_FORTIFY_SKILL || fx->effect == MGEF_DRAIN_SKILL ||
                               fx->effect == MGEF_DAMAGE_SKILL || fx->effect == MGEF_RESTORE_SKILL ||
                               fx->effect == MGEF_ABSORB_SKILL;
                n += snprintf(buf + n, sizeof(buf) - (uint32_t)n, " %s",
                              skill_fx ? rpg_skill_name(r, fx->arg) : rpg_attr_name(r, fx->arg));
            }
            if (fx->magnitude > 0.0f && n < (int)sizeof(buf))
                n += snprintf(buf + n, sizeof(buf) - (uint32_t)n, " %d", (int)fx->magnitude);
            if (!(fx->flags & RFX_CONSTANT) && n < (int)sizeof(buf))
                snprintf(buf + n, sizeof(buf) - (uint32_t)n, " (%ds)", (int)ceilf(fx->remaining));
            ui_text_clipped(c, 274, y, (fx->flags & RFX_HARMFUL) ? C_BAD : C_PLAYER, buf, 31);
            y += UI_LINE_H;
            ++shown;
        }
        if (!shown)
            ui_text(c, 274, y, C_DIM, "(none)", -1);
    }
    footer(c, "X select for casting (D-pad right readies)   O close");
}

/* ------------------------------------------------------------------------ */
/* map                                                                      */
/* ------------------------------------------------------------------------ */
#define MAP_CELL 16
#define MAP_X 12
#define MAP_Y (CONTENT_Y + 12)
#define MAP_COLS 28
#define MAP_ROWS 12

static int player_cell(Ui *ui, int *cx, int *cy)
{
    Game *g = ui->g;
    *cx = (int)floorf(g->body.pos.x / 8192.0f);
    *cy = (int)floorf(g->body.pos.y / 8192.0f);
    return g->zone.hdr && g->zone.hdr->kind == DZONE_KIND_EXTERIOR;
}

static void map_centre(Ui *ui, int *cx, int *cy)
{
    Rpg *r = ui_rpg(ui);
    if (!player_cell(ui, cx, cy)) {
        /* indoors: centre on the explored area */
        int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        uint32_t i;
        for (i = 0; i < r->visited_count; ++i) {
            int x = (int)(r->visited[i] >> 16) - 32768, y = (int)(r->visited[i] & 0xFFFF) - 32768;
            if (i == 0 || x < x0)
                x0 = x;
            if (i == 0 || x > x1)
                x1 = x;
            if (i == 0 || y < y0)
                y0 = y;
            if (i == 0 || y > y1)
                y1 = y;
        }
        *cx = (x0 + x1) / 2;
        *cy = (y0 + y1) / 2;
    }
    *cx += ui->map_dx;
    *cy += ui->map_dy;
}

static void update_map(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    if (p & GB_CIRCLE) {
        ui_close(ui);
        return;
    }
    if (p & GB_LEFT)
        --ui->map_dx;
    if (p & GB_RIGHT)
        ++ui->map_dx;
    if (p & GB_UP)
        ++ui->map_dy;
    if (p & GB_DOWN)
        --ui->map_dy;
    if (p & GB_TRIANGLE)
        ui->map_dx = ui->map_dy = 0;
}

static void draw_map(Ui *ui, UiCanvas *c)
{
    Rpg *r = ui_rpg(ui);
    Game *g = ui->g;
    char buf[96];
    int cx, cy, px, py, x0, y1, outside = player_cell(ui, &px, &py);
    uint32_t i;
    map_centre(ui, &cx, &cy);
    x0 = cx - MAP_COLS / 2;
    y1 = cy + MAP_ROWS / 2; /* top row (north up) */
    snprintf(buf, sizeof(buf), "%s%s", outside ? "" : "Inside: ", game_cell_name(g));
    ui_text_clipped(c, 12, CONTENT_Y, C_TITLE, buf, 50);
    snprintf(buf, sizeof(buf), "(%d, %d)", cx, cy);
    ui_text(c, 470 - (int)strlen(buf) * UI_CHAR_W, CONTENT_Y, C_DIM, buf, -1);
    ui_rect(c, MAP_X, MAP_Y, MAP_COLS * MAP_CELL, MAP_ROWS * MAP_CELL, 0xC0281C14u);
    /* land known to the world, then explored cells */
    if (g->world && g->world->hdr) {
        for (i = 0; i < g->world->hdr->ext_count; ++i) {
            const DWorldExt *e = &g->world->ext[i];
            int col = e->x - x0, row = y1 - e->y;
            if (col < 0 || col >= MAP_COLS || row < 0 || row >= MAP_ROWS)
                continue;
            ui_rect(c, MAP_X + col * MAP_CELL + 1, MAP_Y + row * MAP_CELL + 1, MAP_CELL - 2, MAP_CELL - 2,
                    0xFF403830u);
        }
    }
    for (i = 0; i < r->visited_count; ++i) {
        int x = (int)(r->visited[i] >> 16) - 32768, y = (int)(r->visited[i] & 0xFFFF) - 32768;
        int col = x - x0, row = y1 - y;
        if (col < 0 || col >= MAP_COLS || row < 0 || row >= MAP_ROWS)
            continue;
        ui_rect(c, MAP_X + col * MAP_CELL + 1, MAP_Y + row * MAP_CELL + 1, MAP_CELL - 2, MAP_CELL - 2,
                0xFF4C7C8Cu);
    }
    if (outside) {
        float fx = g->body.pos.x / 8192.0f - (float)x0, fy = (float)(y1 + 1) - g->body.pos.y / 8192.0f;
        int sx = MAP_X + (int)(fx * MAP_CELL), sy = MAP_Y + (int)(fy * MAP_CELL);
        if (fx >= 0.0f && fx < MAP_COLS && fy >= 0.0f && fy < MAP_ROWS) {
            float dx = -sinf(g->yaw), dy = cosf(g->yaw);
            ui_rect(c, sx - 2, sy - 2, 5, 5, C_BAD);
            ui_rect(c, sx + (int)(dx * 5.0f) - 1, sy - (int)(dy * 5.0f) - 1, 3, 3, C_TITLE);
        }
    }
    if (r->has_mark && r->mark_cell == 0) {
        float fx = r->mark_pos[0] / 8192.0f - (float)x0, fy = (float)(y1 + 1) - r->mark_pos[1] / 8192.0f;
        if (fx >= 0.0f && fx < MAP_COLS && fy >= 0.0f && fy < MAP_ROWS)
            ui_text(c, MAP_X + (int)(fx * MAP_CELL) - 3, MAP_Y + (int)(fy * MAP_CELL) - 4, C_LINK, "M", 1);
    }
    snprintf(buf, sizeof(buf), "%u cells explored", (unsigned)r->visited_count);
    ui_text(c, 12, MAP_Y + MAP_ROWS * MAP_CELL + 4, C_DIM, buf, -1);
    footer(c, "D-pad pan   /\\ recentre   O close   red: you  M: mark");
}

/* ------------------------------------------------------------------------ */
/* dispatch                                                                 */
/* ------------------------------------------------------------------------ */
void ui_menu_build(Ui *ui)
{
    switch (ui->tab) {
    case UI_TAB_STATS:
        build_stats(ui);
        break;
    case UI_TAB_INVENTORY:
        build_inventory(ui);
        break;
    case UI_TAB_MAGIC:
        build_magic(ui);
        break;
    default:
        break;
    }
}

void ui_menu_update(Ui *ui, const InputState *in)
{
    switch (ui->tab) {
    case UI_TAB_STATS:
        update_stats(ui, in);
        break;
    case UI_TAB_INVENTORY:
        update_inventory(ui, in);
        break;
    case UI_TAB_MAGIC:
        update_magic(ui, in);
        break;
    case UI_TAB_MAP:
        update_map(ui, in);
        break;
    default:
        break;
    }
}

void ui_menu_draw(Ui *ui, UiCanvas *c)
{
    switch (ui->tab) {
    case UI_TAB_STATS:
        draw_stats(ui, c);
        break;
    case UI_TAB_INVENTORY:
        draw_inventory(ui, c);
        break;
    case UI_TAB_MAGIC:
        draw_magic(ui, c);
        break;
    case UI_TAB_MAP:
        draw_map(ui, c);
        break;
    default:
        break;
    }
}
