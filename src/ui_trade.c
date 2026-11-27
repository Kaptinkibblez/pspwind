/*
 * ui_trade.c - item exchange and service screens.
 *
 *   Loot         container / corpse on the left, player on the right;
 *                left/right switch side, cross moves the stack across,
 *                triangle takes everything, circle closes
 *   Barter       merchant / player columns with prices; cross trades one,
 *                square the whole stack
 *   Services     persuasion (admire, intimidate, taunt, bribes; the NPC
 *                answers from the Persuasion topics), spells, training,
 *                repair, travel
 *   Alchemy      four ingredient slots, best apparatus picked
 *                automatically, effect preview, name, brew
 *   Repair       self repair with a hammer / prongs
 *   Spellmaking  and Enchanting share the effect editor: up/down pick a
 *                field, left/right change it (L/R by ten), circle back
 *
 * Every screen opened from a conversation returns to it on circle.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define COL_ROWS 20
#define COL_Y 42
#define FOOT_Y 256
#define INFO_Y 228

static Vm *tvm(Ui *ui)
{
    return &ui->g->rpg->vm;
}

static void foot(UiCanvas *c, const char *text)
{
    ui_rect(c, 10, FOOT_Y - 3, 460, 1, C_BORDER);
    ui_text(c, 12, FOOT_Y, C_DIM, text, -1);
}

static const char *ref_name(Ui *ui, const VmRef *ref)
{
    Rpg *r = ui_rpg(ui);
    const DRpgObject *o = rpg_object(r, ref->object);
    const char *n = o ? rpg_name(r, o) : "";
    return *n ? n : "Container";
}

static int stack_ok(Ui *ui, int stack, uint32_t owner)
{
    const VmItem *it = stack >= 0 ? vm_item_at(tvm(ui), stack) : NULL;
    return it && it->owner == owner && it->count > 0;
}

/* ------------------------------------------------------------------------ */
/* two-column loot / barter                                                 */
/* ------------------------------------------------------------------------ */
static uint32_t merchant_list(Ui *ui, int32_t *out, uint32_t max)
{
    Rpg *r = ui_rpg(ui);
    int32_t tmp[256];
    uint32_t n = ui_list_stacks(ui, ui->npc.key, -1, tmp, 256, 0), i, m = 0;
    for (i = 0; i < n && m < max; ++i) {
        const VmItem *it = vm_item_at(tvm(ui), tmp[i]);
        if (rpg_is_equipped(r, &ui->npc, tmp[i]) || !rpg_merchant_buys(r, &ui->npc, rpg_object(r, it->item)))
            continue;
        out[m++] = tmp[i];
    }
    return m;
}

static uint32_t player_sellable(Ui *ui, int32_t *out, uint32_t max)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    int32_t tmp[256];
    uint32_t n = ui_list_stacks(ui, pc.key, -1, tmp, 256, 0), i, m = 0;
    for (i = 0; i < n && m < max; ++i) {
        const VmItem *it = vm_item_at(tvm(ui), tmp[i]);
        if (rpg_merchant_buys(r, &ui->npc, rpg_object(r, it->item)))
            out[m++] = tmp[i];
    }
    return m;
}

static void refresh_columns(Ui *ui)
{
    VmRef pc = ui_pc(ui);
    if (ui->mode == UI_BARTER) {
        ui->stack_count = merchant_list(ui, ui->stacks, 256);
        ui->stack2_count = player_sellable(ui, ui->stacks2, 256);
    } else {
        ui->stack_count = ui_list_stacks(ui, ui->npc.key, -1, ui->stacks, 256, 1);
        ui->stack2_count = ui_list_stacks(ui, pc.key, -1, ui->stacks2, 256, 1);
    }
    if (ui->sel >= (int)ui->stack_count)
        ui->sel = PW_MAX(0, (int)ui->stack_count - 1);
    if (ui->sel2 >= (int)ui->stack2_count)
        ui->sel2 = PW_MAX(0, (int)ui->stack2_count - 1);
    ui_list_nav(&ui->sel, &ui->list_top, (int)ui->stack_count, COL_ROWS, 0);
    ui_list_nav(&ui->sel2, &ui->top2, (int)ui->stack2_count, COL_ROWS, 0);
}

void ui_open_loot(Ui *ui, const VmRef *owner)
{
    ui->npc = *owner;
    ui->mode = UI_LOOT;
    ui->column = 0;
    ui->sel = ui->sel2 = ui->list_top = ui->top2 = 0;
    vm_seed(tvm(ui), owner);
    refresh_columns(ui);
    if (!ui->stack_count)
        ui->column = 1;
}

static int selected_stack(Ui *ui)
{
    if (ui->column == 0)
        return ui->sel < (int)ui->stack_count ? ui->stacks[ui->sel] : -1;
    return ui->sel2 < (int)ui->stack2_count ? ui->stacks2[ui->sel2] : -1;
}

static void update_columns(Ui *ui, const InputState *in)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    uint32_t p = in->pressed;
    char msg[96];
    int stack;
    if (p & GB_CIRCLE) {
        ui_close(ui);
        return;
    }
    if (p & (GB_LEFT | GB_RIGHT))
        ui->column = (p & GB_RIGHT) ? 1 : 0;
    if (ui->column == 0)
        ui_list_nav(&ui->sel, &ui->list_top, (int)ui->stack_count, COL_ROWS, p);
    else
        ui_list_nav(&ui->sel2, &ui->top2, (int)ui->stack2_count, COL_ROWS, p);
    stack = selected_stack(ui);
    msg[0] = '\0';
    if (ui->mode == UI_LOOT) {
        if ((p & GB_CROSS) && stack >= 0) {
            const VmItem *it = vm_item_at(tvm(ui), stack);
            rpg_move_stack(r, stack, ui->column == 0 ? pc.key : ui->npc.key, it->count);
            refresh_columns(ui);
        }
        if (p & GB_TRIANGLE) {
            uint32_t i, n = ui->stack_count;
            int32_t all[256];
            memcpy(all, ui->stacks, n * sizeof(all[0]));
            for (i = 0; i < n; ++i) {
                const VmItem *it = vm_item_at(tvm(ui), all[i]);
                if (it && it->owner == ui->npc.key && it->count > 0)
                    rpg_move_stack(r, all[i], pc.key, it->count);
            }
            refresh_columns(ui);
            ui_close(ui);
        }
        return;
    }
    /* barter */
    if ((p & (GB_CROSS | GB_SQUARE)) && stack >= 0) {
        const VmItem *it = vm_item_at(tvm(ui), stack);
        int count = (p & GB_SQUARE) ? it->count : 1;
        int ok = ui->column == 0 ? rpg_buy(r, &ui->npc, stack, count, msg, sizeof(msg))
                                 : rpg_sell(r, &ui->npc, stack, count, msg, sizeof(msg));
        if (!ok && msg[0])
            ui_status(ui, msg);
        refresh_columns(ui);
    }
}

static void draw_column(Ui *ui, UiCanvas *c, int x, const int32_t *stacks, uint32_t count, int sel, int top,
                        int active, int prices, int buying)
{
    Rpg *r = ui_rpg(ui);
    char buf[96];
    int i;
    for (i = 0; i < COL_ROWS && top + i < (int)count; ++i) {
        int idx = top + i, y = COL_Y + i * UI_LINE_H;
        const VmItem *it = vm_item_at(tvm(ui), stacks[idx]);
        if (idx == sel && active)
            ui_row_select(c, x, y, 36);
        else if (idx == sel)
            ui_rect(c, x - 2, y - 1, 36 * UI_CHAR_W + 4, UI_LINE_H, 0x60604018u);
        ui_stack_label(ui, stacks[idx], buf, sizeof(buf));
        ui_text_clipped(c, x, y, rpg_is_equipped(r, &(VmRef){ 0, 0, 0, 0, 0, it->owner }, stacks[idx])
                                     ? C_NOTICE : C_TEXT, buf, prices ? 29 : 36);
        if (prices) {
            snprintf(buf, sizeof(buf), "%5d", rpg_price(r, &ui->npc, rpg_item_value(r, it), buying));
            ui_text(c, x + 31 * UI_CHAR_W, y, C_DIM, buf, -1);
        }
    }
    if (!count)
        ui_text(c, x, COL_Y, C_DIM, "(empty)", -1);
    ui_scrollbar(c, x + 37 * UI_CHAR_W, COL_Y, COL_ROWS * UI_LINE_H, (int)count, top, COL_ROWS);
}

static void draw_columns(Ui *ui, UiCanvas *c)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    char buf[96];
    int barter = ui->mode == UI_BARTER, stack = selected_stack(ui);
    ui_frame(c, 4, 4, 472, 264, C_PANEL);
    ui_text_centered(c, 240, 10, C_TITLE, barter ? "Barter" : ref_name(ui, &ui->npc));
    if (barter)
        snprintf(buf, sizeof(buf), "%s  (gold %d)", ref_name(ui, &ui->npc), rpg_merchant_gold(r, &ui->npc));
    else
        snprintf(buf, sizeof(buf), "%s", ref_name(ui, &ui->npc));
    ui_text_clipped(c, 12, 28, ui->column == 0 ? C_TITLE : C_DIM, buf, 36);
    snprintf(buf, sizeof(buf), "You  (gold %d)", rpg_gold(r, &pc));
    ui_text_clipped(c, 246, 28, ui->column == 1 ? C_TITLE : C_DIM, buf, 36);
    ui_rect(c, 239, 26, 1, COL_ROWS * UI_LINE_H + 16, C_BORDER);
    draw_column(ui, c, 12, ui->stacks, ui->stack_count, ui->sel, ui->list_top, ui->column == 0, barter, 1);
    draw_column(ui, c, 246, ui->stacks2, ui->stack2_count, ui->sel2, ui->top2, ui->column == 1, barter, 0);
    ui_rect(c, 10, INFO_Y - 4, 460, 1, C_BORDER);
    if (stack >= 0) {
        const VmItem *it = vm_item_at(tvm(ui), stack);
        const DRpgObject *o = rpg_object(r, it->item);
        snprintf(buf, sizeof(buf), "%s  weight %.1f  value %d", rpg_name(r, o), (double)o->weight,
                 rpg_item_value(r, it));
        ui_text_clipped(c, 12, INFO_Y, C_TEXT, buf, 76);
    }
    snprintf(buf, sizeof(buf), "Encumbrance %d/%d", (int)rpg_encumbrance(r, &pc), (int)rpg_capacity(r, &pc));
    ui_text(c, 12, INFO_Y + UI_LINE_H + 2, C_DIM, buf, -1);
    foot(c, barter ? "<- -> side   X trade one   [] trade stack   O back"
                   : "<- -> side   X take/put   /\\ take all   O close");
}

/* ------------------------------------------------------------------------ */
/* services                                                                 */
/* ------------------------------------------------------------------------ */
static const char *const persuade_names[6] = { "Admire", "Intimidate", "Taunt", "Bribe 10 Gold",
                                               "Bribe 100 Gold", "Bribe 1000 Gold" };
static const char *const persuade_gmst[6] = { "sAdmire", "sIntimidate", "sTaunt", "sBribe 10 Gold",
                                              "sBribe 100 Gold", "sBribe 1000 Gold" };
static const char *const persuade_topic[4] = { "admire", "intimidate", "taunt", "bribe" };

static uint32_t damaged_stacks(Ui *ui, int32_t *out, uint32_t max)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    int32_t tmp[256];
    uint32_t n = ui_list_stacks(ui, pc.key, -1, tmp, 256, 0), i, m = 0;
    for (i = 0; i < n && m < max; ++i) {
        const VmItem *it = vm_item_at(tvm(ui), tmp[i]);
        const DRpgObject *o = rpg_object(r, it->item);
        int mx = rpg_item_health_max(r, o);
        if ((o->type == DRPG_OT_WEAP || o->type == DRPG_OT_ARMO) && mx > 0 && it->health != VM_FULL &&
            it->health < mx)
            out[m++] = tmp[i];
    }
    return m;
}

static void build_service(Ui *ui)
{
    Rpg *r = ui_rpg(ui);
    int i;
    ui->svc_line_count = 0;
    switch (ui->service) {
    case UI_SVC_PERSUADE:
        for (i = 0; i < 6; ++i)
            ui->svc_lines[ui->svc_line_count++] = i;
        break;
    case UI_SVC_SPELLS:
        ui->spell_count = (uint32_t)rpg_spells_for_sale(r, &ui->npc, ui->spells, 96);
        ui->svc_line_count = (int)PW_MIN(ui->spell_count, 16u);
        for (i = 0; i < ui->svc_line_count; ++i)
            ui->svc_lines[i] = i;
        break;
    case UI_SVC_TRAINING: {
        int sk[3], n = rpg_trainer_skills(r, &ui->npc, sk);
        for (i = 0; i < n; ++i)
            ui->svc_lines[ui->svc_line_count++] = sk[i];
        break;
    }
    case UI_SVC_REPAIR:
        ui->stack_count = damaged_stacks(ui, ui->stacks, 256);
        ui->svc_line_count = (int)PW_MIN(ui->stack_count, 16u);
        for (i = 0; i < ui->svc_line_count; ++i)
            ui->svc_lines[i] = i;
        break;
    case UI_SVC_TRAVEL:
        ui->svc_line_count = PW_MIN(rpg_travel_count(r, &ui->npc), 16);
        for (i = 0; i < ui->svc_line_count; ++i)
            ui->svc_lines[i] = i;
        break;
    default:
        break;
    }
    if (ui->sel >= ui->svc_line_count)
        ui->sel = PW_MAX(0, ui->svc_line_count - 1);
}

static void travel_info(Ui *ui, int index, char *name, uint32_t cap, int *price)
{
    Rpg *r = ui_rpg(ui);
    Game *g = ui->g;
    const DRpgObject *o = rpg_object(r, ui->npc.object);
    const DRpgTravel *t = o ? &r->db->travel[o->stat[1] + index] : NULL;
    float npc_pos[3] = { g->body.pos.x, g->body.pos.y, g->body.pos.z };
    int zi = -1;
    if (ui->npc.inst >= 0 && g->zone.hdr && (uint32_t)ui->npc.inst < g->zone.instance_count)
        memcpy(npc_pos, g->zone.instances[ui->npc.inst].pos, sizeof(npc_pos));
    *price = rpg_travel_price(r, &ui->npc, index, g->zone.hdr && g->zone.hdr->kind == DZONE_KIND_EXTERIOR,
                              npc_pos);
    if (!t) {
        snprintf(name, cap, "?");
        return;
    }
    if (g->world && g->world->hdr)
        zi = t->cell ? world_find_interior(g->world, t->cell)
                     : world_find_exterior(g->world, (int)floorf(t->pos[0] / 8192.0f),
                                           (int)floorf(t->pos[1] / 8192.0f));
    if (zi >= 0)
        snprintf(name, cap, "%s", world_zone_name(g->world, (uint32_t)zi));
    else
        snprintf(name, cap, "(%d, %d)", (int)floorf(t->pos[0] / 8192.0f), (int)floorf(t->pos[1] / 8192.0f));
}

void ui_open_service(Ui *ui, int service)
{
    Dialogue *d = &ui->g->rpg->dlg;
    ui->npc = d->actor;
    ui->return_mode = UI_DIALOGUE;
    ui->sel = ui->list_top = 0;
    ui->sel2 = ui->top2 = 0;
    ui->column = 0;
    ui->status[0] = '\0';
    switch (service) {
    case UI_SVC_BARTER:
        ui->mode = UI_BARTER;
        vm_seed(tvm(ui), &ui->npc);
        rpg_merchant_gold(ui_rpg(ui), &ui->npc);
        refresh_columns(ui);
        return;
    case UI_SVC_SPELLMAKING:
    case UI_SVC_ENCHANT:
        ui->mode = service == UI_SVC_SPELLMAKING ? UI_SPELLMAKING : UI_ENCHANT;
        ui->fx_count = 0;
        ui->fx_sel = 0;
        ui->fx_field = -1;
        ui->name[0] = '\0';
        ui->kb_active = 0;
        rpg_known_effects(ui_rpg(ui), ui->known_fx);
        if (service == UI_SVC_ENCHANT) {
            memset(&ui->ench, 0, sizeof(ui->ench));
            ui->ench.item_stack = ui->ench.gem_stack = -1;
            ui->ench.type = 2;
        }
        return;
    default:
        ui->mode = UI_SERVICE;
        ui->service = service;
        ui_doc_reset(&ui->doc, 72);
        build_service(ui);
        return;
    }
}

static void update_service(Ui *ui, const InputState *in)
{
    Rpg *r = ui_rpg(ui);
    Dialogue *d = &ui->g->rpg->dlg;
    uint32_t p = in->pressed;
    char msg[128];
    int line;
    if (p & GB_CIRCLE) {
        ui_close(ui);
        return;
    }
    ui_list_nav(&ui->sel, &ui->list_top, ui->svc_line_count, 16, p);
    if (!(p & GB_CROSS) || ui->sel >= ui->svc_line_count)
        return;
    line = ui->svc_lines[ui->sel];
    msg[0] = '\0';
    switch (ui->service) {
    case UI_SVC_PERSUADE: {
        int success, change, kind = line >= RPG_PERSUADE_BRIBE10 ? 3 : line;
        char topic[32];
        VmRef pc = ui_pc(ui);
        int gold = rpg_gold(r, &pc);
        static const int bribe[3] = { 10, 100, 1000 };
        if (line >= RPG_PERSUADE_BRIBE10 && gold < bribe[line - RPG_PERSUADE_BRIBE10]) {
            ui_status(ui, rpgdb_gmst_string(r->db, "sNotifyMessage24", "You don't have enough gold."));
            return;
        }
        change = rpg_persuade(r, &ui->npc, line, &success);
        snprintf(topic, sizeof(topic), "%s %s", persuade_topic[kind], success ? "success" : "fail");
        if (!dlg_say(d, topic))
            snprintf(d->fixed, sizeof(d->fixed), "%s", success ? "(Success)" : "(Failure)");
        ui_doc_reset(&ui->doc, 72);
        ui_doc_add(&ui->doc, K_TEXT, d->fixed, NULL, 0);
        snprintf(msg, sizeof(msg), "Disposition %s%d", change >= 0 ? "+" : "", change);
        ui_status(ui, msg);
        return;
    }
    case UI_SVC_SPELLS:
        rpg_buy_spell(r, &ui->npc, ui->spells[line], msg, sizeof(msg));
        break;
    case UI_SVC_TRAINING:
        rpg_train(r, &ui->npc, line, msg, sizeof(msg));
        break;
    case UI_SVC_REPAIR:
        rpg_repair_service(r, &ui->npc, ui->stacks[line], msg, sizeof(msg));
        break;
    case UI_SVC_TRAVEL: {
        char name[64];
        int price;
        travel_info(ui, line, name, sizeof(name), &price);
        if (rpg_travel(r, &ui->npc, line, price, msg, sizeof(msg))) {
            ui_end_dialogue(ui);
            if (msg[0])
                ui_toast(ui, msg);
            return;
        }
        break;
    }
    default:
        break;
    }
    if (msg[0])
        ui_status(ui, msg);
    build_service(ui);
}

static void draw_service(Ui *ui, UiCanvas *c)
{
    static const char *const titles[UI_SVC_COUNT] = { "Persuasion", "Barter", "Spells", "Training", "Repair",
                                                      "Travel", "Spellmaking", "Enchanting" };
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    char buf[112];
    int i, y;
    ui_frame(c, 40, 20, 400, 232, C_PANEL);
    snprintf(buf, sizeof(buf), "%s - %s", titles[ui->service], ref_name(ui, &ui->npc));
    ui_text_centered(c, 240, 26, C_TITLE, buf);
    snprintf(buf, sizeof(buf), "Gold %d", rpg_gold(r, &pc));
    ui_text(c, 430 - (int)strlen(buf) * UI_CHAR_W, 38, C_DIM, buf, -1);
    if (ui->service == UI_SVC_PERSUADE) {
        snprintf(buf, sizeof(buf), "Disposition %d", rpg_disposition(r, &ui->npc));
        ui_text(c, 52, 38, C_DIM, buf, -1);
    }
    for (i = 0; i < ui->svc_line_count && i < 16; ++i) {
        int line = ui->svc_lines[i], price = 0;
        y = 52 + i * UI_LINE_H;
        if (i == ui->sel)
            ui_row_select(c, 52, y, 62);
        switch (ui->service) {
        case UI_SVC_PERSUADE:
            ui_text(c, 52, y, C_TEXT, rpgdb_gmst_string(r->db, persuade_gmst[line], persuade_names[line]), -1);
            continue;
        case UI_SVC_SPELLS:
            snprintf(buf, sizeof(buf), "%s", rpg_spell_name(r, ui->spells[line]));
            price = rpg_spell_price(r, &ui->npc, ui->spells[line]);
            break;
        case UI_SVC_TRAINING:
            snprintf(buf, sizeof(buf), "%s (%d)", rpg_skill_name(r, line), (int)rpg_skill(r, &pc, line));
            price = rpg_training_price(r, &ui->npc, line);
            break;
        case UI_SVC_REPAIR:
            ui_stack_label(ui, ui->stacks[line], buf, sizeof(buf));
            price = rpg_repair_price(r, &ui->npc, ui->stacks[line]);
            break;
        case UI_SVC_TRAVEL:
            travel_info(ui, line, buf, sizeof(buf), &price);
            break;
        default:
            break;
        }
        ui_text_clipped(c, 52, y, C_TEXT, buf, 50);
        snprintf(buf, sizeof(buf), "%5d gold", price);
        ui_text(c, 430 - 10 * UI_CHAR_W, y, price > rpg_gold(r, &pc) ? C_BAD : C_DIM, buf, -1);
    }
    if (!ui->svc_line_count)
        ui_text(c, 52, 52, C_DIM, "(nothing to offer)", -1);
    y = 52 + 16 * UI_LINE_H + 4;
    ui_rect(c, 48, y - 3, 384, 1, C_BORDER);
    if (ui->service == UI_SVC_PERSUADE) {
        ui_draw_doc(c, &ui->doc, 52, y, 4, (int)PW_MAX(0, (int)ui->doc.line_count - 4), -1);
    } else if (ui->service == UI_SVC_SPELLS && ui->sel < ui->svc_line_count) {
        const DRpgSpell *s = rpg_spell(r, ui->spells[ui->svc_lines[ui->sel]]);
        int n = 0, k;
        const DRpgEffect *e = s ? rpg_spell_effects(r, s, &n) : NULL;
        for (k = 0; k < n && k < 3; ++k) {
            rpg_describe_effect(r, &e[k], buf, sizeof(buf));
            ui_text_clipped(c, 52, y + k * UI_LINE_H, C_LINK, buf, 62);
        }
        if (s) {
            snprintf(buf, sizeof(buf), "Cost %d  Chance %d%%", rpg_spell_cost(r, s),
                     (int)PW_CLAMP(rpg_spell_chance(r, &pc, s), 0.0f, 100.0f));
            ui_text(c, 52, y + 3 * UI_LINE_H, C_DIM, buf, -1);
        }
    }
    ui_text(c, 52, 240, C_DIM, "X choose   O back", -1);
}

/* ------------------------------------------------------------------------ */
/* alchemy                                                                  */
/* ------------------------------------------------------------------------ */
static float apparatus_quality(const DRpgObject *o)
{
    float q;
    int32_t bits = o->stat[1];
    memcpy(&q, &bits, sizeof(q));
    return q;
}

static void pick_tools(Ui *ui)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    uint32_t cursor = 0, item;
    int32_t count;
    int slot, k;
    float best[4] = { 0, 0, 0, 0 };
    for (k = 0; k < 4; ++k)
        ui->alch.tools[k] = -1;
    while ((slot = vm_item_next(tvm(ui), pc.key, &cursor, &item, &count)) >= 0) {
        const DRpgObject *o = rpg_object(r, item);
        if (!o || o->type != DRPG_OT_APPA || o->stat[0] < 0 || o->stat[0] > 3)
            continue;
        if (apparatus_quality(o) > best[o->stat[0]]) {
            best[o->stat[0]] = apparatus_quality(o);
            ui->alch.tools[o->stat[0]] = slot;
        }
    }
}

void ui_open_alchemy(Ui *ui)
{
    int k;
    ui->mode = UI_ALCHEMY;
    for (k = 0; k < 4; ++k)
        ui->alch.stacks[k] = -1;
    pick_tools(ui);
    ui->alch.name[0] = '\0';
    ui->name[0] = '\0';
    ui->sel = 0;
    ui->column = 0;
    ui->kb_active = 0;
}

static void list_ingredients(Ui *ui)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    int32_t tmp[256];
    uint32_t n = ui_list_stacks(ui, pc.key, -1, tmp, 256, 0), i;
    ui->stack2_count = 0;
    for (i = 0; i < n; ++i) {
        const VmItem *it = vm_item_at(tvm(ui), tmp[i]);
        const DRpgObject *o = rpg_object(r, it->item);
        int k, used = 0;
        for (k = 0; k < 4; ++k)
            used |= ui->alch.stacks[k] == tmp[i];
        if (o->type == DRPG_OT_INGR && !used)
            ui->stacks2[ui->stack2_count++] = tmp[i];
    }
    ui->sel2 = ui->top2 = 0;
}

static void validate_alchemy(Ui *ui)
{
    VmRef pc = ui_pc(ui);
    int k;
    for (k = 0; k < 4; ++k)
        if (ui->alch.stacks[k] >= 0 && !stack_ok(ui, ui->alch.stacks[k], pc.key))
            ui->alch.stacks[k] = -1;
    for (k = 0; k < 4; ++k)
        if (ui->alch.tools[k] >= 0 && !stack_ok(ui, ui->alch.tools[k], pc.key))
            ui->alch.tools[k] = -1;
}

static void update_alchemy(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    char msg[96];
    int dummy = 0;
    validate_alchemy(ui);
    if (ui->kb_active) {
        int k = ui_keyboard_update(ui, p);
        if (k != 0) {
            if (k > 0)
                snprintf(ui->alch.name, sizeof(ui->alch.name), "%s", ui->name);
            ui->kb_active = 0;
        }
        return;
    }
    if (ui->column == 1) { /* picking an ingredient */
        if (p & GB_CIRCLE) {
            ui->column = 0;
            return;
        }
        ui_list_nav(&ui->sel2, &ui->top2, (int)ui->stack2_count, 14, p);
        if ((p & GB_CROSS) && ui->sel2 < (int)ui->stack2_count) {
            ui->alch.stacks[ui->sel] = ui->stacks2[ui->sel2];
            ui->column = 0;
        }
        return;
    }
    if (p & GB_CIRCLE) {
        ui_close(ui);
        return;
    }
    ui_list_nav(&ui->sel, &dummy, 6, 6, p);
    if (p & GB_SQUARE && ui->sel < 4)
        ui->alch.stacks[ui->sel] = -1;
    if (!(p & GB_CROSS))
        return;
    if (ui->sel < 4) {
        list_ingredients(ui);
        if (ui->stack2_count)
            ui->column = 1;
        else
            ui_status(ui, "You have no other ingredients.");
    } else if (ui->sel == 4) {
        snprintf(ui->name, sizeof(ui->name), "%s", ui->alch.name);
        ui->kb_active = 1;
        ui->kb_sel = 0;
    } else {
        if (!ui->alch.name[0])
            snprintf(ui->alch.name, sizeof(ui->alch.name), "Potion");
        rpg_alchemy_brew(ui_rpg(ui), &ui->alch, msg, sizeof(msg));
        validate_alchemy(ui);
        if (msg[0])
            ui_status(ui, msg);
    }
}

static void draw_alchemy(Ui *ui, UiCanvas *c)
{
    static const char *const tool_names[4] = { "Mortar & Pestle", "Alembic", "Calcinator", "Retort" };
    Rpg *r = ui_rpg(ui);
    DRpgEffect fx[8];
    char buf[96];
    int k, n, value, y;
    ui_frame(c, 4, 4, 472, 264, C_PANEL);
    ui_text_centered(c, 240, 10, C_TITLE, "Alchemy");
    for (k = 0; k < 4; ++k) {
        y = 28 + k * UI_LINE_H;
        ui_text(c, 12, y, C_DIM, tool_names[k], -1);
        if (ui->alch.tools[k] >= 0) {
            const VmItem *it = vm_item_at(tvm(ui), ui->alch.tools[k]);
            ui_text_clipped(c, 116, y, C_TEXT, rpg_name(r, rpg_object(r, it->item)), 20);
        } else {
            ui_text(c, 116, y, k == 0 ? C_BAD : C_DIM, "-", -1);
        }
    }
    for (k = 0; k < 6; ++k) {
        y = 74 + k * (UI_LINE_H + 3);
        if (k == ui->sel && ui->column == 0)
            ui_row_select(c, 12, y, 36);
        if (k < 4) {
            snprintf(buf, sizeof(buf), "Ingredient %d: ", k + 1);
            ui_text(c, 12, y, C_DIM, buf, -1);
            if (ui->alch.stacks[k] >= 0)
                ui_stack_label(ui, ui->alch.stacks[k], buf, sizeof(buf));
            else
                snprintf(buf, sizeof(buf), "(empty)");
            ui_text_clipped(c, 12 + 14 * UI_CHAR_W, y, ui->alch.stacks[k] >= 0 ? C_TEXT : C_DIM, buf, 22);
        } else if (k == 4) {
            snprintf(buf, sizeof(buf), "Name: %s", ui->alch.name[0] ? ui->alch.name : "Potion");
            ui_text_clipped(c, 12, y, C_TEXT, buf, 36);
        } else {
            ui_text(c, 12, y, C_TITLE, "Create", -1);
        }
    }
    ui_rect(c, 238, 26, 1, 200, C_BORDER);
    ui_text(c, 246, 28, C_TITLE, "Effects", -1);
    n = rpg_alchemy_preview(r, &ui->alch, fx, 8, &value);
    ui_doc_reset(&ui->doc, 37);
    for (k = 0; k < n; ++k) {
        rpg_describe_effect(r, &fx[k], buf, sizeof(buf));
        ui_doc_add(&ui->doc, K_TEXT, buf, NULL, 0);
    }
    if (!n) {
        ui_text(c, 246, 40, C_DIM, "(no shared effects)", -1);
    } else {
        int lines = (int)PW_MIN(ui->doc.line_count, 18u);
        for (k = 0; k < lines; ++k) {
            const UiLine *ln = &ui->doc.lines[k];
            ui_text(c, 246, 40 + k * UI_LINE_H, C_LINK, ui->doc.text + ln->ofs, ln->len);
        }
        snprintf(buf, sizeof(buf), "Value %d", value);
        ui_text(c, 246, 40 + (lines + 1) * UI_LINE_H, C_DIM, buf, -1);
    }
    if (ui->column == 1) {
        int i;
        ui_frame(c, 60, 60, 360, 150, C_PANEL2);
        ui_text(c, 70, 66, C_TITLE, "Choose an ingredient", -1);
        for (i = 0; i < 14 && ui->top2 + i < (int)ui->stack2_count; ++i) {
            int idx = ui->top2 + i;
            y = 80 + i * UI_LINE_H;
            if (idx == ui->sel2)
                ui_row_select(c, 70, y, 56);
            ui_stack_label(ui, ui->stacks2[idx], buf, sizeof(buf));
            ui_text_clipped(c, 70, y, C_TEXT, buf, 56);
        }
    }
    if (ui->kb_active)
        ui_keyboard_draw(ui, c, "Potion name");
    foot(c, "X choose   [] clear slot   O back");
}

/* ------------------------------------------------------------------------ */
/* self repair                                                              */
/* ------------------------------------------------------------------------ */
void ui_open_repair(Ui *ui, int tool_stack)
{
    ui->mode = UI_REPAIR;
    ui->repair_tool = tool_stack;
    ui->sel = ui->list_top = 0;
    ui->stack_count = damaged_stacks(ui, ui->stacks, 256);
}

static void update_repair(Ui *ui, const InputState *in)
{
    VmRef pc = ui_pc(ui);
    uint32_t p = in->pressed;
    char msg[96];
    if ((p & GB_CIRCLE) || !stack_ok(ui, ui->repair_tool, pc.key)) {
        ui_close(ui);
        return;
    }
    ui_list_nav(&ui->sel, &ui->list_top, (int)ui->stack_count, 18, p);
    if ((p & GB_CROSS) && ui->sel < (int)ui->stack_count) {
        msg[0] = '\0';
        rpg_repair_self(ui_rpg(ui), ui->repair_tool, ui->stacks[ui->sel], msg, sizeof(msg));
        if (msg[0])
            ui_status(ui, msg);
        ui->stack_count = damaged_stacks(ui, ui->stacks, 256);
        if (ui->sel >= (int)ui->stack_count)
            ui->sel = PW_MAX(0, (int)ui->stack_count - 1);
    }
}

static void draw_repair(Ui *ui, UiCanvas *c)
{
    char buf[96];
    int i;
    ui_frame(c, 60, 20, 360, 232, C_PANEL);
    ui_text_centered(c, 240, 26, C_TITLE, "Repair");
    if (vm_item_at(tvm(ui), ui->repair_tool)) {
        ui_stack_label(ui, ui->repair_tool, buf, sizeof(buf));
        ui_text_clipped(c, 72, 40, C_DIM, buf, 56);
    }
    for (i = 0; i < 18 && ui->list_top + i < (int)ui->stack_count; ++i) {
        int idx = ui->list_top + i, y = 56 + i * UI_LINE_H;
        if (idx == ui->sel)
            ui_row_select(c, 72, y, 56);
        ui_stack_label(ui, ui->stacks[idx], buf, sizeof(buf));
        ui_text_clipped(c, 72, y, C_TEXT, buf, 56);
    }
    if (!ui->stack_count)
        ui_text(c, 72, 56, C_DIM, "(nothing needs repair)", -1);
    ui_text(c, 72, 240, C_DIM, "X repair   O back", -1);
}

/* ------------------------------------------------------------------------ */
/* spellmaking and enchanting: effect editor                                */
/* ------------------------------------------------------------------------ */
enum { FF_EFFECT = 0, FF_TARGET, FF_RANGE, FF_MIN, FF_MAX, FF_DURATION, FF_AREA, FF_REMOVE, FF_COUNT };
static const char *const range_names[3] = { "Self", "Touch", "Target" };
static const char *const ench_types[4] = { "Cast Once", "Cast When Strikes", "Cast When Used", "Constant Effect" };

static DRpgEffect *editor_fx(Ui *ui, int *count)
{
    if (ui->mode == UI_ENCHANT) {
        *count = ui->ench.count;
        return ui->ench.effects;
    }
    *count = ui->fx_count;
    return ui->fx;
}

static void editor_set_count(Ui *ui, int n)
{
    if (ui->mode == UI_ENCHANT)
        ui->ench.count = n;
    else
        ui->fx_count = n;
}

static int effect_usable(Ui *ui, int e)
{
    Rpg *r = ui_rpg(ui);
    const DRpgMgef *m;
    if (e < 0 || e >= RPGDB_EFFECTS || !ui->known_fx[e] || !r->db->mgefs)
        return 0;
    m = &r->db->mgefs[e];
    if (!m->present)
        return 0;
    return (m->flags & (ui->mode == UI_ENCHANT ? DRPG_MF_ENCHANTING : DRPG_MF_SPELLMAKING)) != 0;
}

static int range_allowed(Ui *ui, int effect, int range)
{
    Rpg *r = ui_rpg(ui);
    uint32_t f = r->db->mgefs[effect].flags;
    static const uint32_t bits[3] = { DRPG_MF_CAST_SELF, DRPG_MF_CAST_TOUCH, DRPG_MF_CAST_TARGET };
    if (ui->mode == UI_ENCHANT) {
        if (ui->ench.type == 1 && range != RPG_RANGE_TOUCH && range != RPG_RANGE_TARGET)
            return 0;
        if (ui->ench.type == 3 && range != RPG_RANGE_SELF)
            return 0;
    }
    if (!(f & (DRPG_MF_CAST_SELF | DRPG_MF_CAST_TOUCH | DRPG_MF_CAST_TARGET)))
        return 1;
    return (f & bits[range]) != 0;
}

static void fix_effect(Ui *ui, DRpgEffect *e)
{
    Rpg *r = ui_rpg(ui);
    uint32_t f = r->db->mgefs[e->effect].flags;
    int k;
    if (f & DRPG_MF_TARGET_SKILL) {
        if (e->skill < 0)
            e->skill = 0;
        e->attribute = -1;
    } else if (f & DRPG_MF_TARGET_ATTRIBUTE) {
        if (e->attribute < 0)
            e->attribute = 0;
        e->skill = -1;
    } else {
        e->skill = e->attribute = -1;
    }
    for (k = 0; k < 3 && !range_allowed(ui, e->effect, e->range); ++k)
        e->range = (uint8_t)((e->range + 1) % 3);
    if (f & DRPG_MF_NO_MAGNITUDE)
        e->mag_min = e->mag_max = 1;
    if (e->mag_max < e->mag_min)
        e->mag_max = e->mag_min;
    if ((f & DRPG_MF_NO_DURATION) || (ui->mode == UI_ENCHANT && ui->ench.type == 3))
        e->duration = (f & DRPG_MF_NO_DURATION) ? 0 : 1;
    else if (e->duration == 0)
        e->duration = 1;
    if (e->range == RPG_RANGE_SELF)
        e->area = 0;
}

static int next_effect(Ui *ui, int from, int dir)
{
    int i, e = from;
    for (i = 0; i < RPGDB_EFFECTS; ++i) {
        e = (e + dir + RPGDB_EFFECTS) % RPGDB_EFFECTS;
        if (effect_usable(ui, e))
            return e;
    }
    return -1;
}

static void editor_add(Ui *ui)
{
    int n, e = next_effect(ui, RPGDB_EFFECTS - 1, 1);
    DRpgEffect *fx = editor_fx(ui, &n);
    if (n >= RPG_CUSTOM_EFFECTS)
        return;
    if (e < 0) {
        ui_status(ui, "You know no effects you can use.");
        return;
    }
    memset(&fx[n], 0, sizeof(fx[n]));
    fx[n].effect = (int16_t)e;
    fx[n].skill = fx[n].attribute = -1;
    fx[n].mag_min = fx[n].mag_max = 1;
    fx[n].duration = 1;
    fx[n].range = RPG_RANGE_SELF;
    fix_effect(ui, &fx[n]);
    editor_set_count(ui, n + 1);
    ui->fx_sel = n;
    ui->fx_field = 0;
}

static void editor_change(Ui *ui, DRpgEffect *e, int field, int delta)
{
    Rpg *r = ui_rpg(ui);
    uint32_t f = r->db->mgefs[e->effect].flags;
    int step = delta;
    switch (field) {
    case FF_EFFECT: {
        int ne = next_effect(ui, e->effect, delta > 0 ? 1 : -1);
        if (ne >= 0)
            e->effect = (int16_t)ne;
        break;
    }
    case FF_TARGET:
        if (f & DRPG_MF_TARGET_SKILL)
            e->skill = (int8_t)((e->skill + (delta > 0 ? 1 : SKILL_COUNT - 1)) % SKILL_COUNT);
        else if (f & DRPG_MF_TARGET_ATTRIBUTE)
            e->attribute = (int8_t)((e->attribute + (delta > 0 ? 1 : ATTR_COUNT - 1)) % ATTR_COUNT);
        break;
    case FF_RANGE: {
        int k;
        for (k = 0; k < 3; ++k) {
            e->range = (uint8_t)((e->range + (delta > 0 ? 1 : 2)) % 3);
            if (range_allowed(ui, e->effect, e->range))
                break;
        }
        break;
    }
    case FF_MIN:
        e->mag_min = (uint16_t)PW_CLAMP((int)e->mag_min + step, 1, 100);
        break;
    case FF_MAX:
        e->mag_max = (uint16_t)PW_CLAMP((int)e->mag_max + step, 1, 100);
        if (e->mag_max < e->mag_min)
            e->mag_min = e->mag_max;
        break;
    case FF_DURATION:
        e->duration = (uint16_t)PW_CLAMP((int)e->duration + step, 1, 300);
        break;
    case FF_AREA:
        e->area = (uint16_t)PW_CLAMP((int)e->area + step, 0, 50);
        break;
    default:
        break;
    }
    fix_effect(ui, e);
}

static int field_visible(Ui *ui, const DRpgEffect *e, int field)
{
    uint32_t f = ui_rpg(ui)->db->mgefs[e->effect].flags;
    switch (field) {
    case FF_TARGET:
        return (f & (DRPG_MF_TARGET_SKILL | DRPG_MF_TARGET_ATTRIBUTE)) != 0;
    case FF_MIN:
    case FF_MAX:
        return !(f & DRPG_MF_NO_MAGNITUDE);
    case FF_DURATION:
        return !(f & DRPG_MF_NO_DURATION) && !(ui->mode == UI_ENCHANT && ui->ench.type == 3);
    case FF_AREA:
        return e->range != RPG_RANGE_SELF;
    default:
        return 1;
    }
}

/* returns 1 while the editor consumed the input */
static int update_editor(Ui *ui, uint32_t p)
{
    int n, guard;
    DRpgEffect *fx = editor_fx(ui, &n), *e;
    if (ui->fx_field < 0 || ui->fx_sel >= n)
        return 0;
    e = &fx[ui->fx_sel];
    if (p & GB_CIRCLE) {
        ui->fx_field = -1;
        return 1;
    }
    for (guard = 0; guard < FF_COUNT; ++guard) {
        if (p & GB_UP)
            ui->fx_field = (ui->fx_field + FF_COUNT - 1) % FF_COUNT;
        else if (p & GB_DOWN)
            ui->fx_field = (ui->fx_field + 1) % FF_COUNT;
        if (field_visible(ui, e, ui->fx_field) || !(p & (GB_UP | GB_DOWN)))
            break;
    }
    if (p & (GB_LEFT | GB_RIGHT))
        editor_change(ui, e, ui->fx_field, (p & GB_RIGHT) ? 1 : -1);
    if (p & (GB_L | GB_R))
        editor_change(ui, e, ui->fx_field, (p & GB_R) ? 10 : -10);
    if ((p & GB_CROSS) && ui->fx_field == FF_REMOVE) {
        memmove(&fx[ui->fx_sel], &fx[ui->fx_sel + 1], (size_t)(n - ui->fx_sel - 1) * sizeof(*fx));
        editor_set_count(ui, n - 1);
        ui->fx_field = -1;
        ui->fx_sel = PW_MAX(0, ui->fx_sel - 1);
    }
    return 1;
}

static void draw_editor(Ui *ui, UiCanvas *c, int x, int y)
{
    Rpg *r = ui_rpg(ui);
    int n, k;
    DRpgEffect *fx = editor_fx(ui, &n), *e;
    char buf[96];
    static const char *const labels[FF_COUNT] = { "Effect", "Target", "Range", "Magnitude min",
                                                  "Magnitude max", "Duration", "Area", "Remove effect" };
    if (ui->fx_field < 0 || ui->fx_sel >= n)
        return;
    e = &fx[ui->fx_sel];
    ui_frame(c, x, y, 250, 108, C_PANEL2);
    for (k = 0; k < FF_COUNT; ++k) {
        int yy = y + 8 + k * (UI_LINE_H + 2);
        uint32_t col = field_visible(ui, e, k) ? C_TEXT : 0xFF505458u;
        if (k == ui->fx_field)
            ui_row_select(c, x + 8, yy, 39);
        ui_text(c, x + 8, yy, C_DIM, labels[k], -1);
        buf[0] = '\0';
        switch (k) {
        case FF_EFFECT:
            snprintf(buf, sizeof(buf), "%s", rpg_effect_name(r, e->effect));
            break;
        case FF_TARGET:
            snprintf(buf, sizeof(buf), "%s", e->skill >= 0 ? rpg_skill_name(r, e->skill)
                                             : e->attribute >= 0 ? rpg_attr_name(r, e->attribute) : "-");
            break;
        case FF_RANGE:
            snprintf(buf, sizeof(buf), "%s", range_names[e->range % 3]);
            break;
        case FF_MIN:
            snprintf(buf, sizeof(buf), "%d", e->mag_min);
            break;
        case FF_MAX:
            snprintf(buf, sizeof(buf), "%d", e->mag_max);
            break;
        case FF_DURATION:
            snprintf(buf, sizeof(buf), "%d s", e->duration);
            break;
        case FF_AREA:
            snprintf(buf, sizeof(buf), "%d ft", e->area);
            break;
        default:
            break;
        }
        ui_text_clipped(c, x + 8 + 15 * UI_CHAR_W, yy, col, buf, 24);
    }
}

void ui_open_enchant(Ui *ui, int self, int gem_stack)
{
    if (self) {
        memset(&ui->npc, 0, sizeof(ui->npc));
        ui->mode = UI_ENCHANT;
        ui->fx_count = 0;
        ui->fx_sel = 0;
        ui->fx_field = -1;
        ui->name[0] = '\0';
        ui->kb_active = 0;
        ui->column = 0;
        ui->sel = 0;
        rpg_known_effects(ui_rpg(ui), ui->known_fx);
        memset(&ui->ench, 0, sizeof(ui->ench));
        ui->ench.item_stack = -1;
        ui->ench.gem_stack = gem_stack;
        ui->ench.type = 2;
        return;
    }
    ui_open_service(ui, UI_SVC_ENCHANT);
}

/* rows: spellmaking  Name, effects..., Add, Buy
 *       enchanting   Item, Soul gem, Type, Name, effects..., Add, Enchant */
static int craft_rows(Ui *ui, int *first_fx)
{
    int n;
    editor_fx(ui, &n);
    *first_fx = ui->mode == UI_ENCHANT ? 4 : 1;
    return *first_fx + n + (n < RPG_CUSTOM_EFFECTS ? 1 : 0) + 1;
}

static uint8_t item_types_allowed(Ui *ui)
{
    Rpg *r = ui_rpg(ui);
    const VmItem *it = ui->ench.item_stack >= 0 ? vm_item_at(tvm(ui), ui->ench.item_stack) : NULL;
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    if (!o)
        return 0x0F;
    switch (o->type) {
    case DRPG_OT_BOOK:
        return 1u << 0;
    case DRPG_OT_WEAP:
        return (o->stat[0] >= 11) ? (1u << 1) : (uint8_t)((1u << 1) | (1u << 2) | (1u << 3));
    default:
        return (1u << 2) | (1u << 3);
    }
}

static void list_enchant_picks(Ui *ui, int gems)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    int32_t tmp[256];
    uint32_t n = ui_list_stacks(ui, pc.key, -1, tmp, 256, 0), i;
    ui->stack2_count = 0;
    for (i = 0; i < n; ++i) {
        const VmItem *it = vm_item_at(tvm(ui), tmp[i]);
        const DRpgObject *o = rpg_object(r, it->item);
        int ok;
        if (gems) {
            ok = rpg_is_soulgem(r, o) && it->soul;
        } else {
            int pts = o->type == DRPG_OT_WEAP ? o->stat[4] : o->stat[2];
            ok = (o->type == DRPG_OT_WEAP || o->type == DRPG_OT_ARMO || o->type == DRPG_OT_CLOT ||
                  o->type == DRPG_OT_BOOK) && (o->link[0] == 0 || o->link[0] == DRPG_NONE) && pts > 0;
        }
        if (ok)
            ui->stacks2[ui->stack2_count++] = tmp[i];
    }
    ui->sel2 = ui->top2 = 0;
}

static void update_craft(Ui *ui, const InputState *in)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    uint32_t p = in->pressed;
    int first, rows = craft_rows(ui, &first), n, dummy = 0, enchant = ui->mode == UI_ENCHANT;
    char msg[128];
    editor_fx(ui, &n);
    if (enchant) {
        if (ui->ench.item_stack >= 0 && !stack_ok(ui, ui->ench.item_stack, pc.key))
            ui->ench.item_stack = -1;
        if (ui->ench.gem_stack >= 0 && !stack_ok(ui, ui->ench.gem_stack, pc.key))
            ui->ench.gem_stack = -1;
    }
    if (ui->kb_active) {
        int k = ui_keyboard_update(ui, p);
        if (k != 0) {
            if (k > 0 && enchant)
                snprintf(ui->ench.name, sizeof(ui->ench.name), "%s", ui->name);
            ui->kb_active = 0;
        }
        return;
    }
    if (ui->column == 1) { /* item / gem picker */
        if (p & GB_CIRCLE) {
            ui->column = 0;
            return;
        }
        ui_list_nav(&ui->sel2, &ui->top2, (int)ui->stack2_count, 14, p);
        if ((p & GB_CROSS) && ui->sel2 < (int)ui->stack2_count) {
            int k;
            if (ui->sel == 0)
                ui->ench.item_stack = ui->stacks2[ui->sel2];
            else
                ui->ench.gem_stack = ui->stacks2[ui->sel2];
            ui->column = 0;
            /* keep the type valid for the item */
            for (k = 0; k < 4 && !(item_types_allowed(ui) & (1u << ui->ench.type)); ++k)
                ui->ench.type = (ui->ench.type + 1) % 4;
            for (k = 0; k < ui->ench.count; ++k)
                fix_effect(ui, &ui->ench.effects[k]);
        }
        return;
    }
    if (update_editor(ui, p))
        return;
    if (p & GB_CIRCLE) {
        ui_close(ui);
        return;
    }
    ui_list_nav(&ui->sel, &dummy, rows, rows, p);
    if (enchant && ui->sel == 2 && (p & (GB_LEFT | GB_RIGHT))) {
        int k;
        uint8_t allowed = item_types_allowed(ui);
        for (k = 0; k < 4; ++k) {
            ui->ench.type = (ui->ench.type + ((p & GB_RIGHT) ? 1 : 3)) % 4;
            if (allowed & (1u << ui->ench.type))
                break;
        }
        for (k = 0; k < ui->ench.count; ++k)
            fix_effect(ui, &ui->ench.effects[k]);
    }
    if (!(p & GB_CROSS))
        return;
    if (enchant && ui->sel < 2) {
        list_enchant_picks(ui, ui->sel == 1);
        if (ui->stack2_count)
            ui->column = 1;
        else
            ui_status(ui, ui->sel ? "You have no filled soul gems." : "You have nothing that can be enchanted.");
    } else if (ui->sel == first - 1) {
        snprintf(ui->name, sizeof(ui->name), "%s", enchant ? ui->ench.name : ui->name);
        ui->kb_active = 1;
        ui->kb_sel = 0;
    } else if (ui->sel >= first && ui->sel < first + n) {
        ui->fx_sel = ui->sel - first;
        ui->fx_field = 0;
    } else if (ui->sel == first + n && n < RPG_CUSTOM_EFFECTS) {
        editor_add(ui);
        ui->sel = first + ui->fx_sel;
    } else if (ui->sel == rows - 1) {
        msg[0] = '\0';
        if (enchant) {
            if (rpg_enchant(r, ui->npc.kind ? &ui->npc : NULL, &ui->ench, msg, sizeof(msg))) {
                ui->ench.count = 0;
                ui->ench.item_stack = -1;
                ui->ench.gem_stack = -1;
                ui->ench.name[0] = '\0';
                ui->sel = 0;
            }
        } else if (!ui->name[0]) {
            snprintf(msg, sizeof(msg), "Name the spell first.");
        } else if (rpg_make_spell(r, &ui->npc, ui->fx, ui->fx_count, ui->name, msg, sizeof(msg))) {
            ui->fx_count = 0;
            ui->name[0] = '\0';
            ui->sel = 0;
        }
        if (msg[0])
            ui_status(ui, msg);
    }
}

static void draw_craft(Ui *ui, UiCanvas *c)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    int first, rows = craft_rows(ui, &first), n, k, y, enchant = ui->mode == UI_ENCHANT;
    DRpgEffect *fx = editor_fx(ui, &n);
    char buf[112];
    ui_frame(c, 4, 4, 472, 264, C_PANEL);
    snprintf(buf, sizeof(buf), "%s%s%s", enchant ? "Enchanting" : "Spellmaking", ui->npc.kind ? " - " : "",
             ui->npc.kind ? ref_name(ui, &ui->npc) : "");
    ui_text_centered(c, 240, 10, C_TITLE, buf);
    for (k = 0; k < rows; ++k) {
        y = 28 + k * (UI_LINE_H + 2);
        if (k == ui->sel && ui->column == 0 && ui->fx_field < 0)
            ui_row_select(c, 12, y, 44);
        if (enchant && k == 0) {
            ui_text(c, 12, y, C_DIM, "Item:", -1);
            if (ui->ench.item_stack >= 0)
                ui_stack_label(ui, ui->ench.item_stack, buf, sizeof(buf));
            else
                snprintf(buf, sizeof(buf), "(choose)");
            ui_text_clipped(c, 12 + 10 * UI_CHAR_W, y, C_TEXT, buf, 34);
        } else if (enchant && k == 1) {
            ui_text(c, 12, y, C_DIM, "Soul gem:", -1);
            if (ui->ench.gem_stack >= 0)
                ui_stack_label(ui, ui->ench.gem_stack, buf, sizeof(buf));
            else
                snprintf(buf, sizeof(buf), "(choose)");
            ui_text_clipped(c, 12 + 10 * UI_CHAR_W, y, C_TEXT, buf, 34);
        } else if (enchant && k == 2) {
            ui_text(c, 12, y, C_DIM, "Type:", -1);
            snprintf(buf, sizeof(buf), "< %s >", ench_types[ui->ench.type & 3]);
            ui_text(c, 12 + 10 * UI_CHAR_W, y, C_TEXT, buf, -1);
        } else if (k == first - 1) {
            const char *nm = enchant ? ui->ench.name : ui->name;
            ui_text(c, 12, y, C_DIM, "Name:", -1);
            ui_text_clipped(c, 12 + 10 * UI_CHAR_W, y, nm[0] ? C_TEXT : C_DIM, nm[0] ? nm : "(press X)", 34);
        } else if (k >= first && k < first + n) {
            rpg_describe_effect(r, &fx[k - first], buf, sizeof(buf));
            ui_text_clipped(c, 18, y, C_LINK, buf, 43);
        } else if (k == first + n && n < RPG_CUSTOM_EFFECTS) {
            ui_text(c, 18, y, C_NOTICE, "+ Add effect", -1);
        } else {
            ui_text(c, 12, y, C_TITLE, enchant ? "Enchant" : "Buy spell", -1);
        }
    }
    /* totals */
    ui_rect(c, 290, 26, 1, 196, C_BORDER);
    y = 28;
    if (enchant) {
        int cap = 0, charge = 0, points;
        float chance = 0.0f;
        points = rpg_enchant_points(r, &ui->ench, &cap, &charge, &chance);
        snprintf(buf, sizeof(buf), "Enchantment %d / %d", points, cap);
        ui_text(c, 298, y, points > cap ? C_BAD : C_TEXT, buf, -1);
        snprintf(buf, sizeof(buf), "Soul charge %d", charge);
        ui_text(c, 298, y + UI_LINE_H, C_TEXT, buf, -1);
        snprintf(buf, sizeof(buf), "Chance %d%%", ui->npc.kind ? 100 : (int)PW_CLAMP(chance, 0.0f, 100.0f));
        ui_text(c, 298, y + 2 * UI_LINE_H, C_TEXT, buf, -1);
        if (ui->npc.kind) {
            int price = rpg_price(r, &ui->npc, (int)((float)points *
                                                     rpg_gmst_f(r, "fEnchantmentValueMult", 1000.0f) / 100.0f), 1);
            snprintf(buf, sizeof(buf), "Price %d gold", price);
            ui_text(c, 298, y + 3 * UI_LINE_H, price > rpg_gold(r, &pc) ? C_BAD : C_TEXT, buf, -1);
        }
    } else {
        int cost = 0, price = n ? rpg_spellmaking_price(r, &ui->npc, ui->fx, n, &cost) : 0;
        snprintf(buf, sizeof(buf), "Spell cost %d", cost);
        ui_text(c, 298, y, C_TEXT, buf, -1);
        snprintf(buf, sizeof(buf), "Price %d gold", price);
        ui_text(c, 298, y + UI_LINE_H, price > rpg_gold(r, &pc) ? C_BAD : C_TEXT, buf, -1);
    }
    snprintf(buf, sizeof(buf), "Gold %d", rpg_gold(r, &pc));
    ui_text(c, 298, y + 5 * UI_LINE_H, C_DIM, buf, -1);
    draw_editor(ui, c, 222, 120);
    if (ui->column == 1) {
        int i;
        ui_frame(c, 60, 60, 360, 150, C_PANEL2);
        ui_text(c, 70, 66, C_TITLE, ui->sel == 1 ? "Choose a soul gem" : "Choose an item", -1);
        for (i = 0; i < 14 && ui->top2 + i < (int)ui->stack2_count; ++i) {
            int idx = ui->top2 + i;
            y = 80 + i * UI_LINE_H;
            if (idx == ui->sel2)
                ui_row_select(c, 70, y, 56);
            ui_stack_label(ui, ui->stacks2[idx], buf, sizeof(buf));
            ui_text_clipped(c, 70, y, C_TEXT, buf, 56);
        }
    }
    if (ui->kb_active)
        ui_keyboard_draw(ui, c, enchant ? "Item name" : "Spell name");
    foot(c, ui->fx_field >= 0 ? "Up/Down field   <- -> change (L/R x10)   X remove   O done"
                              : "X choose   <- -> type   O back");
}

/* ------------------------------------------------------------------------ */
/* dispatch                                                                 */
/* ------------------------------------------------------------------------ */
void ui_trade_update(Ui *ui, const InputState *in)
{
    switch (ui->mode) {
    case UI_LOOT:
    case UI_BARTER:
        update_columns(ui, in);
        break;
    case UI_SERVICE:
        update_service(ui, in);
        break;
    case UI_ALCHEMY:
        update_alchemy(ui, in);
        break;
    case UI_REPAIR:
        update_repair(ui, in);
        break;
    case UI_SPELLMAKING:
    case UI_ENCHANT:
        update_craft(ui, in);
        break;
    default:
        break;
    }
}

void ui_trade_draw(Ui *ui, UiCanvas *c)
{
    switch (ui->mode) {
    case UI_LOOT:
    case UI_BARTER:
        draw_columns(ui, c);
        break;
    case UI_SERVICE:
        draw_service(ui, c);
        break;
    case UI_ALCHEMY:
        draw_alchemy(ui, c);
        break;
    case UI_REPAIR:
        draw_repair(ui, c);
        break;
    case UI_SPELLMAKING:
    case UI_ENCHANT:
        draw_craft(ui, c);
        break;
    default:
        break;
    }
}
