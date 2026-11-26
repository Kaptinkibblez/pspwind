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

static void actor_combat(Game *g, uint32_t i, float dt);
static int door_lock(Game *g, uint32_t inst);
static void rpg_teleport(void *user, uint32_t cell, const float pos[3], float rot_z);
static void rpg_locate(void *user, uint32_t *cell, float pos[3], float *rot_z);
static void rpg_killed(void *user, const VmRef *ref);
static int rpg_hostiles(void *user, float radius);

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

void game_set_flying(Game *g, int on)
{
    g->flying = on;
    g->body.noclip = (uint8_t)on;
    g->body.vel.z = 0.0f;
    game_message(g, on ? "Fly mode (debug)" : "Walking mode");
}

void game_message(Game *g, const char *text)
{
    size_t n = strlen(text);
    if (n >= sizeof(g->message))
        n = sizeof(g->message) - 1;
    memcpy(g->message, text, n);
    g->message[n] = '\0';
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

/* ------------------------------------------------------------------------ */
/* RPG layer binding                                                        */
/* ------------------------------------------------------------------------ */
static uint32_t instance_key(const Game *g, uint32_t inst)
{
    const DZoneInstance *in = &g->zone.instances[inst];
    return vm_reference_key(&g->rpg->vm, in->object_hash, g->zone_hash, in->refnum);
}

VmRef game_instance_ref(const Game *g, uint32_t inst)
{
    VmRef r;
    memset(&r, 0, sizeof(r));
    r.vmi = -1;
    r.inst = -1;
    if (!g->rpg || !g->zone.hdr || inst >= g->zone.hdr->instance_count)
        return r;
    r.kind = VM_REF_OBJECT;
    r.inst = (int32_t)inst;
    r.object = g->zone.instances[inst].object_hash;
    r.key = instance_key(g, inst);
    r.vmi = g->inst_vmi ? g->inst_vmi[inst] : -1;
    return r;
}

static void bind_scripts(Game *g)
{
    uint32_t i;
    for (i = 0; i < g->zone.hdr->instance_count; ++i)
        g->inst_vmi[i] = -1;
    if (!g->rpg)
        return;
    vm_unload_zone(&g->rpg->vm);
    for (i = 0; i < g->zone.hdr->instance_count; ++i) {
        const DZoneInstance *in = &g->zone.instances[i];
        uint32_t key = instance_key(g, i);
        g->inst_vmi[i] = (int16_t)vm_bind_reference(&g->rpg->vm, key, in->object_hash, (int32_t)i);
        g->zone.inst_hidden[i] = (uint8_t)vm_disabled(&g->rpg->vm, key);
    }
    g->cell_changed = 2;
}

const char *game_instance_name(const Game *g, uint32_t inst)
{
    const DRpgObject *o;
    if (!g->rpg || !g->zone.hdr || inst >= g->zone.hdr->instance_count)
        return "";
    o = rpgdb_object(&g->rpg->db, g->zone.instances[inst].object_hash);
    return o ? rpgdb_name(&g->rpg->db, o->name) : "";
}

const char *game_cell_name(const Game *g)
{
    const DZoneHeader *h = g->zone.hdr;
    uint32_t i;
    int cx, cy;
    if (!h)
        return "";
    if (h->kind != DZONE_KIND_EXTERIOR)
        return g->zone.name ? g->zone.name : "";
    cx = (int)floorf(g->body.pos.x / 8192.0f);
    cy = (int)floorf(g->body.pos.y / 8192.0f);
    for (i = 0; i < h->cell_count; ++i) {
        const DZoneCell *c = &g->zone.cells[i];
        if (c->x != cx || c->y != cy)
            continue;
        if (g->zone.strings[c->name_ofs])
            return g->zone.strings + c->name_ofs;
        if (g->rpg && c->region_hash) {
            const DRpgObject *r = rpgdb_object(&g->rpg->db, c->region_hash);
            if (r)
                return rpgdb_name(&g->rpg->db, r->name);
        }
        break;
    }
    return "Wilderness";
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

    g->zone_hash = g->world->zone_list[zone].entry_hash;
    g->inst_vmi = (int16_t *)mem_try_alloc(MEM_REGION_ZONE,
                                           (g->zone.hdr->instance_count + 1u) * sizeof(int16_t), 4);
    if (!g->inst_vmi) {
        snprintf(err, errlen, "out of zone memory for script bindings");
        return -1;
    }
    bind_scripts(g);
    g->actor_timer = (float *)mem_try_alloc(MEM_REGION_ZONE, (g->actors.count + 1u) * sizeof(float), 4);
    if (!g->actor_timer) {
        snprintf(err, errlen, "out of zone memory for combat timers");
        return -1;
    }
    for (i = 0; i < g->actors.count; ++i) {
        g->actor_timer[i] = 1.0f;
        if (g->rpg) {
            VmRef ar = game_actor_ref(g, i);
            if (rpg_is_dead(&g->rpg->rpg, &ar)) {
                /* corpses stay down: jump to the end of the death animation */
                actor_play(&g->actors.list[i], pw_hash_name("death1"), 0);
                g->actors.list[i].time = 60.0f;
            }
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
        if (!activatable(in) || z->inst_hidden[i])
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
    if (g->rpg && !(in->kind == DZONE_INST_DOOR && in->door != DZONE_NONE)) {
        const char *name = game_instance_name(g, g->target);
        if (*name)
            return name;
    }
    if (in->kind == DZONE_INST_DOOR && in->door != DZONE_NONE) {
        const DZoneDoor *d = &g->zone.doors[in->door];
        if (d->dest_zone != DZONE_NO_ZONE)
            return world_zone_name(g->world, d->dest_zone);
    }
    return in->kind < PW_COUNTOF(names) ? names[in->kind] : "Use";
}

static int find_actor(const Game *g, uint32_t inst)
{
    uint32_t i;
    for (i = 0; i < g->actors.count; ++i)
        if (g->actors.list[i].instance == inst)
            return (int)i;
    return -1;
}

void game_take(Game *g, uint32_t inst)
{
    char buf[96];
    const char *name = game_instance_name(g, inst);
    if (!g->rpg || inst >= g->zone.hdr->instance_count || g->zone.inst_hidden[inst])
        return;
    {
        VmRef ref = game_instance_ref(g, inst);
        Vm *vm = &g->rpg->vm;
        vm_item_add(vm, vm->player_hash, ref.object, 1);
        vm_set_prop_key(vm, ref.key, MWP_DISABLED, 1.0f);
        g->zone.inst_hidden[inst] = 1;
    }
    snprintf(buf, sizeof(buf), "Taken: %s", *name ? name : "item");
    game_message(g, buf);
    if (g->target == inst)
        g->target = GAME_NO_TARGET;
}

static void open_loot(Game *g, uint32_t inst)
{
    g->ui_owner = game_instance_ref(g, inst);
    vm_seed(&g->rpg->vm, &g->ui_owner);
    g->ui_instance = inst;
    g->ui_event = GAME_UI_LOOT;
}

static void start_dialogue(Game *g, uint32_t inst)
{
    VmRef ref = game_instance_ref(g, inst);
    if (rpg_is_dead(&g->rpg->rpg, &ref)) {
        open_loot(g, inst);
        return;
    }
    if (vm_prop(&g->rpg->vm, &ref, MWP_HOSTILE) != 0.0f)
        return; /* no talking in the middle of a fight */
    if (dlg_start(&g->rpg->dlg, &ref, game_instance_name(g, inst)))
        g->ui_event = GAME_UI_DIALOGUE;
}

/* The engine's default action for an object (also what a script's
 * "Activate" performs). */
static void default_activate(Game *g, uint32_t inst)
{
    const DZoneInstance *in = &g->zone.instances[inst];
    const DRpgObject *o = g->rpg ? rpgdb_object(&g->rpg->db, in->object_hash) : NULL;
    uint32_t i;
    if (in->kind == DZONE_INST_DOOR) {
        if (in->door != DZONE_NONE) {
            const DZoneDoor *d = &g->zone.doors[in->door];
            if (door_lock(g, inst) > 0) {
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
    case DZONE_INST_CREATURE:
        if (g->rpg)
            start_dialogue(g, inst);
        else
            game_message(g, "Dialogue needs rpg.dpak (run rpg_compiler).");
        break;
    case DZONE_INST_CONTAINER:
        if (!g->rpg)
            game_message(g, "Containers need rpg.dpak (run rpg_compiler).");
        else if (door_lock(g, inst) > 0)
            game_message(g, "Locked.");
        else
            open_loot(g, inst);
        break;
    case DZONE_INST_ITEM:
    case DZONE_INST_LIGHT:
        if (!g->rpg) {
            game_message(g, "Items need rpg.dpak (run rpg_compiler).");
        } else if (o && o->type == DRPG_OT_BOOK) {
            g->ui_event = GAME_UI_BOOK;
            g->ui_object = in->object_hash;
            g->ui_instance = inst;
        } else if (in->kind == DZONE_INST_LIGHT && !(o && (o->flags & DRPG_OF_CARRY))) {
            game_message(g, "Nothing happens.");
        } else {
            game_take(g, inst);
        }
        break;
    default:
        game_message(g, "Nothing happens.");
        break;
    }
}

static void activate(Game *g, uint32_t inst)
{
    if (g->rpg) {
        VmRef ref = game_instance_ref(g, inst);
        /* scripted objects with OnActivate decide themselves (they call
         * Activate for the default action) */
        if (vm_activate(&g->rpg->vm, &ref))
            return;
    }
    default_activate(g, inst);
}

/* ------------------------------------------------------------------------ */
/* VM host: world verbs                                                     */
/* ------------------------------------------------------------------------ */
static int ref_position(const Game *g, const VmRef *r, float out[3], float *zrot)
{
    if (r->kind == VM_REF_PLAYER) {
        out[0] = g->body.pos.x;
        out[1] = g->body.pos.y;
        out[2] = g->body.pos.z;
        *zrot = g->yaw;
        return 1;
    }
    if (r->kind == VM_REF_OBJECT && r->inst >= 0 && (uint32_t)r->inst < g->zone.hdr->instance_count) {
        int a = find_actor(g, (uint32_t)r->inst);
        if (a >= 0) {
            out[0] = g->actors.list[a].pos.x;
            out[1] = g->actors.list[a].pos.y;
            out[2] = g->actors.list[a].pos.z;
            *zrot = g->actors.list[a].yaw;
        } else {
            const DZoneInstance *in = &g->zone.instances[r->inst];
            out[0] = in->pos[0];
            out[1] = in->pos[1];
            out[2] = in->pos[2];
            *zrot = in->rot[2];
        }
        return 1;
    }
    return 0;
}

static int set_ref_position(Game *g, const VmRef *r, const float p[3], float zrot, int set_rot)
{
    if (r->kind == VM_REF_PLAYER) {
        g->body.pos.x = p[0];
        g->body.pos.y = p[1];
        g->body.pos.z = p[2];
        g->body.vel.z = 0.0f;
        if (set_rot)
            g->yaw = pw_wrap_angle(zrot);
        return 1;
    }
    if (r->kind == VM_REF_OBJECT && r->inst >= 0 && (uint32_t)r->inst < g->zone.hdr->instance_count) {
        int a = find_actor(g, (uint32_t)r->inst);
        if (a >= 0) {
            Vec4 pos = vec4(p[0], p[1], p[2], 1.0f);
            actor_set_placement(&g->actors.list[a], &pos, set_rot ? zrot : g->actors.list[a].yaw);
        } else {
            /* moves the drawn model; static collision stays where it was cooked */
            DZoneInstance *in = (DZoneInstance *)&g->zone.instances[r->inst];
            memcpy(in->pos, p, sizeof(in->pos));
            if (set_rot)
                in->rot[2] = zrot;
            zone_placement_matrix(&g->zone.inst_world[r->inst], in->pos, in->rot, in->scale);
        }
        return 1;
    }
    return 0;
}

static int axis_of(const VmCtx *ctx, VmValue v)
{
    const char *s = vm_arg_string(ctx, v);
    int c = s[0] | 0x20;
    return c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : -1;
}

static int host_call(void *user, Vm *vm, uint16_t func, const VmRef *ref, const VmCtx *ctx,
                     const VmValue *args, int argc, VmValue *ret)
{
    Game *g = (Game *)user;
    float p[3], q[3], zr, zr2;
    PW_UNUSED(vm);
    if (!g->zone.hdr)
        return 0;
    switch (func) {
    case MWF_ACTIVATE:
        if (ref->kind == VM_REF_OBJECT && ref->inst >= 0)
            default_activate(g, (uint32_t)ref->inst);
        return 1;
    case MWF_GETDISTANCE: {
        VmRef other = vm_resolve(&g->rpg->vm, pw_hash_name(vm_arg_string(ctx, args[0])));
        if (ref_position(g, ref, p, &zr) && ref_position(g, &other, q, &zr2)) {
            float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
            ret->f = sqrtf(dx * dx + dy * dy + dz * dz);
        } else {
            ret->f = 1.0e6f;
        }
        return 1;
    }
    case MWF_GETPOS:
    case MWF_GETANGLE: {
        int axis = axis_of(ctx, args[0]);
        if (axis < 0 || !ref_position(g, ref, p, &zr)) {
            ret->f = 0.0f;
            return 1;
        }
        if (func == MWF_GETPOS)
            ret->f = p[axis];
        else
            ret->f = axis == 2 ? zr * (180.0f / 3.14159265f) : 0.0f;
        return 1;
    }
    case MWF_SETPOS:
    case MWF_SETANGLE: {
        int axis = axis_of(ctx, args[0]);
        if (axis < 0 || !ref_position(g, ref, p, &zr))
            return 1;
        if (func == MWF_SETPOS) {
            p[axis] = args[1].f;
            set_ref_position(g, ref, p, zr, 0);
        } else if (axis == 2) {
            set_ref_position(g, ref, p, args[1].f * (3.14159265f / 180.0f), 1);
        }
        return 1;
    }
    case MWF_POSITION:
        if (argc >= 4) {
            p[0] = args[0].f;
            p[1] = args[1].f;
            p[2] = args[2].f;
            set_ref_position(g, ref, p, args[3].f * (3.14159265f / 180.0f), 1);
        }
        return 1;
    case MWF_POSITIONCELL:
        if (argc >= 5 && ref->kind == VM_REF_PLAYER) {
            int zi = world_find_interior(g->world, pw_hash_name(vm_arg_string(ctx, args[4])));
            if (zi < 0)
                zi = world_find_exterior(g->world, (int)floorf(args[0].f / 8192.0f),
                                         (int)floorf(args[1].f / 8192.0f));
            if (zi >= 0) {
                g->request.pending = 1;
                g->request.zone = (uint32_t)zi;
                g->request.has_pos = 1;
                g->request.pos[0] = args[0].f;
                g->request.pos[1] = args[1].f;
                g->request.pos[2] = args[2].f;
                g->request.rot[0] = g->request.rot[1] = 0.0f;
                g->request.rot[2] = args[3].f * (3.14159265f / 180.0f);
            }
            return 1;
        }
        return 0;
    case MWF_CELLCHANGED:
        ret->i = g->cell_changed > 0;
        return 1;
    case MWF_GETINTERIOR:
        ret->i = g->zone.hdr->kind != DZONE_KIND_EXTERIOR;
        return 1;
    case MWF_GETPCRUNNING:
        ret->i = g->running && !g->sneaking;
        return 1;
    case MWF_GETPCSNEAKING:
        ret->i = g->sneaking;
        return 1;
    case MWF_GETPCJUMPING:
        ret->i = !g->body.on_ground && !g->body.swimming && !g->flying;
        return 1;
    case MWF_GETPCSLEEP:
        ret->i = 0;
        return 1;
    case MWF_GETPLAYERCONTROLSDISABLED:
        ret->i = g->controls_disabled;
        return 1;
    case MWF_DISABLEPLAYERCONTROLS:
    case MWF_ENABLEPLAYERCONTROLS:
        g->controls_disabled = func == MWF_DISABLEPLAYERCONTROLS;
        return 1;
    case MWF_ENABLENAMEMENU:
    case MWF_ENABLERACEMENU:
    case MWF_ENABLECLASSMENU:
    case MWF_ENABLEBIRTHMENU:
    case MWF_ENABLESTATREVIEWMENU:
        g->ui_chargen |= func == MWF_ENABLENAMEMENU ? GAME_CG_NAME : func == MWF_ENABLERACEMENU ? GAME_CG_RACE
                         : func == MWF_ENABLECLASSMENU ? GAME_CG_CLASS : func == MWF_ENABLEBIRTHMENU ? GAME_CG_BIRTH
                                                                                              : GAME_CG_REVIEW;
        g->ui_event = GAME_UI_CHARGEN;
        return 1;
    case MWF_ENABLEINVENTORYMENU:
        g->menus_enabled |= GAME_MENU_INVENTORY;
        return 1;
    case MWF_ENABLEMAGICMENU:
        g->menus_enabled |= GAME_MENU_MAGIC;
        return 1;
    case MWF_ENABLEMAPMENU:
        g->menus_enabled |= GAME_MENU_MAP;
        return 1;
    case MWF_ENABLESTATSMENU:
        g->menus_enabled |= GAME_MENU_STATS;
        return 1;
    case MWF_ENABLEREST:
        g->menus_enabled |= GAME_MENU_REST;
        return 1;
    case MWF_SHOWRESTMENU:
        g->ui_event = GAME_UI_REST;
        return 1;
    case MWF_GETWEAPONDRAWN:
        ret->i = g->rpg->rpg.draw_state == 1;
        return 1;
    case MWF_GETSPELLREADIED:
        ret->i = g->rpg->rpg.draw_state == 2;
        return 1;
    case MWF_GETLOCKED:
        ret->i = (ref->kind == VM_REF_OBJECT && ref->inst >= 0) ? door_lock(g, (uint32_t)ref->inst) > 0 : 0;
        return 1;
    case MWF_LOCK:
    case MWF_UNLOCK:
        if (ref->kind == VM_REF_OBJECT) {
            float lv = func == MWF_UNLOCK ? -1.0f : (argc > 0 ? (float)args[0].i : 100.0f);
            vm_set_prop(vm, ref, MWP_LOCK, lv);
        }
        return 1;
    case MWF_PLAYGROUP:
    case MWF_LOOPGROUP:
        if (ref->kind == VM_REF_OBJECT && ref->inst >= 0) {
            int a = find_actor(g, (uint32_t)ref->inst);
            if (a >= 0) {
                actor_play(&g->actors.list[a], pw_hash_name(vm_arg_string(ctx, args[0])),
                           func == MWF_LOOPGROUP);
                return 1;
            }
        }
        return 0;
    default:
        return 0;
    }
}

static int host_find_ref(void *user, uint32_t object_hash, VmRef *out)
{
    Game *g = (Game *)user;
    uint32_t i;
    if (!g->zone.hdr)
        return 0;
    for (i = 0; i < g->zone.hdr->instance_count; ++i) {
        if (g->zone.instances[i].object_hash == object_hash) {
            *out = game_instance_ref(g, i);
            return 1;
        }
    }
    return 0;
}

static const char *host_cell_name(void *user)
{
    return game_cell_name((const Game *)user);
}

static int host_interior(void *user)
{
    const Game *g = (const Game *)user;
    return g->zone.hdr && g->zone.hdr->kind != DZONE_KIND_EXTERIOR;
}

static void host_set_disabled(void *user, const VmRef *ref, int disabled)
{
    Game *g = (Game *)user;
    if (g->zone.hdr && ref->inst >= 0 && (uint32_t)ref->inst < g->zone.hdr->instance_count)
        g->zone.inst_hidden[ref->inst] = (uint8_t)(disabled != 0);
}

int game_attach_rpg(Game *g, GameRpg *rpg, const char *data_dir, char *err, uint32_t errlen)
{
    char path[256];
    VmHost host;
    snprintf(path, sizeof(path), "%s/rpg.dpak", data_dir);
    if (rpgdb_open(&rpg->db, path, err, errlen) != 0)
        return -1;
    memset(&host, 0, sizeof(host));
    host.user = g;
    host.call = host_call;
    host.find_ref = host_find_ref;
    host.cell_name = host_cell_name;
    host.interior = host_interior;
    host.set_disabled = host_set_disabled;
    if (vm_init(&rpg->vm, &rpg->db, &host) != 0 || dlg_init(&rpg->dlg, &rpg->vm, &rpg->db) != 0) {
        snprintf(err, errlen, "out of script memory");
        return -1;
    }
    {
        RpgHost rh;
        memset(&rh, 0, sizeof(rh));
        rh.user = g;
        rh.teleport = rpg_teleport;
        rh.locate = rpg_locate;
        rh.killed = rpg_killed;
        rh.hostiles_near = rpg_hostiles;
        if (rpg_init(&rpg->rpg, &rpg->vm, &rpg->db, &rh) != 0) {
            snprintf(err, errlen, "rpg.dpak lacks the v2 rules tables");
            return -1;
        }
    }
    /* Morrowind's character generation scripts unlock the menus step by
     * step; without them everything is available from the start */
    g->menus_enabled = rpgdb_script(&rpg->db, pw_hash_name("chargen")) >= 0 ? 0u : GAME_MENU_ALL;
    g->rpg = rpg;
    if (g->zone.hdr)
        bind_scripts(g);
    return 0;
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
        if (g->rpg && g->actor_timer) {
            VmRef r = game_actor_ref(g, i);
            if (rpg_is_dead(&g->rpg->rpg, &r))
                continue; /* corpses neither turn nor block */
            if (vm_prop(&g->rpg->vm, &r, MWP_HOSTILE) != 0.0f) {
                actor_combat(g, i, dt);
                dx = g->body.pos.x - a->pos.x;
                dy = g->body.pos.y - a->pos.y;
                d2 = dx * dx + dy * dy;
                goto obstacle;
            }
        }
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
    obstacle:
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

void game_debug_cycle_animation(Game *g, int dir)
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

/* ------------------------------------------------------------------------ */
/* combat                                                                   */
/* ------------------------------------------------------------------------ */
VmRef game_actor_ref(const Game *g, uint32_t actor)
{
    return game_instance_ref(g, g->actors.list[actor].instance);
}

static int actor_dead(const Game *g, uint32_t actor)
{
    VmRef r;
    if (!g->rpg)
        return 0;
    r = game_actor_ref(g, actor);
    return rpg_is_dead(&g->rpg->rpg, &r);
}

/* nearest living actor whose body the view ray crosses, within max_dist */
static int ray_actor(Game *g, float max_dist, float *dist)
{
    Vec4 o, d;
    uint32_t i, wall_inst;
    int best = -1;
    float best_t = max_dist, wall_t;
    view_ray(g, &o, &d);
    for (i = 0; i < g->actors.count; ++i) {
        const Actor *a = &g->actors.list[i];
        Vec4 c = vec4(a->pos.x, a->pos.y, a->pos.z + 70.0f * a->scale, 1.0f);
        float t;
        if (actor_dead(g, i) || g->zone.inst_hidden[a->instance])
            continue;
        if (ray_sphere(&o, &d, &c, 48.0f * a->scale, &t) && t < best_t) {
            best_t = t;
            best = (int)i;
        }
    }
    if (best >= 0 && phys_raycast(&g->phys, &o, &d, best_t, &wall_t, &wall_inst) && wall_t + 8.0f < best_t)
        best = -1;
    if (dist)
        *dist = best_t;
    return best;
}

static void hit_message(Game *g, const RpgHit *h, const char *name)
{
    char buf[80];
    if (!h->hit)
        snprintf(buf, sizeof(buf), "You miss %s.", name);
    else if (h->blocked)
        snprintf(buf, sizeof(buf), "%s blocks.", name);
    else if (h->killed)
        snprintf(buf, sizeof(buf), "%s is dead.", name);
    else
        snprintf(buf, sizeof(buf), "You hit %s for %d.", name, (int)(h->damage + 0.5f));
    game_message(g, buf);
}

static int door_lock(Game *g, uint32_t inst)
{
    const DZoneInstance *in = &g->zone.instances[inst];
    int lock = (in->door != DZONE_NONE) ? g->zone.doors[in->door].lock_level : 0;
    if (g->rpg) {
        VmRef r = game_instance_ref(g, inst);
        float v = vm_prop(&g->rpg->vm, &r, MWP_LOCK);
        if (v > 0.0f)
            lock = (int)v;
        else if (v < 0.0f)
            lock = 0;
    }
    return lock;
}

static void player_attack(Game *g, float strength)
{
    Rpg *r = &g->rpg->rpg;
    VmRef pc = vm_player_ref(&g->rpg->vm);
    int st = rpg_equipped(r, &pc, RPG_SLOT_WEAPON), a;
    const VmItem *it = st >= 0 ? vm_item_at(&g->rpg->vm, st) : NULL;
    const DRpgObject *w = it ? rpg_object(r, it->item) : NULL;
    float reach, t;
    if (w && w->type == DRPG_OT_LOCK) {
        /* a readied lockpick works on the targeted lock */
        char msg[96];
        int lvl;
        if (g->target != GAME_NO_TARGET && g->zone.instances[g->target].kind == DZONE_INST_DOOR) {
            VmRef d = game_instance_ref(g, g->target);
            if (rpg_pick_lock(r, &d, door_lock(g, g->target), &lvl, msg, sizeof(msg)) && lvl == 0)
                vm_set_prop(&g->rpg->vm, &d, MWP_LOCK, -1.0f);
            game_message(g, msg);
        }
        return;
    }
    if (w && w->type == DRPG_OT_PROB)
        return;
    if (rpg_weapon_ranged(r, &pc)) {
        if (!rpg_consume_ammo(r, &pc)) {
            game_message(g, "You have no ammunition.");
            return;
        }
        reach = 4096.0f;
    } else {
        reach = rpg_weapon_reach(r, &pc) + BODY_RADIUS;
    }
    a = ray_actor(g, reach, &t);
    if (a < 0) {
        /* a swing at the air still costs fatigue */
        rpg_dyn_add(r, &pc, 2, -rpg_gmst_f(r, "fFatigueAttackBase", 2.0f));
        return;
    }
    {
        VmRef v = game_actor_ref(g, (uint32_t)a);
        RpgHit h = rpg_attack(r, &pc, &v, strength);
        hit_message(g, &h, game_instance_name(g, g->actors.list[a].instance));
        g->enemy_key = v.key;
        g->enemy_time = 6.0f;
        if (h.hit && !h.killed)
            actor_play(&g->actors.list[a], pw_hash_name("hit1"), 0);
    }
}

static void apply_area(Game *g, const RpgCast *c, const VmRef *caster, const Vec4 *at, uint32_t skip_key)
{
    Rpg *r = &g->rpg->rpg;
    float radius = c->area * 22.1f;
    uint32_t i;
    VmRef pc = vm_player_ref(&g->rpg->vm);
    if (radius <= 0.0f)
        return;
    for (i = 0; i < g->actors.count; ++i) {
        const Actor *a = &g->actors.list[i];
        float dx = a->pos.x - at->x, dy = a->pos.y - at->y, dz = a->pos.z + 60.0f - at->z;
        VmRef v = game_actor_ref(g, i);
        if (v.key == skip_key || actor_dead(g, i) || dx * dx + dy * dy + dz * dz > radius * radius)
            continue;
        rpg_apply(r, caster, &v, c->effects, c->count, RPG_RANGE_TARGET, c->source, c->chance, 1);
    }
    {
        float dx = g->body.pos.x - at->x, dy = g->body.pos.y - at->y;
        if (skip_key != pc.key && dx * dx + dy * dy <= radius * radius)
            rpg_apply(r, caster, &pc, c->effects, c->count, RPG_RANGE_TARGET, c->source, c->chance, 1);
    }
}

static void player_cast(Game *g)
{
    Rpg *r = &g->rpg->rpg;
    VmRef pc = vm_player_ref(&g->rpg->vm);
    RpgCast c;
    if (!rpg_begin_cast(r, &pc, &c)) {
        game_message(g, c.message[0] ? c.message : "You cannot cast that.");
        return;
    }
    if (c.has[RPG_RANGE_SELF])
        rpg_apply(r, &pc, &pc, c.effects, c.count, RPG_RANGE_SELF, c.source, c.chance, 1);
    if (c.has[RPG_RANGE_TOUCH]) {
        float t;
        int a = ray_actor(g, GAME_ACTIVATE_DIST, &t);
        if (a >= 0) {
            VmRef v = game_actor_ref(g, (uint32_t)a);
            rpg_apply(r, &pc, &v, c.effects, c.count, RPG_RANGE_TOUCH, c.source, c.chance, 1);
            g->enemy_key = v.key;
            g->enemy_time = 6.0f;
        } else if (g->target != GAME_NO_TARGET) {
            /* doors and containers: Open / Lock */
            VmRef d = game_instance_ref(g, g->target);
            rpg_apply(r, &pc, &d, c.effects, c.count, RPG_RANGE_TOUCH, c.source, c.chance, 1);
        }
    }
    if (c.has[RPG_RANGE_TARGET]) {
        Vec4 o, d, at;
        float t, wall_t;
        uint32_t wall_inst;
        int a = ray_actor(g, 4096.0f, &t);
        VmRef v;
        memset(&v, 0, sizeof(v));
        view_ray(g, &o, &d);
        if (a >= 0) {
            v = game_actor_ref(g, (uint32_t)a);
            rpg_apply(r, &pc, &v, c.effects, c.count, RPG_RANGE_TARGET, c.source, c.chance, 1);
            g->enemy_key = v.key;
            g->enemy_time = 6.0f;
        } else if (phys_raycast(&g->phys, &o, &d, 4096.0f, &wall_t, &wall_inst)) {
            t = wall_t;
        } else {
            t = 4096.0f;
        }
        at = vec4(o.x + d.x * t, o.y + d.y * t, o.z + d.z * t, 1.0f);
        apply_area(g, &c, &pc, &at, v.key);
    }
    game_message(g, rpg_spell_name(r, c.source)[0] ? rpg_spell_name(r, c.source) : "Cast.");
}

/* hostile actors turn on the player, close in and strike (in place of the
 * full AI packages of Milestone 6) */
static void actor_combat(Game *g, uint32_t i, float dt)
{
    Rpg *r = &g->rpg->rpg;
    Actor *a = &g->actors.list[i];
    VmRef self = game_actor_ref(g, i), pc = vm_player_ref(&g->rpg->vm);
    float dx = g->body.pos.x - a->pos.x, dy = g->body.pos.y - a->pos.y, d = sqrtf(dx * dx + dy * dy);
    float reach = rpg_weapon_reach(r, &self) + BODY_RADIUS * 2.0f;
    float want = atan2f(dx, dy);
    Vec4 p = a->pos;
    if (d > 3000.0f || g->player_dead || rpg_effect(r, &self, MGEF_PARALYZE, -1) > 0.0f)
        return;
    if (d > reach * 0.9f) {
        float step = rpg_walk_speed(r, &self) * dt, ground;
        if (step > d - reach * 0.8f)
            step = d - reach * 0.8f;
        p.x += dx / d * step;
        p.y += dy / d * step;
        if (phys_ground(&g->phys, p.x, p.y, p.z + PHYS_STEP_UP, &ground) && ground > p.z - 256.0f)
            p.z = ground;
    }
    actor_set_placement(a, &p, want);
    if (g->actor_timer[i] > 0.0f)
        g->actor_timer[i] -= dt;
    if (d <= reach && g->actor_timer[i] <= 0.0f) {
        float strength = 0.5f + 0.5f * vm_random01(&g->rpg->vm);
        RpgHit h = rpg_attack(r, &self, &pc, strength);
        actor_play(a, pw_hash_name("attack1"), 0);
        g->actor_timer[i] = 1.2f + 0.8f * vm_random01(&g->rpg->vm);
        if (h.hit && h.damage > 0.0f) {
            char buf[80];
            snprintf(buf, sizeof(buf), "%s hits you for %d.", game_instance_name(g, a->instance),
                     (int)(h.damage + 0.5f));
            game_message(g, buf);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* rpg host                                                                 */
/* ------------------------------------------------------------------------ */
static void rpg_teleport(void *user, uint32_t cell, const float pos[3], float rot_z)
{
    Game *g = (Game *)user;
    int zi = cell ? world_find_interior(g->world, cell)
                  : world_find_exterior(g->world, (int)floorf(pos[0] / 8192.0f), (int)floorf(pos[1] / 8192.0f));
    if (zi < 0) {
        game_message(g, "That place was not cooked.");
        return;
    }
    g->request.pending = 1;
    g->request.zone = (uint32_t)zi;
    g->request.has_pos = 1;
    memcpy(g->request.pos, pos, sizeof(g->request.pos));
    g->request.rot[0] = g->request.rot[1] = 0.0f;
    g->request.rot[2] = rot_z;
}

static void rpg_locate(void *user, uint32_t *cell, float pos[3], float *rot_z)
{
    Game *g = (Game *)user;
    *cell = g->zone.hdr && g->zone.hdr->kind != DZONE_KIND_EXTERIOR
                ? g->world->zone_list[g->zone.index].cell_hash : 0;
    pos[0] = g->body.pos.x;
    pos[1] = g->body.pos.y;
    pos[2] = g->body.pos.z;
    *rot_z = g->yaw;
}

static void rpg_killed(void *user, const VmRef *ref)
{
    Game *g = (Game *)user;
    uint32_t i;
    if (ref->kind == VM_REF_PLAYER) {
        g->player_dead = 1;
        g->ui_event = GAME_UI_DEATH;
        return;
    }
    for (i = 0; i < g->actors.count; ++i) {
        VmRef a = game_actor_ref(g, i);
        if (a.key == ref->key)
            actor_play(&g->actors.list[i], pw_hash_name("death1"), 0);
    }
}

static int rpg_hostiles(void *user, float radius)
{
    Game *g = (Game *)user;
    uint32_t i;
    for (i = 0; i < g->actors.count; ++i) {
        const Actor *a = &g->actors.list[i];
        VmRef r = game_actor_ref(g, i);
        float dx = g->body.pos.x - a->pos.x, dy = g->body.pos.y - a->pos.y;
        if (!actor_dead(g, i) && vm_prop(&g->rpg->vm, &r, MWP_HOSTILE) != 0.0f &&
            dx * dx + dy * dy < radius * radius)
            return 1;
    }
    return 0;
}

void game_respawn(Game *g)
{
    char err[120];
    VmRef pc;
    int d;
    if (!g->rpg)
        return;
    pc = vm_player_ref(&g->rpg->vm);
    vm_set_prop(&g->rpg->vm, &pc, MWP_DEAD, 0.0f);
    for (d = 0; d < 3; ++d)
        vm_set_prop(&g->rpg->vm, &pc, MWP_DYN_HEALTH + d, rpg_dyn_max(&g->rpg->rpg, &pc, d));
    g->player_dead = 0;
    game_enter_zone(g, g->zone.index, NULL, NULL, err, sizeof(err));
}

static void game_rpg_tick(Game *g, float dt)
{
    Vm *vm;
    if (!g->rpg)
        return;
    vm = &g->rpg->vm;
    vm->menu_mode = g->menu_mode;
    vm_tick(vm, dt);
    if (!g->menu_mode)
        rpg_tick(&g->rpg->rpg, dt);
    if (g->cell_changed > 0)
        g->cell_changed--;
    if (vm->has_force_greeting) {
        VmRef r = vm->force_greeting;
        vm->has_force_greeting = 0;
        if (!g->rpg->dlg.active && r.inst >= 0 && g->ui_event == GAME_UI_NONE)
            start_dialogue(g, (uint32_t)r.inst);
    }
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
    if (g->menu_mode) {
        /* menus pause the world; scripts keep running and see MenuMode */
        game_rpg_tick(g, dt);
        return;
    }
    if (g->controls_disabled) {
        InputState *mut = &g->frozen_input;
        memset(mut, 0, sizeof(*mut));
        mut->pressed = in->pressed & GB_R;
        in = mut;
        pressed = in->pressed;
        held = 0;
    }

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
    if (g->rpg && (pressed & GB_LEFT)) {
        Rpg *r = &g->rpg->rpg;
        r->draw_state = r->draw_state == 1 ? 0 : 1;
        game_message(g, r->draw_state == 1 ? "Weapon ready" : "Weapon away");
    }
    if (g->rpg && (pressed & GB_RIGHT)) {
        Rpg *r = &g->rpg->rpg;
        r->draw_state = r->draw_state == 2 ? 0 : 2;
        game_message(g, r->draw_state == 2 ? "Magic ready" : "Magic away");
    }

    /* ---- movement */
    if (g->rpg && !g->flying) {
        Rpg *r = &g->rpg->rpg;
        VmRef pc = vm_player_ref(&g->rpg->vm);
        float walk = rpg_walk_speed(r, &pc);
        int paralyzed = rpg_effect(r, &pc, MGEF_PARALYZE, -1) > 0.0f;
        speed = g->body.swimming ? walk * (0.5f + 0.005f * rpg_skill(r, &pc, SKILL_ATHLETICS)) +
                                       rpg_effect(r, &pc, MGEF_SWIFT_SWIM, -1)
                : g->sneaking ? walk * rpg_gmst_f(r, "fSneakSpeedMultiplier", 0.75f)
                : g->running ? rpg_run_speed(r, &pc) : walk;
        if (rpg_effect(r, &pc, MGEF_LEVITATE, -1) > 0.0f)
            speed = walk + rpg_effect(r, &pc, MGEF_LEVITATE, -1);
        g->body.levitate = (uint8_t)(rpg_effect(r, &pc, MGEF_LEVITATE, -1) > 0.0f);
        g->body.water_walk = (uint8_t)(rpg_effect(r, &pc, MGEF_WATER_WALKING, -1) > 0.0f);
        g->body.fall_mult = rpg_effect(r, &pc, MGEF_SLOW_FALL, -1) > 0.0f ? 0.15f : 1.0f;
        if (rpg_encumbrance(r, &pc) > rpg_capacity(r, &pc)) {
            speed = 0.0f;
            if (!g->encumbered_notice && (move_x != 0.0f || move_y != 0.0f))
                vm_notify(&g->rpg->vm, rpgdb_gmst_string(&g->rpg->db, "sNotifyMessage59",
                                                         "You are carrying too much to move."));
            g->encumbered_notice = 1;
        } else {
            g->encumbered_notice = 0;
        }
        if (paralyzed || g->player_dead)
            speed = 0.0f;
        r->moving = (move_x != 0.0f || move_y != 0.0f) && speed > 0.0f;
        r->running = g->running && !g->sneaking;
        r->swimming = g->body.swimming;
        r->sneaking = g->sneaking;
    } else {
        speed = g->flying ? FLY_SPEED : (g->body.swimming ? SWIM_SPEED
                                         : (g->sneaking ? SNEAK_SPEED
                                            : (g->running ? RUN_SPEED : WALK_SPEED)));
    }
    pw_sincosf(g->yaw, &sy, &cy);
    /* forward = (sin yaw, cos yaw); right = (cos yaw, -sin yaw) */
    wish = vec4((sy * move_y + cy * move_x) * speed, (cy * move_y - sy * move_x) * speed, 0.0f, 0.0f);
    if (g->flying || g->body.swimming || g->body.levitate) {
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
    {
        int jump = !g->flying && (pressed & GB_L) != 0 && !g->player_dead;
        int grounded = g->body.on_ground;
        float jump_speed = JUMP_SPEED;
        if (g->rpg) {
            VmRef pc = vm_player_ref(&g->rpg->vm);
            jump_speed = rpg_jump_speed(&g->rpg->rpg, &pc);
        }
        phys_move(&g->phys, &g->body, &wish, jump, jump_speed, dt);
        if (g->rpg && !g->flying) {
            if (jump && grounded && !g->body.on_ground)
                rpg_player_jumped(&g->rpg->rpg);
            if (!g->body.on_ground && !g->body.swimming && !g->body.levitate) {
                if (!g->was_airborne || g->body.pos.z > g->fall_top)
                    g->fall_top = g->body.pos.z;
                g->was_airborne = 1;
            } else {
                if (g->was_airborne && g->body.on_ground)
                    rpg_player_landed(&g->rpg->rpg, g->fall_top - g->body.pos.z);
                g->was_airborne = 0;
            }
        }
    }
    if (g->body.pos.z < KILL_Z) {
        char err[8];
        game_message(g, "You fell out of the world.");
        game_enter_zone(g, g->zone.index, NULL, NULL, err, sizeof(err));
        return;
    }

    /* ---- activation and combat */
    g->target = find_target(g);
    if (g->cast_cooldown > 0.0f)
        g->cast_cooldown -= dt;
    if (g->enemy_time > 0.0f)
        g->enemy_time -= dt;
    if (!g->flying && !g->player_dead) {
        int state = g->rpg ? g->rpg->rpg.draw_state : 0;
        if (state == 1) {
            if (held & GB_R) {
                g->attack_holding = 1;
                g->attack_charge = PW_MIN(1.0f, g->attack_charge + dt);
            } else if (g->attack_holding) {
                player_attack(g, PW_MAX(0.1f, g->attack_charge));
                g->attack_holding = 0;
                g->attack_charge = 0.0f;
            }
        } else if (state == 2) {
            if ((pressed & GB_R) && g->cast_cooldown <= 0.0f) {
                player_cast(g);
                g->cast_cooldown = 0.8f;
            }
        } else if ((pressed & GB_R) && g->target != GAME_NO_TARGET) {
            activate(g, g->target);
        }
    }
    if (g->rpg) {
        int cx = (int)floorf(g->body.pos.x / 8192.0f), cy2 = (int)floorf(g->body.pos.y / 8192.0f);
        if (g->zone.hdr->kind == DZONE_KIND_EXTERIOR)
            rpg_visit_cell(&g->rpg->rpg, cx, cy2);
    }
    game_rpg_tick(g, dt);
    check_zone_border(g);
}
