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
 *   D-pad left/right  cycle the animation of the nearest actor (debug)
 *   SELECT  fly mode (debug noclip)        START  pause menu (platform)
 * Scheme B swaps the roles: analog looks, face buttons walk.
 */
#ifndef PSPWIND_GAME_H
#define PSPWIND_GAME_H

#include "actor.h"
#include "common.h"
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

typedef struct {
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
void game_update(Game *g, const InputState *in, float dt);
void game_camera(const Game *g, Camera *cam);
const char *game_target_label(const Game *g);
void game_message(Game *g, const char *text);

#endif /* PSPWIND_GAME_H */
