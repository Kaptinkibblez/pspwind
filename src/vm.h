/*
 * vm.h - MWScript bytecode interpreter and the scripted game state.
 *
 * The VM owns everything scripts and dialogue read or write:
 *   - globals (typed, initial values from GLOB) and game time
 *   - script instances: per-reference local variables that survive zone
 *     changes, global scripts (StartScript / start scripts), targeted ones
 *   - the per-reference stat store (attributes, skills, dynamics, AI
 *     settings, disposition, ...) seeded lazily from the NPC/creature record
 *   - inventories (owner, item) -> count, seeded from the record lists
 *   - journal (quest indices, dated entries), known topics, heard infos,
 *     the player's faction ranks / reputation / expulsion
 *   - message boxes with buttons, dialogue choices, Goodbye, ForceGreeting
 *
 * World-bound verbs (positions, distances, activation, sounds, animation)
 * go through VmHost.call; a verb the host does not handle yields its
 * documented neutral value (0, or 1 for SayDone) and is counted in
 * vm->unhandled so missing engine features are visible in the debug HUD.
 */
#ifndef PSPWIND_VM_H
#define PSPWIND_VM_H

#include "common.h"
#include "fmt.h"
#include "mwfuncs.h"
#include "rpgdb.h"

#define VM_MAX_INSTANCES 1024
#define VM_MAX_LOCALS 16384
#define VM_STACK 64
#define VM_MAX_MESSAGES 8
#define VM_MAX_BUTTONS 8
#define VM_MAX_JOURNAL 1024
#define VM_PROP_SLOTS 16384 /* power of two */
#define VM_ITEM_SLOTS 8192  /* power of two */
#define VM_SPELL_SALT 0x5bd1e995u /* spell lists live in the item store under key ^ salt */
#define VM_FULL 0xFFFFu           /* VmItem.health: pristine */
#define VM_MAX_CHOICES 24
#define VM_MAX_NOTIFY 4
#define VM_OP_BUDGET 200000 /* per script run: guards against endless while loops */
#define VM_SNIPPET_MAX 8192

typedef union {
    int32_t i;
    float f;
} VmValue;

enum { VM_REF_NONE = 0, VM_REF_PLAYER = 1, VM_REF_OBJECT = 2 };

typedef struct {
    uint8_t kind;      /* VM_REF_* */
    uint8_t pad;
    int16_t vmi;       /* script instance or -1 */
    int32_t inst;      /* host handle (zone instance) or -1 when not loaded */
    uint32_t object;   /* object id hash */
    uint32_t key;      /* persistent reference key (stat store, enable state) */
} VmRef;

enum {
    VMI_GLOBAL = 1 << 0,    /* global script (keyed by script hash) */
    VMI_RUNNING = 1 << 1,   /* global script currently started */
    VMI_LOADED = 1 << 2,    /* local script whose reference is in the loaded zone */
    VMI_TARGETED = 1 << 3,  /* global script running on target (StartScript ref) */
    VMI_INVENTORY = 1 << 4  /* script of an item the player carries (runs every frame) */
};

/* one-frame events a local script can poll (OnActivate, OnDeath, ...) */
enum {
    VMEV_ACTIVATE = 1 << 0,
    VMEV_DEATH = 1 << 1,
    VMEV_KNOCKOUT = 1 << 2,
    VMEV_MURDER = 1 << 3
};

typedef struct {
    uint32_t key;      /* reference key; script hash for global scripts */
    uint32_t object;   /* object hash (0 for global scripts) */
    uint32_t target;   /* targeted global scripts: reference object hash */
    uint16_t script;
    uint16_t flags;    /* VMI_* */
    uint16_t events;   /* VMEV_*, cleared after each run */
    uint16_t pad;
    uint32_t locals;   /* first slot in the locals pool */
    int32_t inst;      /* zone instance while VMI_LOADED */
} VmInstance;

typedef struct {
    char text[320];
    char buttons[VM_MAX_BUTTONS][48];
    uint8_t nbuttons;
} VmMessage;

typedef struct {
    uint16_t topic;    /* journal topic (quest) */
    int16_t index;
    uint32_t info;     /* DRpgInfo index */
    uint16_t day;      /* day of month */
    uint8_t month;     /* 0..11 */
    uint8_t pad;
    uint32_t days_passed;
} VmJournalEntry;

typedef struct {
    char text[96];
    int32_t value;
} VmChoice;

typedef struct {
    uint32_t key;
    uint16_t prop;
    uint16_t used;
    float value;
} VmProp;

/* Inventory stack. Stacks of one item differ by state (variant 0 is the
 * pristine stack); slots never move, so a slot index identifies a stack. */
typedef struct {
    uint32_t owner;
    uint32_t item;
    int32_t count;
    uint16_t variant;
    uint16_t health;   /* condition or uses left; VM_FULL = as cooked */
    float charge;      /* enchantment charge; < 0 = full */
    uint32_t soul;     /* soul gems: creature id hash, 0 = empty */
} VmItem;

/* notifications for the mechanics layer (rpg.c) */
enum { VMN_SPELLS = 1, VMN_ITEMS = 2, VMN_KILLED = 3 };

struct Vm;

/* context of one script run (exposed for the host's verb handlers) */
typedef struct {
    VmValue *locals;
    uint32_t nlocals;
    int32_t script;          /* DRpgScript index of the locals, -1 none */
    const char *strings;
    uint32_t strings_size;
    VmRef self;              /* implicit reference */
    int32_t vmi;             /* running instance or -1 (snippets) */
} VmCtx;

typedef struct {
    void *user;
    /* World verbs. Return 1 when handled (*ret set for functions). String
     * arguments are read with vm_arg_string(ctx, args[i]). */
    int (*call)(void *user, struct Vm *vm, uint16_t func, const VmRef *ref, const VmCtx *ctx,
                const VmValue *args, int argc, VmValue *ret);
    /* Resolves an object id to a loaded reference; 0 when not loaded. */
    int (*find_ref)(void *user, uint32_t object_hash, VmRef *out);
    /* Display name of the player's current cell ("" when unknown) and
     * whether it is an interior. */
    const char *(*cell_name)(void *user);
    int (*interior)(void *user);
    /* Enable state changed (ref key, disabled) so the host can hide/show. */
    void (*set_disabled)(void *user, const VmRef *ref, int disabled);
} VmHost;

typedef struct Vm {
    RpgDb *db;
    VmHost host;
    uint32_t player_hash;
    char pc_name[32];
    /* character creation overrides of the player record (0 / -1 = record) */
    uint32_t pc_race, pc_class;
    int pc_female;

    VmValue *globals;
    uint32_t global_count;

    VmInstance *inst;
    uint32_t inst_count;
    VmValue *locals;
    uint32_t locals_used;

    VmProp *props;
    uint32_t prop_count;
    VmItem *items;
    uint32_t item_count;

    int16_t *quest_index;    /* per topic */
    uint8_t *quest_flags;    /* per topic: VMQ_* */
    VmJournalEntry *journal;
    uint32_t journal_count;
    uint8_t *known_topics;   /* bitset over topics */
    uint8_t *heard_infos;    /* bitset over infos */
    int8_t *pc_rank;         /* per faction, -1 = not a member */
    uint8_t *pc_expelled;    /* per faction */
    int16_t *pc_facrep;      /* per faction */

    VmMessage msg[VM_MAX_MESSAGES];
    uint32_t msg_head, msg_count;
    int32_t button;          /* answer to the last message box, -1 none */

    /* dialogue hooks (valid while dialogue result scripts run) */
    VmChoice choices[VM_MAX_CHOICES];
    uint32_t choice_count;
    int goodbye;
    int has_force_greeting;
    VmRef force_greeting;

    /* time */
    int32_t g_hour, g_day, g_month, g_year, g_dayspassed, g_timescale;
    float hour, timescale;
    int32_t day, month, year, days_passed;
    float frame_dt;

    uint32_t rng;
    int menu_mode;
    int journal_updated;     /* set when an entry was added since last poll */

    char notify[VM_MAX_NOTIFY][96];
    uint32_t notify_count;

    /* mechanics hooks (rpg.c): modified stats, maxima, change notices */
    void *mech;
    float (*stat_get)(void *mech, const VmRef *ref, int prop, float raw);
    float (*stat_max)(void *mech, const VmRef *ref, int dyn_prop);
    void (*notify_change)(void *mech, int what, const VmRef *ref, uint32_t id);

    /* diagnostics */
    uint32_t errors, unhandled, runs;
    char last_error[96];
} Vm;

enum { VMQ_STARTED = 1 << 0, VMQ_FINISHED = 1 << 1 };

/* Allocates state in the SCRIPT region and starts a new game. */
int vm_init(Vm *vm, RpgDb *db, const VmHost *host);
/* Resets every scripted state: globals, journal, stores, start scripts. */
void vm_new_game(Vm *vm);

/* Advances game time and runs global scripts plus the local scripts of
 * every loaded reference once. */
void vm_tick(Vm *vm, float dt);

/* Zone binding: mark all local instances unloaded, then bind each scripted
 * reference of the new zone. Returns the instance index or -1 (no script). */
void vm_unload_zone(Vm *vm);
int vm_bind_reference(Vm *vm, uint32_t key, uint32_t object, int32_t inst);
/* Reference key for a placed reference: object hash for unique objects,
 * else a mix of the zone entry hash and the refnum. */
uint32_t vm_reference_key(const Vm *vm, uint32_t object, uint32_t zone_hash, uint32_t refnum);

/* Activation: returns 1 when the reference's script handles OnActivate
 * (the event is queued for its next run), 0 for default behaviour. */
int vm_activate(Vm *vm, const VmRef *ref);
void vm_post_event(Vm *vm, const VmRef *ref, uint16_t events);

/* Runs a dialogue result-script blob on behalf of the speaker. */
int vm_run_snippet(Vm *vm, const uint8_t *blob, uint32_t avail, const VmRef *speaker);
/* Runs one script instance now (tests / immediate execution). */
int vm_run_instance(Vm *vm, uint32_t vmi);

/* references */
VmRef vm_player_ref(const Vm *vm);
VmRef vm_resolve(Vm *vm, uint32_t object_hash);
const DRpgObject *vm_ref_object(const Vm *vm, const VmRef *ref);

/* globals */
int vm_global_index(const Vm *vm, const char *name);
float vm_global_float(const Vm *vm, int index);
void vm_set_global_float(Vm *vm, int index, float v);

/* stat store */
float vm_prop(Vm *vm, const VmRef *ref, int prop);
void vm_set_prop(Vm *vm, const VmRef *ref, int prop, float v);
float vm_prop_key(Vm *vm, uint32_t key, uint32_t object, int prop);
void vm_set_prop_key(Vm *vm, uint32_t key, int prop, float v);
int vm_disabled(Vm *vm, uint32_t key);
int vm_disposition(Vm *vm, const VmRef *ref);
/* stat as scripts and dialogue see it (modified by effects when rpg.c is attached) */
float vm_stat(Vm *vm, const VmRef *ref, int prop);

/* inventories */
int32_t vm_item_count(Vm *vm, uint32_t owner, uint32_t item);   /* all stacks */
/* Adds to the pristine stack (delta > 0) or removes, pristine stack first. */
void vm_item_add(Vm *vm, uint32_t owner, uint32_t item, int32_t delta);
/* Adds/removes units of the stack with exactly this state; returns its slot or -1. */
int vm_item_add_state(Vm *vm, uint32_t owner, uint32_t item, int32_t delta, uint16_t health,
                      float charge, uint32_t soul);
VmItem *vm_item_at(Vm *vm, int slot);
/* Makes sure the record inventory/spells of a reference are in the stores. */
void vm_seed(Vm *vm, const VmRef *ref);
/* Iterates an owner's items: returns the next slot index >= *cursor with a
 * positive count, or -1. */
int vm_item_next(Vm *vm, uint32_t owner, uint32_t *cursor, uint32_t *item, int32_t *count);

/* journal and topics */
int vm_journal_index(const Vm *vm, uint32_t quest_hash);
int vm_journal(Vm *vm, uint32_t quest_hash, int index);       /* adds an entry */
void vm_set_journal_index(Vm *vm, uint32_t quest_hash, int index);
void vm_add_topic(Vm *vm, uint32_t topic_hash);
int vm_topic_known(const Vm *vm, uint32_t topic);
void vm_mark_heard(Vm *vm, uint32_t info);
int vm_heard(const Vm *vm, uint32_t info);

/* player factions */
int vm_pc_rank(const Vm *vm, uint32_t faction_hash);         /* -1 = not a member */
void vm_pc_set_rank(Vm *vm, uint32_t faction_hash, int rank);
int vm_pc_expelled(const Vm *vm, uint32_t faction_hash);
int vm_pc_facrep(const Vm *vm, uint32_t faction_hash);
int vm_faction_reaction(Vm *vm, uint32_t f1, uint32_t f2);
uint32_t vm_ref_faction(Vm *vm, const VmRef *ref);           /* 0 = none */
int vm_ref_rank(Vm *vm, const VmRef *ref);                   /* -1 = none */

/* message boxes */
void vm_message(Vm *vm, const char *text, const char (*buttons)[48], int nbuttons);
const VmMessage *vm_message_front(const Vm *vm);
void vm_message_answer(Vm *vm, int button);
void vm_notify(Vm *vm, const char *text);

/* text defines: sigil '%' (dialogue, journal) or '^' (message boxes) */
void vm_fix_defines(Vm *vm, char sigil, const char *in, char *out, uint32_t cap,
                    const VmRef *speaker);

/* game time helpers */
void vm_date_string(const Vm *vm, uint16_t day, uint8_t month, char *out, uint32_t cap);
const char *vm_month_name(const Vm *vm, int month);

/* player race / class / sex (character creation overrides the record) */
uint32_t vm_pc_race(const Vm *vm);
uint32_t vm_pc_class(const Vm *vm);
int vm_pc_female(const Vm *vm);
/* deterministic game RNG */
uint32_t vm_random(Vm *vm);
float vm_random01(Vm *vm);
/* sets a declared local of an object's script instance (created lazily);
 * returns 0 when the object has no script or no such variable */
int vm_set_local(Vm *vm, uint32_t object, const char *name, float value);
/* advances game time by hours (rest, travel, training) */
void vm_advance_hours(Vm *vm, float hours);

/* reports a script error (counted, last message kept) */
void vm_error(Vm *vm, const char *msg);

/* argument helper for host verbs: string argument text or "" */
const char *vm_arg_string(const VmCtx *ctx, VmValue v);

#endif /* PSPWIND_VM_H */
