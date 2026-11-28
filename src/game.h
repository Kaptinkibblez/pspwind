/*
 * game.h - first-person exploration layer (portable, host-testable).
 *
 * Owns the loaded zone, its collision world and actors, the player body and
 * camera, doors and activation. Platform code feeds an InputState per frame
 * and services "zone change" requests (it shows a loading card, then calls
 * game_enter_zone) - so the whole loop runs headless in the host tests.
 *
 * PSP controls (scheme A, default):
 *   analog stick  walk / strafe            triangle / cross  look up / down
 *   square / circle  turn left / right     R  activate (doors, objects)
 *   L  jump        D-pad up  run/walk      D-pad down  sneak
 *   D-pad left  ready / sheathe weapon     D-pad right  ready / put away magic
 *   R with a weapon ready: hold to wind up, release to strike (bows shoot,
 *   a readied lockpick picks the targeted lock); with magic ready: cast
 *   SELECT  journal menu (ui.c)            START  pause menu (fly mode, options)
 * Scheme B swaps the roles: analog looks, face buttons walk.
 */
#ifndef PSPWIND_GAME_H
#define PSPWIND_GAME_H

#include "actor.h"
#include "ai.h"
#include "audio.h"
#include "common.h"
#include "dialogue.h"
#include "rpg.h"
#include "rpgdb.h"
#include "vm.h"
#include "gfx.h"
#include "phys.h"
#include "zone.h"

enum {
    GB_UP = 1 << 0, GB_DOWN = 1 << 1, GB_LEFT = 1 << 2, GB_RIGHT = 1 << 3,
    GB_TRIANGLE = 1 << 4, GB_CROSS = 1 << 5, GB_SQUARE = 1 << 6, GB_CIRCLE = 1 << 7,
    GB_L = 1 << 8, GB_R = 1 << 9, GB_START = 1 << 10, GB_SELECT = 1 << 11
};

typedef struct {
    uint32_t held;
    uint32_t pressed; /* went down this frame */
    float ax, ay;     /* analog, -1..1, +y = up/forward */
} InputState;

enum { CONTROL_SCHEME_A = 0, CONTROL_SCHEME_B = 1 };

typedef struct {
    int scheme;
    int invert_y;
    float turn_speed;  /* radians/s at full deflection */
    float pitch_speed;
} Controls;

typedef struct {
    uint32_t instance;
    float angle;  /* radians open */
    float target;
} SwingDoor;

typedef struct {
    int pending;
    uint32_t zone;
    int has_pos;
    float pos[3];
    float rot[3];
} ZoneRequest;

/* RPG layer: database, script VM and dialogue (rpg.dpak). */
typedef struct {
    RpgDb db;
    Vm vm;
    Dialogue dlg;
    Rpg rpg;
} GameRpg;

/* requests for the UI layer */
enum { GAME_UI_NONE = 0, GAME_UI_DIALOGUE, GAME_UI_BOOK, GAME_UI_LOOT, GAME_UI_CHARGEN, GAME_UI_REST,
       GAME_UI_DEATH };
/* character creation steps requested by scripts (EnableNameMenu ...) */
enum { GAME_CG_NAME = 1, GAME_CG_RACE = 2, GAME_CG_CLASS = 4, GAME_CG_BIRTH = 8, GAME_CG_REVIEW = 16,
       GAME_CG_ALL = 31 };
/* menus the player may open (scripts enable them during character creation) */
enum { GAME_MENU_STATS = 1, GAME_MENU_INVENTORY = 2, GAME_MENU_MAGIC = 4, GAME_MENU_MAP = 8,
       GAME_MENU_REST = 16, GAME_MENU_ALL = 31 };

/* an actor living outside its cooked place: a follower or summon that went
 * to another zone, or something spawned at run time (saved with the game) */
#define GAME_ROAMERS 64

/* Milestone 7: arrows, bolts, thrown weapons and spell bolts in flight */
#define GAME_SHOTS 24
#define GAME_SHOT_GRAVITY 90.0f /* arrows arc a little (spells fly straight) */
typedef struct {
    float pos[3], vel[3];
    float life;                 /* seconds before it fizzles */
    float gravity;              /* units / s^2 */
    float strength;             /* weapon shots: how far the bow was drawn */
    uint32_t weapon, ammo;      /* weapon shots: what was fired */
    uint32_t source;            /* spell bolts: spell / enchantment hash */
    const DRpgEffect *effects;  /* spell bolts: the target-range effects */
    int count;
    float chance, area;
    uint32_t shooter_key;
    int shooter;                /* actor index or AI_TARGET_PLAYER */
    int mesh;                   /* zone mesh of the ammo, -1 = glowing bolt */
    uint32_t color;             /* ABGR of a glowing bolt */
    uint8_t active, harmful, pad[2];
} GameShot;
typedef struct {
    uint32_t key;           /* reference key (stats, inventory, scripts) */
    uint32_t object;
    uint32_t zone_entry;    /* zones.dpak entry hash of the zone it is in */
    float pos[3];
    float yaw;
    float life;             /* summons: seconds left, < 0 forever */
    uint32_t summoner;
    int32_t summon_effect;
    DRpgAiPackage pkg;
    uint8_t ally, has_pkg, pad[2];
} GameRoamer;

typedef struct Game {
    World *world;
    Zone zone;
    PhysWorld phys;
    ActorSet actors;
    SwingDoor *doors;
    uint32_t door_count;
    PhysBody body;
    float yaw, pitch;
    int running, sneaking, flying;
    Controls controls;
    ZoneRequest request;
    uint32_t target;      /* instance under the crosshair or 0xFFFFFFFF */
    char message[80];
    float message_time;
    float time;
    uint32_t zone_loads;
    /* RPG layer (NULL when rpg.dpak is absent: exploration only) */
    GameRpg *rpg;
    uint32_t zone_hash;     /* pack entry hash of the loaded zone (reference keys) */
    int16_t *inst_vmi;      /* per instance: script instance or -1 */
    int menu_mode;          /* set by the UI: world paused, MenuMode == 1 */
    int controls_disabled;  /* DisablePlayerControls */
    int cell_changed;       /* CellChanged stays true for the first frames */
    int ui_event;           /* GAME_UI_* for the UI layer */
    uint32_t ui_object;     /* book object hash for GAME_UI_BOOK */
    uint32_t ui_instance;   /* its instance */
    VmRef ui_owner;         /* container / corpse for GAME_UI_LOOT */
    uint32_t ui_chargen;    /* GAME_CG_* steps requested */
    InputState frozen_input;
    /* combat and character state */
    uint32_t menus_enabled; /* GAME_MENU_* */
    float attack_charge;    /* seconds R has been held with a weapon ready */
    int attack_holding;
    float cast_cooldown;
    int player_dead;
    float fall_top;
    int was_airborne;
    uint32_t enemy_key;     /* last actor hit (HUD health bar) */
    float enemy_time;
    float *actor_timer;     /* per actor: seconds to its next attack */
    int encumbered_notice;
    /* Milestone 6: behaviour and sound */
    AiState ai;
    Audio *audio;           /* NULL without sounds.dpak / music.dpak */
    float step_dist;        /* player footsteps */
    int step_side;
    float ambient_timer;    /* region ambient sounds */
    float light_timer;      /* light loop selection */
    uint32_t light_inst[4]; /* instances with a looping sound (+1, 0 = none) */
    uint32_t light_voice[4];
    /* Milestone 7: persistence across zones */
    uint32_t *inst_key;     /* per instance: reference key override (0 = derived) */
    GameRoamer roam[GAME_ROAMERS];
    uint32_t roam_count;
    int loading;            /* load_game: the old zone is not stored */
    GameShot shots[GAME_SHOTS];
    uint32_t shots_fired, shots_hit;
} Game;

#define GAME_NO_TARGET 0xFFFFFFFFu
#define GAME_EYE_HEIGHT 116.0f
#define GAME_ACTIVATE_DIST 192.0f

void game_init(Game *g, World *w);
void game_default_controls(Controls *c);
/* Loads a zone and places the player (pos/rot NULL: zone spawn point;
 * pos is the feet position, rot the Morrowind euler rotation). */
int game_enter_zone(Game *g, uint32_t zone, const float *pos, const float *rot, char *err,
                    uint32_t errlen);
/* Opens <data_dir>/rpg.dpak and starts a new game (scripts, dialogue). */
int game_attach_rpg(Game *g, GameRpg *rpg, const char *data_dir, char *err, uint32_t errlen);
void game_update(Game *g, const InputState *in, float dt);
/* Script reference of a zone instance. */
VmRef game_instance_ref(const Game *g, uint32_t inst);
/* Picks an item instance up into the player's inventory. */
void game_take(Game *g, uint32_t inst);
/* Display name of the player's cell (interior name, exterior cell or region). */
const char *game_cell_name(const Game *g);
/* Record display name of an instance ("" when unknown). */
const char *game_instance_name(const Game *g, uint32_t inst);
void game_camera(const Game *g, Camera *cam);
const char *game_target_label(const Game *g);
void game_message(Game *g, const char *text);
void game_set_flying(Game *g, int on);
/* debug: plays the next animation group on the nearest actor */
void game_debug_cycle_animation(Game *g, int dir);
/* new game after character creation or death */
void game_respawn(Game *g);
VmRef game_actor_ref(const Game *g, uint32_t actor);
/* actor index of a reference key, or -1 */
int game_actor_index(const Game *g, uint32_t key);
/* talk to (or loot) an actor instance; guards use it to confront criminals */
void game_talk(Game *g, uint32_t inst);
/* the player takes count of item from something owned by owner: marks the
 * goods stolen and reports the theft when seen; returns 1 when witnessed */
int game_theft(Game *g, uint32_t owner, uint32_t item, int32_t count);
/* places an item (its model) in the zone; instance or -1 */
int game_spawn_item(Game *g, uint32_t object, const float pos[3], float yaw);
/* rests or waits; sleeping outdoors can be interrupted by a creature from
 * the region's sleep list (it appears, hostile). Returns hours passed;
 * msg gets the interruption notice, the refusal (enemies nearby, 0 hours)
 * or "". */
int game_rest(Game *g, int hours, int sleep, char *msg, uint32_t cap);
/* Projectiles. shooter: AI_TARGET_PLAYER or an actor index; dir need not
 * be normalised. game_fire_weapon fires the shooter's ranged weapon
 * (consuming the ammo; strength 0..1 = draw); returns the shot or -1 (no
 * ranged weapon / ammo). game_fire_spell launches target-range effects. */
int game_fire_weapon(Game *g, int shooter, const float from[3], const float dir[3], float strength);
int game_fire_spell(Game *g, int shooter, const float from[3], const float dir[3], uint32_t source,
                    const DRpgEffect *effects, int count, float chance);
/* Model matrix of an active shot (the ammo model's +Y along the flight). */
void game_shot_matrix(const Game *g, uint32_t shot, Mat4 *out);
/* plays a SOUN id without position (UI, the player's own sounds) */
void game_sound(Game *g, const char *id);
/* an actor says its Voice-dialogue line for topic (hello, idle, attack,
 * hit, flee, thief): recorded voice + subtitle; 1 when something was said */
int game_voice(Game *g, int actor, const char *topic);

#endif /* PSPWIND_GAME_H */
