/*
 * game.c - first-person exploration layer (see game.h).
 */
#include "game.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mem.h"

#define WALK_SPEED 110.0f
#define RUN_SPEED 260.0f
#define SNEAK_SPEED 60.0f
#define SWIM_SPEED 140.0f
#define FLY_SPEED 900.0f
#define JUMP_SPEED 360.0f
#define BODY_RADIUS 28.0f
#define BODY_HEIGHT 128.0f
#define SNEAK_HEIGHT 96.0f
#define DOOR_SWING_SPEED 2.2f     /* radians/s */
#define DOOR_OPEN_ANGLE 1.5708f
#define ACTOR_POSE_DIST 3000.0f
#define ACTOR_FACE_DIST 300.0f
#define KILL_Z -16384.0f

void game_default_controls(Controls *c)
{
    c->scheme = CONTROL_SCHEME_A;
    c->invert_y = 0;
    c->turn_speed = 2.4f;
    c->pitch_speed = 1.6f;
}

void game_init(Game *g, World *w)
{
    memset(g, 0, sizeof(*g));
    g->world = w;
    g->running = 1;
    g->target = GAME_NO_TARGET;
    game_default_controls(&g->controls);
}

void game_message(Game *g, const char *text)
{
    snprintf(g->message, sizeof(g->message), "%s", text);
    g->message_time = 3.0f;
}

static float eye_height(const Game *g)
{
    return g->sneaking ? GAME_EYE_HEIGHT - 32.0f : GAME_EYE_HEIGHT;
}

void game_camera(const Game *g, Camera *cam)
{
    cam->pos = vec4(g->body.pos.x, g->body.pos.y, g->body.pos.z + eye_height(g), 1.0f);
    cam->yaw = g->yaw;
    cam->pitch = g->pitch;
    cam->fov_y = 60.0f * PW_DEG2RAD;
}

int game_enter_zone(Game *g, uint32_t zone, const float *pos, const float *rot, char *err,
                    uint32_t errlen)
{
    uint32_t i, n = 0;
    Vec4 feet;
    float yaw = g->yaw, ground;
    if (zone_load(g->world, &g->zone, zone, err, errlen) != 0)
        return -1;
    if (actors_init(&g->actors, &g->zone) != 0) {
        snprintf(err, errlen, "out of zone memory for actors");
        return -1;
    }
    if (phys_build(&g->phys, &g->zone) != 0) {
        snprintf(err, errlen, "out of zone memory for collision");
        return -1;
    }
    for (i = 0; i < g->zone.hdr->instance_count; ++i)
        if (g->zone.instances[i].kind == DZONE_INST_DOOR &&
            g->zone.instances[i].door == DZONE_NONE)
            n++;
    g->doors = (SwingDoor *)mem_try_alloc(MEM_REGION_ZONE, (n + 1u) * sizeof(SwingDoor), 16);
    if (!g->doors) {
        snprintf(err, errlen, "out of zone memory for doors");
        return -1;
    }
    g->door_count = 0;
    for (i = 0; i < g->zone.hdr->instance_count; ++i) {
        if (g->zone.instances[i].kind == DZONE_INST_DOOR &&
            g->zone.instances[i].door == DZONE_NONE) {
            SwingDoor *d = &g->doors[g->door_count++];
            d->instance = i;
            d->angle = d->target = 0.0f;
        }
    }

    if (pos) {
        feet = vec4(pos[0], pos[1], pos[2], 1.0f);
        if (rot)
            yaw = rot[2];
    } else {
        Vec4 eye;
        zone_spawn_point(&g->zone, &eye, &yaw);
        feet = vec4(eye.x, eye.y, eye.z - 128.0f, 1.0f);
    }
    phys_body_init(&g->body, &feet, BODY_RADIUS, BODY_HEIGHT);
    /* settle onto whatever is below (door markers sit slightly above floors) */
    if (phys_ground(&g->phys, feet.x, feet.y, feet.z + PHYS_STEP_UP + 64.0f, &ground) &&
        ground > feet.z - 256.0f) {
        g->body.pos.z = ground;
        g->body.on_ground = 1;
    }
    g->yaw = pw_wrap_angle(yaw);
    g->pitch = 0.0f;
    g->target = GAME_NO_TARGET;
    g->request.pending = 0;
    g->zone_loads++;
    return 0;
}

/* ------------------------------------------------------------------------ */
/* activation                                                               */
/* ------------------------------------------------------------------------ */
static int activatable(const DZoneInstance *in)
{
    if (in->flags & DZONE_IF_MERGED)
        return 0;
    switch (in->kind) {
    case DZONE_INST_DOOR:
    case DZONE_INST_CONTAINER:
    case DZONE_INST_ACTIVATOR:
    case DZONE_INST_ITEM:
    case DZONE_INST_NPC:
    case DZONE_INST_CREATURE:
    case DZONE_INST_LIGHT:
        return 1;
    default:
        return 0;
    }
}

static void view_ray(const Game *g, Vec4 *origin, Vec4 *dir)
{
    Camera cam;
    game_camera(g, &cam);
    *origin = cam.pos;
    gfx_camera_forward(&cam, dir);
}

static uint32_t find_target(Game *g)
{
    const Zone *z = &g->zone;
    Vec4 o, d;
    uint32_t i, best = GAME_NO_TARGET, wall_inst;
    float best_t = GAME_ACTIVATE_DIST, wall_t;
    view_ray(g, &o, &d);
    for (i = 0; i < z->hdr->instance_count; ++i) {
        const DZoneInstance *in = &z->instances[i];
        Vec4 c;
        float t, r;
        if (!activatable(in))
            continue;
        c = vec4(in->bound[0], in->bound[1], in->bound[2], 1.0f);
        r = in->bound[3] * 0.8f; /* bound spheres are generous */
        if (r < 24.0f)
            r = 24.0f;
        if (ray_sphere(&o, &d, &c, r, &t) && t < best_t) {
            best_t = t;
            best = i;
        }
    }
    if (best == GAME_NO_TARGET)
        return best;
    /* a wall between the eye and the object blocks it */
    if (phys_raycast(&g->phys, &o, &d, best_t, &wall_t, &wall_inst) && wall_inst != best &&
        wall_t + 8.0f < best_t)
        return GAME_NO_TARGET;
    return best;
}

const char *game_target_label(const Game *g)
{
    static const char *names[] = { "Static", "Activate", "Door", "Open", "Light", "Take",
                                   "Talk", "Creature", "Creature", "Use" };
    const DZoneInstance *in;
    if (g->target == GAME_NO_TARGET || !g->zone.hdr)
        return NULL;
    in = &g->zone.instances[g->target];
    if (in->kind == DZONE_INST_DOOR && in->door != DZONE_NONE) {
        const DZoneDoor *d = &g->zone.doors[in->door];
        if (d->dest_zone != DZONE_NO_ZONE)
            return world_zone_name(g->world, d->dest_zone);
    }
    return in->kind < PW_COUNTOF(names) ? names[in->kind] : "Use";
}

static void activate(Game *g, uint32_t inst)
{
    const DZoneInstance *in = &g->zone.instances[inst];
    uint32_t i;
    if (in->kind == DZONE_INST_DOOR) {
        if (in->door != DZONE_NONE) {
            const DZoneDoor *d = &g->zone.doors[in->door];
            if (d->lock_level > 0) {
                game_message(g, "Locked.");
                return;
            }
            if (d->dest_zone == DZONE_NO_ZONE) {
                game_message(g, "This door leads nowhere (destination not cooked).");
                return;
            }
            g->request.pending = 1;
            g->request.zone = d->dest_zone;
            g->request.has_pos = 1;
            memcpy(g->request.pos, d->dest_pos, sizeof(g->request.pos));
            memcpy(g->request.rot, d->dest_rot, sizeof(g->request.rot));
            return;
        }
        for (i = 0; i < g->door_count; ++i) {
            if (g->doors[i].instance == inst) {
                g->doors[i].target = g->doors[i].target > 0.0f ? 0.0f : DOOR_OPEN_ANGLE;
                return;
            }
        }
        return;
    }
    switch (in->kind) {
    case DZONE_INST_NPC:
        game_message(g, "Dialogue arrives in Milestone 4.");
        break;
    case DZONE_INST_CONTAINER:
        game_message(g, "Containers open in Milestone 5.");
        break;
    case DZONE_INST_ITEM:
    case DZONE_INST_LIGHT:
        game_message(g, "Inventory arrives in Milestone 5.");
        break;
    default:
        game_message(g, "Nothing happens.");
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* world simulation                                                         */
/* ------------------------------------------------------------------------ */
static void update_doors(Game *g, float dt)
{
    uint32_t i;
    for (i = 0; i < g->door_count; ++i) {
        SwingDoor *d = &g->doors[i];
        const DZoneInstance *in = &g->zone.instances[d->instance];
        float step = DOOR_SWING_SPEED * dt;
        if (d->angle == d->target)
            continue;
        if (fabsf(d->target - d->angle) <= step)
            d->angle = d->target;
        else
            d->angle += d->target > d->angle ? step : -step;
        zone_placement_matrix(&g->zone.inst_world[d->instance], in->pos, in->rot, in->scale);
        mat4_rotate_z(&g->zone.inst_world[d->instance], -d->angle);
    }
}

static void update_actors(Game *g, float dt)
{
    uint32_t i;
    g->phys.obstacle_count = 0;
    for (i = 0; i < g->actors.count; ++i) {
        Actor *a = &g->actors.list[i];
        float dx = g->body.pos.x - a->pos.x, dy = g->body.pos.y - a->pos.y;
        float d2 = dx * dx + dy * dy;
        if (a->rec->flags & DZONE_ACTOR_F_CREATURE) {
            /* creatures idle in place until AI (Milestone 6) */
        } else if (d2 < ACTOR_FACE_DIST * ACTOR_FACE_DIST) {
            /* NPCs turn to face the player (Morrowind z rotation is clockwise) */
            float want = atan2f(dx, dy), diff = pw_wrap_angle(want - a->yaw);
            float step = 2.5f * dt;
            if (diff > step)
                diff = step;
            else if (diff < -step)
                diff = -step;
            if (fabsf(diff) > 1e-4f) {
                Vec4 p = a->pos;
                actor_set_placement(a, &p, pw_wrap_angle(a->yaw + diff));
            }
        }
        if (g->phys.obstacle_count < PHYS_MAX_OBSTACLES && d2 < 1024.0f * 1024.0f) {
            PhysObstacle *o = &g->phys.obstacles[g->phys.obstacle_count++];
            o->x = a->pos.x;
            o->y = a->pos.y;
            o->z = a->pos.z;
            o->radius = BODY_RADIUS * a->scale;
            o->height = BODY_HEIGHT * a->scale;
            o->id = i;
        }
    }
    {
        Vec4 eye = vec4(g->body.pos.x, g->body.pos.y, g->body.pos.z + eye_height(g), 1.0f);
        actors_update(&g->actors, dt, &eye, ACTOR_POSE_DIST);
    }
}

static void cycle_nearest_animation(Game *g, int dir)
{
    uint32_t i, best = 0xFFFFFFFFu;
    float best_d = 1e30f;
    for (i = 0; i < g->actors.count; ++i) {
        const Actor *a = &g->actors.list[i];
        float dx = g->body.pos.x - a->pos.x, dy = g->body.pos.y - a->pos.y;
        if (a->anim && dx * dx + dy * dy < best_d) {
            best_d = dx * dx + dy * dy;
            best = i;
        }
    }
    if (best != 0xFFFFFFFFu) {
        Actor *a = &g->actors.list[best];
        const Anim *an = a->anim;
        uint32_t n = an->hdr->group_count, cur = 0, k;
        char buf[80];
        const char *name;
        for (k = 0; k < n; ++k)
            if (an->groups[k].name_hash == a->group)
                cur = k;
        cur = (cur + n + (uint32_t)(dir > 0 ? 1 : n - 1)) % n;
        actor_play(a, an->groups[cur].name_hash, 1);
        name = (const char *)an->base + an->hdr->strings_ofs + an->groups[cur].name_ofs;
        snprintf(buf, sizeof(buf), "Actor %u plays '%.40s'", (unsigned)best, name);
        game_message(g, buf);
    } else {
        game_message(g, "No animated actor in this zone.");
    }
}

static void check_zone_border(Game *g)
{
    const DZoneHeader *h = g->zone.hdr;
    int cx = (int)floorf(g->body.pos.x / 8192.0f), cy = (int)floorf(g->body.pos.y / 8192.0f);
    int zi;
    if (h->kind != DZONE_KIND_EXTERIOR || g->request.pending)
        return;
    if (cx >= h->cell_x0 && cx <= h->cell_x1 && cy >= h->cell_y0 && cy <= h->cell_y1)
        return;
    zi = world_find_exterior(g->world, cx, cy);
    if (zi < 0 || (uint32_t)zi == g->zone.index)
        return;
    g->request.pending = 1;
    g->request.zone = (uint32_t)zi;
    g->request.has_pos = 1;
    g->request.pos[0] = g->body.pos.x;
    g->request.pos[1] = g->body.pos.y;
    g->request.pos[2] = g->body.pos.z;
    g->request.rot[0] = g->request.rot[1] = 0.0f;
    g->request.rot[2] = g->yaw;
}

void game_update(Game *g, const InputState *in, float dt)
{
    const Controls *c = &g->controls;
    float move_x = 0.0f, move_y = 0.0f, look_x = 0.0f, look_y = 0.0f, speed;
    float sy, cy;
    Vec4 wish;
    uint32_t pressed = in->pressed, held = in->held;

    if (!g->zone.hdr || g->request.pending)
        return;
    g->time += dt;
    if (g->message_time > 0.0f)
        g->message_time -= dt;

    /* ---- input mapping */
    if (c->scheme == CONTROL_SCHEME_A) {
        move_x = in->ax;
        move_y = in->ay;
        look_x = (float)((held & GB_CIRCLE) != 0) - (float)((held & GB_SQUARE) != 0);
        look_y = (float)((held & GB_TRIANGLE) != 0) - (float)((held & GB_CROSS) != 0);
    } else {
        look_x = in->ax;
        look_y = in->ay;
        move_x = (float)((held & GB_CIRCLE) != 0) - (float)((held & GB_SQUARE) != 0);
        move_y = (float)((held & GB_TRIANGLE) != 0) - (float)((held & GB_CROSS) != 0);
    }
    if (c->invert_y)
        look_y = -look_y;
    g->yaw = pw_wrap_angle(g->yaw + look_x * c->turn_speed * dt);
    g->pitch += look_y * c->pitch_speed * dt;
    g->pitch = g->pitch > 1.45f ? 1.45f : (g->pitch < -1.45f ? -1.45f : g->pitch);

    if (pressed & GB_UP) {
        g->running = !g->running;
        game_message(g, g->running ? "Running" : "Walking");
    }
    if (pressed & GB_DOWN) {
        g->sneaking = !g->sneaking;
        g->body.height = g->sneaking ? SNEAK_HEIGHT : BODY_HEIGHT;
        game_message(g, g->sneaking ? "Sneaking" : "Standing");
    }
    if (pressed & GB_SELECT) {
        g->flying = !g->flying;
        g->body.noclip = (uint8_t)g->flying;
        g->body.vel.z = 0.0f;
        game_message(g, g->flying ? "Fly mode (debug)" : "Walking mode");
    }
    if (pressed & GB_LEFT)
        cycle_nearest_animation(g, -1);
    if (pressed & GB_RIGHT)
        cycle_nearest_animation(g, 1);

    /* ---- movement */
    speed = g->flying ? FLY_SPEED : (g->body.swimming ? SWIM_SPEED
                                     : (g->sneaking ? SNEAK_SPEED
                                        : (g->running ? RUN_SPEED : WALK_SPEED)));
    pw_sincosf(g->yaw, &sy, &cy);
    /* forward = (sin yaw, cos yaw); right = (cos yaw, -sin yaw) */
    wish = vec4((sy * move_y + cy * move_x) * speed, (cy * move_y - sy * move_x) * speed, 0.0f, 0.0f);
    if (g->flying || g->body.swimming) {
        float sp, cp;
        pw_sincosf(g->pitch, &sp, &cp);
        wish.x *= cp;
        wish.y *= cp;
        wish.z = sp * move_y * speed;
        if (g->flying) {
            if (held & GB_R)
                wish.z += speed;
            if (held & GB_L)
                wish.z -= speed;
        }
    }
    update_doors(g, dt);
    update_actors(g, dt);
    phys_move(&g->phys, &g->body, &wish, !g->flying && (pressed & GB_L) != 0, JUMP_SPEED, dt);
    if (g->body.pos.z < KILL_Z) {
        char err[8];
        game_message(g, "You fell out of the world.");
        game_enter_zone(g, g->zone.index, NULL, NULL, err, sizeof(err));
        return;
    }

    /* ---- activation */
    g->target = find_target(g);
    if ((pressed & GB_R) && !g->flying) {
        if (g->target != GAME_NO_TARGET)
            activate(g, g->target);
    }
    check_zone_border(g);
}
