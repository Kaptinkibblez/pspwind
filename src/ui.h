/*
 * ui.h - PSP console UI: 8x8 bitmap font, dialogue, journal, books,
 * message boxes and notifications.
 *
 * The UI is portable: each frame it emits a list of rectangles and text
 * runs (UiCanvas) that the PSP backend (ui_gu.c) turns into GE sprites and
 * the host backend rasterises into an RGBA image (tests, screenshots).
 * Glyphs are the 8x8 MSX font on a 6 pixel advance: 80 x 30 characters.
 *
 * Controls while a window is open:
 *   Dialogue  up/down choose, left/right switch topic list / text links,
 *             cross ask / follow link / answer, circle goodbye, L/R scroll
 *   Menu      SELECT opens; L/R switch tabs (Journal, Quests, Topics),
 *             up/down move, cross open, circle back / close
 *   Book      up/down or L/R scroll, square take, cross/circle close
 *   Message   left/right choose a button, cross confirm
 */
#ifndef PSPWIND_UI_H
#define PSPWIND_UI_H

#include "common.h"
#include "game.h"

#define UI_SCREEN_W 480
#define UI_SCREEN_H 272
#define UI_CHAR_W 6
#define UI_CHAR_H 8
#define UI_LINE_H 9

enum { UI_CMD_RECT = 0, UI_CMD_TEXT = 1 };

typedef struct {
    uint8_t type;
    uint8_t pad;
    int16_t x, y, w, h;   /* text: w = chars */
    uint16_t ofs;         /* text pool offset */
    uint32_t color;       /* ABGR */
} UiCmd;

#define UI_MAX_CMDS 1024
#define UI_TEXT_POOL 24576

typedef struct {
    UiCmd cmds[UI_MAX_CMDS];
    uint32_t count;
    char pool[UI_TEXT_POOL];
    uint32_t pool_used;
    uint32_t dropped;
} UiCanvas;

void ui_canvas_clear(UiCanvas *c);
void ui_rect(UiCanvas *c, int x, int y, int w, int h, uint32_t abgr);
void ui_text(UiCanvas *c, int x, int y, uint32_t abgr, const char *s, int len);
/* Host/test backend: draws into an RGBA8888 (ABGR in memory) 480x272 image. */
void ui_render_rgba(const UiCanvas *c, uint32_t *pixels, int stride);
/* PSP backend (ui_gu.c): call between sceGuStart and sceGuFinish. */
void ui_render_gu(const UiCanvas *c);

/* ------------------------------------------------------------------ state */
enum { UI_NONE = 0, UI_DIALOGUE, UI_MENU, UI_BOOK };
enum { UI_TAB_JOURNAL = 0, UI_TAB_QUESTS, UI_TAB_TOPICS, UI_TAB_COUNT };

#define UI_DOC_LINES 1024
#define UI_DOC_SEGS 512
#define UI_DOC_LINKS 256
#define UI_DOC_TEXT 32768
#define UI_TOASTS 4

typedef struct {
    uint32_t ofs;      /* into the document text */
    uint16_t len;
    uint8_t kind;      /* colour class */
    uint8_t pad;
} UiLine;

typedef struct {
    uint16_t line, col, len, link;
} UiSeg;

typedef struct {
    uint16_t topic;
    uint16_t line;
} UiLink;

/* a laid-out, scrollable text with optional topic hyperlinks */
typedef struct {
    char text[UI_DOC_TEXT];
    uint32_t text_len;
    UiLine lines[UI_DOC_LINES];
    uint32_t line_count;
    UiSeg segs[UI_DOC_SEGS];
    uint32_t seg_count;
    UiLink links[UI_DOC_LINKS];
    uint32_t link_count;
    int width;         /* characters per line */
} UiDoc;

typedef struct {
    Game *g;
    int mode;
    /* dialogue */
    int focus;         /* 0 topic list, 1 links in the text */
    int sel_topic, topic_top;
    int sel_link;
    int sel_choice;
    int scroll;        /* first visible line */
    int follow;        /* keep the newest text in view */
    uint32_t seen_revision;
    /* menu */
    int tab;
    int sel, list_top;
    int detail;        /* -1 list, else the opened quest/topic index */
    uint16_t items[512];
    uint32_t item_count;
    /* book */
    uint32_t book_object, book_instance;
    char book_title[64];
    /* message box */
    int msg_sel;
    /* notifications */
    char toast[UI_TOASTS][96];
    float toast_time[UI_TOASTS];
    UiDoc doc;
} Ui;

void ui_init(Ui *ui, Game *g);
/* 1 while a window or modal message captures the input */
int ui_active(const Ui *ui);
void ui_update(Ui *ui, const InputState *in, float dt);
void ui_draw(Ui *ui, UiCanvas *c);

/* exposed for tests */
void ui_doc_reset(UiDoc *d, int width);
void ui_doc_add(UiDoc *d, int kind, const char *text, const DlgLink *links, int nlinks);

#endif /* PSPWIND_UI_H */
