/*
 * ui_char.c - character screens and the in-game HUD.
 *
 *   Character creation  name (on-screen keyboard), race and sex, class,
 *                       birthsign, review; scripts open the steps one by
 *                       one (EnableNameMenu ...), a new game without a
 *                       CharGen script shows them all. Every confirmed step
 *                       rebuilds the character (rpg_chargen), so scripts
 *                       and the review see the current choice.
 *   Level up            pick three attributes (multipliers from the skill
 *                       increases of this level), then the level rises
 *   Rest / wait         hours with left/right, triangle until healed,
 *                       square toggles rest (heals) and wait
 *   Death               load the newest save, or start again from the
 *                       zone's entrance
 *   Saves               eight slots (0 = quicksave) with name, level,
 *                       place, game date and play time; save or load
 *   HUD                 health / magicka / fatigue bars, readied weapon or
 *                       spell, last enemy's health, crosshair, target and
 *                       game messages
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_internal.h"

#define FOOT_Y 256

static Vm *cvm(Ui *ui)
{
    return &ui->g->rpg->vm;
}

static void foot(UiCanvas *c, const char *text)
{
    ui_rect(c, 10, FOOT_Y - 3, 460, 1, C_BORDER);
    ui_text(c, 12, FOOT_Y, C_DIM, text, -1);
}

/* ------------------------------------------------------------------------ */
/* character creation                                                       */
/* ------------------------------------------------------------------------ */
enum { CG_ROWS = 22 };

static int lowest_step(uint32_t steps)
{
    int i;
    for (i = 0; i < 5; ++i)
        if (steps & (1u << i))
            return 1 << i;
    return 0;
}

static void cg_list(Ui *ui)
{
    Rpg *r = ui_rpg(ui);
    const RpgDb *db = r->db;
    uint32_t i, cur = 0;
    ui->cg_count = 0;
    if (ui->cg_step == GAME_CG_RACE) {
        for (i = 0; i < db->hdr->races.count && ui->cg_count < 64; ++i)
            if (db->races[i].flags & 1u)
                ui->cg_items[ui->cg_count++] = db->races[i].hash;
        cur = ui->cg_race;
    } else if (ui->cg_step == GAME_CG_CLASS || ui->cg_step == GAME_CG_BIRTH) {
        int want = ui->cg_step == GAME_CG_CLASS ? DRPG_OT_CLAS : DRPG_OT_BSGN;
        for (i = 0; i < db->hdr->objects.count && ui->cg_count < 64; ++i) {
            const DRpgObject *o = &db->objects[i];
            if (o->type != want || (want == DRPG_OT_CLAS && !(o->flags & DRPG_OF_PLAYABLE)))
                continue;
            ui->cg_items[ui->cg_count++] = o->hash;
        }
        cur = ui->cg_step == GAME_CG_CLASS ? ui->cg_class : ui->cg_sign;
    }
    /* alphabetical */
    for (i = 1; i < ui->cg_count; ++i) {
        uint32_t v = ui->cg_items[i], j = i;
        const char *nv = ui->cg_step == GAME_CG_RACE ? rpgdb_name(db, rpgdb_race(db, v)->name)
                                                     : rpg_name(r, rpg_object(r, v));
        while (j > 0) {
            uint32_t w = ui->cg_items[j - 1];
            const char *nw = ui->cg_step == GAME_CG_RACE ? rpgdb_name(db, rpgdb_race(db, w)->name)
                                                         : rpg_name(r, rpg_object(r, w));
            if (strcmp(nv, nw) >= 0)
                break;
            ui->cg_items[j] = w;
            --j;
        }
        ui->cg_items[j] = v;
    }
    ui->sel = 0;
    ui->list_top = 0;
    for (i = 0; i < ui->cg_count; ++i)
        if (ui->cg_items[i] == cur)
            ui->sel = (int)i;
    ui_list_nav(&ui->sel, &ui->list_top, (int)ui->cg_count, CG_ROWS, 0);
}

static void cg_describe(Ui *ui)
{
    static char text[2048];
    Rpg *r = ui_rpg(ui);
    RpgDb *db = r->db;
    uint32_t sel = ui->sel < (int)ui->cg_count ? ui->cg_items[ui->sel] : 0;
    ui_doc_reset(&ui->doc, 40);
    text[0] = '\0';
    if (ui->cg_step == GAME_CG_RACE) {
        const DRpgRace *rc = sel ? rpgdb_race(db, sel) : NULL;
        if (rc && rc->desc != DRPG_NONE)
            rpgdb_text(db, rc->desc, text, sizeof(text));
    } else if (ui->cg_step == GAME_CG_BIRTH) {
        const DRpgObject *o = sel ? rpg_object(r, sel) : NULL;
        if (o && o->link[1] != DRPG_NONE && o->link[1])
            rpgdb_text(db, o->link[1], text, sizeof(text));
    }
    if (text[0])
        ui_doc_add(&ui->doc, K_DIM, text, NULL, 0);
    ui->detail = ui->sel;
}

static void cg_enter(Ui *ui, int step)
{
    ui->cg_step = step;
    ui->kb_active = 0;
    if (step == GAME_CG_NAME) {
        snprintf(ui->name, sizeof(ui->name), "%s", cvm(ui)->pc_name);
        ui->kb_sel = 0;
        ui->kb_active = 1;
    } else if (step == GAME_CG_REVIEW) {
        ui->sel = 4;
    } else {
        cg_list(ui);
        cg_describe(ui);
    }
}

static void cg_apply(Ui *ui)
{
    rpg_chargen(ui_rpg(ui), ui->name, ui->cg_race, ui->cg_female, ui->cg_class, ui->cg_sign);
}

void ui_open_chargen(Ui *ui, uint32_t steps)
{
    Rpg *r = ui_rpg(ui);
    const RpgDb *db = r->db;
    uint32_t i;
    if (!steps)
        return;
    if (ui->mode != UI_CHARGEN) {
        ui->cg_steps = 0;
        ui->cg_race = r->race;
        ui->cg_class = r->cls;
        ui->cg_sign = r->sign;
        ui->cg_female = r->female;
        snprintf(ui->name, sizeof(ui->name), "%s", cvm(ui)->pc_name);
        /* defaults for anything the player record does not name */
        if (!ui->cg_race || !rpgdb_race(db, ui->cg_race))
            for (i = 0; i < db->hdr->races.count; ++i)
                if (db->races[i].flags & 1u) {
                    ui->cg_race = db->races[i].hash;
                    break;
                }
        if (!ui->cg_class || !rpg_object(r, ui->cg_class))
            for (i = 0; i < db->hdr->objects.count; ++i)
                if (db->objects[i].type == DRPG_OT_CLAS && (db->objects[i].flags & DRPG_OF_PLAYABLE)) {
                    ui->cg_class = db->objects[i].hash;
                    break;
                }
        if (!ui->cg_sign || !rpg_object(r, ui->cg_sign))
            for (i = 0; i < db->hdr->objects.count; ++i)
                if (db->objects[i].type == DRPG_OT_BSGN) {
                    ui->cg_sign = db->objects[i].hash;
                    break;
                }
    }
    ui->return_mode = UI_NONE;
    ui->cg_steps |= steps;
    ui->mode = UI_CHARGEN;
    cg_enter(ui, lowest_step(ui->cg_steps));
}

static void cg_done_step(Ui *ui)
{
    int next;
    ui->cg_steps &= ~(uint32_t)ui->cg_step;
    cg_apply(ui);
    next = lowest_step(ui->cg_steps);
    if (!next) {
        ui->cg_step = 0;
        ui_close(ui);
        return;
    }
    cg_enter(ui, next);
}

static void update_chargen(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    if (ui->cg_step == GAME_CG_NAME) {
        int k = ui_keyboard_update(ui, p);
        if (k > 0) {
            snprintf(cvm(ui)->pc_name, sizeof(cvm(ui)->pc_name), "%s", ui->name);
            cg_done_step(ui);
        } else if (k < 0 && cvm(ui)->pc_name[0]) {
            cg_done_step(ui); /* keep the old name */
        }
        return;
    }
    if (ui->cg_step == GAME_CG_REVIEW) {
        int dummy = 0;
        ui_list_nav(&ui->sel, &dummy, 5, 5, p);
        if (p & GB_CROSS) {
            if (ui->sel == 4) {
                cg_done_step(ui);
            } else {
                static const int steps[4] = { GAME_CG_NAME, GAME_CG_RACE, GAME_CG_CLASS, GAME_CG_BIRTH };
                ui->cg_steps |= (uint32_t)steps[ui->sel];
                cg_enter(ui, steps[ui->sel]);
            }
        }
        return;
    }
    if (ui_list_nav(&ui->sel, &ui->list_top, (int)ui->cg_count, CG_ROWS, p) || ui->detail != ui->sel)
        cg_describe(ui);
    if (ui->cg_step == GAME_CG_RACE && (p & GB_TRIANGLE))
        ui->cg_female = !ui->cg_female;
    if ((p & GB_CROSS) && ui->sel < (int)ui->cg_count) {
        uint32_t v = ui->cg_items[ui->sel];
        if (ui->cg_step == GAME_CG_RACE)
            ui->cg_race = v;
        else if (ui->cg_step == GAME_CG_CLASS)
            ui->cg_class = v;
        else
            ui->cg_sign = v;
        cg_done_step(ui);
    }
}

static void draw_race_info(Ui *ui, UiCanvas *c, int x, int y)
{
    Rpg *r = ui_rpg(ui);
    const DRpgRace *rc = ui->sel < (int)ui->cg_count ? rpgdb_race(r->db, ui->cg_items[ui->sel]) : NULL;
    char buf[64];
    int a, k, row = 0;
    if (!rc)
        return;
    snprintf(buf, sizeof(buf), "Sex: %s  (/\\ change)", ui->cg_female ? "Female" : "Male");
    ui_text(c, x, y, C_NOTICE, buf, -1);
    y += UI_LINE_H + 3;
    for (a = 0; a < ATTR_COUNT; ++a) {
        snprintf(buf, sizeof(buf), "%-12s %3d", rpg_attr_name(r, a), rc->attribute[a][ui->cg_female ? 1 : 0]);
        ui_text(c, x + (a & 1) * 120, y + (a / 2) * UI_LINE_H, C_TEXT, buf, -1);
    }
    y += 4 * UI_LINE_H + 3;
    ui_text(c, x, y, C_TITLE, "Skill bonuses", -1);
    y += UI_LINE_H;
    for (k = 0; k < 7; ++k) {
        if (rc->bonus_skill[k] < 0)
            continue;
        snprintf(buf, sizeof(buf), "%s +%d", rpg_skill_name(r, rc->bonus_skill[k]), rc->bonus[k]);
        ui_text(c, x + (row & 1) * 120, y + (row / 2) * UI_LINE_H, C_TEXT, buf, -1);
        ++row;
    }
    y += ((row + 1) / 2) * UI_LINE_H + 4;
    {
        int n, i;
        const DRpgObject *o = rpg_object(r, rc->hash);
        if (o && o->list2_count) {
            ui_text(c, x, y, C_TITLE, "Specials", -1);
            y += UI_LINE_H;
            for (i = 0; i < o->list2_count && i < 4; ++i) {
                const DRpgListItem *li = &r->db->lists[o->list2_first + (uint32_t)i];
                ui_text_clipped(c, x, y, C_LINK, rpg_spell_name(r, li->hash), 40);
                y += UI_LINE_H;
            }
        }
        n = (int)PW_MIN(ui->doc.line_count, (uint32_t)PW_MAX(0, (FOOT_Y - 8 - y) / UI_LINE_H));
        ui_draw_doc(c, &ui->doc, x, y + 2, n, 0, -1);
    }
}

static void draw_class_info(Ui *ui, UiCanvas *c, int x, int y)
{
    static const char *const spec[3] = { "Combat", "Magic", "Stealth" };
    Rpg *r = ui_rpg(ui);
    uint32_t cls = ui->sel < (int)ui->cg_count ? ui->cg_items[ui->sel] : 0;
    char buf[64];
    int s, t, row;
    if (!cls)
        return;
    snprintf(buf, sizeof(buf), "Specialization: %s", spec[PW_CLAMP(rpg_class_spec(r, cls), 0, 2)]);
    ui_text(c, x, y, C_TEXT, buf, -1);
    y += UI_LINE_H;
    snprintf(buf, sizeof(buf), "Favored: %s, %s", rpg_attr_name(r, rpg_class_attr(r, cls, 0)),
             rpg_attr_name(r, rpg_class_attr(r, cls, 1)));
    ui_text(c, x, y, C_TEXT, buf, -1);
    y += UI_LINE_H + 4;
    for (t = 0; t < 2; ++t) {
        ui_text(c, x, y, C_TITLE, t ? "Minor skills" : "Major skills", -1);
        y += UI_LINE_H;
        row = 0;
        for (s = 0; s < SKILL_COUNT; ++s)
            if (rpg_class_skill_type(r, cls, s) == t) {
                ui_text(c, x + (row & 1) * 120, y + (row / 2) * UI_LINE_H, C_TEXT, rpg_skill_name(r, s), -1);
                ++row;
            }
        y += ((row + 1) / 2) * UI_LINE_H + 4;
    }
}

static void draw_sign_info(Ui *ui, UiCanvas *c, int x, int y)
{
    Rpg *r = ui_rpg(ui);
    const DRpgObject *o = ui->sel < (int)ui->cg_count ? rpg_object(r, ui->cg_items[ui->sel]) : NULL;
    int i, n;
    if (!o)
        return;
    for (i = 0; i < o->list2_count && i < 6; ++i) {
        const DRpgListItem *li = &r->db->lists[o->list2_first + (uint32_t)i];
        ui_text_clipped(c, x, y, C_LINK, rpg_spell_name(r, li->hash), 40);
        y += UI_LINE_H;
    }
    n = (int)PW_MIN(ui->doc.line_count, (uint32_t)PW_MAX(0, (FOOT_Y - 8 - y) / UI_LINE_H));
    ui_draw_doc(c, &ui->doc, x, y + 4, n, 0, -1);
}

static void draw_review(Ui *ui, UiCanvas *c)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    const DRpgRace *rc = rpgdb_race(r->db, ui->cg_race);
    const DRpgObject *cls = rpg_object(r, ui->cg_class), *sign = rpg_object(r, ui->cg_sign);
    const char *values[4];
    static const char *const labels[4] = { "Name", "Race", "Class", "Birthsign" };
    char buf[96];
    int i, y, t, s;
    values[0] = ui->name[0] ? ui->name : "-";
    values[1] = rc ? rpgdb_name(r->db, rc->name) : "-";
    values[2] = cls ? rpg_name(r, cls) : "-";
    values[3] = sign ? rpg_name(r, sign) : "-";
    for (i = 0; i < 5; ++i) {
        y = 30 + i * (UI_LINE_H + 3);
        if (i == ui->sel)
            ui_row_select(c, 14, y, 34);
        if (i < 4) {
            snprintf(buf, sizeof(buf), "%-10s %s", labels[i], values[i]);
            ui_text_clipped(c, 14, y, C_TEXT, buf, 34);
        } else {
            ui_text(c, 14, y, C_TITLE, "Done", -1);
        }
    }
    y = 30 + 5 * (UI_LINE_H + 3) + 6;
    for (i = 0; i < 3; ++i) {
        static const char *const dn[3] = { "Health", "Magicka", "Fatigue" };
        snprintf(buf, sizeof(buf), "%-8s %d", dn[i], (int)rpg_dyn_max(r, &pc, i));
        ui_text(c, 14, y, C_TEXT, buf, -1);
        y += UI_LINE_H;
    }
    y += 4;
    for (i = 0; i < ATTR_COUNT; ++i) {
        snprintf(buf, sizeof(buf), "%-12s %3d", rpg_attr_name(r, i), (int)rpg_attr(r, &pc, i));
        ui_text(c, 14, y, C_TEXT, buf, -1);
        y += UI_LINE_H;
    }
    y = 30;
    for (t = 0; t < 3; ++t) {
        static const char *const hn[3] = { "Major skills", "Minor skills", "Misc skills" };
        ui_text(c, 240, y, C_TITLE, hn[t], -1);
        y += UI_LINE_H;
        for (s = 0, i = 0; s < SKILL_COUNT; ++s) {
            /* misc skills are many: two columns */
            int col = t == 2 ? (i & 1) : 0, row = t == 2 ? i / 2 : i;
            if (rpg_skill_type(r, s) != t)
                continue;
            snprintf(buf, sizeof(buf), "%-14s%3d", rpg_skill_name(r, s), (int)rpg_skill(r, &pc, s));
            ui_text(c, 240 + col * 114, y + row * UI_LINE_H, C_TEXT, buf, -1);
            ++i;
        }
        y += (t == 2 ? (i + 1) / 2 : i) * UI_LINE_H + 3;
    }
}

static void draw_chargen(Ui *ui, UiCanvas *c)
{
    static const char *const titles[5] = { "Name", "Choose your race", "Choose your class", "Choose your birthsign",
                                           "Review" };
    Rpg *r = ui_rpg(ui);
    int i, t = 0;
    for (i = 0; i < 5; ++i)
        if (ui->cg_step == (1 << i))
            t = i;
    ui_rect(c, 0, 0, UI_SCREEN_W, UI_SCREEN_H, 0xFF100C08u);
    ui_frame(c, 4, 4, 472, 264, C_PANEL);
    ui_text_centered(c, 240, 12, C_TITLE, titles[t]);
    if (ui->cg_step == GAME_CG_NAME) {
        ui_keyboard_draw(ui, c, "What is your name?");
        return;
    }
    if (ui->cg_step == GAME_CG_REVIEW) {
        draw_review(ui, c);
        foot(c, "X change / done");
        return;
    }
    for (i = 0; i < CG_ROWS && ui->list_top + i < (int)ui->cg_count; ++i) {
        int idx = ui->list_top + i, y = 30 + i * UI_LINE_H;
        uint32_t v = ui->cg_items[idx];
        const char *name = ui->cg_step == GAME_CG_RACE ? rpgdb_name(r->db, rpgdb_race(r->db, v)->name)
                                                       : rpg_name(r, rpg_object(r, v));
        if (idx == ui->sel)
            ui_row_select(c, 14, y, 26);
        ui_text_clipped(c, 14, y, C_TEXT, name, 26);
    }
    ui_scrollbar(c, 176, 30, CG_ROWS * UI_LINE_H, (int)ui->cg_count, ui->list_top, CG_ROWS);
    ui_rect(c, 184, 28, 1, CG_ROWS * UI_LINE_H, C_BORDER);
    if (ui->cg_step == GAME_CG_RACE)
        draw_race_info(ui, c, 192, 30);
    else if (ui->cg_step == GAME_CG_CLASS)
        draw_class_info(ui, c, 192, 30);
    else
        draw_sign_info(ui, c, 192, 30);
    foot(c, ui->cg_step == GAME_CG_RACE ? "Up/Down choose   /\\ sex   X confirm" : "Up/Down choose   X confirm");
}

/* ------------------------------------------------------------------------ */
/* level up                                                                 */
/* ------------------------------------------------------------------------ */
void ui_open_levelup(Ui *ui)
{
    ui->mode = UI_LEVELUP;
    ui->return_mode = UI_NONE;
    ui->lv_count = 0;
    ui->sel = 0;
}

static void update_levelup(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    int dummy = 0, k;
    ui_list_nav(&ui->sel, &dummy, ATTR_COUNT + 1, ATTR_COUNT + 1, p);
    if (!(p & GB_CROSS))
        return;
    if (ui->sel == ATTR_COUNT) {
        if (ui->lv_count == 3) {
            Rpg *r = ui_rpg(ui);
            VmRef pc = ui_pc(ui);
            char msg[96];
            if (rpg_level_up(r, ui->lv_pick)) {
                snprintf(msg, sizeof(msg), "You are now level %d.", rpg_level(r, &pc));
                ui_toast(ui, msg);
            }
            r->level_pending = 0;
            ui_close(ui);
        }
        return;
    }
    for (k = 0; k < ui->lv_count; ++k)
        if (ui->lv_pick[k] == ui->sel) {
            memmove(&ui->lv_pick[k], &ui->lv_pick[k + 1], (size_t)(ui->lv_count - k - 1) * sizeof(int));
            --ui->lv_count;
            return;
        }
    if (ui->lv_count < 3)
        ui->lv_pick[ui->lv_count++] = ui->sel;
}

static void draw_levelup(Ui *ui, UiCanvas *c)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    char buf[96], id[32];
    int a, k, y, lvl = rpg_level(r, &pc) + 1;
    ui_frame(c, 70, 20, 340, 232, C_PANEL);
    snprintf(buf, sizeof(buf), "Level %d", lvl);
    ui_text_centered(c, 240, 28, C_TITLE, buf);
    snprintf(id, sizeof(id), "Level_Up_Level%d", lvl);
    ui_doc_reset(&ui->doc, 52);
    ui_doc_add(&ui->doc, K_DIM, rpgdb_gmst_string(r->db, id, rpgdb_gmst_string(r->db, "Level_Up_Default",
                                                  "Through discipline and training you have become stronger.")),
               NULL, 0);
    ui_draw_doc(c, &ui->doc, 84, 42, (int)PW_MIN(ui->doc.line_count, 4u), 0, -1);
    y = 82;
    for (a = 0; a < ATTR_COUNT; ++a) {
        int picked = 0, mult = a == ATTR_LUCK ? 1 : rpg_level_multiplier(r, a);
        for (k = 0; k < ui->lv_count; ++k)
            picked |= ui->lv_pick[k] == a;
        if (a == ui->sel)
            ui_row_select(c, 96, y, 46);
        if (picked)
            ui_rect(c, 86, y + 1, 6, 6, C_NOTICE);
        snprintf(buf, sizeof(buf), "%-14s %3d   x%d", rpg_attr_name(r, a), (int)vm_prop(cvm(ui), &pc,
                                                                                         MWP_ATTR_STRENGTH + a),
                 mult);
        ui_text(c, 96, y, picked ? C_NOTICE : C_TEXT, buf, -1);
        y += UI_LINE_H + 3;
    }
    if (ui->sel == ATTR_COUNT)
        ui_row_select(c, 96, y + 4, 46);
    ui_text(c, 96, y + 4, ui->lv_count == 3 ? C_TITLE : C_DIM, "Done", -1);
    snprintf(buf, sizeof(buf), "Choose three attributes (%d/3)   X toggle", ui->lv_count);
    ui_text(c, 84, 238, C_DIM, buf, -1);
}

/* ------------------------------------------------------------------------ */
/* rest and wait                                                            */
/* ------------------------------------------------------------------------ */
void ui_open_rest(Ui *ui)
{
    char msg[96];
    if (!rpg_can_rest(ui_rpg(ui), msg, sizeof(msg))) {
        ui_toast(ui, msg);
        if (ui->return_mode != UI_NONE)
            ui_close(ui);
        return;
    }
    ui->mode = UI_REST;
    ui->rest_hours = 1;
    ui->rest_sleep = 1;
}

static int hours_to_heal(Ui *ui)
{
    Rpg *r = ui_rpg(ui);
    VmRef pc = ui_pc(ui);
    float hp = rpg_dyn_max(r, &pc, 0) - rpg_dyn(r, &pc, 0), mp = rpg_dyn_max(r, &pc, 1) - rpg_dyn(r, &pc, 1);
    float hr = 0.1f * rpg_attr(r, &pc, ATTR_ENDURANCE);
    float mr = rpg_gmst_f(r, "fRestMagicMult", 0.15f) * rpg_attr(r, &pc, ATTR_INTELLIGENCE);
    int h = 1;
    if (hp > 0.0f && hr > 0.0f)
        h = PW_MAX(h, (int)ceilf(hp / hr));
    if (mp > 0.0f && mr > 0.0f && rpg_effect(r, &pc, MGEF_STUNTED_MAGICKA, -1) <= 0.0f)
        h = PW_MAX(h, (int)ceilf(mp / mr));
    return PW_CLAMP(h, 1, 24);
}

static void update_rest(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    if (p & GB_CIRCLE) {
        ui_close(ui);
        return;
    }
    if (p & (GB_LEFT | GB_DOWN))
        ui->rest_hours = ui->rest_hours > 1 ? ui->rest_hours - 1 : 24;
    if (p & (GB_RIGHT | GB_UP))
        ui->rest_hours = ui->rest_hours < 24 ? ui->rest_hours + 1 : 1;
    if (p & GB_SQUARE)
        ui->rest_sleep = !ui->rest_sleep;
    if (p & GB_TRIANGLE) {
        ui->rest_hours = hours_to_heal(ui);
        ui->rest_sleep = 1;
    }
    if (p & GB_CROSS) {
        Rpg *r = ui_rpg(ui);
        int h = rpg_rest(r, ui->rest_hours, ui->rest_sleep);
        char msg[64];
        snprintf(msg, sizeof(msg), "%s %d hour%s.", ui->rest_sleep ? "You rested for" : "You waited for", h,
                 h == 1 ? "" : "s");
        ui_toast(ui, msg);
        ui->return_mode = UI_NONE;
        if (ui->rest_sleep && r->level_pending && !ui->g->player_dead)
            ui_open_levelup(ui);
        else
            ui_close(ui);
    }
}

static void draw_rest(Ui *ui, UiCanvas *c)
{
    static const char *const months[12] = { "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand",
                                            "Second Seed", "Midyear", "Sun's Height", "Last Seed",
                                            "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star" };
    Vm *vm = cvm(ui);
    char buf[96];
    float hour = vm->g_hour >= 0 ? vm_global_float(vm, vm->g_hour) : 0.0f;
    int day = vm->g_day >= 0 ? (int)vm_global_float(vm, vm->g_day) : 1;
    int month = vm->g_month >= 0 ? (int)vm_global_float(vm, vm->g_month) : 0;
    int hh = (int)hour, mm = (int)((hour - (float)hh) * 60.0f);
    ui_frame(c, 120, 80, 240, 112, C_PANEL);
    ui_text_centered(c, 240, 88, C_TITLE, ui->rest_sleep ? "Rest" : "Wait");
    snprintf(buf, sizeof(buf), "%d %s, %d:%02d %s", day, months[PW_CLAMP(month, 0, 11)],
             hh % 12 == 0 ? 12 : hh % 12, mm, hh < 12 ? "a.m." : "p.m.");
    ui_text_centered(c, 240, 104, C_TEXT, buf);
    snprintf(buf, sizeof(buf), "< %d hour%s >", ui->rest_hours, ui->rest_hours == 1 ? "" : "s");
    ui_text_centered(c, 240, 124, C_NOTICE, buf);
    ui_bar(c, 140, 140, 200, 5, (float)ui->rest_hours, 24.0f, C_TITLE);
    ui_text_centered(c, 240, 156, C_DIM, "X rest   /\\ until healed");
    ui_text_centered(c, 240, 168, C_DIM, ui->rest_sleep ? "[] wait instead   O cancel" : "[] rest instead   O cancel");
}

/* ------------------------------------------------------------------------ */
/* saved games                                                              */
/* ------------------------------------------------------------------------ */
void ui_set_saves(Ui *ui, const char *dir, float *playtime)
{
    ui->save_dir = dir;
    ui->playtime = playtime;
}

static void scan_slots(Ui *ui)
{
    int k;
    for (k = 0; k < SAVE_SLOTS; ++k)
        save_slot_info(ui->save_dir, k, &ui->slots[k]);
}

void ui_open_saves(Ui *ui, int saving)
{
    if (!ui->save_dir || !ui->g->rpg)
        return;
    ui->return_mode = ui->mode == UI_DEATH ? UI_DEATH : UI_NONE;
    ui->mode = UI_SAVELOAD;
    ui->save_mode = saving;
    ui->sel = saving ? 1 : PW_MAX(0, save_latest(ui->save_dir));
    ui->confirm = 0;
    scan_slots(ui);
}

static void update_saves(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    int dummy = 0;
    char err[120];
    if (p & GB_CIRCLE) {
        ui_close(ui);
        return;
    }
    if (ui_list_nav(&ui->sel, &dummy, SAVE_SLOTS, SAVE_SLOTS, p))
        ui->confirm = 0;
    if (!(p & GB_CROSS))
        return;
    if (ui->save_mode) {
        if (ui->slots[ui->sel].used && !ui->confirm) {
            ui->confirm = 1;
            ui_status(ui, "X again to overwrite this save.");
            return;
        }
        if (save_game(ui->g, ui->save_dir, ui->sel, ui->playtime ? *ui->playtime : 0.0f, err, sizeof(err)) == 0) {
            ui_toast(ui, "Game saved.");
            ui->return_mode = UI_NONE;
            ui_close(ui);
        } else {
            ui_status(ui, err);
        }
        ui->confirm = 0;
        scan_slots(ui);
        return;
    }
    if (!ui->slots[ui->sel].used) {
        ui_status(ui, "This slot is empty.");
        return;
    }
    if (load_game(ui->g, ui->save_dir, ui->sel, ui->playtime, err, sizeof(err)) == 0) {
        ui_toast(ui, "Game loaded.");
        ui->return_mode = UI_NONE;
        ui_close(ui);
    } else {
        ui_status(ui, err);
    }
}

static void draw_saves(Ui *ui, UiCanvas *c)
{
    static const char *const months[12] = { "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand",
                                            "Second Seed", "Midyear", "Sun's Height", "Last Seed",
                                            "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star" };
    char buf[112];
    int k;
    ui_frame(c, 30, 16, 420, 240, C_PANEL);
    ui_text_centered(c, 240, 24, C_TITLE, ui->save_mode ? "Save Game" : "Load Game");
    for (k = 0; k < SAVE_SLOTS; ++k) {
        const SaveSlotInfo *si = &ui->slots[k];
        int y = 42 + k * 24;
        if (k == ui->sel)
            ui_rect(c, 40, y - 3, 400, 22, C_SEL_BG);
        snprintf(buf, sizeof(buf), "%s", k == 0 ? "Quicksave" : "");
        if (!si->used) {
            snprintf(buf, sizeof(buf), "%d  %s", k, k == 0 ? "Quicksave  (empty)" : "(empty)");
            ui_text(c, 46, y + 4, C_DIM, buf, -1);
            continue;
        }
        {
            int secs = (int)si->hdr.playtime, hh = (int)si->hdr.hour;
            snprintf(buf, sizeof(buf), "%d  %s, level %d%s", k, si->hdr.name, si->hdr.level, k == 0 ? "  (quick)" : "");
            ui_text_clipped(c, 46, y, C_TEXT, buf, 40);
            snprintf(buf, sizeof(buf), "%d:%02d played", secs / 3600, (secs / 60) % 60);
            ui_text(c, 434 - (int)strlen(buf) * UI_CHAR_W, y, C_DIM, buf, -1);
            snprintf(buf, sizeof(buf), "   %s - %d %s, %d:00", si->hdr.cell, si->hdr.day,
                     months[PW_CLAMP(si->hdr.month, 0, 11)], hh);
            ui_text_clipped(c, 46, y + 9, C_DIM, buf, 64);
        }
    }
    ui_text(c, 46, 240, C_DIM, ui->save_mode ? "X save   O back" : "X load   O back", -1);
}

/* ------------------------------------------------------------------------ */
/* death                                                                    */
/* ------------------------------------------------------------------------ */
static void update_death(Ui *ui, const InputState *in)
{
    char err[120];
    int latest = ui->save_dir ? save_latest(ui->save_dir) : -1;
    if ((in->pressed & GB_CROSS) && latest >= 0) {
        if (load_game(ui->g, ui->save_dir, latest, ui->playtime, err, sizeof(err)) == 0) {
            ui->return_mode = UI_NONE;
            ui_close(ui);
            ui_toast(ui, "Game loaded.");
        } else {
            ui_status(ui, err);
        }
        return;
    }
    if ((in->pressed & GB_TRIANGLE) && ui->save_dir) {
        ui_open_saves(ui, 0);
        return;
    }
    if (in->pressed & (GB_SQUARE | GB_START) || ((in->pressed & GB_CROSS) && latest < 0)) {
        game_respawn(ui->g);
        ui->return_mode = UI_NONE;
        ui_close(ui);
    }
}

static void draw_death(Ui *ui, UiCanvas *c)
{
    int latest = ui->save_dir ? save_latest(ui->save_dir) : -1;
    ui_rect(c, 0, 0, UI_SCREEN_W, UI_SCREEN_H, 0x90000030u);
    ui_frame(c, 120, 96, 240, 74, C_PANEL);
    ui_text_centered(c, 240, 106, C_BAD, "You have died.");
    if (latest >= 0) {
        ui_text_centered(c, 240, 124, C_TEXT, "X  load the last save");
        ui_text_centered(c, 240, 136, C_TEXT, "/\\  choose a save");
        ui_text_centered(c, 240, 148, C_DIM, "[]  start again from here");
    } else {
        ui_text_centered(c, 240, 130, C_TEXT, "X  start again from here");
    }
}

/* ------------------------------------------------------------------------ */
/* HUD                                                                      */
/* ------------------------------------------------------------------------ */
void ui_draw_hud(Ui *ui, UiCanvas *c)
{
    Game *g = ui->g;
    const char *label = game_target_label(g);
    char buf[96];
    /* crosshair */
    ui_rect(c, 236, 135, 3, 1, 0xC0FFFFFFu);
    ui_rect(c, 241, 135, 3, 1, 0xC0FFFFFFu);
    ui_rect(c, 240, 131, 1, 3, 0xC0FFFFFFu);
    ui_rect(c, 240, 137, 1, 3, 0xC0FFFFFFu);
    if (label && *label) {
        int n;
        snprintf(buf, sizeof(buf), "%.48s", label);
        n = (int)strlen(buf);
        ui_rect(c, 240 - n * UI_CHAR_W / 2 - 3, 150, n * UI_CHAR_W + 6, 11, 0x90000000u);
        ui_text(c, 240 - n * UI_CHAR_W / 2, 152, C_LINK, buf, n);
    }
    if (g->message_time > 0.0f && g->message[0]) {
        int n = (int)PW_MIN(strlen(g->message), 70u);
        ui_rect(c, 240 - n * UI_CHAR_W / 2 - 3, 238, n * UI_CHAR_W + 6, 11, 0x90000000u);
        ui_text(c, 240 - n * UI_CHAR_W / 2, 240, C_PLAYER, g->message, n);
    }
    if (!g->rpg)
        return;
    {
        Rpg *r = ui_rpg(ui);
        VmRef pc = ui_pc(ui);
        static const uint32_t cols[3] = { C_HEALTH, C_MAGICKA, C_FATIGUE };
        int i;
        for (i = 0; i < 3; ++i)
            ui_bar(c, 8, 244 + i * 8, 64, 5, rpg_dyn(r, &pc, i), rpg_dyn_max(r, &pc, i), cols[i]);
        /* readied weapon / spell */
        buf[0] = '\0';
        if (r->draw_state == 1) {
            int w = rpg_equipped(r, &pc, RPG_SLOT_WEAPON);
            if (w >= 0)
                ui_stack_label(ui, w, buf, sizeof(buf));
            else
                snprintf(buf, sizeof(buf), "Hand-to-hand");
        } else if (r->draw_state == 2) {
            if (r->sel_item >= 0 && vm_item_at(cvm(ui), r->sel_item))
                snprintf(buf, sizeof(buf), "%s",
                         rpg_name(r, rpg_object(r, vm_item_at(cvm(ui), r->sel_item)->item)));
            else if (r->sel_spell)
                snprintf(buf, sizeof(buf), "%s", rpg_spell_name(r, r->sel_spell));
            else
                snprintf(buf, sizeof(buf), "(no spell selected)");
        }
        if (buf[0]) {
            int n = (int)PW_MIN(strlen(buf), 36u);
            ui_rect(c, 472 - n * UI_CHAR_W - 4, 256, n * UI_CHAR_W + 6, 11, 0x90000000u);
            ui_text(c, 472 - n * UI_CHAR_W - 1, 258, r->draw_state == 2 ? C_MAGICKA | 0xFF404040u : C_TEXT,
                    buf, n);
            if (r->draw_state == 1 && g->attack_holding)
                ui_bar(c, 472 - 60, 250, 58, 3, PW_MIN(g->attack_charge, 1.0f), 1.0f, C_TITLE);
        }
        if (g->sneaking)
            ui_text(c, 80, 258, C_DIM, "Sneaking", -1);
        if (r->level_pending)
            ui_text(c, 80, 244, C_NOTICE, "Level up: rest", -1);
        /* last enemy hit */
        if (g->enemy_time > 0.0f && g->enemy_key) {
            uint32_t a;
            for (a = 0; a < g->actors.count; ++a) {
                VmRef ref = game_actor_ref(g, a);
                if (ref.key != g->enemy_key)
                    continue;
                ui_bar(c, 200, 10, 80, 5, rpg_dyn(r, &ref, 0), rpg_dyn_max(r, &ref, 0), C_HEALTH);
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* dispatch                                                                 */
/* ------------------------------------------------------------------------ */
void ui_char_update(Ui *ui, const InputState *in)
{
    switch (ui->mode) {
    case UI_CHARGEN:
        update_chargen(ui, in);
        break;
    case UI_LEVELUP:
        update_levelup(ui, in);
        break;
    case UI_REST:
        update_rest(ui, in);
        break;
    case UI_DEATH:
        update_death(ui, in);
        break;
    case UI_SAVELOAD:
        update_saves(ui, in);
        break;
    default:
        break;
    }
}

void ui_char_draw(Ui *ui, UiCanvas *c)
{
    switch (ui->mode) {
    case UI_CHARGEN:
        draw_chargen(ui, c);
        break;
    case UI_LEVELUP:
        draw_levelup(ui, c);
        break;
    case UI_REST:
        draw_rest(ui, c);
        break;
    case UI_DEATH:
        draw_death(ui, c);
        break;
    case UI_SAVELOAD:
        draw_saves(ui, c);
        break;
    default:
        break;
    }
}
