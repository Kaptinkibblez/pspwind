/*
 * ui.c - portable UI logic: layout, input, and the canvas command list.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "font8x8.h"
#include "ui_internal.h"

static const uint32_t kind_color[] = { C_TEXT, C_TITLE, C_PLAYER, C_NOTICE, C_DIM, C_TITLE };

static UiDoc g_msgdoc;

/* ------------------------------------------------------------------------ */
/* canvas                                                                   */
/* ------------------------------------------------------------------------ */
void ui_canvas_clear(UiCanvas *c)
{
    c->count = 0;
    c->pool_used = 0;
    c->dropped = 0;
}

void ui_rect(UiCanvas *c, int x, int y, int w, int h, uint32_t abgr)
{
    UiCmd *cmd;
    if (w <= 0 || h <= 0)
        return;
    if (c->count >= UI_MAX_CMDS) {
        c->dropped++;
        return;
    }
    cmd = &c->cmds[c->count++];
    cmd->type = UI_CMD_RECT;
    cmd->pad = 0;
    cmd->x = (int16_t)x;
    cmd->y = (int16_t)y;
    cmd->w = (int16_t)w;
    cmd->h = (int16_t)h;
    cmd->ofs = 0;
    cmd->color = abgr;
}

void ui_text(UiCanvas *c, int x, int y, uint32_t abgr, const char *s, int len)
{
    UiCmd *cmd;
    if (len < 0)
        len = (int)strlen(s);
    if (len == 0)
        return;
    if (c->count >= UI_MAX_CMDS || c->pool_used + (uint32_t)len > UI_TEXT_POOL) {
        c->dropped++;
        return;
    }
    cmd = &c->cmds[c->count++];
    cmd->type = UI_CMD_TEXT;
    cmd->pad = 0;
    cmd->x = (int16_t)x;
    cmd->y = (int16_t)y;
    cmd->w = (int16_t)len;
    cmd->h = UI_CHAR_H;
    cmd->ofs = (uint16_t)c->pool_used;
    cmd->color = abgr;
    memcpy(c->pool + c->pool_used, s, (size_t)len);
    c->pool_used += (uint32_t)len;
}

static uint32_t blend(uint32_t dst, uint32_t src)
{
    uint32_t a = src >> 24, ia = 255u - a, out = 0xFF000000u;
    int sh;
    for (sh = 0; sh < 24; sh += 8) {
        uint32_t s = (src >> sh) & 0xFFu, d = (dst >> sh) & 0xFFu;
        out |= ((s * a + d * ia) / 255u) << sh;
    }
    return out;
}

void ui_render_rgba(const UiCanvas *c, uint32_t *pixels, int stride)
{
    uint32_t i;
    for (i = 0; i < c->count; ++i) {
        const UiCmd *cmd = &c->cmds[i];
        int x, y;
        if (cmd->type == UI_CMD_RECT) {
            for (y = PW_MAX(0, cmd->y); y < PW_MIN(UI_SCREEN_H, cmd->y + cmd->h); ++y)
                for (x = PW_MAX(0, cmd->x); x < PW_MIN(UI_SCREEN_W, cmd->x + cmd->w); ++x)
                    pixels[y * stride + x] = blend(pixels[y * stride + x], cmd->color);
        } else {
            int k;
            for (k = 0; k < cmd->w; ++k) {
                const uint8_t *glyph = &font8x8[(unsigned char)c->pool[cmd->ofs + k] * 8];
                int gx = cmd->x + k * UI_CHAR_W;
                for (y = 0; y < 8; ++y) {
                    int py = cmd->y + y;
                    if (py < 0 || py >= UI_SCREEN_H)
                        continue;
                    for (x = 0; x < 8; ++x) {
                        int px = gx + x;
                        if (px < 0 || px >= UI_SCREEN_W || !(glyph[y] & (0x80 >> x)))
                            continue;
                        pixels[py * stride + px] = blend(pixels[py * stride + px], cmd->color);
                    }
                }
            }
        }
    }
}

void ui_frame(UiCanvas *c, int x, int y, int w, int h, uint32_t bg)
{
    ui_rect(c, x, y, w, h, bg);
    ui_rect(c, x, y, w, 1, C_BORDER);
    ui_rect(c, x, y + h - 1, w, 1, C_BORDER);
    ui_rect(c, x, y, 1, h, C_BORDER);
    ui_rect(c, x + w - 1, y, 1, h, C_BORDER);
}

void ui_text_clipped(UiCanvas *c, int x, int y, uint32_t col, const char *s, int max_chars)
{
    int n = (int)strlen(s);
    ui_text(c, x, y, col, s, n > max_chars ? max_chars : n);
}

void ui_text_centered(UiCanvas *c, int cx, int y, uint32_t col, const char *s)
{
    int n = (int)strlen(s);
    ui_text(c, cx - n * UI_CHAR_W / 2, y, col, s, n);
}

/* ------------------------------------------------------------------------ */
/* documents: word wrap + hyperlink segments                                */
/* ------------------------------------------------------------------------ */
void ui_doc_reset(UiDoc *d, int width)
{
    d->text_len = 0;
    d->line_count = 0;
    d->seg_count = 0;
    d->link_count = 0;
    d->width = width;
}

static void doc_line(UiDoc *d, uint32_t ofs, uint32_t len, int kind)
{
    UiLine *l;
    if (d->line_count >= UI_DOC_LINES)
        return;
    l = &d->lines[d->line_count++];
    l->ofs = ofs;
    l->len = (uint16_t)len;
    l->kind = (uint8_t)kind;
    l->pad = 0;
}

void ui_doc_add(UiDoc *d, int kind, const char *text, const DlgLink *links, int nlinks)
{
    uint32_t n = (uint32_t)strlen(text), base, pos = 0;
    uint32_t first_line = d->line_count;
    int i;
    if (d->text_len + n + 1 > UI_DOC_TEXT)
        n = d->text_len + 1 < UI_DOC_TEXT ? UI_DOC_TEXT - d->text_len - 1 : 0;
    base = d->text_len;
    memcpy(d->text + base, text, n);
    d->text[base + n] = '\0';
    d->text_len += n + 1;
    if (n == 0) {
        doc_line(d, base, 0, kind);
        return;
    }
    while (pos < n) {
        uint32_t end = pos, brk, line_end;
        /* one paragraph at a time */
        while (end < n && d->text[base + end] != '\n')
            end++;
        if (end == pos) {
            doc_line(d, base + pos, 0, kind);
            pos = end + 1;
            continue;
        }
        while (pos < end) {
            if (end - pos <= (uint32_t)d->width) {
                line_end = end;
                brk = end;
            } else {
                brk = pos + (uint32_t)d->width;
                while (brk > pos && d->text[base + brk] != ' ')
                    brk--;
                if (brk == pos)
                    brk = pos + (uint32_t)d->width; /* a word longer than the line */
                line_end = brk;
            }
            doc_line(d, base + pos, line_end - pos, kind);
            pos = brk;
            while (pos < end && d->text[base + pos] == ' ')
                pos++;
        }
        pos = end + 1;
    }
    /* link segments, split at line breaks */
    for (i = 0; i < nlinks && d->link_count < UI_DOC_LINKS; ++i) {
        uint32_t ls = base + links[i].start, le = ls + links[i].len, li;
        int added = 0;
        for (li = first_line; li < d->line_count; ++li) {
            const UiLine *l = &d->lines[li];
            uint32_t s0 = PW_MAX(ls, l->ofs), s1 = PW_MIN(le, l->ofs + l->len);
            if (s0 >= s1 || d->seg_count >= UI_DOC_SEGS)
                continue;
            if (!added) {
                d->links[d->link_count].topic = links[i].topic;
                d->links[d->link_count].line = (uint16_t)li;
                added = 1;
            }
            d->segs[d->seg_count].line = (uint16_t)li;
            d->segs[d->seg_count].col = (uint16_t)(s0 - l->ofs);
            d->segs[d->seg_count].len = (uint16_t)(s1 - s0);
            d->segs[d->seg_count].link = (uint16_t)d->link_count;
            d->seg_count++;
        }
        if (added)
            d->link_count++;
    }
}

void ui_draw_doc(UiCanvas *c, const UiDoc *d, int x, int y, int rows, int first, int sel_link)
{
    uint32_t si = 0;
    int r;
    while (si < d->seg_count && d->segs[si].line < (uint32_t)first)
        si++;
    for (r = 0; r < rows; ++r) {
        uint32_t li = (uint32_t)(first + r), col = 0;
        const UiLine *l;
        uint32_t color;
        int ly = y + r * UI_LINE_H;
        if (li >= d->line_count)
            break;
        l = &d->lines[li];
        color = kind_color[l->kind];
        while (si < d->seg_count && d->segs[si].line == li) {
            const UiSeg *s = &d->segs[si];
            if (s->col > col)
                ui_text(c, x + (int)col * UI_CHAR_W, ly, color, d->text + l->ofs + col, s->col - (int)col);
            if ((int)s->link == sel_link)
                ui_rect(c, x + s->col * UI_CHAR_W - 1, ly - 1, s->len * UI_CHAR_W + 1, UI_CHAR_H + 1,
                        C_LINK_SEL_BG);
            ui_text(c, x + s->col * UI_CHAR_W, ly, C_LINK, d->text + l->ofs + s->col, s->len);
            col = (uint32_t)s->col + s->len;
            si++;
        }
        if (col < l->len)
            ui_text(c, x + (int)col * UI_CHAR_W, ly, color, d->text + l->ofs + col, (int)(l->len - col));
    }
}

void ui_scrollbar(UiCanvas *c, int x, int y, int h, int total, int first, int rows)
{
    int th, ty;
    if (total <= rows)
        return;
    ui_rect(c, x, y, 2, h, 0x60FFFFFFu);
    th = PW_MAX(6, h * rows / total);
    ty = y + (h - th) * first / PW_MAX(1, total - rows);
    ui_rect(c, x, ty, 2, th, C_TITLE);
}

/* ------------------------------------------------------------------------ */
/* setup                                                                    */
/* ------------------------------------------------------------------------ */
void ui_init(Ui *ui, Game *g)
{
    memset(ui, 0, sizeof(*ui));
    ui->g = g;
    ui->detail = -1;
    ui->seen_revision = 0xFFFFFFFFu;
}

static Vm *ui_vm(Ui *ui)
{
    return ui->g->rpg ? &ui->g->rpg->vm : NULL;
}

static const VmMessage *modal(const Ui *ui)
{
    const Vm *vm = ui->g->rpg ? &ui->g->rpg->vm : NULL;
    const VmMessage *m = vm ? vm_message_front(vm) : NULL;
    return (m && m->nbuttons) ? m : NULL;
}

int ui_active(const Ui *ui)
{
    return ui->mode != UI_NONE || modal(ui) != NULL;
}

void ui_toast(Ui *ui, const char *text)
{
    int i;
    for (i = 0; i < UI_TOASTS - 1; ++i) {
        memcpy(ui->toast[i], ui->toast[i + 1], sizeof(ui->toast[i]));
        ui->toast_time[i] = ui->toast_time[i + 1];
    }
    {
        size_t n = strlen(text);
        if (n >= sizeof(ui->toast[0]))
            n = sizeof(ui->toast[0]) - 1;
        memcpy(ui->toast[UI_TOASTS - 1], text, n);
        ui->toast[UI_TOASTS - 1][n] = '\0';
    }
    ui->toast_time[UI_TOASTS - 1] = 4.0f;
}

void ui_status(Ui *ui, const char *text)
{
    snprintf(ui->status, sizeof(ui->status), "%s", text);
    ui->status_time = text[0] ? 3.0f : 0.0f;
}

void ui_bar(UiCanvas *c, int x, int y, int w, int h, float cur, float max, uint32_t col)
{
    int fill = max > 0.0f ? (int)((float)w * PW_CLAMP(cur / max, 0.0f, 1.0f) + 0.5f) : 0;
    ui_rect(c, x - 1, y - 1, w + 2, h + 2, 0xC0000000u);
    ui_rect(c, x, y, w, h, 0xC0302820u);
    if (fill > 0)
        ui_rect(c, x, y, fill, h, col);
}

int ui_list_nav(int *sel, int *top, int count, int rows, uint32_t pressed)
{
    int old = *sel;
    if (count <= 0) {
        *sel = 0;
        *top = 0;
        return 0;
    }
    if (pressed & GB_UP)
        *sel = (*sel + count - 1) % count;
    if (pressed & GB_DOWN)
        *sel = (*sel + 1) % count;
    if (*sel >= count)
        *sel = count - 1;
    if (*sel < *top)
        *top = *sel;
    if (*sel >= *top + rows)
        *top = *sel - rows + 1;
    *top = PW_CLAMP(*top, 0, PW_MAX(0, count - rows));
    return *sel != old;
}

void ui_row_select(UiCanvas *c, int x, int y, int cols)
{
    ui_rect(c, x - 2, y - 1, cols * UI_CHAR_W + 4, UI_LINE_H, C_SEL_BG);
}

Rpg *ui_rpg(Ui *ui)
{
    return &ui->g->rpg->rpg;
}

VmRef ui_pc(Ui *ui)
{
    return vm_player_ref(&ui->g->rpg->vm);
}

/* ---- on-screen keyboard */
#define KB_COLS 13
#define KB_ROWS 5
static const char kb_chars[KB_ROWS * KB_COLS + 1] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 '-";
#define KB_KEYS (KB_ROWS * KB_COLS + 2) /* + Delete, Done */

int ui_keyboard_update(Ui *ui, uint32_t p)
{
    int sel = ui->kb_sel, row = sel / KB_COLS, col = sel % KB_COLS;
    size_t n = strlen(ui->name);
    if (p & GB_LEFT)
        col = (col + KB_COLS - 1) % KB_COLS;
    if (p & GB_RIGHT)
        col = (col + 1) % KB_COLS;
    if (p & GB_UP)
        row = (row + KB_ROWS) % (KB_ROWS + 1);
    if (p & GB_DOWN)
        row = (row + 1) % (KB_ROWS + 1);
    if (row == KB_ROWS)
        col = col < KB_COLS / 2 ? 0 : 1;
    sel = row * KB_COLS + col;
    ui->kb_sel = sel;
    if (p & GB_SQUARE && n > 0)
        ui->name[n - 1] = '\0';
    if (p & GB_CIRCLE)
        return -1;
    if (p & (GB_START | GB_TRIANGLE))
        return ui->name[0] ? 1 : 0;
    if (p & GB_CROSS) {
        if (sel == KB_ROWS * KB_COLS) {
            if (n > 0)
                ui->name[n - 1] = '\0';
        } else if (sel == KB_ROWS * KB_COLS + 1) {
            return ui->name[0] ? 1 : 0;
        } else if (n + 1 < sizeof(ui->name) && n < 24) {
            ui->name[n] = kb_chars[sel];
            ui->name[n + 1] = '\0';
        }
    }
    return 0;
}

void ui_keyboard_draw(Ui *ui, UiCanvas *c, const char *title)
{
    int r, k, x0 = 240 - (KB_COLS * 24) / 2, y0 = 100;
    char buf[48];
    ui_frame(c, x0 - 16, 50, KB_COLS * 24 + 32, 170, C_PANEL2);
    ui_text_centered(c, 240, 58, C_TITLE, title);
    snprintf(buf, sizeof(buf), "%s_", ui->name);
    ui_rect(c, x0, 74, KB_COLS * 24, 14, 0xC0000000u);
    ui_text(c, x0 + 4, 77, C_TEXT, buf, -1);
    for (r = 0; r < KB_ROWS; ++r)
        for (k = 0; k < KB_COLS; ++k) {
            int i = r * KB_COLS + k, x = x0 + k * 24, y = y0 + r * 16;
            char ch[2];
            ch[0] = kb_chars[i] == ' ' ? '_' : kb_chars[i];
            ch[1] = '\0';
            if (i == ui->kb_sel)
                ui_rect(c, x + 2, y - 3, 18, 14, C_SEL_BG);
            ui_text(c, x + 9, y, i == ui->kb_sel ? C_TITLE : C_TEXT, ch, 1);
        }
    for (k = 0; k < 2; ++k) {
        int i = KB_ROWS * KB_COLS + k, x = x0 + k * (KB_COLS * 12), y = y0 + KB_ROWS * 16;
        if (i == ui->kb_sel)
            ui_rect(c, x + 2, y - 3, KB_COLS * 12 - 4, 14, C_SEL_BG);
        ui_text_centered(c, x + KB_COLS * 6, y, i == ui->kb_sel ? C_TITLE : C_TEXT, k ? "Done" : "Delete");
    }
    ui_text_centered(c, 240, 206, C_DIM, "X type  [] delete  START done  O back");
}

/* ---- item labels and details */
static int stack_health(Ui *ui, const VmItem *it, int *max)
{
    const DRpgObject *o = rpg_object(ui_rpg(ui), it->item);
    *max = o ? rpg_item_health_max(ui_rpg(ui), o) : 0;
    return it->health == VM_FULL ? *max : (int)it->health;
}

void ui_stack_label(Ui *ui, int stack, char *out, uint32_t cap)
{
    Rpg *r = ui_rpg(ui);
    VmItem *it = vm_item_at(&ui->g->rpg->vm, stack);
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    int n = 0, max, h;
    if (!o) {
        snprintf(out, cap, "?");
        return;
    }
    n = snprintf(out, cap, "%s", rpg_name(r, o));
    if (it->count > 1 && n < (int)cap)
        n += snprintf(out + n, cap - (uint32_t)n, " (%d)", (int)it->count);
    h = stack_health(ui, it, &max);
    if (max > 0 && h < max && n < (int)cap && o->type != DRPG_OT_LIGH)
        n += snprintf(out + n, cap - (uint32_t)n, " %d%%", max > 0 ? h * 100 / max : 0);
    if (rpg_is_soulgem(r, o) && it->soul && n < (int)cap) {
        const DRpgObject *soul = rpg_object(r, it->soul);
        snprintf(out + n, cap - (uint32_t)n, " [%s]", soul ? rpg_name(r, soul) : "soul");
    }
}

static const char *const type_names[DRPG_OT_COUNT] = {
    "NPC", "Creature", "Weapon", "Armor", "Clothing", "Book", "Potion", "Ingredient", "Misc",
    "Apparatus", "Lockpick", "Probe", "Repair item", "Light", "Container", "", "", "Door", "",
    "", "Class", "Race", "Region", "Birthsign" };

void ui_item_details(Ui *ui, int stack, int x, int y, int cols, int rows, UiCanvas *c)
{
    Rpg *r = ui_rpg(ui);
    VmItem *it = vm_item_at(&ui->g->rpg->vm, stack);
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    char buf[112];
    int line = 0, max, h, i, n = 0;
    const DRpgEffect *e = NULL;
    const DRpgSpell *en = NULL;
#define DLINE(col, ...)                                                    \
    do {                                                                   \
        if (line < rows) {                                                 \
            snprintf(buf, sizeof(buf), __VA_ARGS__);                       \
            ui_text_clipped(c, x, y + line * UI_LINE_H, col, buf, cols);   \
        }                                                                  \
        ++line;                                                            \
    } while (0)
    if (!o)
        return;
    DLINE(C_TITLE, "%s", rpg_name(r, o));
    DLINE(C_DIM, "%s", type_names[o->type]);
    if (o->type == DRPG_OT_WEAP) {
        const uint8_t *cs = (const uint8_t *)&o->link[1], *th = (const uint8_t *)&o->link[2];
        if (o->stat[0] >= 9 && o->stat[0] <= 13)
            DLINE(C_TEXT, "Attack %d-%d", cs[0], cs[1]);
        else
            DLINE(C_TEXT, "Chop %d-%d  Slash %d-%d  Thrust %d-%d", cs[0], cs[1], cs[2], cs[3], th[0], th[1]);
    } else if (o->type == DRPG_OT_ARMO) {
        DLINE(C_TEXT, "Armor rating %d", (int)o->stat[3]);
    } else if (o->type == DRPG_OT_LOCK || o->type == DRPG_OT_PROB || o->type == DRPG_OT_REPA ||
               o->type == DRPG_OT_APPA) {
        float q;
        int32_t bits = o->stat[1];
        memcpy(&q, &bits, sizeof(q));
        DLINE(C_TEXT, "Quality %.2f", (double)q);
    }
    h = stack_health(ui, it, &max);
    if (max > 0 && o->type != DRPG_OT_LIGH)
        DLINE(C_TEXT, "%s %d/%d", o->type == DRPG_OT_WEAP || o->type == DRPG_OT_ARMO ? "Condition" : "Uses", h, max);
    DLINE(C_TEXT, "Weight %.1f   Value %d", (double)o->weight, rpg_item_value(r, it));
    if (o->type == DRPG_OT_ALCH || o->type == DRPG_OT_INGR) {
        e = rpg_object_effects(r, o, &n);
        if (o->type == DRPG_OT_INGR)
            n = PW_MIN(n, rpg_ingredient_known_effects(r));
    } else if (o->type <= DRPG_OT_BOOK && o->type >= DRPG_OT_WEAP && o->link[0]) {
        en = rpg_spell(r, o->link[0]);
        if (en) {
            static const char *const etype[4] = { "Cast once", "Cast when strikes", "Cast when used",
                                                  "Constant effect" };
            float ch = it->charge < 0.0f ? rpg_item_charge_max(r, o) : it->charge;
            e = rpg_spell_effects(r, en, &n);
            if (en->type < 4)
                DLINE(C_NOTICE, "%s", etype[en->type]);
            if (en->type == 1 || en->type == 2)
                DLINE(C_TEXT, "Charge %d/%d", (int)ch, (int)rpg_item_charge_max(r, o));
        }
    }
    if (rpg_is_soulgem(r, o)) {
        const DRpgObject *soul = it->soul ? rpg_object(r, it->soul) : NULL;
        DLINE(C_TEXT, "Soul: %s", soul ? rpg_name(r, soul) : "empty");
    }
    for (i = 0; i < n; ++i) {
        char eb[96];
        if (o->type == DRPG_OT_INGR) {
            snprintf(eb, sizeof(eb), "%s", rpg_effect_name(r, e[i].effect));
            if (e[i].skill >= 0 && (e[i].effect == MGEF_FORTIFY_SKILL || e[i].effect == MGEF_DRAIN_SKILL ||
                                    e[i].effect == MGEF_DAMAGE_SKILL || e[i].effect == MGEF_RESTORE_SKILL ||
                                    e[i].effect == MGEF_ABSORB_SKILL))
                snprintf(eb, sizeof(eb), "%s %s", rpg_effect_name(r, e[i].effect), rpg_skill_name(r, e[i].skill));
            else if (e[i].attribute >= 0)
                snprintf(eb, sizeof(eb), "%s %s", rpg_effect_name(r, e[i].effect),
                         rpg_attr_name(r, e[i].attribute));
        } else {
            rpg_describe_effect(r, &e[i], eb, sizeof(eb));
        }
        DLINE(C_LINK, "%s", eb);
    }
    if (o->type == DRPG_OT_INGR && n < 4) {
        int total;
        rpg_object_effects(r, o, &total);
        for (i = n; i < total; ++i)
            DLINE(C_DIM, "?");
    }
#undef DLINE
}

/* ------------------------------------------------------------------------ */
/* dialogue                                                                 */
/* ------------------------------------------------------------------------ */
#define DLG_TEXT_X 10
#define DLG_TEXT_Y 24
#define DLG_TEXT_COLS 52
#define DLG_ROWS 26
#define DLG_LIST_X 338
#define DLG_LIST_COLS 21

static int dlg_rows(const Ui *ui)
{
    const Dialogue *d = &ui->g->rpg->dlg;
    return d->in_choice ? DLG_ROWS - (int)d->choice_count - 1 : DLG_ROWS;
}

static void build_dialogue_doc(Ui *ui)
{
    Dialogue *d = &ui->g->rpg->dlg;
    uint32_t cursor = 0;
    int kind;
    const char *text;
    ui_doc_reset(&ui->doc, DLG_TEXT_COLS);
    while (dlg_history_next(d, &cursor, &kind, &text)) {
        if (kind == DLG_ENTRY_TITLE) {
            ui_doc_add(&ui->doc, K_TITLE, text, NULL, 0);
        } else if (kind == DLG_ENTRY_TEXT) {
            DlgLink links[64];
            int n = dlg_links(d, text, links, 64);
            ui_doc_add(&ui->doc, K_TEXT, text, links, n);
            ui_doc_add(&ui->doc, K_TEXT, "", NULL, 0);
        } else if (kind == DLG_ENTRY_PLAYER) {
            char buf[128];
            snprintf(buf, sizeof(buf), "> %s", text);
            ui_doc_add(&ui->doc, K_PLAYER, buf, NULL, 0);
        } else {
            ui_doc_add(&ui->doc, K_NOTICE, text, NULL, 0);
        }
    }
    if (ui->follow) {
        int rows = dlg_rows(ui);
        ui->scroll = (int)ui->doc.line_count > rows ? (int)ui->doc.line_count - rows : 0;
    }
    if (ui->sel_link >= (int)ui->doc.link_count)
        ui->sel_link = (int)ui->doc.link_count - 1;
    ui->seen_revision = d->revision;
}

static const char *const svc_gmst[UI_SVC_COUNT] = { "sPersuasion", "sBarter", "sSpells", "sTraining",
                                                    "sRepair", "sTravel", "sSpellmakingMenuTitle",
                                                    "sEnchanting" };
static const char *const svc_names[UI_SVC_COUNT] = { "Persuasion", "Barter", "Spells", "Training",
                                                     "Repair", "Travel", "Spellmaking", "Enchanting" };

static void find_services(Ui *ui)
{
    Rpg *r = ui_rpg(ui);
    Dialogue *d = &ui->g->rpg->dlg;
    uint32_t s = rpg_services(r, &d->actor);
    ui->svc_count = 0;
    ui->npc = d->actor;
    if (d->creature)
        return;
    ui->svc[ui->svc_count++] = UI_SVC_PERSUADE;
    if (s & (RPG_SVC_WEAPON | RPG_SVC_ARMOR | RPG_SVC_CLOTHING | RPG_SVC_BOOKS | RPG_SVC_INGREDIENTS |
             RPG_SVC_PICKS | RPG_SVC_PROBES | RPG_SVC_LIGHTS | RPG_SVC_APPARATUS | RPG_SVC_REPAIRITEMS |
             RPG_SVC_MISC | RPG_SVC_MAGICITEMS | RPG_SVC_POTIONS))
        ui->svc[ui->svc_count++] = UI_SVC_BARTER;
    if (s & RPG_SVC_SPELLS)
        ui->svc[ui->svc_count++] = UI_SVC_SPELLS;
    if (s & RPG_SVC_TRAINING)
        ui->svc[ui->svc_count++] = UI_SVC_TRAINING;
    if (s & RPG_SVC_REPAIR)
        ui->svc[ui->svc_count++] = UI_SVC_REPAIR;
    if (rpg_travel_count(r, &d->actor) > 0)
        ui->svc[ui->svc_count++] = UI_SVC_TRAVEL;
    if (s & RPG_SVC_SPELLMAKING)
        ui->svc[ui->svc_count++] = UI_SVC_SPELLMAKING;
    if (s & RPG_SVC_ENCHANTING)
        ui->svc[ui->svc_count++] = UI_SVC_ENCHANT;
}

void ui_end_dialogue(Ui *ui)
{
    Dialogue *d = &ui->g->rpg->dlg;
    ui->return_mode = UI_NONE;
    rpg_end_conversation(ui_rpg(ui), &d->actor);
    dlg_end(d);
    ui_close(ui);
}

static void open_dialogue(Ui *ui)
{
    find_services(ui);
    ui->mode = UI_DIALOGUE;
    ui->focus = 0;
    ui->sel_topic = 0;
    ui->topic_top = 0;
    ui->sel_link = -1;
    ui->sel_choice = 0;
    ui->follow = 1;
    ui->seen_revision = 0xFFFFFFFFu;
}

static void build_menu(Ui *ui);

void ui_close(Ui *ui)
{
    int back = ui->return_mode;
    ui->return_mode = UI_NONE;
    ui->detail = -1;
    ui->kb_active = 0;
    ui->mode = back;
    if (back == UI_MENU) {
        build_menu(ui);
    } else if (back == UI_DIALOGUE) {
        ui->seen_revision = 0xFFFFFFFFu; /* the document was reused: rebuild */
        ui->follow = 1;
    }
}

static void ensure_link_visible(Ui *ui)
{
    int rows = dlg_rows(ui), line;
    if (ui->sel_link < 0 || ui->sel_link >= (int)ui->doc.link_count)
        return;
    line = ui->doc.links[ui->sel_link].line;
    if (line < ui->scroll)
        ui->scroll = line;
    else if (line >= ui->scroll + rows)
        ui->scroll = line - rows + 1;
    ui->follow = 0;
}

static void update_dialogue(Ui *ui, const InputState *in)
{
    Dialogue *d = &ui->g->rpg->dlg;
    uint32_t p = in->pressed;
    int svc = d->goodbye ? 0 : ui->svc_count;
    int items = svc + (int)d->list_count + 1; /* services + topics + Goodbye */
    int rows = dlg_rows(ui), max_scroll;
    if (!d->active) {
        ui_close(ui);
        return;
    }
    if (d->revision != ui->seen_revision) {
        ui->follow = 1;
        build_dialogue_doc(ui);
    }
    max_scroll = PW_MAX(0, (int)ui->doc.line_count - rows);
    if (p & GB_L) {
        ui->scroll = PW_MAX(0, ui->scroll - (rows - 2));
        ui->follow = 0;
    }
    if (p & GB_R) {
        ui->scroll = PW_MIN(max_scroll, ui->scroll + (rows - 2));
        ui->follow = ui->scroll >= max_scroll;
    }
    if (d->in_choice) {
        if (p & GB_UP)
            ui->sel_choice = (ui->sel_choice + (int)d->choice_count - 1) % (int)d->choice_count;
        if (p & GB_DOWN)
            ui->sel_choice = (ui->sel_choice + 1) % (int)d->choice_count;
        if (p & GB_CROSS) {
            int c = ui->sel_choice;
            ui->sel_choice = 0;
            dlg_answer(d, (uint32_t)c);
        }
        return;
    }
    if (d->goodbye) {
        if (p & (GB_CROSS | GB_CIRCLE))
            ui_end_dialogue(ui);
        return;
    }
    if (p & GB_CIRCLE) {
        ui_end_dialogue(ui);
        return;
    }
    if (ui->focus == 0) {
        if (p & GB_UP)
            ui->sel_topic = (ui->sel_topic + items - 1) % items;
        if (p & GB_DOWN)
            ui->sel_topic = (ui->sel_topic + 1) % items;
        if ((p & GB_RIGHT) && ui->doc.link_count) {
            ui->focus = 1;
            ui->sel_link = (int)ui->doc.link_count - 1;
            ensure_link_visible(ui);
        }
        if (p & GB_CROSS) {
            if (ui->sel_topic < svc) {
                int service = ui->svc[ui->sel_topic];
                /* everyone listens to flattery; other services can be refused */
                if (service == UI_SVC_PERSUADE || !dlg_service_refused(d))
                    ui_open_service(ui, service);
            } else if (ui->sel_topic - svc >= (int)d->list_count) {
                ui_end_dialogue(ui);
            } else {
                dlg_ask(d, d->list[ui->sel_topic - svc]);
            }
        }
    } else {
        if (ui->sel_link >= (int)ui->doc.link_count)
            ui->sel_link = (int)ui->doc.link_count - 1;
        if (!ui->doc.link_count || (p & GB_LEFT)) {
            ui->focus = 0;
            ui->sel_link = -1;
            return;
        }
        if (p & GB_UP)
            ui->sel_link = PW_MAX(0, ui->sel_link - 1);
        if (p & GB_DOWN)
            ui->sel_link = PW_MIN((int)ui->doc.link_count - 1, ui->sel_link + 1);
        if (p & (GB_UP | GB_DOWN))
            ensure_link_visible(ui);
        if (p & GB_CROSS) {
            uint32_t topic = ui->doc.links[ui->sel_link].topic;
            dlg_ask(d, topic);
            /* keep the list cursor in sync with the asked topic */
            {
                uint32_t i;
                for (i = 0; i < d->list_count; ++i)
                    if (d->list[i] == topic)
                        ui->sel_topic = (int)i + svc;
            }
            ui->sel_link = INT16_MAX;
        }
    }
    if (ui->sel_topic >= items)
        ui->sel_topic = items - 1;
}

static void draw_dialogue(Ui *ui, UiCanvas *c)
{
    Dialogue *d = &ui->g->rpg->dlg;
    Vm *vm = &ui->g->rpg->vm;
    char buf[96];
    int svc = d->goodbye ? 0 : ui->svc_count;
    int rows = dlg_rows(ui), i, items = svc + (int)d->list_count + 1, list_rows = DLG_ROWS;
    if (d->revision != ui->seen_revision)
        build_dialogue_doc(ui);
    if (ui->sel_link >= (int)ui->doc.link_count)
        ui->sel_link = ui->doc.link_count ? (int)ui->doc.link_count - 1 : -1;
    ui_frame(c, 4, 4, 472, 264, C_PANEL);
    ui_rect(c, 5, 5, 470, 13, C_PANEL2);
    ui_text_centered(c, 240, 8, C_TITLE, d->actor_name);
    if (!d->creature) {
        snprintf(buf, sizeof(buf), "Disposition %d", vm_disposition(vm, &d->actor));
        ui_text(c, 470 - (int)strlen(buf) * UI_CHAR_W, 8, C_DIM, buf, -1);
    }
    ui_rect(c, DLG_LIST_X - 8, 19, 1, 246, C_BORDER);
    /* history */
    ui_draw_doc(c, &ui->doc, DLG_TEXT_X, DLG_TEXT_Y, rows, ui->scroll, ui->focus == 1 ? ui->sel_link : -1);
    ui_scrollbar(c, DLG_LIST_X - 13, DLG_TEXT_Y, rows * UI_LINE_H, (int)ui->doc.line_count, ui->scroll, rows);
    /* choices */
    if (d->in_choice) {
        int y = DLG_TEXT_Y + (rows + 1) * UI_LINE_H;
        for (i = 0; i < (int)d->choice_count; ++i) {
            snprintf(buf, sizeof(buf), "%d. %s", i + 1, d->choices[i].text);
            if (i == ui->sel_choice)
                ui_rect(c, DLG_TEXT_X - 2, y - 1, DLG_TEXT_COLS * UI_CHAR_W + 4, UI_LINE_H, C_SEL_BG);
            ui_text_clipped(c, DLG_TEXT_X, y, i == ui->sel_choice ? C_LINK : C_PLAYER, buf, DLG_TEXT_COLS);
            y += UI_LINE_H;
        }
    }
    /* topic list (+ Goodbye) */
    if (ui->sel_topic < ui->topic_top)
        ui->topic_top = ui->sel_topic;
    if (ui->sel_topic >= ui->topic_top + list_rows)
        ui->topic_top = ui->sel_topic - list_rows + 1;
    for (i = 0; i < list_rows && ui->topic_top + i < items; ++i) {
        int idx = ui->topic_top + i, y = DLG_TEXT_Y + i * UI_LINE_H, t = idx - svc;
        int selected = idx == ui->sel_topic && ui->focus == 0 && !d->in_choice;
        const char *name = idx < svc ? rpgdb_gmst_string(&ui->g->rpg->db, svc_gmst[ui->svc[idx]], svc_names[ui->svc[idx]])
                           : t < (int)d->list_count ? rpgdb_topic_name(&ui->g->rpg->db, d->list[t])
                                                    : rpgdb_gmst_string(&ui->g->rpg->db, "sGoodbye", "Goodbye");
        uint32_t col = d->in_choice || (d->goodbye && t < (int)d->list_count) ? C_DIM
                       : idx < svc ? C_NOTICE
                       : t >= (int)d->list_count ? C_TITLE
                       : d->list_exhausted[t] ? C_DIM : C_TEXT;
        if (selected)
            ui_rect(c, DLG_LIST_X - 2, y - 1, DLG_LIST_COLS * UI_CHAR_W + 4, UI_LINE_H, C_SEL_BG);
        ui_text_clipped(c, DLG_LIST_X, y, col, name, DLG_LIST_COLS);
    }
    ui_scrollbar(c, 470, DLG_TEXT_Y, list_rows * UI_LINE_H, items, ui->topic_top, list_rows);
}

/* ------------------------------------------------------------------------ */
/* journal menu                                                             */
/* ------------------------------------------------------------------------ */
#define MENU_X 14
#define MENU_Y 34
#define MENU_COLS 74
#define MENU_ROWS 24

static const char *tab_names[UI_TAB_COUNT] = { "Stats", "Inventory", "Magic", "Map", "Journal" };
static const char *jtab_names[UI_J_COUNT] = { "Entries", "Quests", "Topics" };

static int tab_enabled(const Ui *ui, int tab)
{
    static const uint32_t need[UI_TAB_COUNT] = { GAME_MENU_STATS, GAME_MENU_INVENTORY, GAME_MENU_MAGIC,
                                                 GAME_MENU_MAP, 0 };
    return !need[tab] || (ui->g->menus_enabled & need[tab]);
}

static void add_entry_text(Ui *ui, uint32_t info, int kind)
{
    char raw[1024], fixed[1024];
    GameRpg *r = ui->g->rpg;
    rpgdb_text(&r->db, r->db.infos[info].text, raw, sizeof(raw));
    vm_fix_defines(&r->vm, '%', raw, fixed, sizeof(fixed), NULL);
    ui_doc_add(&ui->doc, kind, fixed, NULL, 0);
}

static void quest_name(Ui *ui, uint32_t topic, char *out, uint32_t cap)
{
    GameRpg *r = ui->g->rpg;
    const DRpgTopic *t = &r->db.topics[topic];
    uint32_t i;
    for (i = 0; i < t->info_count; ++i) {
        const DRpgInfo *in = &r->db.infos[t->info_first + i];
        if (in->flags & DRPG_IF_QUEST_NAME) {
            rpgdb_text(&r->db, in->text, out, cap);
            if (*out)
                return;
        }
    }
    strncpy(out, rpgdb_topic_name(&r->db, topic), cap - 1);
    out[cap - 1] = '\0';
}

static void build_journal(Ui *ui);

static void build_menu(Ui *ui)
{
    if (ui->tab != UI_TAB_JOURNAL) {
        ui_menu_build(ui);
        return;
    }
    build_journal(ui);
}

static void build_journal(Ui *ui)
{
    GameRpg *r = ui->g->rpg;
    Vm *vm = &r->vm;
    uint32_t i, t, k;
    char date[64], name[128];
    ui_doc_reset(&ui->doc, MENU_COLS);
    ui->item_count = 0;
    if (ui->jtab == UI_J_ENTRIES) {
        for (i = 0; i < vm->journal_count; ++i) {
            const VmJournalEntry *e = &vm->journal[i];
            vm_date_string(vm, e->day, e->month, date, sizeof(date));
            snprintf(name, sizeof(name), "%s (Day %u)", date, (unsigned)e->days_passed);
            ui_doc_add(&ui->doc, K_HEADER, name, NULL, 0);
            add_entry_text(ui, e->info, K_TEXT);
            ui_doc_add(&ui->doc, K_TEXT, "", NULL, 0);
        }
        if (!vm->journal_count)
            ui_doc_add(&ui->doc, K_DIM, "Your journal is empty.", NULL, 0);
        ui->list_top = PW_MAX(0, (int)ui->doc.line_count - MENU_ROWS);
        return;
    }
    if (ui->detail >= 0) {
        if (ui->jtab == UI_J_QUESTS) {
            quest_name(ui, (uint32_t)ui->detail, name, sizeof(name));
            ui_doc_add(&ui->doc, K_TITLE, name, NULL, 0);
            ui_doc_add(&ui->doc, K_TEXT, "", NULL, 0);
            for (i = 0; i < vm->journal_count; ++i) {
                if (vm->journal[i].topic != (uint16_t)ui->detail)
                    continue;
                vm_date_string(vm, vm->journal[i].day, vm->journal[i].month, date, sizeof(date));
                ui_doc_add(&ui->doc, K_HEADER, date, NULL, 0);
                add_entry_text(ui, vm->journal[i].info, K_TEXT);
                ui_doc_add(&ui->doc, K_TEXT, "", NULL, 0);
            }
        } else {
            const DRpgTopic *tp = &r->db.topics[ui->detail];
            ui_doc_add(&ui->doc, K_TITLE, rpgdb_topic_name(&r->db, (uint32_t)ui->detail), NULL, 0);
            ui_doc_add(&ui->doc, K_TEXT, "", NULL, 0);
            for (k = 0; k < tp->info_count; ++k) {
                if (!vm_heard(vm, tp->info_first + k))
                    continue;
                add_entry_text(ui, tp->info_first + k, K_TEXT);
                ui_doc_add(&ui->doc, K_TEXT, "", NULL, 0);
            }
        }
        ui->list_top = 0;
        return;
    }
    if (ui->jtab == UI_J_QUESTS) {
        int pass;
        for (pass = 0; pass < 2; ++pass) {
            for (t = 0; t < r->db.hdr->topics.count && ui->item_count < PW_COUNTOF(ui->items); ++t) {
                uint8_t f = vm->quest_flags[t];
                if (!(f & VMQ_STARTED) || ((f & VMQ_FINISHED) != 0) != (pass == 1))
                    continue;
                quest_name(ui, t, name, sizeof(name));
                name[MENU_COLS - 12] = '\0'; /* one line per list item */
                if (pass == 1) {
                    char tmp[160];
                    snprintf(tmp, sizeof(tmp), "%s (finished)", name);
                    ui_doc_add(&ui->doc, K_DIM, tmp, NULL, 0);
                } else {
                    ui_doc_add(&ui->doc, K_TEXT, name, NULL, 0);
                }
                ui->items[ui->item_count++] = (uint16_t)t;
            }
        }
        if (!ui->item_count)
            ui_doc_add(&ui->doc, K_DIM, "No quests yet.", NULL, 0);
    } else {
        for (t = 0; t < r->db.hdr->topics.count && ui->item_count < PW_COUNTOF(ui->items); ++t) {
            const DRpgTopic *tp = &r->db.topics[t];
            int heard = 0;
            if (tp->type != DRPG_TT_TOPIC)
                continue;
            for (k = 0; k < tp->info_count && !heard; ++k)
                heard = vm_heard(vm, tp->info_first + k);
            if (!heard)
                continue;
            ui_doc_add(&ui->doc, K_TEXT, rpgdb_topic_name(&r->db, t), NULL, 0);
            ui->items[ui->item_count++] = (uint16_t)t;
        }
        if (!ui->item_count)
            ui_doc_add(&ui->doc, K_DIM, "No topics recorded yet.", NULL, 0);
    }
    /* one line per item: lists never wrap (names are short) */
    ui->sel = PW_MIN(ui->sel, PW_MAX(0, (int)ui->item_count - 1));
}

static void open_menu(Ui *ui)
{
    ui->mode = UI_MENU;
    ui->detail = -1;
    while (!tab_enabled(ui, ui->tab))
        ui->tab = (ui->tab + 1) % UI_TAB_COUNT;
    ui->sel = 0;
    ui->list_top = 0;
    build_menu(ui);
}

static void update_menu(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    int lines = (int)ui->doc.line_count;
    int is_list = ui->jtab != UI_J_ENTRIES && ui->detail < 0;
    if (p & GB_SELECT) {
        ui_close(ui);
        return;
    }
    if (p & (GB_L | GB_R)) {
        int k;
        for (k = 0; k < UI_TAB_COUNT; ++k) {
            ui->tab = (ui->tab + ((p & GB_R) ? 1 : UI_TAB_COUNT - 1)) % UI_TAB_COUNT;
            if (tab_enabled(ui, ui->tab))
                break;
        }
        ui->detail = -1;
        ui->sel = 0;
        ui->list_top = 0;
        build_menu(ui);
        return;
    }
    if (ui->tab != UI_TAB_JOURNAL) {
        ui_menu_update(ui, in);
        return;
    }
    if (p & GB_TRIANGLE) {
        ui->jtab = (ui->jtab + 1) % UI_J_COUNT;
        ui->detail = -1;
        ui->sel = 0;
        build_menu(ui);
        return;
    }
    if (p & GB_CIRCLE) {
        if (ui->detail >= 0) {
            ui->detail = -1;
            build_menu(ui);
        } else {
            ui_close(ui);
        }
        return;
    }
    if (is_list) {
        if ((p & GB_UP) && ui->item_count)
            ui->sel = (ui->sel + (int)ui->item_count - 1) % (int)ui->item_count;
        if ((p & GB_DOWN) && ui->item_count)
            ui->sel = (ui->sel + 1) % (int)ui->item_count;
        if (ui->sel < ui->list_top)
            ui->list_top = ui->sel;
        if (ui->sel >= ui->list_top + MENU_ROWS)
            ui->list_top = ui->sel - MENU_ROWS + 1;
        if ((p & GB_CROSS) && ui->item_count) {
            ui->detail = ui->items[ui->sel];
            build_menu(ui);
        }
    } else {
        int max_top = PW_MAX(0, lines - MENU_ROWS);
        if (p & GB_UP)
            ui->list_top = PW_MAX(0, ui->list_top - 1);
        if (p & GB_DOWN)
            ui->list_top = PW_MIN(max_top, ui->list_top + 1);
        if (p & GB_LEFT)
            ui->list_top = PW_MAX(0, ui->list_top - MENU_ROWS);
        if (p & GB_RIGHT)
            ui->list_top = PW_MIN(max_top, ui->list_top + MENU_ROWS);
    }
}

static void draw_menu(Ui *ui, UiCanvas *c)
{
    int i, x = 12;
    int is_list = ui->jtab != UI_J_ENTRIES && ui->detail < 0;
    ui_frame(c, 4, 4, 472, 264, C_PANEL);
    for (i = 0; i < UI_TAB_COUNT; ++i) {
        int w = (int)strlen(tab_names[i]) * UI_CHAR_W + 12;
        if (i == ui->tab)
            ui_rect(c, x, 9, w, 14, C_SEL_BG);
        ui_rect(c, x, 22, w, 1, C_BORDER);
        ui_text(c, x + 6, 12, i == ui->tab ? C_TITLE : tab_enabled(ui, i) ? C_DIM : 0xFF505458u, tab_names[i], -1);
        x += w + 4;
    }
    ui_text(c, 470 - 22 * UI_CHAR_W, 12, C_DIM, "L/R tabs  SELECT close", -1);
    if (ui->tab != UI_TAB_JOURNAL) {
        ui_menu_draw(ui, c);
        return;
    }
    ui_text(c, 470 - 32 * UI_CHAR_W, 258, C_DIM, jtab_names[ui->jtab], -1);
    ui_text(c, 470 - 24 * UI_CHAR_W, 258, C_DIM, "/\\ next page  O back", -1);
    if (is_list && ui->item_count)
        ui_rect(c, MENU_X - 2, MENU_Y + (ui->sel - ui->list_top) * UI_LINE_H - 1,
                MENU_COLS * UI_CHAR_W + 4, UI_LINE_H, C_SEL_BG);
    ui_draw_doc(c, &ui->doc, MENU_X, MENU_Y, MENU_ROWS, ui->list_top, -1);
    ui_scrollbar(c, 468, MENU_Y, MENU_ROWS * UI_LINE_H, (int)ui->doc.line_count, ui->list_top, MENU_ROWS);
}

/* ------------------------------------------------------------------------ */
/* books                                                                    */
/* ------------------------------------------------------------------------ */
#define BOOK_COLS 70
#define BOOK_ROWS 25

void ui_open_book(Ui *ui, uint32_t object, uint32_t inst)
{
    static char text[UI_DOC_TEXT - 1];
    GameRpg *r = ui->g->rpg;
    const DRpgObject *o = rpgdb_object(&r->db, object);
    ui->mode = UI_BOOK;
    ui->book_object = object;
    ui->book_instance = inst;
    ui->list_top = 0;
    strncpy(ui->book_title, o ? rpgdb_name(&r->db, o->name) : "", sizeof(ui->book_title) - 1);
    ui->book_title[sizeof(ui->book_title) - 1] = '\0';
    ui_doc_reset(&ui->doc, BOOK_COLS);
    if (!o || !rpgdb_text(&r->db, o->link[1], text, sizeof(text)))
        strcpy(text, "(The pages are blank.)");
    ui_doc_add(&ui->doc, K_TEXT, text, NULL, 0);
    /* skill books teach on the first read, wherever they are read */
    rpg_read_book(&r->rpg, object);
    game_sound(ui->g, "Book Open");
}

static void update_book(Ui *ui, const InputState *in)
{
    uint32_t p = in->pressed;
    int max_top = PW_MAX(0, (int)ui->doc.line_count - BOOK_ROWS);
    if (p & (GB_CROSS | GB_CIRCLE)) {
        ui_close(ui);
        return;
    }
    if ((p & GB_SQUARE) && ui->book_instance != GAME_NO_TARGET) {
        game_take(ui->g, ui->book_instance);
        ui_close(ui);
        return;
    }
    if (p & GB_UP)
        ui->list_top = PW_MAX(0, ui->list_top - 1);
    if (p & GB_DOWN)
        ui->list_top = PW_MIN(max_top, ui->list_top + 1);
    if (p & (GB_L | GB_LEFT))
        ui->list_top = PW_MAX(0, ui->list_top - BOOK_ROWS);
    if (p & (GB_R | GB_RIGHT))
        ui->list_top = PW_MIN(max_top, ui->list_top + BOOK_ROWS);
}

static void draw_book(Ui *ui, UiCanvas *c)
{
    ui_frame(c, 10, 6, 460, 260, 0xF0283848u);
    ui_text_centered(c, 240, 12, C_TITLE, ui->book_title);
    ui_rect(c, 20, 23, 440, 1, C_BORDER);
    ui_draw_doc(c, &ui->doc, 30, 30, BOOK_ROWS, ui->list_top, -1);
    ui_scrollbar(c, 460, 30, BOOK_ROWS * UI_LINE_H, (int)ui->doc.line_count, ui->list_top, BOOK_ROWS);
    ui_text(c, 30, 256, C_DIM,
            ui->book_instance != GAME_NO_TARGET ? "Up/Down scroll   [] take   X close" : "Up/Down scroll   X close",
            -1);
}

/* ------------------------------------------------------------------------ */
/* message boxes and toasts                                                 */
/* ------------------------------------------------------------------------ */
static void update_modal(Ui *ui, const VmMessage *m, const InputState *in)
{
    uint32_t p = in->pressed;
    int n = m->nbuttons;
    if (p & (GB_LEFT | GB_UP))
        ui->msg_sel = (ui->msg_sel + n - 1) % n;
    if (p & (GB_RIGHT | GB_DOWN))
        ui->msg_sel = (ui->msg_sel + 1) % n;
    if (ui->msg_sel >= n)
        ui->msg_sel = 0;
    if (p & GB_CROSS) {
        vm_message_answer(ui_vm(ui), ui->msg_sel);
        ui->msg_sel = 0;
    }
}

static void draw_modal(Ui *ui, UiCanvas *c, const VmMessage *m)
{
    int i, w = 0, h, x, y, rows, bx;
    char label[64];
    ui_doc_reset(&g_msgdoc, 56);
    ui_doc_add(&g_msgdoc, K_TEXT, m->text, NULL, 0);
    rows = (int)PW_MIN(g_msgdoc.line_count, 16u);
    for (i = 0; i < rows; ++i)
        w = PW_MAX(w, (int)g_msgdoc.lines[i].len);
    {
        int bw = 0;
        for (i = 0; i < m->nbuttons; ++i)
            bw += (int)strlen(m->buttons[i]) + 6;
        w = PW_MAX(w, bw);
    }
    w = w * UI_CHAR_W + 24;
    h = rows * UI_LINE_H + 36;
    x = (UI_SCREEN_W - w) / 2;
    y = (UI_SCREEN_H - h) / 2;
    ui_frame(c, x, y, w, h, 0xF0181410u);
    ui_draw_doc(c, &g_msgdoc, x + 12, y + 10, rows, 0, -1);
    bx = 0;
    for (i = 0; i < m->nbuttons; ++i)
        bx += ((int)strlen(m->buttons[i]) + 6) * UI_CHAR_W;
    bx = x + (w - bx) / 2;
    for (i = 0; i < m->nbuttons; ++i) {
        int bw = ((int)strlen(m->buttons[i]) + 4) * UI_CHAR_W;
        snprintf(label, sizeof(label), "%s", m->buttons[i]);
        if (i == ui->msg_sel)
            ui_rect(c, bx, y + h - 20, bw, 13, C_SEL_BG);
        ui_rect(c, bx, y + h - 8, bw, 1, C_BORDER);
        ui_text(c, bx + 2 * UI_CHAR_W, y + h - 17, i == ui->msg_sel ? C_TITLE : C_TEXT, label, -1);
        bx += bw + 2 * UI_CHAR_W;
    }
}

static void draw_toasts(Ui *ui, UiCanvas *c)
{
    int i, y = 226;
    for (i = UI_TOASTS - 1; i >= 0; --i) {
        int n;
        if (ui->toast_time[i] <= 0.0f || !ui->toast[i][0])
            continue;
        n = (int)strlen(ui->toast[i]);
        if (n > 76)
            n = 76;
        ui_rect(c, 240 - n * UI_CHAR_W / 2 - 4, y - 2, n * UI_CHAR_W + 8, 12, C_TOAST_BG);
        ui_text(c, 240 - n * UI_CHAR_W / 2, y, C_TEXT, ui->toast[i], n);
        y -= 13;
    }
}

/* ------------------------------------------------------------------------ */
/* frame entry points                                                       */
/* ------------------------------------------------------------------------ */
void ui_update(Ui *ui, const InputState *in, float dt)
{
    Game *g = ui->g;
    Vm *vm = ui_vm(ui);
    const VmMessage *m;
    int i, opened = 0;
    InputState none;
    memset(&none, 0, sizeof(none));
    for (i = 0; i < UI_TOASTS; ++i)
        if (ui->toast_time[i] > 0.0f)
            ui->toast_time[i] -= dt;
    if (!g->rpg) {
        g->ui_event = GAME_UI_NONE;
        return;
    }
    /* notifications and button-less messages become toasts */
    for (i = 0; i < (int)vm->notify_count; ++i)
        ui_toast(ui, vm->notify[i]);
    vm->notify_count = 0;
    while ((m = vm_message_front(vm)) != NULL && m->nbuttons == 0) {
        ui_toast(ui, m->text);
        vm_message_answer(vm, 0);
    }
    /* requests from the game */
    if (g->ui_event == GAME_UI_DIALOGUE) {
        open_dialogue(ui);
        opened = 1;
    } else if (g->ui_event == GAME_UI_BOOK) {
        ui->return_mode = UI_NONE;
        ui_open_book(ui, g->ui_object, g->ui_instance);
        opened = 1;
    } else if (g->ui_event == GAME_UI_LOOT) {
        ui_open_loot(ui, &g->ui_owner);
        opened = 1;
    } else if (g->ui_event == GAME_UI_CHARGEN) {
        ui_open_chargen(ui, g->ui_chargen);
        g->ui_chargen = 0;
        opened = 1;
    } else if (g->ui_event == GAME_UI_REST) {
        ui->return_mode = UI_NONE;
        ui_open_rest(ui);
        opened = 1;
    } else if (g->ui_event == GAME_UI_DEATH) {
        ui->return_mode = UI_NONE;
        ui->mode = UI_DEATH;
        opened = 1;
    }
    g->ui_event = GAME_UI_NONE;
    if (ui->status_time > 0.0f)
        ui->status_time -= dt;
    if (opened)
        in = &none;
    if (ui_active(ui) && !opened && (in->pressed & (GB_UP | GB_DOWN | GB_LEFT | GB_RIGHT | GB_CROSS | GB_CIRCLE |
                                                     GB_SQUARE | GB_TRIANGLE | GB_L | GB_R)))
        game_sound(g, "Menu Click");
    m = modal(ui);
    if (m) {
        update_modal(ui, m, in);
    } else {
        switch (ui->mode) {
        case UI_NONE:
            if ((in->pressed & GB_SELECT) && !g->player_dead)
                open_menu(ui);
            break;
        case UI_DIALOGUE:
            update_dialogue(ui, in);
            break;
        case UI_MENU:
            update_menu(ui, in);
            break;
        case UI_BOOK:
            update_book(ui, in);
            break;
        case UI_LOOT:
        case UI_BARTER:
        case UI_SERVICE:
        case UI_ALCHEMY:
        case UI_REPAIR:
        case UI_SPELLMAKING:
        case UI_ENCHANT:
            ui_trade_update(ui, in);
            break;
        case UI_CHARGEN:
        case UI_LEVELUP:
        case UI_REST:
        case UI_DEATH:
        case UI_SAVELOAD:
            ui_char_update(ui, in);
            break;
        default:
            break;
        }
    }
    g->menu_mode = ui_active(ui);
}

void ui_draw(Ui *ui, UiCanvas *c)
{
    const VmMessage *m;
    ui_canvas_clear(c);
    if (ui->mode == UI_NONE)
        ui_draw_hud(ui, c);
    if (!ui->g->rpg) {
        draw_toasts(ui, c);
        return;
    }
    switch (ui->mode) {
    case UI_DIALOGUE:
        draw_dialogue(ui, c);
        break;
    case UI_MENU:
        draw_menu(ui, c);
        break;
    case UI_BOOK:
        draw_book(ui, c);
        break;
    case UI_LOOT:
    case UI_BARTER:
    case UI_SERVICE:
    case UI_ALCHEMY:
    case UI_REPAIR:
    case UI_SPELLMAKING:
    case UI_ENCHANT:
        ui_trade_draw(ui, c);
        break;
    case UI_CHARGEN:
    case UI_LEVELUP:
    case UI_REST:
    case UI_DEATH:
    case UI_SAVELOAD:
        ui_char_draw(ui, c);
        break;
    default:
        break;
    }
    if (ui->mode != UI_NONE && ui->status_time > 0.0f && ui->status[0]) {
        /* screen status replaces the footer hints for a moment */
        int n = (int)PW_MIN(strlen(ui->status), 76u);
        ui_rect(c, 8, 252, 464, 14, 0xFF201810u);
        ui_rect(c, 8, 252, 464, 1, C_BORDER);
        ui_text(c, 240 - n * UI_CHAR_W / 2, 256, C_NOTICE, ui->status, n);
    }
    m = modal(ui);
    if (m)
        draw_modal(ui, c, m);
    draw_toasts(ui, c);
}
