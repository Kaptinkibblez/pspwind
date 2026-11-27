/*
 * ai.c - actor behaviour (see ai.h).
 */
#include "ai.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game.h"

#define AI_BODY_RADIUS 28.0f
#define AI_BODY_HEIGHT 128.0f
#define AI_ARRIVE 56.0f
#define AI_EYE 110.0f
#define AI_STITCH_DIST 700.0f

static Rpg *rpg_of(Game *g)
{
    return &g->rpg->rpg;
}

static Vm *vm_of(Game *g)
{
    return &g->rpg->vm;
}

static float gmst_f(Game *g, const char *id, float fallback)
{
    return rpg_gmst_f(rpg_of(g), id, fallback);
}

static void actor_xyz(const Game *g, int i, float p[3])
{
    const Actor *a = &g->actors.list[i];
    p[0] = a->pos.x;
    p[1] = a->pos.y;
    p[2] = a->pos.z;
}

static float dist2(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1];
    return dx * dx + dy * dy;
}

static float dist3(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void player_xyz(const Game *g, float p[3])
{
    p[0] = g->body.pos.x;
    p[1] = g->body.pos.y;
    p[2] = g->body.pos.z;
}

static int is_dead(Game *g, int i)
{
    VmRef r = game_actor_ref(g, (uint32_t)i);
    return rpg_is_dead(rpg_of(g), &r) || g->zone.inst_hidden[g->actors.list[i].instance];
}

static int is_creature(const Game *g, int i)
{
    return (g->actors.list[i].rec->flags & DZONE_ACTOR_F_CREATURE) != 0;
}

static double game_hours(Game *g)
{
    Vm *vm = vm_of(g);
    return (double)vm->days_passed * 24.0 + (double)vm->hour;
}

static void play_sound(Game *g, int actor, uint32_t sound, int cat)
{
    AudioPlayOpts o;
    float p[3];
    if (!g->audio || !sound)
        return;
    memset(&o, 0, sizeof(o));
    actor_xyz(g, actor, p);
    p[2] += 64.0f;
    o.pos = p;
    o.owner = game_actor_ref(g, (uint32_t)actor).key;
    o.cat = cat;
    audio_play_sound(g->audio, sound, &o);
}

/* creature sound generator, or the NPC/default id */
static void play_gen(Game *g, int actor, int type, const char *npc_sound)
{
    VmRef r = game_actor_ref(g, (uint32_t)actor);
    uint32_t s = 0;
    if (is_creature(g, actor))
        s = rpgdb_soundgen(rpg_of(g)->db, r.object, type);
    if (!s && npc_sound)
        s = pw_hash_name(npc_sound);
    play_sound(g, actor, s, (type <= DRPG_SNDG_SWIM_RIGHT) ? AUDIO_CAT_FOOTSTEPS : AUDIO_CAT_EFFECTS);
}

static void face(Game *g, int i, const float p[3], float dt, float rate)
{
    Actor *a = &g->actors.list[i];
    float want = atan2f(p[0] - a->pos.x, p[1] - a->pos.y), diff = pw_wrap_angle(want - a->yaw);
    float step = rate * dt;
    Vec4 pos = a->pos;
    if (diff > step)
        diff = step;
    else if (diff < -step)
        diff = -step;
    if (fabsf(diff) > 1e-4f)
        actor_set_placement(a, &pos, pw_wrap_angle(a->yaw + diff));
}

static void play_loop(Actor *a, const char *group)
{
    uint32_t h = pw_hash_name(group);
    if (a->group != h)
        actor_play(a, h, 1);
}

/* ------------------------------------------------------------------------ */
/* path grid                                                                */
/* ------------------------------------------------------------------------ */
typedef struct {
    const DPgrdHeader *hdr;
    const float (*pts)[3];
    const uint16_t *first;
    const uint16_t *edges;
} Grid;

static int load_grid(Game *g, uint32_t hash, Grid *out)
{
    uint32_t size = 0, need;
    const uint8_t *blob = (const uint8_t *)pak_load(&g->rpg->db.pak, hash, MEM_REGION_ZONE, 16, &size);
    const DPgrdHeader *h = (const DPgrdHeader *)blob;
    if (!blob || size < sizeof(DPgrdHeader) || h->magic != DPGR_MAGIC || h->version != 1 || !h->count)
        return 0;
    need = (uint32_t)sizeof(DPgrdHeader) + h->count * 12u + (h->count + 1u) * 2u + h->edge_count * 2u;
    if (need > size)
        return 0;
    out->hdr = h;
    out->pts = (const float(*)[3])(blob + sizeof(DPgrdHeader));
    out->first = (const uint16_t *)(blob + sizeof(DPgrdHeader) + h->count * 12u);
    out->edges = out->first + h->count + 1;
    if (out->first[h->count] != h->edge_count)
        return 0;
    return 1;
}

static void build_graph(Game *g)
{
    AiState *ai = &g->ai;
    Grid grids[32];
    uint32_t ngrids = 0, total = 0, edges = 0, i, base;
    char key[96];
    const DZoneHeader *h = g->zone.hdr;
    ai->count = ai->edge_count = 0;
    if (h->kind == DZONE_KIND_EXTERIOR) {
        for (i = 0; i < h->cell_count && ngrids < 32; ++i) {
            snprintf(key, sizeof(key), "pgrd/%d,%d", g->zone.cells[i].x, g->zone.cells[i].y);
            if (load_grid(g, pw_hash_name(key), &grids[ngrids]))
                ngrids++;
        }
    } else if (g->zone.name) {
        snprintf(key, sizeof(key), "pgrd/%s", g->zone.name);
        if (load_grid(g, pw_hash_name(key), &grids[0]))
            ngrids = 1;
    }
    for (i = 0; i < ngrids; ++i) {
        total += grids[i].hdr->count;
        edges += grids[i].hdr->edge_count;
    }
    if (!total || total > AI_GRAPH_MAX)
        return;
    /* room for the stitching edges between neighbouring cells */
    edges += total * 2u;
    ai->pts = (float(*)[3])mem_try_alloc(MEM_REGION_ZONE, total * sizeof(float[3]), 16);
    ai->first = (uint16_t *)mem_try_alloc(MEM_REGION_ZONE, (total + 1u) * sizeof(uint16_t), 4);
    ai->edges = (uint16_t *)mem_try_alloc(MEM_REGION_ZONE, edges * sizeof(uint16_t), 4);
    ai->gscore = (float *)mem_try_alloc(MEM_REGION_ZONE, total * sizeof(float), 4);
    ai->came = (int16_t *)mem_try_alloc(MEM_REGION_ZONE, total * sizeof(int16_t), 4);
    ai->heap = (uint16_t *)mem_try_alloc(MEM_REGION_ZONE, total * sizeof(uint16_t), 4);
    ai->closed = (uint8_t *)mem_try_alloc(MEM_REGION_ZONE, total, 4);
    if (!ai->pts || !ai->first || !ai->edges || !ai->gscore || !ai->came || !ai->heap || !ai->closed) {
        ai->pts = NULL;
        return;
    }
    base = 0;
    for (i = 0; i < ngrids; ++i) {
        const Grid *gr = &grids[i];
        uint32_t p;
        for (p = 0; p < gr->hdr->count; ++p) {
            uint32_t self = base + p, j;
            memcpy(ai->pts[self], gr->pts[p], sizeof(float[3]));
            ai->first[self] = (uint16_t)ai->edge_count;
            for (j = gr->first[p]; j < gr->first[p + 1]; ++j)
                if (gr->edges[j] < gr->hdr->count)
                    ai->edges[ai->edge_count++] = (uint16_t)(base + gr->edges[j]);
            /* stitch: link to the nearest point of every other grid nearby
             * (each point of each grid does this, so links exist both ways
             * wherever two cells' grids come close) */
            if (ngrids > 1) {
                uint32_t o;
                for (o = 0; o < ngrids && ai->edge_count < edges; ++o) {
                    const Grid *og = &grids[o];
                    uint32_t q, best = 0xFFFFFFFFu, obase = 0, t;
                    float bd = AI_STITCH_DIST * AI_STITCH_DIST;
                    if (o == i)
                        continue;
                    for (t = 0; t < o; ++t)
                        obase += grids[t].hdr->count;
                    for (q = 0; q < og->hdr->count; ++q) {
                        float d = dist2(gr->pts[p], og->pts[q]);
                        if (d < bd) {
                            bd = d;
                            best = q;
                        }
                    }
                    if (best != 0xFFFFFFFFu)
                        ai->edges[ai->edge_count++] = (uint16_t)(obase + best);
                }
            }
        }
        base += gr->hdr->count;
    }
    ai->first[total] = (uint16_t)ai->edge_count;
    ai->count = total;
}

int ai_nearest_point(Game *g, const float p[3])
{
    AiState *ai = &g->ai;
    uint32_t i;
    int best = -1;
    float bd = 1e30f;
    for (i = 0; i < ai->count; ++i) {
        float d = dist2(ai->pts[i], p) + 4.0f * (ai->pts[i][2] - p[2]) * (ai->pts[i][2] - p[2]);
        if (d < bd) {
            bd = d;
            best = (int)i;
        }
    }
    return best;
}

static void heap_push(AiState *ai, uint32_t *n, uint16_t v, const float *f)
{
    uint32_t i = (*n)++;
    ai->heap[i] = v;
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (f[ai->heap[p]] <= f[ai->heap[i]])
            break;
        {
            uint16_t t = ai->heap[p];
            ai->heap[p] = ai->heap[i];
            ai->heap[i] = t;
        }
        i = p;
    }
}

static uint16_t heap_pop(AiState *ai, uint32_t *n, const float *f)
{
    uint16_t top = ai->heap[0];
    uint32_t i = 0;
    ai->heap[0] = ai->heap[--(*n)];
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < *n && f[ai->heap[l]] < f[ai->heap[m]])
            m = l;
        if (r < *n && f[ai->heap[r]] < f[ai->heap[m]])
            m = r;
        if (m == i)
            break;
        {
            uint16_t t = ai->heap[m];
            ai->heap[m] = ai->heap[i];
            ai->heap[i] = t;
        }
        i = m;
    }
    return top;
}

int ai_find_path(Game *g, const float from[3], const float to[3], int16_t *out, int max)
{
    AiState *ai = &g->ai;
    int s, t, n = 0;
    uint32_t i, open = 0;
    MemMark m;
    float *f;
    if (!ai->pts || !ai->count)
        return -1;
    s = ai_nearest_point(g, from);
    t = ai_nearest_point(g, to);
    if (s < 0 || t < 0)
        return -1;
    m = mem_mark_hi(MEM_REGION_ZONE);
    f = (float *)mem_try_alloc_hi(MEM_REGION_ZONE, ai->count * sizeof(float), 4);
    if (!f)
        return -1;
    for (i = 0; i < ai->count; ++i) {
        ai->gscore[i] = 1e30f;
        ai->came[i] = -1;
        ai->closed[i] = 0;
    }
    ai->gscore[s] = 0.0f;
    f[s] = dist3(ai->pts[s], ai->pts[t]);
    heap_push(ai, &open, (uint16_t)s, f);
    while (open) {
        uint16_t c = heap_pop(ai, &open, f);
        uint32_t e;
        if (ai->closed[c])
            continue;
        if (c == t)
            break;
        ai->closed[c] = 1;
        for (e = ai->first[c]; e < ai->first[c + 1u]; ++e) {
            uint16_t nb = ai->edges[e];
            float gs = ai->gscore[c] + dist3(ai->pts[c], ai->pts[nb]);
            if (ai->closed[nb] || gs >= ai->gscore[nb] || open >= ai->count)
                continue;
            ai->gscore[nb] = gs;
            ai->came[nb] = (int16_t)c;
            f[nb] = gs + dist3(ai->pts[nb], ai->pts[t]);
            heap_push(ai, &open, nb, f);
        }
    }
    mem_release_hi(MEM_REGION_ZONE, m);
    if (s != t && ai->came[t] < 0) {
        ai->path_fails++;
        return -1;
    }
    /* walk back from the goal, then reverse */
    {
        int16_t c = (int16_t)t;
        while (c >= 0 && n < max) {
            out[n++] = c;
            if (c == s)
                break;
            c = ai->came[c];
        }
        for (i = 0; i < (uint32_t)n / 2; ++i) {
            int16_t tmp = out[i];
            out[i] = out[n - 1 - (int)i];
            out[n - 1 - (int)i] = tmp;
        }
    }
    ai->paths++;
    return n;
}

/* ------------------------------------------------------------------------ */
/* setup                                                                    */
/* ------------------------------------------------------------------------ */
static void init_actor(Game *g, int i)
{
    AiActor *s = &g->ai.actors[i];
    Actor *a = &g->actors.list[i];
    VmRef r = game_actor_ref(g, (uint32_t)i);
    const DRpgObject *o = rpg_object(rpg_of(g), r.object);
    Vec4 feet = a->pos;
    memset(s, 0, sizeof(*s));
    s->home[0] = a->pos.x;
    s->home[1] = a->pos.y;
    s->home[2] = a->pos.z;
    s->target = AI_TARGET_NONE;
    s->think = AI_THINK_PERIOD * (float)(i % 4) / 4.0f;
    s->timer = 1.0f + (float)(i % 5);
    s->life = -1.0f;
    s->pkg.type = DRPG_AI_WANDER;
    if (o && (o->type == DRPG_OT_NPC || o->type == DRPG_OT_CREA) && o->stat[4] > 0 &&
        (uint32_t)o->stat[3] < rpg_of(g)->db->hdr->aipkgs.count) {
        s->pkg = rpg_of(g)->db->aipkgs[o->stat[3]];
        s->has_pkg = 1;
    }
    s->pkg_start = game_hours(g);
    phys_body_init(&s->body, &feet, AI_BODY_RADIUS * a->scale, AI_BODY_HEIGHT * a->scale);
    s->body.ignore_obstacle = (uint32_t)i;
    vm_seed(vm_of(g), &r);
}

void ai_zone_init(Game *g)
{
    AiState *ai = &g->ai;
    uint32_t i;
    ai->actors = NULL;
    ai->capacity = 0;
    ai->pts = NULL;
    ai->count = 0;
    ai->combat = 0;
    if (!g->rpg || !g->zone.hdr)
        return;
    ai->capacity = g->actors.capacity;
    ai->actors = (AiActor *)mem_try_alloc(MEM_REGION_ZONE, (ai->capacity + 1u) * sizeof(AiActor), 16);
    if (!ai->actors) {
        ai->capacity = 0;
        return;
    }
    for (i = 0; i < g->actors.count; ++i)
        init_actor(g, (int)i);
    build_graph(g);
}

/* ------------------------------------------------------------------------ */
/* perception                                                               */
/* ------------------------------------------------------------------------ */
static int clear_line(Game *g, const float a[3], const float b[3])
{
    Vec4 o = vec4(a[0], a[1], a[2] + AI_EYE, 1.0f), d;
    float dx = b[0] - a[0], dy = b[1] - a[1], dz = (b[2] + AI_EYE) - (a[2] + AI_EYE);
    float len = sqrtf(dx * dx + dy * dy + dz * dz), t;
    uint32_t inst;
    if (len < 1.0f)
        return 1;
    d = vec4(dx / len, dy / len, dz / len, 0.0f);
    return !phys_raycast(&g->phys, &o, &d, len, &t, &inst) || t >= len - 16.0f;
}

int ai_has_los(Game *g, int actor)
{
    float a[3], p[3];
    actor_xyz(g, actor, a);
    player_xyz(g, p);
    return clear_line(g, a, p);
}

int ai_detects_player(Game *g, int actor)
{
    Rpg *r = rpg_of(g);
    VmRef pc = vm_player_ref(vm_of(g)), self = game_actor_ref(g, (uint32_t)actor);
    float a[3], p[3], d, pcs, obs, chance, cham, fwd_x, fwd_y, dot;
    const Actor *ac = &g->actors.list[actor];
    actor_xyz(g, actor, a);
    player_xyz(g, p);
    d = dist3(a, p);
    if (d > AI_ACTIVE_DIST || !clear_line(g, a, p))
        return 0;
    if (rpg_effect(r, &pc, MGEF_INVISIBILITY, -1) > 0.0f)
        return 0;
    cham = PW_MIN(100.0f, rpg_effect(r, &pc, MGEF_CHAMELEON, -1));
    if (!g->sneaking && cham <= 0.0f)
        return 1;
    /* OpenMW's awareness check */
    pcs = 0.0f;
    if (g->sneaking)
        pcs = (rpg_skill(r, &pc, SKILL_SNEAK) + 0.2f * rpg_attr(r, &pc, ATTR_AGILITY) +
               0.1f * rpg_attr(r, &pc, ATTR_LUCK)) * rpg_fatigue_term(r, &pc);
    pcs += cham;
    pcs *= gmst_f(g, "fSneakDistanceBase", 0.5f) + gmst_f(g, "fSneakDistanceMultiplier", 0.002f) * d;
    obs = (rpg_skill(r, &self, SKILL_SNEAK) + 0.2f * rpg_attr(r, &self, ATTR_AGILITY) +
           0.1f * rpg_attr(r, &self, ATTR_LUCK) - rpg_effect(r, &self, MGEF_BLIND, -1)) *
          rpg_fatigue_term(r, &self);
    pw_sincosf(ac->yaw, &fwd_x, &fwd_y);
    dot = (p[0] - a[0]) * fwd_x + (p[1] - a[1]) * fwd_y;
    obs *= dot >= 0.0f ? gmst_f(g, "fSneakViewMult", 1.5f) : gmst_f(g, "fSneakNoViewMult", 0.5f);
    chance = pcs - obs;
    return vm_random01(vm_of(g)) * 100.0f >= chance;
}

int ai_is_guard(Game *g, int actor)
{
    VmRef r = game_actor_ref(g, (uint32_t)actor);
    const DRpgObject *o = rpg_object(rpg_of(g), r.object);
    return o && o->type == DRPG_OT_NPC && o->link[1] == pw_hash_name("guard");
}

/* ------------------------------------------------------------------------ */
/* movement                                                                 */
/* ------------------------------------------------------------------------ */
static void plan(Game *g, int i, const float dest[3])
{
    AiActor *s = &g->ai.actors[i];
    float p[3];
    int n;
    memcpy(s->dest, dest, sizeof(s->dest));
    s->path_len = s->path_pos = 0;
    s->stuck_timer = 0.0f;
    actor_xyz(g, i, p);
    if (g->ai.pts && dist2(p, dest) > 512.0f * 512.0f) {
        n = ai_find_path(g, p, dest, s->path, AI_PATH_MAX);
        if (n > 0)
            s->path_len = (uint8_t)n;
    }
}

/* steps towards s->dest; returns 1 on arrival, -1 when stuck for good */
static int walk(Game *g, int i, float dt, int run, float arrive)
{
    AiActor *s = &g->ai.actors[i];
    Actor *a = &g->actors.list[i];
    Rpg *r = rpg_of(g);
    VmRef self = game_actor_ref(g, (uint32_t)i);
    float p[3], goal[3], dx, dy, len, speed;
    Vec4 wish, before = a->pos;
    actor_xyz(g, i, p);
    /* next waypoint */
    for (;;) {
        if (s->path_pos < s->path_len) {
            memcpy(goal, g->ai.pts[s->path[s->path_pos]], sizeof(goal));
            if (dist2(p, goal) < AI_ARRIVE * AI_ARRIVE ||
                (s->path_pos + 1 < s->path_len && dist2(p, s->dest) < dist2(goal, s->dest) * 0.5f)) {
                s->path_pos++;
                continue;
            }
        } else {
            memcpy(goal, s->dest, sizeof(goal));
            if (dist2(p, goal) < arrive * arrive) {
                s->moving = 0;
                return 1;
            }
        }
        break;
    }
    dx = goal[0] - p[0];
    dy = goal[1] - p[1];
    len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f)
        return 1;
    speed = run ? rpg_run_speed(r, &self) : rpg_walk_speed(r, &self);
    if (rpg_effect(r, &self, MGEF_PARALYZE, -1) > 0.0f)
        speed = 0.0f;
    wish = vec4(dx / len * speed, dy / len * speed, 0.0f, 0.0f);
    s->body.pos = a->pos;
    phys_move(&g->phys, &s->body, &wish, 0, 0.0f, dt);
    {
        Vec4 np = s->body.pos;
        float want = atan2f(dx, dy), diff = pw_wrap_angle(want - a->yaw), step = 6.0f * dt;
        diff = PW_CLAMP(diff, -step, step);
        actor_set_placement(a, &np, pw_wrap_angle(a->yaw + diff));
    }
    play_loop(a, run ? "runforward" : "walkforward");
    s->moving = 1;
    s->running = (uint8_t)run;
    /* footsteps every ~ 0.9 strides */
    {
        float mdx = a->pos.x - before.x, mdy = a->pos.y - before.y, moved = sqrtf(mdx * mdx + mdy * mdy);
        s->step_dist += moved;
        if (s->step_dist > (run ? 150.0f : 100.0f)) {
            s->step_dist = 0.0f;
            s->step_side ^= 1;
            play_gen(g, i, s->body.swimming ? DRPG_SNDG_SWIM_LEFT + s->step_side : DRPG_SNDG_LEFT + s->step_side,
                     s->step_side ? "FootBareRight" : "FootBareLeft");
        }
        if (moved < speed * dt * 0.2f) {
            s->stuck_timer += dt;
            if (s->stuck_timer > 1.0f) {
                s->stuck_timer = 0.0f;
                if (s->path_pos < s->path_len)
                    s->path_pos++;
                else
                    return -1;
            }
        } else {
            s->stuck_timer = PW_MAX(0.0f, s->stuck_timer - dt);
        }
    }
    return 0;
}

static void stand(Game *g, int i)
{
    AiActor *s = &g->ai.actors[i];
    Actor *a = &g->actors.list[i];
    s->moving = 0;
    if (a->group == pw_hash_name("walkforward") || a->group == pw_hash_name("runforward"))
        actor_play(a, pw_hash_name("idle"), 1);
}

/* ------------------------------------------------------------------------ */
/* packages                                                                 */
/* ------------------------------------------------------------------------ */
void ai_set_package(Game *g, int actor, const DRpgAiPackage *p)
{
    AiActor *s;
    if (actor < 0 || (uint32_t)actor >= g->actors.count || !g->ai.actors)
        return;
    s = &g->ai.actors[actor];
    s->pkg = *p;
    s->has_pkg = 1;
    s->pkg_done = 0;
    s->pkg_start = game_hours(g);
    s->greeted = 0;
    if (s->state != AI_COMBAT && s->state != AI_FLEE)
        s->state = AI_IDLE;
    s->timer = 0.0f;
    actor_xyz(g, actor, s->home);
}

int ai_package_done(Game *g, int actor)
{
    if (actor < 0 || (uint32_t)actor >= g->actors.count || !g->ai.actors)
        return 0;
    return g->ai.actors[actor].pkg_done;
}

int ai_current_package(Game *g, int actor)
{
    if (actor < 0 || (uint32_t)actor >= g->actors.count || !g->ai.actors)
        return -1;
    return g->ai.actors[actor].has_pkg ? g->ai.actors[actor].pkg.type : -1;
}

static int find_target_actor(Game *g, uint32_t object)
{
    uint32_t i;
    for (i = 0; i < g->actors.count; ++i)
        if (g->zone.instances[g->actors.list[i].instance].object_hash == object && !is_dead(g, (int)i))
            return (int)i;
    return -1;
}

static int target_pos(Game *g, uint32_t object, float out[3])
{
    int t;
    if (!object || object == pw_hash_name("player")) {
        player_xyz(g, out);
        return 1;
    }
    t = find_target_actor(g, object);
    if (t < 0) {
        uint32_t i;
        for (i = 0; i < g->zone.instance_count; ++i)
            if (g->zone.instances[i].object_hash == object && !g->zone.inst_hidden[i]) {
                memcpy(out, g->zone.instances[i].pos, sizeof(float[3]));
                return 1;
            }
        return 0;
    }
    actor_xyz(g, t, out);
    return 1;
}

static int pick_idle(Game *g, int i)
{
    AiActor *s = &g->ai.actors[i];
    uint32_t total = 0, k, roll;
    static const char *const groups[8] = { "idle2", "idle3", "idle4", "idle5", "idle6", "idle7", "idle8",
                                           "idle9" };
    for (k = 0; k < 8; ++k)
        total += s->pkg.idle[k];
    if (!total)
        return 0;
    roll = vm_random(vm_of(g)) % (total + 100u);
    for (k = 0; k < 8; ++k) {
        if (roll < s->pkg.idle[k]) {
            actor_play(&g->actors.list[i], pw_hash_name(groups[k]), 0);
            return 1;
        }
        roll -= s->pkg.idle[k];
    }
    return 0;
}

static void wander(Game *g, int i, float dt)
{
    AiActor *s = &g->ai.actors[i];
    Actor *a = &g->actors.list[i];
    float p[3], pc[3];
    actor_xyz(g, i, p);
    player_xyz(g, pc);
    /* greet the player when he comes close (NPCs): stop and face him */
    if (!is_creature(g, i)) {
        VmRef self = game_actor_ref(g, (uint32_t)i);
        float greet = vm_stat(vm_of(g), &self, MWP_AI_HELLO) * gmst_f(g, "iGreetDistanceMultiplier", 6.0f);
        if (greet > 0.0f && dist2(p, pc) < greet * greet && s->sees_player) {
            stand(g, i);
            face(g, i, pc, dt, 2.5f);
            s->state = AI_IDLE;
            s->timer = PW_MAX(s->timer, 1.0f);
            return;
        }
    }
    if (s->state == AI_WALK) {
        int rc = walk(g, i, dt, 0, AI_ARRIVE);
        if (rc != 0) {
            stand(g, i);
            s->state = AI_IDLE;
            s->timer = 2.0f + 6.0f * vm_random01(vm_of(g));
        }
        return;
    }
    s->timer -= dt;
    if (s->timer > 0.0f)
        return;
    s->timer = 3.0f + 5.0f * vm_random01(vm_of(g));
    if (s->pkg.distance > 0 && vm_random01(vm_of(g)) < 0.6f) {
        float dest[3];
        float rad = (float)s->pkg.distance;
        int found = 0, k;
        if (g->ai.pts) {
            for (k = 0; k < 8 && !found; ++k) {
                uint32_t q = vm_random(vm_of(g)) % g->ai.count;
                if (dist2(g->ai.pts[q], s->home) <= rad * rad) {
                    memcpy(dest, g->ai.pts[q], sizeof(dest));
                    found = 1;
                }
            }
        }
        if (!found) {
            float ang = 6.2831853f * vm_random01(vm_of(g)), d = rad * vm_random01(vm_of(g)), ground;
            dest[0] = s->home[0] + sinf(ang) * d;
            dest[1] = s->home[1] + cosf(ang) * d;
            dest[2] = s->home[2];
            if (phys_ground(&g->phys, dest[0], dest[1], dest[2] + 256.0f, &ground) &&
                (!g->phys.has_water || ground > g->phys.water_z - 20.0f))
                found = 1;
            dest[2] = ground;
        }
        if (found) {
            plan(g, i, dest);
            s->state = AI_WALK;
            return;
        }
    }
    if (!pick_idle(g, i) && a->group != pw_hash_name("idle"))
        actor_play(a, pw_hash_name("idle"), 1);
    /* creatures moan now and then */
    if (is_creature(g, i) && vm_random01(vm_of(g)) < 0.15f)
        play_gen(g, i, DRPG_SNDG_MOAN, NULL);
}

static void run_package(Game *g, int i, float dt)
{
    AiActor *s = &g->ai.actors[i];
    float p[3], t[3];
    actor_xyz(g, i, p);
    /* duration (game hours) */
    if (s->pkg.duration && !s->pkg_done && game_hours(g) - s->pkg_start >= (double)s->pkg.duration &&
        (s->pkg.type == DRPG_AI_FOLLOW || s->pkg.type == DRPG_AI_ESCORT || s->pkg.type == DRPG_AI_WANDER))
        s->pkg_done = 1;
    switch (s->pkg.type) {
    case DRPG_AI_TRAVEL:
    case DRPG_AI_ESCORT:
        if (s->pkg_done) {
            stand(g, i);
            return;
        }
        if (s->pkg.type == DRPG_AI_ESCORT && target_pos(g, s->pkg.target, t) &&
            dist2(p, t) > 600.0f * 600.0f) {
            /* wait for the escorted one */
            stand(g, i);
            face(g, i, t, dt, 2.5f);
            return;
        }
        if (s->state != AI_WALK || dist2(s->dest, s->pkg.pos) > 1.0f) {
            plan(g, i, s->pkg.pos);
            s->state = AI_WALK;
        }
        if (walk(g, i, dt, 0, AI_ARRIVE) != 0) {
            stand(g, i);
            s->state = AI_IDLE;
            s->pkg_done = 1;
        }
        return;
    case DRPG_AI_FOLLOW:
        if (s->pkg_done || !target_pos(g, s->pkg.target, t)) {
            wander(g, i, dt);
            return;
        }
        if (dist2(p, t) > 300.0f * 300.0f) {
            if (s->state != AI_WALK || dist2(s->dest, t) > 200.0f * 200.0f)
                plan(g, i, t);
            s->state = AI_WALK;
            if (walk(g, i, dt, dist2(p, t) > 800.0f * 800.0f, 150.0f) != 0)
                s->state = AI_IDLE;
        } else {
            stand(g, i);
            s->state = AI_IDLE;
            face(g, i, t, dt, 2.5f);
        }
        return;
    case DRPG_AI_ACTIVATE:
        if (s->pkg_done)
            return;
        if (!target_pos(g, s->pkg.target, t)) {
            s->pkg_done = 1;
            return;
        }
        if (s->state != AI_WALK) {
            plan(g, i, t);
            s->state = AI_WALK;
        }
        if (walk(g, i, dt, 0, 120.0f) != 0) {
            uint32_t k;
            stand(g, i);
            s->state = AI_IDLE;
            s->pkg_done = 1;
            for (k = 0; k < g->zone.instance_count; ++k)
                if (g->zone.instances[k].object_hash == s->pkg.target && !g->zone.inst_hidden[k]) {
                    VmRef tr = game_instance_ref(g, k);
                    vm_activate(vm_of(g), &tr);
                    break;
                }
        }
        return;
    default:
        wander(g, i, dt);
        return;
    }
}

/* ------------------------------------------------------------------------ */
/* combat                                                                   */
/* ------------------------------------------------------------------------ */
void ai_start_combat(Game *g, int actor, int target)
{
    AiActor *s;
    VmRef r;
    if (actor < 0 || (uint32_t)actor >= g->actors.count || !g->ai.actors || is_dead(g, actor))
        return;
    s = &g->ai.actors[actor];
    if (s->state == AI_COMBAT && s->target == target)
        return;
    r = game_actor_ref(g, (uint32_t)actor);
    if (target == AI_TARGET_PLAYER) {
        if (s->ally)
            return;
        vm_set_prop(vm_of(g), &r, MWP_HOSTILE, 1.0f);
    }
    s->state = AI_COMBAT;
    s->target = target;
    s->attack_timer = 0.5f;
    s->spell_timer = 1.0f;
    s->lost_timer = 0.0f;
    s->path_len = s->path_pos = 0;
    play_gen(g, actor, DRPG_SNDG_ROAR, NULL);
}

void ai_stop_combat(Game *g, int actor)
{
    AiActor *s;
    VmRef r;
    if (actor < 0 || (uint32_t)actor >= g->actors.count || !g->ai.actors)
        return;
    s = &g->ai.actors[actor];
    r = game_actor_ref(g, (uint32_t)actor);
    vm_set_prop(vm_of(g), &r, MWP_HOSTILE, 0.0f);
    s->state = AI_IDLE;
    s->target = AI_TARGET_NONE;
    s->timer = 2.0f;
    stand(g, actor);
}

void ai_attacked(Game *g, int actor, int attacker)
{
    AiActor *s;
    if (actor < 0 || (uint32_t)actor >= g->actors.count || !g->ai.actors || is_dead(g, actor))
        return;
    s = &g->ai.actors[actor];
    play_gen(g, actor, DRPG_SNDG_SCREAM, "Health Damage");
    if (s->state == AI_FLEE)
        return;
    if (attacker == AI_TARGET_PLAYER && s->ally) {
        /* summons forgive their master */
        return;
    }
    ai_start_combat(g, actor, attacker);
}

static int target_xyz(Game *g, int target, float out[3], VmRef *ref)
{
    if (target == AI_TARGET_PLAYER) {
        if (g->player_dead)
            return 0;
        player_xyz(g, out);
        *ref = vm_player_ref(vm_of(g));
        return 1;
    }
    if (target < 0 || (uint32_t)target >= g->actors.count || is_dead(g, target))
        return 0;
    actor_xyz(g, target, out);
    *ref = game_actor_ref(g, (uint32_t)target);
    return 1;
}

/* the spell this actor would cast: offence (target range) or a self heal */
static const DRpgSpell *choose_spell(Game *g, const VmRef *self, int heal)
{
    Rpg *r = rpg_of(g);
    uint32_t cursor = 0, item;
    int32_t count;
    const DRpgSpell *best = NULL;
    float magicka = rpg_dyn(r, self, 1);
    int best_cost = -1;
    while (vm_item_next(vm_of(g), self->key ^ VM_SPELL_SALT, &cursor, &item, &count) >= 0) {
        const DRpgSpell *sp = rpg_spell(r, item);
        int n, k, ok = 0, cost;
        const DRpgEffect *e;
        if (!sp || sp->kind != 0 || sp->type != 0)
            continue;
        e = rpg_spell_effects(r, sp, &n);
        for (k = 0; k < n; ++k) {
            const DRpgMgef *m = rpgdb_mgef(r->db, e[k].effect);
            if (heal ? (e[k].effect == MGEF_RESTORE_HEALTH && e[k].range == RPG_RANGE_SELF)
                     : (e[k].range == RPG_RANGE_TARGET && m && (m->flags & DRPG_MF_HARMFUL)))
                ok = 1;
        }
        cost = rpg_spell_cost(r, sp);
        if (ok && (float)cost <= magicka && cost > best_cost) {
            best = sp;
            best_cost = cost;
        }
    }
    return best;
}

static void cast_at(Game *g, int i, const DRpgSpell *sp, const VmRef *self, const VmRef *tgt, int range)
{
    Rpg *r = rpg_of(g);
    int n;
    const DRpgEffect *e = rpg_spell_effects(r, sp, &n);
    float chance = rpg_spell_chance(r, self, sp);
    const DRpgMgef *m = n > 0 ? rpgdb_mgef(r->db, e[0].effect) : NULL;
    rpg_dyn_add(r, self, 1, -(float)rpg_spell_cost(r, sp));
    actor_play(&g->actors.list[i], pw_hash_name("spellcast"), 0);
    if (m)
        play_sound(g, i, m->sound[0], AUDIO_CAT_EFFECTS);
    if (vm_random01(vm_of(g)) * 100.0f >= chance)
        return;
    rpg_apply(r, self, tgt, e, n, range, sp->hash, chance, 1);
    if (tgt->kind == VM_REF_PLAYER && range == RPG_RANGE_TARGET) {
        char buf[80];
        snprintf(buf, sizeof(buf), "%s casts %s at you.", game_instance_name(g, g->actors.list[i].instance),
                 rpg_spell_name(r, sp->hash));
        game_message(g, buf);
    }
}

static void strike(Game *g, int i, const VmRef *self, const VmRef *tgt, int target)
{
    Rpg *r = rpg_of(g);
    float strength = 0.5f + 0.5f * vm_random01(vm_of(g));
    RpgHit h;
    actor_play(&g->actors.list[i], pw_hash_name(vm_random(vm_of(g)) & 1 ? "attack1" : "attack2"), 0);
    play_sound(g, i, pw_hash_name("Weapon Swish"), AUDIO_CAT_EFFECTS);
    h = rpg_attack(r, self, tgt, strength);
    if (h.hit && h.damage > 0.0f) {
        if (target == AI_TARGET_PLAYER) {
            char buf[80];
            snprintf(buf, sizeof(buf), "%s hits you for %d.", game_instance_name(g, g->actors.list[i].instance),
                     (int)(h.damage + 0.5f));
            game_message(g, buf);
            if (g->audio) {
                AudioPlayOpts o;
                memset(&o, 0, sizeof(o));
                audio_play_sound(g->audio, pw_hash_name("Health Damage"), &o);
            }
        } else {
            ai_attacked(g, target, i);
            if (!h.killed)
                actor_play(&g->actors.list[target], pw_hash_name("hit1"), 0);
        }
    }
}

static int nearest_enemy_of_player(Game *g, int self)
{
    uint32_t k;
    int best = -1;
    float bd = 3000.0f * 3000.0f, p[3], q[3];
    actor_xyz(g, self, p);
    for (k = 0; k < g->actors.count; ++k) {
        AiActor *o = &g->ai.actors[k];
        if ((int)k == self || is_dead(g, (int)k) || o->ally)
            continue;
        if (o->state != AI_COMBAT || o->target != AI_TARGET_PLAYER)
            continue;
        actor_xyz(g, (int)k, q);
        if (dist2(p, q) < bd) {
            bd = dist2(p, q);
            best = (int)k;
        }
    }
    return best;
}

static void combat(Game *g, int i, float dt, int thinking)
{
    AiActor *s = &g->ai.actors[i];
    Rpg *r = rpg_of(g);
    VmRef self = game_actor_ref(g, (uint32_t)i), tgt;
    float p[3], t[3], d, reach;
    actor_xyz(g, i, p);
    if (!target_xyz(g, s->target, t, &tgt)) {
        if (s->target == AI_TARGET_PLAYER)
            vm_set_prop(vm_of(g), &self, MWP_HOSTILE, g->player_dead ? 1.0f : 0.0f);
        s->state = AI_IDLE;
        s->target = AI_TARGET_NONE;
        stand(g, i);
        return;
    }
    d = sqrtf(dist2(p, t));
    reach = rpg_weapon_reach(r, &self) + AI_BODY_RADIUS * 2.0f;
    if (rpg_effect(r, &self, MGEF_PARALYZE, -1) > 0.0f)
        return;
    /* give up on a target that stays far away */
    if (d > AI_ACTIVE_DIST) {
        s->lost_timer += dt;
        if (s->lost_timer > 10.0f) {
            ai_stop_combat(g, i);
            return;
        }
    } else {
        s->lost_timer = 0.0f;
    }
    if (thinking) {
        float hp = rpg_dyn(r, &self, 0) / PW_MAX(1.0f, rpg_dyn_max(r, &self, 0));
        /* flee when badly hurt and timid */
        if (!s->fled && hp < 0.25f && vm_stat(vm_of(g), &self, MWP_AI_FLEE) >= 50.0f) {
            s->state = AI_FLEE;
            s->fled = 1;
            s->timer = 6.0f;
            return;
        }
        /* heal or cast from range */
        if (s->spell_timer <= 0.0f && !is_creature(g, i)) {
            const DRpgSpell *sp = hp < 0.4f ? choose_spell(g, &self, 1) : NULL;
            if (sp) {
                cast_at(g, i, sp, &self, &self, RPG_RANGE_SELF);
                s->spell_timer = 3.0f;
            } else if (d > reach * 1.5f && d < 2000.0f && clear_line(g, p, t)) {
                sp = choose_spell(g, &self, 0);
                if (sp) {
                    cast_at(g, i, sp, &self, &tgt, RPG_RANGE_TARGET);
                    s->spell_timer = 2.5f + vm_random01(vm_of(g));
                }
            }
        }
    }
    s->spell_timer -= dt;
    if (d > reach * 0.9f) {
        if (s->path_len == 0 || dist2(s->dest, t) > 300.0f * 300.0f) {
            plan(g, i, t);
            if (clear_line(g, p, t))
                s->path_len = 0; /* straight at it */
        }
        memcpy(s->dest, t, sizeof(s->dest));
        walk(g, i, dt, 1, reach * 0.8f);
    } else {
        stand(g, i);
        face(g, i, t, dt, 6.0f);
    }
    s->attack_timer -= dt;
    if (d <= reach && s->attack_timer <= 0.0f) {
        strike(g, i, &self, &tgt, s->target);
        s->attack_timer = 1.2f + 0.8f * vm_random01(vm_of(g));
    }
}

static void flee(Game *g, int i, float dt)
{
    AiActor *s = &g->ai.actors[i];
    float p[3], t[3], dx, dy, len;
    VmRef tgt;
    actor_xyz(g, i, p);
    s->timer -= dt;
    if (s->timer <= 0.0f || !target_xyz(g, s->target, t, &tgt)) {
        s->state = s->target != AI_TARGET_NONE ? AI_COMBAT : AI_IDLE;
        return;
    }
    dx = p[0] - t[0];
    dy = p[1] - t[1];
    len = PW_MAX(1.0f, sqrtf(dx * dx + dy * dy));
    s->dest[0] = p[0] + dx / len * 400.0f;
    s->dest[1] = p[1] + dy / len * 400.0f;
    s->dest[2] = p[2];
    s->path_len = s->path_pos = 0;
    walk(g, i, dt, 1, 32.0f);
}

/* guards walk up to a criminal and talk */
static void pursue(Game *g, int i, float dt)
{
    AiActor *s = &g->ai.actors[i];
    VmRef pc = vm_player_ref(vm_of(g));
    float p[3], t[3];
    if (vm_prop(vm_of(g), &pc, MWP_CRIME) < 1.0f) {
        s->state = AI_IDLE;
        return;
    }
    actor_xyz(g, i, p);
    player_xyz(g, t);
    if (dist2(p, t) > 150.0f * 150.0f) {
        if (s->path_len == 0 || dist2(s->dest, t) > 300.0f * 300.0f)
            plan(g, i, t);
        memcpy(s->dest, t, sizeof(s->dest));
        walk(g, i, dt, 1, 120.0f);
        return;
    }
    stand(g, i);
    face(g, i, t, dt, 6.0f);
    s->greet_timer -= dt;
    if (s->greet_timer <= 0.0f && !g->menu_mode && g->ui_event == GAME_UI_NONE) {
        s->greet_timer = 20.0f;
        game_talk(g, g->actors.list[i].instance);
    }
}

/* ------------------------------------------------------------------------ */
/* frame                                                                    */
/* ------------------------------------------------------------------------ */
static void think_hostility(Game *g, int i)
{
    AiActor *s = &g->ai.actors[i];
    VmRef self = game_actor_ref(g, (uint32_t)i);
    float p[3], t[3], d, fight;
    if (s->ally) {
        int e = nearest_enemy_of_player(g, i);
        if (e >= 0)
            ai_start_combat(g, i, e);
        return;
    }
    if (vm_prop(vm_of(g), &self, MWP_HOSTILE) != 0.0f) {
        if (!g->player_dead)
            ai_start_combat(g, i, AI_TARGET_PLAYER);
        return;
    }
    if (g->player_dead)
        return;
    actor_xyz(g, i, p);
    player_xyz(g, t);
    d = sqrtf(dist2(p, t));
    fight = vm_stat(vm_of(g), &self, MWP_AI_FIGHT);
    if (!is_creature(g, i))
        fight += (50.0f - (float)rpg_disposition(rpg_of(g), &self)) * gmst_f(g, "fFightDispMult", 0.2f);
    fight += gmst_f(g, "iFightDistanceBase", 10.0f) - gmst_f(g, "fFightDistanceMultiplier", 0.005f) * d;
    if (fight >= 100.0f && s->sees_player)
        ai_start_combat(g, i, AI_TARGET_PLAYER);
}

void ai_update(Game *g, float dt)
{
    AiState *ai = &g->ai;
    uint32_t i;
    float pc[3];
    if (!g->rpg || !ai->actors)
        return;
    player_xyz(g, pc);
    ai->clock += dt;
    ai->combat = 0;
    for (i = 0; i < g->actors.count && i < ai->capacity; ++i) {
        AiActor *s = &ai->actors[i];
        float p[3];
        int thinking;
        if (s->state == AI_GONE)
            continue;
        if (is_dead(g, (int)i)) {
            if (s->life >= 0.0f && (s->life -= dt) <= -5.0f)
                ai_despawn(g, (int)i);
            continue;
        }
        if (s->life >= 0.0f) {
            s->life -= dt;
            if (s->life <= 0.0f) {
                ai_despawn(g, (int)i);
                continue;
            }
        }
        actor_xyz(g, (int)i, p);
        if (dist2(p, pc) > AI_ACTIVE_DIST * AI_ACTIVE_DIST && s->state != AI_COMBAT && !s->ally)
            continue; /* frozen far away */
        s->think -= dt;
        thinking = s->think <= 0.0f;
        if (thinking) {
            s->think += AI_THINK_PERIOD;
            s->sees_player = (uint8_t)(g->player_dead ? 0 : ai_detects_player(g, (int)i));
            if (s->state != AI_COMBAT && s->state != AI_FLEE && s->state != AI_PURSUE)
                think_hostility(g, (int)i);
        }
        switch (s->state) {
        case AI_COMBAT:
            combat(g, (int)i, dt, thinking);
            break;
        case AI_FLEE:
            flee(g, (int)i, dt);
            break;
        case AI_PURSUE:
            pursue(g, (int)i, dt);
            break;
        default:
            if (s->ally) {
                DRpgAiPackage follow;
                memset(&follow, 0, sizeof(follow));
                follow.type = DRPG_AI_FOLLOW;
                follow.target = pw_hash_name("player");
                s->pkg = follow;
            }
            run_package(g, (int)i, dt);
            break;
        }
        if (s->state == AI_COMBAT && s->target == AI_TARGET_PLAYER)
            ai->combat++;
    }
}

/* ------------------------------------------------------------------------ */
/* crime                                                                    */
/* ------------------------------------------------------------------------ */
void ai_mark_stolen(Game *g, uint32_t item, int32_t count)
{
    AiState *ai = &g->ai;
    uint32_t k;
    for (k = 0; k < ai->stolen_used; ++k)
        if (ai->stolen_item[k] == item) {
            ai->stolen_count[k] += count;
            return;
        }
    if (ai->stolen_used < AI_STOLEN_MAX) {
        ai->stolen_item[ai->stolen_used] = item;
        ai->stolen_count[ai->stolen_used++] = count;
    }
}

int ai_crime(Game *g, int type, int victim, uint32_t owner, int value)
{
    Rpg *r = rpg_of(g);
    VmRef pc = vm_player_ref(vm_of(g));
    uint32_t i;
    int witnessed = 0, bounty;
    float pcp[3];
    (void)owner;
    switch (type) {
    case AI_CRIME_ASSAULT:
        bounty = rpg_gmst_i(r, "iCrimeAttack", 40);
        break;
    case AI_CRIME_MURDER:
        bounty = rpg_gmst_i(r, "iCrimeKilling", 1000);
        break;
    case AI_CRIME_THEFT:
        bounty = PW_MAX(1, (int)((float)value * gmst_f(g, "fCrimeStealing", 1.0f)));
        break;
    case AI_CRIME_PICKPOCKET:
        bounty = rpg_gmst_i(r, "iCrimePickPocket", 25);
        break;
    default:
        bounty = rpg_gmst_i(r, "iCrimeTresspass", 5);
        break;
    }
    player_xyz(g, pcp);
    for (i = 0; i < g->actors.count; ++i) {
        float p[3];
        int sees;
        if (is_dead(g, (int)i) || is_creature(g, (int)i) || g->ai.actors[i].ally)
            continue;
        actor_xyz(g, (int)i, p);
        if (dist2(p, pcp) > 2000.0f * 2000.0f)
            continue;
        sees = ((int)i == victim) || ai_detects_player(g, (int)i);
        if (!sees)
            continue;
        witnessed = 1;
        if (ai_is_guard(g, (int)i) && g->ai.actors[i].state != AI_COMBAT) {
            g->ai.actors[i].state = AI_PURSUE;
            g->ai.actors[i].greet_timer = 0.0f;
            g->ai.actors[i].path_len = 0;
        }
    }
    if (victim >= 0 && (type == AI_CRIME_ASSAULT || type == AI_CRIME_MURDER))
        ai_start_combat(g, victim, AI_TARGET_PLAYER);
    if (!witnessed)
        return 0;
    vm_set_prop(vm_of(g), &pc, MWP_CRIME, vm_prop(vm_of(g), &pc, MWP_CRIME) + (float)bounty);
    g->ai.crimes_seen++;
    vm_notify(vm_of(g), rpgdb_gmst_string(r->db, "sCrimeWitnessed", "Your crime has been witnessed."));
    return 1;
}

static void confiscate(Game *g)
{
    AiState *ai = &g->ai;
    Vm *vm = vm_of(g);
    uint32_t k;
    for (k = 0; k < ai->stolen_used; ++k) {
        int32_t have = vm_item_count(vm, vm->player_hash, ai->stolen_item[k]);
        if (have > 0)
            vm_item_add(vm, vm->player_hash, ai->stolen_item[k], -PW_MIN(have, ai->stolen_count[k]));
    }
    ai->stolen_used = 0;
}

static void clear_pursuit(Game *g)
{
    uint32_t i;
    for (i = 0; i < g->actors.count && g->ai.actors; ++i)
        if (g->ai.actors[i].state == AI_PURSUE)
            g->ai.actors[i].state = AI_IDLE;
}

void ai_pay_fine(Game *g, int thief)
{
    Vm *vm = vm_of(g);
    VmRef pc = vm_player_ref(vm);
    int bounty = (int)vm_prop(vm, &pc, MWP_CRIME);
    int gold = rpg_gold(rpg_of(g), &pc);
    vm_item_add(vm, vm->player_hash, pw_hash_name(RPG_GOLD), -PW_MIN(gold, bounty));
    vm_set_prop(vm, &pc, MWP_CRIME, 0.0f);
    if (thief)
        confiscate(g);
    clear_pursuit(g);
}

void ai_go_to_jail(Game *g)
{
    Rpg *r = rpg_of(g);
    Vm *vm = vm_of(g);
    VmRef pc = vm_player_ref(vm);
    int bounty = (int)vm_prop(vm, &pc, MWP_CRIME), days = PW_CLAMP(bounty / 100, 1, 30), d;
    char msg[96];
    const DRpgMarker *best = NULL;
    uint32_t k;
    for (d = 0; d < days; ++d) {
        int skill = (int)(vm_random(vm) % SKILL_COUNT);
        float v = vm_prop(vm, &pc, MWP_SKILL_BLOCK + skill);
        if (v > 1.0f)
            vm_set_prop(vm, &pc, MWP_SKILL_BLOCK + skill, v - 1.0f);
    }
    vm_advance_hours(vm, (float)days * 24.0f);
    vm_set_prop(vm, &pc, MWP_CRIME, 0.0f);
    confiscate(g);
    clear_pursuit(g);
    for (k = 0; k < r->db->hdr->markers.count; ++k)
        if (r->db->markers[k].kind == DRPG_MARKER_PRISON) {
            best = &r->db->markers[k];
            break;
        }
    if (best && r->host.teleport)
        r->host.teleport(r->host.user, best->cell, best->pos, best->rot_z);
    snprintf(msg, sizeof(msg), "You spent %d day%s in jail.", days, days == 1 ? "" : "s");
    vm_notify(vm, msg);
}

/* ------------------------------------------------------------------------ */
/* spawning                                                                 */
/* ------------------------------------------------------------------------ */
int ai_spawn(Game *g, uint32_t object, const float pos[3], float yaw, int ally, float life)
{
    Rpg *r = rpg_of(g);
    const DRpgObject *o = rpg_object(r, object);
    uint32_t rec, skel;
    int inst, idx;
    float p[3];
    VmRef ref;
    if (!o || o->type != DRPG_OT_CREA || !g->ai.actors)
        return -1;
    skel = (o->link[0] && pak_find(&g->world->meshes, o->link[0])) ? o->link[0] : o->model;
    memcpy(p, pos, sizeof(p));
    {
        float ground;
        if (phys_ground(&g->phys, p[0], p[1], p[2] + 128.0f, &ground) && ground > p[2] - 512.0f)
            p[2] = ground;
    }
    inst = zone_spawn_creature(g->world, &g->zone, object, skel, o->link[0], p, yaw, 1.0f, &rec);
    if (inst < 0)
        return -1;
    /* a game-wide serial keeps spawned references apart across zone visits */
    g->zone.inst_rw[inst].refnum = ZONE_SPAWN_REFNUM | (++g->ai.spawn_serial & 0xFFFFFFu);
    idx = actor_spawn(&g->actors, &g->zone, rec);
    if (idx < 0 || (uint32_t)idx >= g->ai.capacity) {
        g->zone.inst_hidden[inst] = 1;
        return -1;
    }
    ref = game_instance_ref(g, (uint32_t)inst);
    g->inst_vmi[inst] = o->script != DRPG_NONE16 ? (int16_t)vm_bind_reference(vm_of(g), ref.key, object, inst)
                                                  : (int16_t)-1;
    if (g->actor_timer)
        g->actor_timer[idx] = 1.0f;
    init_actor(g, idx);
    g->ai.actors[idx].ally = (uint8_t)ally;
    g->ai.actors[idx].life = life;
    g->ai.spawned++;
    return idx;
}

void ai_despawn(Game *g, int actor)
{
    AiActor *s;
    if (actor < 0 || (uint32_t)actor >= g->actors.count || !g->ai.actors)
        return;
    s = &g->ai.actors[actor];
    s->state = AI_GONE;
    g->zone.inst_hidden[g->actors.list[actor].instance] = 1;
}
