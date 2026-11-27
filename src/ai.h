/*
 * ai.h - actor behaviour: AI packages, path finding, combat, detection,
 * crime and guards, spawned creatures (summons, PlaceAtPC).
 *
 * Packages come from the records (AI_W/T/E/F/A, first one active) or from
 * scripts (AIWander, AITravel, AIFollow, AIEscort, AIActivate):
 *   wander    idles with the record's idle chances; with a distance it
 *             walks to random path-grid points (or open ground) around its
 *             start, greeting the player when he comes close
 *   travel    walks to a point; escort waits for its charge; follow keeps
 *             100-300 units behind; activate walks up and activates
 * Paths: the zone's path grids (pgrd/<cell> in rpg.dpak) merged into one
 * graph, neighbouring exterior cells stitched together, A* between the
 * points nearest to start and goal. Movement uses the player's collision
 * (phys_move) with a body per actor; stuck actors skip ahead or re-plan.
 *
 * Combat follows OpenMW's rules: an actor turns hostile when
 *   fight + (50 - disposition) x fFightDispMult (NPCs)
 *         + iFightDistanceBase - fFightDistanceMultiplier x distance >= 100
 * and it can see the player (sneaking uses the observer's Sneak, distance
 * and facing against the player's Sneak, Agility, Luck and Chameleon).
 * Fighters close in and strike with their weapon or claws, cast their best
 * target spell from range, heal themselves when hurt, and flee when their
 * health runs low and their Flee rating is high. Summoned creatures fight
 * the player's enemies and vanish when the effect ends.
 *
 * Crime: assault, murder, theft (items and containers with an owner) and
 * trespass add a bounty (PCCrimeLevel) when an NPC sees it; guards then
 * pursue and force a greeting (pay the fine / go to jail / resist). Stolen
 * items are remembered so PayFineThief and jail can take them back.
 *
 * Actors farther than AI_ACTIVE_DIST from the player are frozen; decisions
 * are taken at 4 Hz, staggered across actors; movement runs every frame.
 */
#ifndef PSPWIND_AI_H
#define PSPWIND_AI_H

#include "common.h"
#include "fmt.h"
#include "phys.h"

#define AI_PATH_MAX 48
#define AI_GRAPH_MAX 4096
#define AI_STOLEN_MAX 64
#define AI_ACTIVE_DIST 4096.0f
#define AI_THINK_PERIOD 0.25f

enum { AI_IDLE = 0, AI_WALK, AI_COMBAT, AI_FLEE, AI_PURSUE, AI_GONE };
enum { AI_CRIME_ASSAULT = 0, AI_CRIME_MURDER, AI_CRIME_THEFT, AI_CRIME_TRESPASS, AI_CRIME_PICKPOCKET };
#define AI_TARGET_PLAYER (-1)
#define AI_TARGET_NONE (-2)

typedef struct {
    uint8_t state;            /* AI_* */
    uint8_t moving, running;
    uint8_t ally;             /* fights for the player (summons) */
    uint8_t pkg_done;
    uint8_t has_pkg;
    uint8_t greeted;
    uint8_t fled;
    uint8_t sees_player;      /* detection result of the last think */
    uint8_t path_len, path_pos;
    uint8_t step_side;
    DRpgAiPackage pkg;        /* current package */
    double pkg_start;         /* game hours when it started */
    float home[3];            /* wander centre */
    float dest[3];            /* walk destination */
    int16_t path[AI_PATH_MAX];
    int32_t target;           /* combat target: AI_TARGET_PLAYER, actor index or AI_TARGET_NONE */
    float timer;              /* idle / state timer */
    float think;              /* time to the next decision */
    float attack_timer;
    float spell_timer;
    float stuck_timer;
    float lost_timer;
    float greet_timer;
    float step_dist;          /* footstep accumulator */
    float life;               /* spawned: seconds left (< 0 forever) */
    uint32_t summoner;        /* reference key of the caster (summons) */
    int summon_effect;
    PhysBody body;
} AiActor;

typedef struct {
    AiActor *actors;          /* per actor in g->actors (ZONE region) */
    uint32_t capacity;
    /* merged path grid of the zone */
    float (*pts)[3];
    uint16_t *first;          /* count + 1 */
    uint16_t *edges;
    uint32_t count, edge_count;
    /* A* scratch */
    float *gscore;
    int16_t *came;
    uint16_t *heap;
    uint8_t *closed;
    /* persistent (survives zone changes, saved) */
    uint32_t stolen_item[AI_STOLEN_MAX];
    int32_t stolen_count[AI_STOLEN_MAX];
    uint32_t stolen_used;
    uint32_t spawn_serial;
    /* per frame */
    uint32_t combat;          /* actors fighting the player */
    float clock;
    /* stats */
    uint32_t paths, path_fails, spawned, crimes_seen;
} AiState;

struct Game;

/* After a zone (actors, collision) is loaded: per-actor state, path grid. */
void ai_zone_init(struct Game *g);
/* Per frame: thinks, moves, fights. */
void ai_update(struct Game *g, float dt);

/* Path between two points over the zone graph; returns the waypoint count
 * written to out (graph point indices) or -1 when there is no path. */
int ai_find_path(struct Game *g, const float from[3], const float to[3], int16_t *out, int max);
int ai_nearest_point(struct Game *g, const float p[3]);

/* Package control (scripts and tests). */
void ai_set_package(struct Game *g, int actor, const DRpgAiPackage *p);
int ai_package_done(struct Game *g, int actor);
int ai_current_package(struct Game *g, int actor);   /* DRPG_AI_* or -1 */

/* Combat. target: AI_TARGET_PLAYER or an actor index. */
void ai_start_combat(struct Game *g, int actor, int target);
void ai_stop_combat(struct Game *g, int actor);
/* The actor was hit / cast at by attacker (AI_TARGET_PLAYER or actor). */
void ai_attacked(struct Game *g, int actor, int attacker);
int ai_detects_player(struct Game *g, int actor);
int ai_has_los(struct Game *g, int actor);

/* Crime by the player; victim actor or -1; owner hash of stolen goods. */
int ai_crime(struct Game *g, int type, int victim, uint32_t owner, int value);
void ai_mark_stolen(struct Game *g, uint32_t item, int32_t count);
void ai_pay_fine(struct Game *g, int thief);
void ai_go_to_jail(struct Game *g);

/* Spawning: a creature next to pos; returns the actor index or -1. */
int ai_spawn(struct Game *g, uint32_t object, const float pos[3], float yaw, int ally, float life);
void ai_despawn(struct Game *g, int actor);

int ai_is_guard(struct Game *g, int actor);

#endif /* PSPWIND_AI_H */
