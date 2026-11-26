/*
 * phys.c - zone collision and character movement (see phys.h).
 */
#include "phys.h"

#include <math.h>
#include <string.h>

#include "mem.h"

#define CELL_SIZE 256.0f
#define MAX_GRID_CELLS 65536
#define MAX_POLY 8
#define WALL_ITERATIONS 4
#define TERRAIN_SPACING (8192.0f / (DZONE_TERRAIN_GRID - 1))

/* ------------------------------------------------------------------------ */
/* geometry helpers                                                         */
/* ------------------------------------------------------------------------ */
static void tri_normal(float v[3][3], float n[3])
{
    float e1[3], e2[3], len;
    int k;
    for (k = 0; k < 3; ++k) {
        e1[k] = v[1][k] - v[0][k];
        e2[k] = v[2][k] - v[0][k];
    }
    n[0] = e1[1] * e2[2] - e1[2] * e2[1];
    n[1] = e1[2] * e2[0] - e1[0] * e2[2];
    n[2] = e1[0] * e2[1] - e1[1] * e2[0];
    len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 1e-12f) {
        n[0] /= len;
        n[1] /= len;
        n[2] /= len;
    } else {
        n[0] = n[1] = 0.0f;
        n[2] = 1.0f;
    }
}

/* Sutherland-Hodgman against z >= lo (sign = 1) or z <= hi (sign = -1). */
static int clip_z(float in[][3], int n, float plane, float sign, float out[][3])
{
    int i, m = 0;
    for (i = 0; i < n; ++i) {
        const float *cur = in[i], *prev = in[(i + n - 1) % n];
        float dc = (cur[2] - plane) * sign, dp = (prev[2] - plane) * sign;
        if ((dc >= 0.0f) != (dp >= 0.0f)) {
            float t = dp / (dp - dc);
            out[m][0] = prev[0] + (cur[0] - prev[0]) * t;
            out[m][1] = prev[1] + (cur[1] - prev[1]) * t;
            out[m][2] = prev[2] + (cur[2] - prev[2]) * t;
            m++;
        }
        if (dc >= 0.0f) {
            memcpy(out[m], cur, sizeof(float) * 3);
            m++;
        }
    }
    return m;
}

static float seg_closest(float px, float py, const float *a, const float *b, float *qx, float *qy)
{
    float dx = b[0] - a[0], dy = b[1] - a[1];
    float len2 = dx * dx + dy * dy, t = 0.0f;
    if (len2 > 1e-12f) {
        t = ((px - a[0]) * dx + (py - a[1]) * dy) / len2;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }
    *qx = a[0] + dx * t;
    *qy = a[1] + dy * t;
    return (px - *qx) * (px - *qx) + (py - *qy) * (py - *qy);
}

int phys_cylinder_triangle(float cx, float cy, float r, float z0, float z1, float v[3][3],
                           const float n[3], float *push_x, float *push_y)
{
    float a[MAX_POLY][3], b[MAX_POLY][3];
    float area = 0.0f, best = 1e30f, qx = cx, qy = cy, nl;
    int count, i, inside = 1, sign = 0;

    memcpy(a, v, sizeof(float) * 9);
    count = clip_z(a, 3, z0, 1.0f, b);
    if (count == 0)
        return 0;
    count = clip_z(b, count, z1, -1.0f, a);
    if (count == 0)
        return 0;

    for (i = 0; i < count; ++i) {
        const float *p0 = a[i], *p1 = a[(i + 1) % count];
        float x, y, d = seg_closest(cx, cy, p0, p1, &x, &y);
        float cr = (p1[0] - p0[0]) * (cy - p0[1]) - (p1[1] - p0[1]) * (cx - p0[0]);
        area += p0[0] * p1[1] - p1[0] * p0[1];
        if (d < best) {
            best = d;
            qx = x;
            qy = y;
        }
        if (cr > 1e-6f) {
            if (sign < 0)
                inside = 0;
            sign = 1;
        } else if (cr < -1e-6f) {
            if (sign > 0)
                inside = 0;
            sign = -1;
        }
    }
    if (fabsf(area) < 1e-3f)
        inside = 0; /* edge-on (vertical wall): use the closest point */

    if (!inside) {
        float d = sqrtf(best);
        if (d >= r)
            return 0;
        if (d > 1e-4f) {
            *push_x = (cx - qx) / d * (r - d);
            *push_y = (cy - qy) / d * (r - d);
        } else {
            nl = sqrtf(n[0] * n[0] + n[1] * n[1]);
            if (nl < 1e-4f)
                return 0;
            *push_x = n[0] / nl * r;
            *push_y = n[1] / nl * r;
        }
        return 1;
    }
    /* axis inside the footprint (slopes, ledges): leave along the face
     * normal when it has a horizontal component, else via the nearest edge */
    nl = sqrtf(n[0] * n[0] + n[1] * n[1]);
    if (nl > 0.1f) {
        float nx = n[0] / nl, ny = n[1] / nl, far = -1e30f;
        for (i = 0; i < count; ++i) {
            float d = (a[i][0] - cx) * nx + (a[i][1] - cy) * ny;
            if (d > far)
                far = d;
        }
        *push_x = nx * (far + r);
        *push_y = ny * (far + r);
    } else {
        float d = sqrtf(best);
        if (d < 1e-4f)
            return 0;
        *push_x = (qx - cx) / d * (d + r);
        *push_y = (qy - cy) / d * (d + r);
    }
    return 1;
}

static float tri_height_at(float v[3][3], const float n[3], float x, float y, int *inside)
{
    float d0 = (v[1][0] - v[0][0]) * (y - v[0][1]) - (v[1][1] - v[0][1]) * (x - v[0][0]);
    float d1 = (v[2][0] - v[1][0]) * (y - v[1][1]) - (v[2][1] - v[1][1]) * (x - v[1][0]);
    float d2 = (v[0][0] - v[2][0]) * (y - v[2][1]) - (v[0][1] - v[2][1]) * (x - v[2][0]);
    *inside = (d0 >= 0.0f && d1 >= 0.0f && d2 >= 0.0f) || (d0 <= 0.0f && d1 <= 0.0f && d2 <= 0.0f);
    if (fabsf(n[2]) < 1e-6f) {
        *inside = 0;
        return 0.0f;
    }
    return v[0][2] - (n[0] * (x - v[0][0]) + n[1] * (y - v[0][1])) / n[2];
}

/* ------------------------------------------------------------------------ */
/* build                                                                    */
/* ------------------------------------------------------------------------ */
static const DMeshCollision *instance_collision(const Zone *z, uint32_t i, const ZoneMesh **mesh)
{
    const DZoneInstance *in = &z->instances[i];
    const ZoneMesh *m;
    if (in->mesh == DZONE_NONE || (in->flags & DZONE_IF_ACTOR))
        return NULL;
    m = &z->meshes[in->mesh];
    if (!m->hdr || !m->collision || m->collision->tri_count == 0)
        return NULL;
    *mesh = m;
    return m->collision;
}

static void collision_tri(const ZoneMesh *m, const DMeshCollision *c, uint32_t t, float v[3][3])
{
    const float *verts = (const float *)(m->base + c->vertex_ofs);
    int k;
    for (k = 0; k < 3; ++k) {
        uint32_t idx = (c->flags & DMESH_COLL_F_INDEX32)
                           ? ((const uint32_t *)(m->base + c->index_ofs))[t * 3 + (uint32_t)k]
                           : ((const uint16_t *)(m->base + c->index_ofs))[t * 3 + (uint32_t)k];
        if (idx >= c->vertex_count)
            idx = 0;
        v[k][0] = verts[idx * 3 + 0];
        v[k][1] = verts[idx * 3 + 1];
        v[k][2] = verts[idx * 3 + 2];
    }
}

static void fill_tri(PhysTri *t, float v[3][3], uint32_t inst)
{
    int k;
    memcpy(t->v, v, sizeof(t->v));
    tri_normal(v, t->n);
    t->zmin = t->xmin = t->ymin = 1e30f;
    t->zmax = t->xmax = t->ymax = -1e30f;
    for (k = 0; k < 3; ++k) {
        t->xmin = fminf(t->xmin, v[k][0]);
        t->xmax = fmaxf(t->xmax, v[k][0]);
        t->ymin = fminf(t->ymin, v[k][1]);
        t->ymax = fmaxf(t->ymax, v[k][1]);
        t->zmin = fminf(t->zmin, v[k][2]);
        t->zmax = fmaxf(t->zmax, v[k][2]);
    }
    t->inst = inst;
}

static int cell_range(const PhysWorld *pw, float xmin, float xmax, float ymin, float ymax,
                      int *cx0, int *cx1, int *cy0, int *cy1)
{
    float cs = CELL_SIZE;
    *cx0 = (int)floorf((xmin - pw->x0) / cs);
    *cx1 = (int)floorf((xmax - pw->x0) / cs);
    *cy0 = (int)floorf((ymin - pw->y0) / cs);
    *cy1 = (int)floorf((ymax - pw->y0) / cs);
    if (*cx1 < 0 || *cy1 < 0 || *cx0 >= pw->gw || *cy0 >= pw->gh)
        return 0;
    *cx0 = *cx0 < 0 ? 0 : *cx0;
    *cy0 = *cy0 < 0 ? 0 : *cy0;
    *cx1 = *cx1 >= pw->gw ? pw->gw - 1 : *cx1;
    *cy1 = *cy1 >= pw->gh ? pw->gh - 1 : *cy1;
    return 1;
}

int phys_build(PhysWorld *pw, const Zone *z)
{
    const DZoneHeader *h = z->hdr;
    uint32_t i, t, n = 0, ndyn = 0, ndyn_tris = 0, dyn_cursor = 0, items = 0;
    float xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
    memset(pw, 0, sizeof(*pw));
    pw->zone = z;
    pw->has_water = (h->flags & DZONE_F_WATER) != 0;
    pw->water_z = h->water_height;

    for (i = 0; i < h->instance_count; ++i) {
        const ZoneMesh *m;
        const DMeshCollision *c = instance_collision(z, i, &m);
        if (!c)
            continue;
        if (z->instances[i].kind == DZONE_INST_DOOR) {
            ndyn++;
            ndyn_tris += c->tri_count;
        } else {
            n += c->tri_count;
        }
    }
    pw->tris = (PhysTri *)mem_try_alloc(MEM_REGION_ZONE, (n + 1u) * sizeof(PhysTri), 16);
    pw->stamp = (uint16_t *)mem_try_alloc(MEM_REGION_ZONE, (n + 1u) * sizeof(uint16_t), 4);
    pw->dyn = (PhysDynamic *)mem_try_alloc(MEM_REGION_ZONE, (ndyn + 1u) * sizeof(PhysDynamic), 16);
    pw->dyn_model = (float (*)[9])mem_try_alloc(MEM_REGION_ZONE, (ndyn_tris + 1u) * 9 * sizeof(float), 16);
    if (!pw->tris || !pw->stamp || !pw->dyn || !pw->dyn_model)
        return -1;
    memset(pw->stamp, 0, (n + 1u) * sizeof(uint16_t));

    for (i = 0; i < h->instance_count; ++i) {
        const ZoneMesh *m;
        const DMeshCollision *c = instance_collision(z, i, &m);
        if (!c)
            continue;
        if (z->instances[i].kind == DZONE_INST_DOOR) {
            PhysDynamic *d = &pw->dyn[pw->dyn_count++];
            float r2 = 0.0f;
            d->inst = i;
            d->first = dyn_cursor;
            d->count = c->tri_count;
            d->bound[0] = (c->bbox_min[0] + c->bbox_max[0]) * 0.5f;
            d->bound[1] = (c->bbox_min[1] + c->bbox_max[1]) * 0.5f;
            d->bound[2] = (c->bbox_min[2] + c->bbox_max[2]) * 0.5f;
            for (t = 0; t < 3; ++t) {
                float e = (c->bbox_max[t] - c->bbox_min[t]) * 0.5f;
                r2 += e * e;
            }
            d->bound[3] = sqrtf(r2);
            for (t = 0; t < c->tri_count; ++t)
                collision_tri(m, c, t, (float (*)[3])pw->dyn_model[dyn_cursor + t]);
            dyn_cursor += c->tri_count;
            continue;
        }
        for (t = 0; t < c->tri_count; ++t) {
            float v[3][3];
            int k;
            collision_tri(m, c, t, v);
            for (k = 0; k < 3; ++k) {
                Vec4 p = vec4(v[k][0], v[k][1], v[k][2], 1.0f), q;
                mat4_transform_point(&q, &z->inst_world[i], &p);
                v[k][0] = q.x;
                v[k][1] = q.y;
                v[k][2] = q.z;
            }
            fill_tri(&pw->tris[pw->tri_count], v, i);
            xmin = fminf(xmin, pw->tris[pw->tri_count].xmin);
            xmax = fmaxf(xmax, pw->tris[pw->tri_count].xmax);
            ymin = fminf(ymin, pw->tris[pw->tri_count].ymin);
            ymax = fmaxf(ymax, pw->tris[pw->tri_count].ymax);
            pw->tri_count++;
        }
    }

    if (pw->tri_count == 0) {
        xmin = ymin = 0.0f;
        xmax = ymax = CELL_SIZE;
    }
    pw->x0 = xmin - CELL_SIZE;
    pw->y0 = ymin - CELL_SIZE;
    pw->gw = (int)((xmax - pw->x0) / CELL_SIZE) + 2;
    pw->gh = (int)((ymax - pw->y0) / CELL_SIZE) + 2;
    if ((long)pw->gw * pw->gh > MAX_GRID_CELLS)
        return -2; /* would need a coarser grid than any zone should produce */
    pw->cell_start = (uint32_t *)mem_try_alloc(MEM_REGION_ZONE,
                                               ((uint32_t)(pw->gw * pw->gh) + 1u) * 4u, 4);
    if (!pw->cell_start)
        return -1;
    memset(pw->cell_start, 0, ((uint32_t)(pw->gw * pw->gh) + 1u) * 4u);
    for (t = 0; t < pw->tri_count; ++t) {
        int cx0, cx1, cy0, cy1, x, y;
        const PhysTri *tr = &pw->tris[t];
        if (!cell_range(pw, tr->xmin, tr->xmax, tr->ymin, tr->ymax, &cx0, &cx1, &cy0, &cy1))
            continue;
        for (y = cy0; y <= cy1; ++y)
            for (x = cx0; x <= cx1; ++x)
                pw->cell_start[y * pw->gw + x + 1]++;
    }
    for (i = 1; i <= (uint32_t)(pw->gw * pw->gh); ++i)
        pw->cell_start[i] += pw->cell_start[i - 1];
    items = pw->cell_start[pw->gw * pw->gh];
    pw->cell_items = (uint32_t *)mem_try_alloc(MEM_REGION_ZONE, (items + 1u) * 4u, 4);
    if (!pw->cell_items)
        return -1;
    {
        /* fill using a scratch cursor copy on the high end */
        MemMark mk = mem_mark_hi(MEM_REGION_ZONE);
        uint32_t *cursor = (uint32_t *)mem_try_alloc_hi(MEM_REGION_ZONE,
                                                        (uint32_t)(pw->gw * pw->gh) * 4u, 4);
        if (!cursor) {
            mem_release_hi(MEM_REGION_ZONE, mk);
            return -1;
        }
        memcpy(cursor, pw->cell_start, (uint32_t)(pw->gw * pw->gh) * 4u);
        for (t = 0; t < pw->tri_count; ++t) {
            int cx0, cx1, cy0, cy1, x, y;
            const PhysTri *tr = &pw->tris[t];
            if (!cell_range(pw, tr->xmin, tr->xmax, tr->ymin, tr->ymax, &cx0, &cx1, &cy0, &cy1))
                continue;
            for (y = cy0; y <= cy1; ++y)
                for (x = cx0; x <= cx1; ++x)
                    pw->cell_items[cursor[y * pw->gw + x]++] = t;
        }
        mem_release_hi(MEM_REGION_ZONE, mk);
    }
    return 0;
}

void phys_body_init(PhysBody *b, const Vec4 *feet, float radius, float height)
{
    memset(b, 0, sizeof(*b));
    b->pos = *feet;
    b->pos.w = 1.0f;
    b->radius = radius;
    b->height = height;
    b->ground_z = feet->z;
    b->ignore_obstacle = 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------------ */
/* queries                                                                  */
/* ------------------------------------------------------------------------ */
static uint16_t next_stamp(PhysWorld *pw)
{
    if (++pw->stamp_value == 0) {
        memset(pw->stamp, 0, (pw->tri_count + 1u) * sizeof(uint16_t));
        pw->stamp_value = 1;
    }
    return pw->stamp_value;
}

typedef void (*TriVisitor)(PhysWorld *pw, float v[3][3], const float n[3], uint32_t inst,
                           void *user);

static void visit_terrain(PhysWorld *pw, float xmin, float xmax, float ymin, float ymax,
                          TriVisitor fn, void *user)
{
    const Zone *z = pw->zone;
    uint32_t p;
    for (p = 0; p < z->hdr->terrain_count; ++p) {
        const DZoneTerrain *t = &z->terrain[p];
        const float *hg = (const float *)(z->base + t->heights_ofs);
        int i0, i1, j0, j1, i, j;
        const int G = DZONE_TERRAIN_GRID;
        if (xmax < t->origin[0] || xmin > t->origin[0] + 8192.0f || ymax < t->origin[1] ||
            ymin > t->origin[1] + 8192.0f)
            continue;
        i0 = (int)floorf((xmin - t->origin[0]) / TERRAIN_SPACING);
        i1 = (int)floorf((xmax - t->origin[0]) / TERRAIN_SPACING);
        j0 = (int)floorf((ymin - t->origin[1]) / TERRAIN_SPACING);
        j1 = (int)floorf((ymax - t->origin[1]) / TERRAIN_SPACING);
        i0 = i0 < 0 ? 0 : i0;
        j0 = j0 < 0 ? 0 : j0;
        i1 = i1 > G - 2 ? G - 2 : i1;
        j1 = j1 > G - 2 ? G - 2 : j1;
        for (j = j0; j <= j1; ++j) {
            for (i = i0; i <= i1; ++i) {
                float x0 = t->origin[0] + (float)i * TERRAIN_SPACING, x1 = x0 + TERRAIN_SPACING;
                float y0 = t->origin[1] + (float)j * TERRAIN_SPACING, y1 = y0 + TERRAIN_SPACING;
                float va[3][3] = { { x0, y0, hg[j * G + i] }, { x1, y0, hg[j * G + i + 1] },
                                   { x1, y1, hg[(j + 1) * G + i + 1] } };
                float vb[3][3] = { { x0, y0, hg[j * G + i] }, { x1, y1, hg[(j + 1) * G + i + 1] },
                                   { x0, y1, hg[(j + 1) * G + i] } };
                float n[3];
                tri_normal(va, n);
                fn(pw, va, n, 0xFFFFFFFFu, user);
                tri_normal(vb, n);
                fn(pw, vb, n, 0xFFFFFFFFu, user);
            }
        }
    }
}

static void visit(PhysWorld *pw, float xmin, float xmax, float ymin, float ymax, int terrain,
                  int doors, TriVisitor fn, void *user)
{
    int cx0, cx1, cy0, cy1, x, y;
    uint32_t i, k;
    if (pw->tri_count && cell_range(pw, xmin, xmax, ymin, ymax, &cx0, &cx1, &cy0, &cy1)) {
        uint16_t s = next_stamp(pw);
        for (y = cy0; y <= cy1; ++y) {
            for (x = cx0; x <= cx1; ++x) {
                uint32_t c = (uint32_t)(y * pw->gw + x);
                for (i = pw->cell_start[c]; i < pw->cell_start[c + 1]; ++i) {
                    uint32_t ti = pw->cell_items[i];
                    const PhysTri *t = &pw->tris[ti];
                    if (pw->stamp[ti] == s)
                        continue;
                    pw->stamp[ti] = s;
                    if (t->xmax < xmin || t->xmin > xmax || t->ymax < ymin || t->ymin > ymax)
                        continue;
                    if (pw->zone->inst_hidden && pw->zone->inst_hidden[t->inst])
                        continue;
                    fn(pw, (float (*)[3])t->v, t->n, t->inst, user);
                }
            }
        }
    }
    if (doors) {
        for (i = 0; i < pw->dyn_count; ++i) {
            const PhysDynamic *d = &pw->dyn[i];
            const Mat4 *m = &pw->zone->inst_world[d->inst];
            if (pw->zone->inst_hidden && pw->zone->inst_hidden[d->inst])
                continue;
            Vec4 c = vec4(d->bound[0], d->bound[1], d->bound[2], 1.0f), wc;
            float r = d->bound[3] * pw->zone->instances[d->inst].scale;
            mat4_transform_point(&wc, m, &c);
            if (wc.x + r < xmin || wc.x - r > xmax || wc.y + r < ymin || wc.y - r > ymax)
                continue;
            for (k = 0; k < d->count; ++k) {
                const float *mv = pw->dyn_model[d->first + k];
                float v[3][3], n[3];
                int j;
                for (j = 0; j < 3; ++j) {
                    Vec4 p = vec4(mv[j * 3], mv[j * 3 + 1], mv[j * 3 + 2], 1.0f), q;
                    mat4_transform_point(&q, m, &p);
                    v[j][0] = q.x;
                    v[j][1] = q.y;
                    v[j][2] = q.z;
                }
                tri_normal(v, n);
                fn(pw, v, n, d->inst, user);
            }
        }
    }
    if (terrain)
        visit_terrain(pw, xmin, xmax, ymin, ymax, fn, user);
}

typedef struct {
    float x, y, ztop, best;
    int found;
    int skip_terrain;
} GroundQuery;

static void ground_visitor(PhysWorld *pw, float v[3][3], const float n[3], uint32_t inst,
                           void *user)
{
    GroundQuery *q = (GroundQuery *)user;
    int inside;
    float h;
    (void)pw;
    (void)inst;
    if (n[2] < PHYS_WALKABLE_NZ && inst != 0xFFFFFFFFu)
        return;
    h = tri_height_at(v, n, q->x, q->y, &inside);
    if (!inside)
        return;
    if (inst == 0xFFFFFFFFu) {
        if (q->skip_terrain)
            return;
        /* terrain is a hard floor: never fall through it, even from below */
        if (!q->found || h > q->best) {
            q->best = h;
            q->found = 1;
        }
        return;
    }
    if (h <= q->ztop + 0.01f && (!q->found || h > q->best)) {
        q->best = h;
        q->found = 1;
    }
}

static int ground_query(PhysWorld *pw, float x, float y, float z_top, int terrain, float *ground)
{
    GroundQuery q;
    q.x = x;
    q.y = y;
    q.ztop = z_top;
    q.best = -1e30f;
    q.found = 0;
    q.skip_terrain = !terrain;
    visit(pw, x - 0.5f, x + 0.5f, y - 0.5f, y + 0.5f, terrain, 0, ground_visitor, &q);
    if (q.found)
        *ground = q.best;
    return q.found;
}

int phys_ground(PhysWorld *pw, float x, float y, float z_top, float *ground)
{
    return ground_query(pw, x, y, z_top, 1, ground);
}

typedef struct {
    float x, y, zmin, best;
    int found;
} CeilQuery;

static void ceiling_visitor(PhysWorld *pw, float v[3][3], const float n[3], uint32_t inst,
                            void *user)
{
    CeilQuery *q = (CeilQuery *)user;
    int inside;
    float h;
    (void)pw;
    if (n[2] > -0.3f || inst == 0xFFFFFFFFu)
        return;
    h = tri_height_at(v, n, q->x, q->y, &inside);
    if (inside && h >= q->zmin && (!q->found || h < q->best)) {
        q->best = h;
        q->found = 1;
    }
}

/* ------------------------------------------------------------------------ */
/* movement                                                                 */
/* ------------------------------------------------------------------------ */
typedef struct {
    PhysBody *b;
    float z0, z1;
    int hits;
} WallQuery;

static void wall_visitor(PhysWorld *pw, float v[3][3], const float n[3], uint32_t inst,
                         void *user)
{
    WallQuery *q = (WallQuery *)user;
    float px, py;
    (void)inst;
    pw->tests++;
    if (phys_cylinder_triangle(q->b->pos.x, q->b->pos.y, q->b->radius, q->z0, q->z1, v, n, &px,
                               &py)) {
        q->b->pos.x += px;
        q->b->pos.y += py;
        q->hits++;
    }
}

static int resolve_walls(PhysWorld *pw, PhysBody *b)
{
    int iter, total = 0;
    uint32_t i;
    WallQuery q;
    q.b = b;
    q.z0 = b->pos.z + PHYS_STEP_UP;
    q.z1 = b->pos.z + b->height;
    if (b->swimming || !b->on_ground)
        q.z0 = b->pos.z + 4.0f; /* no stepping onto ledges mid-air */
    for (iter = 0; iter < WALL_ITERATIONS; ++iter) {
        float r = b->radius;
        q.hits = 0;
        visit(pw, b->pos.x - r, b->pos.x + r, b->pos.y - r, b->pos.y + r, 1, 1, wall_visitor, &q);
        for (i = 0; i < pw->obstacle_count; ++i) {
            const PhysObstacle *o = &pw->obstacles[i];
            float dx = b->pos.x - o->x, dy = b->pos.y - o->y, d2, rr = r + o->radius;
            if (o->id == b->ignore_obstacle || o->z > q.z1 || o->z + o->height < b->pos.z)
                continue;
            d2 = dx * dx + dy * dy;
            if (d2 < rr * rr) {
                float d = sqrtf(d2);
                if (d < 1e-3f) {
                    dx = 1.0f;
                    dy = 0.0f;
                    d = 1.0f;
                }
                b->pos.x += dx / d * (rr - sqrtf(d2));
                b->pos.y += dy / d * (rr - sqrtf(d2));
                q.hits++;
            }
        }
        total += q.hits;
        if (!q.hits)
            break;
    }
    return total;
}

static float support_height(PhysWorld *pw, const PhysBody *b, float z_top, int *found)
{
    static const float off[5][2] = { { 0, 0 }, { 0.6f, 0 }, { -0.6f, 0 }, { 0, 0.6f }, { 0, -0.6f } };
    float best = -1e30f, g;
    int k;
    *found = 0;
    /* terrain at the axis only (a smooth heightfield); mesh supports across
     * the footprint so the player can stand on ledges and stair edges */
    for (k = 0; k < 5; ++k) {
        if (ground_query(pw, b->pos.x + off[k][0] * b->radius, b->pos.y + off[k][1] * b->radius,
                         z_top, k == 0, &g)) {
            if (g > best)
                best = g;
            *found = 1;
        }
    }
    return best;
}

void phys_move(PhysWorld *pw, PhysBody *b, const Vec4 *wish, int jump, float jump_speed, float dt)
{
    float dx = wish->x * dt, dy = wish->y * dt, len, ground;
    int steps, s, found;
    pw->tests = 0;
    if (dt <= 0.0f)
        return;
    if (b->noclip) {
        b->pos.x += dx;
        b->pos.y += dy;
        b->pos.z += wish->z * dt;
        b->vel.z = 0.0f;
        return;
    }

    /* swimming: chest below the surface (never while levitating or water walking) */
    b->swimming = (uint8_t)(!b->levitate && !b->water_walk && pw->has_water &&
                            b->pos.z + b->height * 0.7f < pw->water_z);
    if (b->levitate) {
        b->vel.z = wish->z;
        b->on_ground = 0;
    } else if (b->swimming) {
        b->vel.z = wish->z;
        if (wish->z == 0.0f && b->pos.z + b->height * 0.8f < pw->water_z)
            b->vel.z = 60.0f; /* buoyancy */
        if (b->pos.z > pw->water_z - b->height * 0.72f && b->vel.z > 0.0f)
            b->vel.z = 0.0f; /* float with the head above water */
    } else {
        if (jump && b->on_ground) {
            b->vel.z = jump_speed;
            b->on_ground = 0;
        }
        if (!b->on_ground) {
            float g = (b->fall_mult > 0.0f && b->vel.z < 0.0f) ? PHYS_GRAVITY * b->fall_mult : PHYS_GRAVITY;
            b->vel.z -= g * dt;
            if (b->vel.z < -PHYS_MAX_FALL_SPEED)
                b->vel.z = -PHYS_MAX_FALL_SPEED;
        }
    }

    /* horizontal: sub-steps no longer than half the radius */
    len = sqrtf(dx * dx + dy * dy);
    steps = (int)(len / (b->radius * 0.5f)) + 1;
    if (steps > 16)
        steps = 16;
    for (s = 0; s < steps; ++s) {
        b->pos.x += dx / (float)steps;
        b->pos.y += dy / (float)steps;
        resolve_walls(pw, b);
    }

    /* vertical */
    b->pos.z += b->vel.z * dt;
    if (b->vel.z > 0.0f) {
        CeilQuery cq;
        cq.x = b->pos.x;
        cq.y = b->pos.y;
        cq.zmin = b->pos.z;
        cq.found = 0;
        cq.best = 1e30f;
        visit(pw, b->pos.x - 0.5f, b->pos.x + 0.5f, b->pos.y - 0.5f, b->pos.y + 0.5f, 0, 0,
              ceiling_visitor, &cq);
        if (cq.found && b->pos.z + b->height > cq.best) {
            b->pos.z = cq.best - b->height;
            b->vel.z = 0.0f;
        }
    }
    ground = support_height(pw, b, b->pos.z + PHYS_STEP_UP, &found);
    b->steep = 0;
    if (found) {
        b->ground_z = ground;
        if (b->vel.z <= 0.0f && !b->swimming) {
            if (b->pos.z <= ground || (b->on_ground && b->pos.z - ground <= PHYS_STEP_DOWN)) {
                b->pos.z = ground;
                b->vel.z = 0.0f;
                b->on_ground = 1;
            } else {
                b->on_ground = 0;
            }
        } else if (b->swimming && b->pos.z < ground) {
            b->pos.z = ground;
            if (b->vel.z < 0.0f)
                b->vel.z = 0.0f;
        } else {
            b->on_ground = 0;
        }
    } else {
        b->on_ground = 0;
    }

    /* water walking: the surface carries the player */
    if (b->water_walk && pw->has_water && b->pos.z < pw->water_z && b->vel.z <= 0.0f &&
        (!found || ground < pw->water_z)) {
        b->pos.z = pw->water_z;
        b->vel.z = 0.0f;
        b->on_ground = 1;
    }

    /* slopes too steep to stand on: slide down them */
    if (b->on_ground) {
        float hx0, hx1, hy0, hy1, nx, ny, nz, nl;
        if (zone_terrain_height(pw->zone, b->pos.x - 32.0f, b->pos.y, &hx0) &&
            zone_terrain_height(pw->zone, b->pos.x + 32.0f, b->pos.y, &hx1) &&
            zone_terrain_height(pw->zone, b->pos.x, b->pos.y - 32.0f, &hy0) &&
            zone_terrain_height(pw->zone, b->pos.x, b->pos.y + 32.0f, &hy1) &&
            fabsf(b->ground_z - (hx0 + hx1) * 0.5f) < 24.0f) {
            nx = hx0 - hx1;
            ny = hy0 - hy1;
            nz = 64.0f;
            nl = sqrtf(nx * nx + ny * ny + nz * nz);
            if (nz / nl < PHYS_WALKABLE_NZ) {
                float hl = sqrtf(nx * nx + ny * ny);
                b->steep = 1;
                b->pos.x += nx / hl * 300.0f * dt;
                b->pos.y += ny / hl * 300.0f * dt;
                resolve_walls(pw, b);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* rays                                                                     */
/* ------------------------------------------------------------------------ */
typedef struct {
    Vec4 o, d;
    float best;
    uint32_t inst;
    int hit;
} RayQuery;

static void ray_visitor(PhysWorld *pw, float v[3][3], const float n[3], uint32_t inst,
                        void *user)
{
    RayQuery *q = (RayQuery *)user;
    Vec4 a = vec4(v[0][0], v[0][1], v[0][2], 1.0f);
    Vec4 b = vec4(v[1][0], v[1][1], v[1][2], 1.0f);
    Vec4 c = vec4(v[2][0], v[2][1], v[2][2], 1.0f);
    float t;
    (void)pw;
    (void)n;
    if (ray_triangle(&q->o, &q->d, &a, &b, &c, &t, NULL, NULL) && t < q->best) {
        q->best = t;
        q->inst = inst;
        q->hit = 1;
    }
}

int phys_raycast(PhysWorld *pw, const Vec4 *origin, const Vec4 *dir, float max_dist, float *t,
                 uint32_t *inst)
{
    RayQuery q;
    Vec4 e;
    q.o = *origin;
    q.o.w = 1.0f;
    vec4_normalize3(&q.d, dir);
    q.d.w = 0.0f;
    q.best = max_dist;
    q.inst = 0xFFFFFFFFu;
    q.hit = 0;
    e = vec4(q.o.x + q.d.x * max_dist, q.o.y + q.d.y * max_dist, 0.0f, 0.0f);
    visit(pw, fminf(q.o.x, e.x), fmaxf(q.o.x, e.x), fminf(q.o.y, e.y), fmaxf(q.o.y, e.y), 1, 1,
          ray_visitor, &q);
    if (q.hit) {
        *t = q.best;
        *inst = q.inst;
    }
    return q.hit;
}
