/*
 * anim.c - DANIM sampling (see anim.h).
 */
#include "anim.h"

#include <math.h>
#include <string.h>

static int in_range(uint32_t ofs, uint32_t count, uint32_t elem, uint32_t size)
{
    if (ofs > size)
        return 0;
    return count == 0 || count <= (size - ofs) / elem;
}

int danim_validate(const uint8_t *base, uint32_t size)
{
    const DAnimHeader *h = (const DAnimHeader *)base;
    const DAnimTrack *t;
    const DAnimGroup *g;
    uint32_t i;
    if (size < sizeof(*h) || h->magic != DANIM_MAGIC || h->version != DANIM_VERSION ||
        h->file_size != size)
        return 1;
    if (h->frame_count == 0 || h->frame_count > 65535u || !(h->fps > 0.0f) || h->fps > 240.0f)
        return 2;
    if ((h->tracks_ofs & 3u) || (h->groups_ofs & 3u) ||
        !in_range(h->tracks_ofs, h->track_count, sizeof(DAnimTrack), size) ||
        !in_range(h->groups_ofs, h->group_count, sizeof(DAnimGroup), size))
        return 3;
    t = (const DAnimTrack *)(base + h->tracks_ofs);
    for (i = 0; i < h->track_count; ++i) {
        if (t[i].flags & DANIM_T_HAS_ROT) {
            uint32_t n = (t[i].flags & DANIM_T_ROT_ANIMATED) ? h->frame_count : 1u;
            if ((t[i].rot_ofs & 1u) || !in_range(t[i].rot_ofs, n, 8, size))
                return 4;
        }
        if (t[i].flags & DANIM_T_HAS_TRANS) {
            uint32_t n = (t[i].flags & DANIM_T_TRANS_ANIMATED) ? h->frame_count : 1u;
            if ((t[i].trans_ofs & 3u) || !in_range(t[i].trans_ofs, n, 12, size))
                return 5;
        }
    }
    g = (const DAnimGroup *)(base + h->groups_ofs);
    for (i = 0; i < h->group_count; ++i) {
        if (g[i].start > g[i].stop || g[i].stop >= h->frame_count ||
            g[i].loop_start < g[i].start || g[i].loop_stop > g[i].stop ||
            g[i].loop_start > g[i].loop_stop)
            return 6;
        if (i > 0 && g[i].name_hash < g[i - 1].name_hash)
            return 7;
    }
    return 0;
}

int anim_bind(Anim *a, const uint8_t *base, uint32_t size)
{
    memset(a, 0, sizeof(*a));
    if (danim_validate(base, size) != 0)
        return -1;
    a->base = base;
    a->size = size;
    a->hdr = (const DAnimHeader *)base;
    a->tracks = (const DAnimTrack *)(base + a->hdr->tracks_ofs);
    a->groups = (const DAnimGroup *)(base + a->hdr->groups_ofs);
    return 0;
}

const DAnimGroup *anim_find_group(const Anim *a, uint32_t name_hash)
{
    uint32_t lo = 0, hi = a->hdr ? a->hdr->group_count : 0;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        if (a->groups[mid].name_hash == name_hash)
            return &a->groups[mid];
        if (a->groups[mid].name_hash < name_hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

float anim_group_frame(const Anim *a, const DAnimGroup *g, float seconds, int loop)
{
    float f = (float)g->start + seconds * a->hdr->fps;
    float ls = (float)g->loop_start, le = (float)g->loop_stop;
    if (!loop || f <= le || le <= ls)
        return f > (float)g->stop ? (float)g->stop : f;
    return ls + fmodf(f - ls, le - ls);
}

int anim_sample(const Anim *a, const DAnimTrack *t, float frame, Quat *rot, Vec4 *trans)
{
    int got = 0, i0, i1;
    float frac;
    const int last = (int)a->hdr->frame_count - 1;
    if (frame < 0.0f)
        frame = 0.0f;
    i0 = (int)frame;
    if (i0 > last)
        i0 = last;
    i1 = i0 < last ? i0 + 1 : last;
    frac = frame - (float)i0;
    if (frac < 0.0f)
        frac = 0.0f;
    if (t->flags & DANIM_T_HAS_ROT) {
        const int16_t *q = (const int16_t *)(a->base + t->rot_ofs);
        if (t->flags & DANIM_T_ROT_ANIMATED) {
            const int16_t *qa = q + 4 * i0, *qb = q + 4 * i1;
            Quat r;
            r.x = (float)qa[0] + ((float)qb[0] - (float)qa[0]) * frac;
            r.y = (float)qa[1] + ((float)qb[1] - (float)qa[1]) * frac;
            r.z = (float)qa[2] + ((float)qb[2] - (float)qa[2]) * frac;
            r.w = (float)qa[3] + ((float)qb[3] - (float)qa[3]) * frac;
            quat_normalize(rot, &r);
        } else {
            Quat r = vec4((float)q[0], (float)q[1], (float)q[2], (float)q[3]);
            quat_normalize(rot, &r);
        }
        got |= DANIM_T_HAS_ROT;
    }
    if (t->flags & DANIM_T_HAS_TRANS) {
        const float *v = (const float *)(a->base + t->trans_ofs);
        if (t->flags & DANIM_T_TRANS_ANIMATED) {
            const float *va = v + 3 * i0, *vb = v + 3 * i1;
            *trans = vec4(va[0] + (vb[0] - va[0]) * frac, va[1] + (vb[1] - va[1]) * frac,
                          va[2] + (vb[2] - va[2]) * frac, 1.0f);
        } else {
            *trans = vec4(v[0], v[1], v[2], 1.0f);
        }
        got |= DANIM_T_HAS_TRANS;
    }
    return got;
}
