/*
 * rpg.h - character mechanics: stats, progression, magic effects,
 * equipment, combat, casting, items, trade and services, crafting, rest,
 * character creation (Morrowind rules, GMST driven).
 *
 * All state lives in the VM stores (stats per reference key, inventories
 * as item stacks with condition/charge/soul) plus the tables here: active
 * magic effects, equipment slots and player-made objects (potions, spells,
 * enchanted items), which overlay the cooked database. rpg.c installs
 * itself as the VM's mechanics hook, so scripts and dialogue see modified
 * attributes, derived disposition and effect-raised maxima.
 *
 * GMSTs come from the load order; when a GMST is missing the engine's
 * default is used (the values Morrowind ships with).
 */
#ifndef PSPWIND_RPG_H
#define PSPWIND_RPG_H

#include "common.h"
#include "mwfuncs.h"
#include "vm.h"

#define RPG_MAX_FX 512
#define RPG_MAX_EQUIP 768
#define RPG_MAX_CUSTOM 96
#define RPG_CUSTOM_EFFECTS 8
#define RPG_MAX_VISITED 1024
#define RPG_GOLD "gold_001"

enum {
    RPG_SLOT_HELMET = 0, RPG_SLOT_CUIRASS, RPG_SLOT_GREAVES, RPG_SLOT_LPAULDRON, RPG_SLOT_RPAULDRON,
    RPG_SLOT_LGAUNTLET, RPG_SLOT_RGAUNTLET, RPG_SLOT_BOOTS, RPG_SLOT_SHIRT, RPG_SLOT_PANTS,
    RPG_SLOT_SKIRT, RPG_SLOT_ROBE, RPG_SLOT_LRING, RPG_SLOT_RRING, RPG_SLOT_AMULET, RPG_SLOT_BELT,
    RPG_SLOT_WEAPON, RPG_SLOT_SHIELD, RPG_SLOT_AMMO, RPG_SLOT_COUNT
};

/* spell ranges (DRpgEffect.range) */
enum { RPG_RANGE_SELF = 0, RPG_RANGE_TOUCH = 1, RPG_RANGE_TARGET = 2 };

/* active effect flags */
enum {
    RFX_CONSTANT = 1 << 0,  /* ability / disease / constant-effect item: no duration */
    RFX_HARMFUL = 1 << 1,
    RFX_STARTED = 1 << 2    /* one-shot part already applied */
};

typedef struct {
    uint32_t key, object;   /* target reference */
    uint32_t source;        /* spell / enchantment / potion / ingredient id hash */
    uint32_t caster;        /* caster reference key */
    uint8_t effect;
    int8_t arg;             /* skill or attribute */
    uint8_t flags;          /* RFX_* */
    uint8_t pad;
    float magnitude;
    float remaining;        /* seconds; ignored for RFX_CONSTANT */
    float duration;
    float applied;          /* amount applied for effects undone on expiry */
} RpgFx;

typedef struct {
    uint32_t key;
    uint8_t slot;           /* RPG_SLOT_* */
    uint8_t pad[3];
    int32_t stack;          /* VM item slot */
} RpgEquip;

/* player-made objects (potions, spells, enchanted items) */
typedef struct {
    DRpgObject obj;         /* obj.name = RPG_CUSTOM_NAME | index */
    DRpgSpell spell;        /* spells and enchantments */
    DRpgEffect effects[RPG_CUSTOM_EFFECTS];
    char name[32];
    uint8_t kind;           /* 0 object, 1 spell, 2 enchantment */
    uint8_t used;
} RpgCustom;
#define RPG_CUSTOM_NAME 0x80000000u

typedef struct {
    /* teleports the player (cell hash of an interior, or 0 = exterior) */
    void (*teleport)(void *user, uint32_t cell, const float pos[3], float rot_z);
    /* where the player is now (for Mark) */
    void (*locate)(void *user, uint32_t *cell, float pos[3], float *rot_z);
    /* an actor died (death animation, collision) */
    void (*killed)(void *user, const VmRef *ref);
    /* hostile living actors within radius of the player (rest check) */
    int (*hostiles_near)(void *user, float radius);
    /* summon effects: places the creature next to the caster for duration
     * seconds (it fights for the caster); returns 1 when it appeared */
    int (*summon)(void *user, const VmRef *caster, uint32_t creature, int effect, float duration);
    void *user;
} RpgHost;

typedef struct {
    int success;            /* the roll succeeded */
    int count;              /* effects */
    const DRpgEffect *effects;
    uint32_t source;        /* spell / enchantment hash */
    int school;             /* skill of the cast (-1 items) */
    float chance;           /* uncapped success chance (resistance rolls) */
    int has[3];             /* any effect with that range */
    float area;             /* largest area of target effects (feet) */
    char message[64];       /* failure reason when !success */
} RpgCast;

typedef struct {
    int hit, critical, killed, blocked;
    float damage;
} RpgHit;

typedef struct Rpg {
    Vm *vm;
    RpgDb *db;
    RpgHost host;
    RpgFx fx[RPG_MAX_FX];
    uint32_t fx_count;
    RpgEquip eq[RPG_MAX_EQUIP];
    uint32_t eq_count;
    RpgCustom custom[RPG_MAX_CUSTOM];
    uint32_t custom_count;
    /* player character */
    uint32_t race, cls, sign;
    int female;
    int chargen_done;
    uint32_t sel_spell;     /* selected spell/power, or 0 */
    int32_t sel_item;       /* selected enchanted item stack, or -1 */
    int has_mark;
    uint32_t mark_cell;
    float mark_pos[3], mark_rot;
    int level_pending;      /* "rest and meditate" reached */
    uint32_t visited[RPG_MAX_VISITED]; /* packed exterior cells (x, y) seen */
    uint32_t visited_count;
    int dirty_constants;    /* player abilities / constant items changed */
    int draw_state;         /* 0 nothing, 1 weapon, 2 magic readied */
    int moving, running, swimming, sneaking; /* player movement this frame (game.c) */
    uint32_t persuade_key;  /* NPC being persuaded this conversation */
    int persuade_perm;      /* permanent part, applied at goodbye */
} Rpg;

int rpg_init(Rpg *r, Vm *vm, RpgDb *db, const RpgHost *host);
void rpg_new_game(Rpg *r);
void rpg_tick(Rpg *r, float dt);

/* ---- lookups (cooked database + player-made objects) */
const DRpgObject *rpg_object(const Rpg *r, uint32_t hash);
const DRpgSpell *rpg_spell(const Rpg *r, uint32_t hash);
const char *rpg_name(const Rpg *r, const DRpgObject *o);
const char *rpg_spell_name(const Rpg *r, uint32_t hash);
const DRpgEffect *rpg_object_effects(const Rpg *r, const DRpgObject *o, int *count);
const DRpgEffect *rpg_spell_effects(const Rpg *r, const DRpgSpell *s, int *count);
const char *rpg_skill_name(const Rpg *r, int skill);
const char *rpg_attr_name(const Rpg *r, int attr);
const char *rpg_effect_name(const Rpg *r, int effect);
/* "Fortify Strength 10 pts for 30 secs on Self" */
void rpg_describe_effect(const Rpg *r, const DRpgEffect *e, char *out, uint32_t cap);
float rpg_gmst_f(const Rpg *r, const char *id, float fallback);
int rpg_gmst_i(const Rpg *r, const char *id, int fallback);

/* ---- stats (modified values include active effects) */
float rpg_attr(Rpg *r, const VmRef *ref, int attr);
float rpg_skill(Rpg *r, const VmRef *ref, int skill);
float rpg_dyn(Rpg *r, const VmRef *ref, int dyn);       /* 0 health, 1 magicka, 2 fatigue */
float rpg_dyn_max(Rpg *r, const VmRef *ref, int dyn);
void rpg_dyn_add(Rpg *r, const VmRef *ref, int dyn, float delta);
float rpg_fatigue_term(Rpg *r, const VmRef *ref);
float rpg_effect(Rpg *r, const VmRef *ref, int effect, int arg); /* summed magnitude */
float rpg_encumbrance(Rpg *r, const VmRef *ref);
float rpg_capacity(Rpg *r, const VmRef *ref);
float rpg_armor_rating(Rpg *r, const VmRef *ref);
int rpg_disposition(Rpg *r, const VmRef *npc);
int rpg_level(Rpg *r, const VmRef *ref);
int rpg_is_dead(Rpg *r, const VmRef *ref);
float rpg_walk_speed(Rpg *r, const VmRef *ref);
float rpg_run_speed(Rpg *r, const VmRef *ref);
float rpg_jump_speed(Rpg *r, const VmRef *ref);
int rpg_gold(Rpg *r, const VmRef *ref);

/* ---- progression */
void rpg_skill_use(Rpg *r, int skill, int use_type, float factor);  /* player */
void rpg_skill_raise(Rpg *r, int skill, int by_trainer);           /* player */
int rpg_skill_type(const Rpg *r, int skill);   /* 0 major, 1 minor, 2 misc */
float rpg_skill_progress(Rpg *r, int skill);   /* 0..1 */
int rpg_level_multiplier(Rpg *r, int attr);
/* raises three attributes (by their multipliers) and the level */
int rpg_level_up(Rpg *r, const int attrs[3]);
void rpg_recalc_player(Rpg *r);                /* derived maxima after attribute changes */

/* ---- magic effects */
/* Applies effects with the given range to target (resistance, reflection,
 * absorption, instant effects); returns the number of effects that landed. */
int rpg_apply(Rpg *r, const VmRef *caster, const VmRef *target, const DRpgEffect *effects, int count,
              int range, uint32_t source, float cast_chance, int harmful_allowed);
void rpg_dispel(Rpg *r, uint32_t key, float magnitude);
void rpg_remove_source(Rpg *r, uint32_t key, uint32_t source);
void rpg_refresh_constants(Rpg *r, const VmRef *ref);
int rpg_kill(Rpg *r, const VmRef *ref, const VmRef *killer);
void rpg_damage(Rpg *r, const VmRef *ref, int dyn, float amount, const VmRef *source);

/* ---- equipment and items */
int rpg_slot_for(Rpg *r, const VmRef *ref, const DRpgObject *o, int *second);
int rpg_equip(Rpg *r, const VmRef *ref, int stack);   /* 1 ok, 0 not equippable, -1 refused */
void rpg_unequip_slot(Rpg *r, const VmRef *ref, int slot);
int rpg_equipped(Rpg *r, const VmRef *ref, int slot); /* stack or -1 */
int rpg_is_equipped(Rpg *r, const VmRef *ref, int stack);
void rpg_autoequip(Rpg *r, const VmRef *ref);
int rpg_item_health_max(const Rpg *r, const DRpgObject *o);  /* 0 = no condition */
float rpg_item_charge_max(Rpg *r, const DRpgObject *o);
int rpg_item_value(Rpg *r, const VmItem *it);
int rpg_find_stack(Rpg *r, uint32_t owner, uint32_t item);   /* first stack with count */
int rpg_move_stack(Rpg *r, int stack, uint32_t to_owner, int32_t count);  /* returns new stack */

enum { RPG_USE_NONE = 0, RPG_USE_EQUIP, RPG_USE_UNEQUIP, RPG_USE_DRINK, RPG_USE_EAT, RPG_USE_CAST,
       RPG_USE_BOOK, RPG_USE_ALCHEMY, RPG_USE_REPAIR, RPG_USE_FAILED };
int rpg_use(Rpg *r, int stack, char *msg, uint32_t msgcap);   /* player */

/* ---- casting and combat */
int rpg_select_spell(Rpg *r, uint32_t spell);
int rpg_select_item(Rpg *r, int stack);
float rpg_spell_chance(Rpg *r, const VmRef *caster, const DRpgSpell *s);
int rpg_spell_cost(Rpg *r, const DRpgSpell *s);
/* rolls and pays for the player's selected spell / item (cast-on-use) */
int rpg_begin_cast(Rpg *r, const VmRef *caster, RpgCast *out);
float rpg_weapon_reach(Rpg *r, const VmRef *ref);
int rpg_weapon_ranged(Rpg *r, const VmRef *ref);
int rpg_weapon_skill(Rpg *r, const VmRef *ref);
/* melee or ranged hit resolution; strength 0..1 */
RpgHit rpg_attack(Rpg *r, const VmRef *attacker, const VmRef *victim, float strength);
/* consumes ammunition / a thrown weapon; 0 when nothing to shoot */
int rpg_consume_ammo(Rpg *r, const VmRef *ref);
int rpg_pick_lock(Rpg *r, const VmRef *target, int lock_level, int *new_level, char *msg, uint32_t cap);

/* ---- trade and services */
enum {
    RPG_SVC_WEAPON = 0x1, RPG_SVC_ARMOR = 0x2, RPG_SVC_CLOTHING = 0x4, RPG_SVC_BOOKS = 0x8,
    RPG_SVC_INGREDIENTS = 0x10, RPG_SVC_PICKS = 0x20, RPG_SVC_PROBES = 0x40, RPG_SVC_LIGHTS = 0x80,
    RPG_SVC_APPARATUS = 0x100, RPG_SVC_REPAIRITEMS = 0x200, RPG_SVC_MISC = 0x400,
    RPG_SVC_SPELLS = 0x800, RPG_SVC_MAGICITEMS = 0x1000, RPG_SVC_POTIONS = 0x2000,
    RPG_SVC_TRAINING = 0x4000, RPG_SVC_SPELLMAKING = 0x8000, RPG_SVC_ENCHANTING = 0x10000,
    RPG_SVC_REPAIR = 0x20000
};
uint32_t rpg_services(Rpg *r, const VmRef *npc);
int rpg_merchant_buys(Rpg *r, const VmRef *npc, const DRpgObject *o);
int rpg_price(Rpg *r, const VmRef *npc, int base, int buying);
int rpg_merchant_gold(Rpg *r, const VmRef *npc);
int rpg_buy(Rpg *r, const VmRef *npc, int stack, int count, char *msg, uint32_t cap);
int rpg_sell(Rpg *r, const VmRef *npc, int stack, int count, char *msg, uint32_t cap);
int rpg_trainer_skills(Rpg *r, const VmRef *npc, int out[3]);
int rpg_training_price(Rpg *r, const VmRef *npc, int skill);
int rpg_train(Rpg *r, const VmRef *npc, int skill, char *msg, uint32_t cap);
int rpg_spells_for_sale(Rpg *r, const VmRef *npc, uint32_t *out, int max);
int rpg_spell_price(Rpg *r, const VmRef *npc, uint32_t spell);
int rpg_buy_spell(Rpg *r, const VmRef *npc, uint32_t spell, char *msg, uint32_t cap);
int rpg_repair_price(Rpg *r, const VmRef *npc, int stack);
int rpg_repair_service(Rpg *r, const VmRef *npc, int stack, char *msg, uint32_t cap);
int rpg_repair_self(Rpg *r, int tool_stack, int stack, char *msg, uint32_t cap);
int rpg_travel_count(Rpg *r, const VmRef *npc);
int rpg_travel_price(Rpg *r, const VmRef *npc, int index, int npc_exterior, const float npc_pos[3]);
int rpg_travel(Rpg *r, const VmRef *npc, int index, int price, char *msg, uint32_t cap);
enum { RPG_PERSUADE_ADMIRE = 0, RPG_PERSUADE_INTIMIDATE, RPG_PERSUADE_TAUNT, RPG_PERSUADE_BRIBE10,
       RPG_PERSUADE_BRIBE100, RPG_PERSUADE_BRIBE1000 };
int rpg_persuade(Rpg *r, const VmRef *npc, int type, int *success);  /* returns disposition change */
void rpg_end_conversation(Rpg *r, const VmRef *npc);

/* ---- crafting */
typedef struct {
    int stacks[4];          /* ingredient stacks, -1 empty */
    int tools[4];           /* mortar, alembic, calcinator, retort stacks, -1 none */
    char name[32];
} RpgAlchemy;
int rpg_alchemy_preview(Rpg *r, const RpgAlchemy *a, DRpgEffect *out, int max, int *value);
int rpg_alchemy_brew(Rpg *r, const RpgAlchemy *a, char *msg, uint32_t cap);
int rpg_ingredient_known_effects(Rpg *r);     /* how many effects the player can see */
int rpg_spellmaking_price(Rpg *r, const VmRef *npc, const DRpgEffect *e, int n, int *cost);
int rpg_make_spell(Rpg *r, const VmRef *npc, const DRpgEffect *e, int n, const char *name, char *msg,
                   uint32_t cap);
int rpg_known_effects(Rpg *r, uint8_t *mask /* RPGDB_EFFECTS bytes */);
typedef struct {
    int item_stack, gem_stack;
    int type;               /* 0 cast once, 1 on strike, 2 on use, 3 constant */
    DRpgEffect effects[RPG_CUSTOM_EFFECTS];
    int count;
    char name[32];
} RpgEnchant;
int rpg_enchant_points(Rpg *r, const RpgEnchant *e, int *capacity, int *charge, float *chance);
int rpg_enchant(Rpg *r, const VmRef *npc, const RpgEnchant *e, char *msg, uint32_t cap);

/* ---- rest, chargen, map */
int rpg_can_rest(Rpg *r, char *msg, uint32_t cap);
/* rests or waits up to hours; returns hours passed */
int rpg_rest(Rpg *r, int hours, int sleep);
void rpg_chargen(Rpg *r, const char *name, uint32_t race, int female, uint32_t cls, uint32_t sign);
void rpg_visit_cell(Rpg *r, int x, int y);
void rpg_player_jumped(Rpg *r);
void rpg_player_landed(Rpg *r, float fall_height);
int rpg_read_book(Rpg *r, uint32_t book);      /* skill raised by a skill book, or -1 */
int rpg_has_disease(Rpg *r, const VmRef *ref);
int rpg_is_soulgem(const Rpg *r, const DRpgObject *o);
int rpg_class_spec(const Rpg *r, uint32_t cls);
int rpg_class_attr(const Rpg *r, uint32_t cls, int i);
int rpg_class_skill_type(const Rpg *r, uint32_t cls, int skill);
int rpg_visited(const Rpg *r, int x, int y);

#endif /* PSPWIND_RPG_H */
