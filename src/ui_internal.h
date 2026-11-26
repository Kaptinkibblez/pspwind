/*
 * ui_internal.h - drawing helpers and screen entry points shared by the UI
 * modules (ui.c, ui_menu.c, ui_trade.c, ui_char.c).
 */
#ifndef PSPWIND_UI_INTERNAL_H
#define PSPWIND_UI_INTERNAL_H

#include "ui.h"

/* colours (ABGR) */
#define C_PANEL 0xE0141010u
#define C_PANEL2 0xF0201814u
#define C_BORDER 0xFF7A6A4Au
#define C_TITLE 0xFF60C8E8u
#define C_TEXT 0xFFD8E4EAu
#define C_DIM 0xFF8A9098u
#define C_LINK 0xFFFFB070u
#define C_LINK_SEL_BG 0xFF905028u
#define C_SEL_BG 0xFF604018u
#define C_PLAYER 0xFFA0D8A8u
#define C_NOTICE 0xFF70D0F0u
#define C_TOAST_BG 0xC0101010u
#define C_HEALTH 0xFF3030D0u
#define C_MAGICKA 0xFFD06030u
#define C_FATIGUE 0xFF30B040u
#define C_BAD 0xFF5050F0u

enum { K_TEXT = 0, K_TITLE, K_PLAYER, K_NOTICE, K_DIM, K_HEADER };

void ui_frame(UiCanvas *c, int x, int y, int w, int h, uint32_t bg);
void ui_text_clipped(UiCanvas *c, int x, int y, uint32_t col, const char *s, int max_chars);
void ui_text_centered(UiCanvas *c, int cx, int y, uint32_t col, const char *s);
void ui_draw_doc(UiCanvas *c, const UiDoc *d, int x, int y, int rows, int first, int sel_link);
void ui_scrollbar(UiCanvas *c, int x, int y, int h, int total, int first, int rows);
void ui_bar(UiCanvas *c, int x, int y, int w, int h, float cur, float max, uint32_t col);
/* list cursor: up/down move with wrap; returns 1 when moved */
int ui_list_nav(int *sel, int *top, int count, int rows, uint32_t pressed);
void ui_row_select(UiCanvas *c, int x, int y, int cols);
void ui_toast(Ui *ui, const char *text);
void ui_status(Ui *ui, const char *text);
void ui_close(Ui *ui);
void ui_open_book(Ui *ui, uint32_t object, uint32_t inst);
/* on-screen keyboard editing ui->name: returns 1 when confirmed */
int ui_keyboard_update(Ui *ui, uint32_t pressed);
void ui_keyboard_draw(Ui *ui, UiCanvas *c, const char *title);

Rpg *ui_rpg(Ui *ui);
VmRef ui_pc(Ui *ui);
/* "Iron Dagger (85%)" style label of a stack */
void ui_stack_label(Ui *ui, int stack, char *out, uint32_t cap);
void ui_item_details(Ui *ui, int stack, int x, int y, int cols, int rows, UiCanvas *c);

/* ui_menu.c: Stats / Inventory / Magic / Map tabs */
/* owner's stacks (filter: -1 all, else an inventory filter), equipped
 * first, then by kind and name; gold only with with_gold */
uint32_t ui_list_stacks(Ui *ui, uint32_t owner, int filter, int32_t *out, uint32_t max, int with_gold);
void ui_menu_build(Ui *ui);
void ui_menu_update(Ui *ui, const InputState *in);
void ui_menu_draw(Ui *ui, UiCanvas *c);

/* ui_trade.c: loot, barter, services, repair, alchemy, spellmaking, enchanting */
void ui_open_loot(Ui *ui, const VmRef *owner);
void ui_open_service(Ui *ui, int service);
void ui_open_alchemy(Ui *ui);
void ui_open_repair(Ui *ui, int tool_stack);
/* self: enchant with a soul gem from the inventory (no enchanter) */
void ui_open_enchant(Ui *ui, int self, int gem_stack);
/* leaves the conversation (and any service screen) */
void ui_end_dialogue(Ui *ui);
void ui_trade_update(Ui *ui, const InputState *in);
void ui_trade_draw(Ui *ui, UiCanvas *c);

/* ui_char.c: character creation, level up, rest, death */
void ui_open_chargen(Ui *ui, uint32_t steps);
void ui_open_levelup(Ui *ui);
void ui_open_rest(Ui *ui);
void ui_char_update(Ui *ui, const InputState *in);
void ui_char_draw(Ui *ui, UiCanvas *c);
void ui_draw_hud(Ui *ui, UiCanvas *c);

#endif /* PSPWIND_UI_INTERNAL_H */
