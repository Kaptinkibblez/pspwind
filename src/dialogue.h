/*
 * dialogue.h - Morrowind-style topic dialogue on top of the VM.
 *
 * Behaviour follows the original game:
 *   - a conversation opens with the first matching info of the Greeting
 *     topics (in name order, no Info Refusal fallback)
 *   - an info matches when its header filters (actor, race, class,
 *     faction/rank, gender, PC faction/rank, cell) and all SCVR conditions
 *     pass, and the speaker's disposition reaches the info's minimum;
 *     topics that only fail on disposition answer from "Info Refusal"
 *   - topic names found in responses (longest match at a word start) are
 *     learned when the speaker has something to say about them; the topic
 *     list shows known topics the speaker can answer, and the same topics
 *     are highlighted as hyperlinks inside the text
 *   - result scripts run after the response is shown; Choice turns the
 *     topic list into answers that re-run the last topic with Choice set,
 *     Goodbye leaves only the goodbye option
 *   - text defines (%PCName, %Name, %Rank, globals...) are expanded
 */
#ifndef PSPWIND_DIALOGUE_H
#define PSPWIND_DIALOGUE_H

#include "common.h"
#include "vm.h"

#define DLG_HISTORY 12288
#define DLG_TEXT_MAX 2048

enum { DLG_ENTRY_TITLE = 1, DLG_ENTRY_TEXT = 2, DLG_ENTRY_PLAYER = 3, DLG_ENTRY_NOTICE = 4 };

typedef struct {
    uint16_t start, len;
    uint16_t topic;
} DlgLink;

typedef struct {
    Vm *vm;
    RpgDb *db;
    int active;
    VmRef actor;
    int creature;
    char actor_name[48];
    int32_t last_topic;  /* topic index or -1 */
    int32_t choice;      /* -1 when not answering a choice */
    int in_choice;
    int goodbye;
    int talked_to;
    VmChoice choices[VM_MAX_CHOICES];
    uint32_t choice_count;
    /* topics shown in the list (known + answerable), alphabetical */
    uint16_t *list;
    uint8_t *list_exhausted;
    uint32_t list_count;
    /* Topic-type keywords sorted by lower-case name */
    uint16_t *keywords;
    uint32_t keyword_count;
    char history[DLG_HISTORY];
    uint32_t history_len;
    uint32_t revision;   /* bumps on any visible change */
    uint32_t infos_tested;
    char text[DLG_TEXT_MAX];
    char fixed[DLG_TEXT_MAX];
    /* recorded voice of a response (INFO sound file): played by the host */
    void (*voice)(void *user, uint32_t sound, const VmRef *speaker);
    void *voice_user;
} Dialogue;

int dlg_init(Dialogue *d, Vm *vm, RpgDb *db);
/* Opens a conversation; returns 1 when a greeting matched (window opens). */
int dlg_start(Dialogue *d, const VmRef *actor, const char *actor_name);
void dlg_ask(Dialogue *d, uint32_t topic);
void dlg_answer(Dialogue *d, uint32_t choice_index);
void dlg_end(Dialogue *d);
/* "Service Refusal" (inverted disposition test): shows the refusal and
 * returns 1 when the speaker will not offer services right now. */
int dlg_service_refused(Dialogue *d);
/* Answers from a Persuasion-type topic ("admire success", "bribe fail"...,
 * lower case): the first matching info is shown; returns 0 when none. */
int dlg_say(Dialogue *d, const char *topic);
/* A Voice-type line (Hello, Idle, Attack, Hit, Flee, Thief...) the speaker
 * would say now, outside conversations: sound file and expanded subtitle.
 * Returns 0 when none matches or a conversation is open. */
int dlg_voice_line(Dialogue *d, const VmRef *speaker, int creature, const char *topic, uint32_t *sound,
                   char *text, uint32_t cap);

/* Keyword spans of the listed topics inside text; returns the count. */
int dlg_links(const Dialogue *d, const char *text, DlgLink *out, int max);
/* All Topic keywords in text (learning uses this). */
int dlg_keywords(const Dialogue *d, const char *text, DlgLink *out, int max);
/* History iteration: returns 0 at the end. */
int dlg_history_next(const Dialogue *d, uint32_t *cursor, int *kind, const char **text);

/* Exposed for tests: index of the info the speaker would answer for a topic
 * (-1 none); *refusal set when the answer comes from Info Refusal. */
int dlg_search(Dialogue *d, uint32_t topic, int fallback, int *refusal);

#endif /* PSPWIND_DIALOGUE_H */
