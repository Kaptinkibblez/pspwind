/*
 * actor.c - animated actors (see actor.h).
 */
#include "actor.h"

#include <math.h>
#include <string.h>

#include "mem.h"

static int16_t find_node(const ZoneMesh *m, uint32_t hash)
{
    uint32_t i;
    for (i = 0; i < m->hdr->node_count && i < 0x7FFF; ++i)
        if (m->nodes[i].name_hash == hash)
            return (int16_t)i;
    return -1;
}

int actors_init(ActorSet *s, const Zone *z)
{
    uint32_t i, k, n = 0;
    memset(s, 0, sizeof(*s));
    if (!z->hdr || z->hdr->actor_count == 0)
        return 0;
    s->list = (Actor *)mem_try_alloc(MEM_REGION_ZONE, z->hdr->actor_count * sizeof(Actor), 64);
    if (!s->list)
        return -1;
    for (i = 0; i < z->hdr->actor_count; ++i) {
        const DZoneActor *rec = &z->actor_recs[i];
        const DZoneInstance *in = &z->instances[rec->instance];
        Actor *a = &s->list[n];
        const ZoneMesh *skel = &z->meshes[rec->skeleton];
        const uint32_t *bones = (const uint32_t *)(z->base + rec->part_bone_ofs);
        uint32_t nodes;
        Vec4 pos;
        if (!skel->hdr || skel->hdr->node_count > ACTOR_MAX_BONES)
            continue;
        memset(a, 0, sizeof(*a));
        a->rec = rec;
        a->instance = rec->instance;
        a->skel = skel;
        nodes = skel->hdr->node_count;
        if (rec->anim != DZONE_NONE && z->anims[rec->anim].hdr)
            a->anim = &z->anims[rec->anim];
        a->bone_track = (int16_t *)mem_try_alloc(MEM_REGION_ZONE, nodes * sizeof(int16_t), 4);
        a->pose = (Mat4 *)mem_try_alloc(MEM_REGION_ZONE, nodes * sizeof(Mat4), 64);
        if (!a->bone_track || !a->pose)
            return -1;
        for (k = 0; k < nodes; ++k) {
            uint32_t t;
            a->bone_track[k] = -1;
            if (!a->anim)
                continue;
            for (t = 0; t < a->anim->hdr->track_count; ++t) {
                if (a->anim->tracks[t].bone_hash == skel->nodes[k].name_hash) {
                    a->bone_track[k] = (int16_t)t;
                    break;
                }
            }
        }
        for (k = 0; k < rec->part_count; ++k) {
            const ZoneMesh *pm = &z->meshes[rec->parts[k]];
            uint32_t j;
            a->part_bone[k] = -1;
            if (!pm->hdr)
                continue;
            a->part_mesh[k] = pm;
            a->part_map[k] = (int16_t *)mem_try_alloc(MEM_REGION_ZONE,
                                                      pm->hdr->node_count * sizeof(int16_t), 4);
            if (!a->part_map[k])
                return -1;
            for (j = 0; j < pm->hdr->node_count; ++j)
                a->part_map[k][j] = find_node(skel, pm->nodes[j].name_hash);
            if (bones[k])
                a->part_bone[k] = find_node(skel, bones[k]);
        }
        a->scale = in->scale;
        pos = vec4(in->pos[0], in->pos[1], in->pos[2], 1.0f);
        actor_set_placement(a, &pos, in->rot[2]);
        actor_play(a, pw_hash_name("idle"), 1);
        /* desynchronise idle loops of neighbours */
        a->time = (float)(i * 7 % 13) * 0.17f;
        actor_pose(a);
        n++;
    }
    s->count = n;
    return 0;
}

void actor_set_placement(Actor *a, const Vec4 *pos, float yaw)
{
    float rot[3] = { 0.0f, 0.0f, 0.0f };
    float p[3];
    a->pos = *pos;
    a->yaw = yaw;
    rot[2] = yaw;
    p[0] = pos->x;
    p[1] = pos->y;
    p[2] = pos->z;
    zone_placement_matrix(&a->world, p, rot, a->scale);
}

void actor_play(Actor *a, uint32_t group_hash, int loop)
{
    const DAnimGroup *g = NULL;
    a->group = group_hash;
    a->time = 0.0f;
    a->loop = (uint8_t)(loop != 0);
    if (a->anim) {
        g = anim_find_group(a->anim, group_hash);
        if (!g)
            g = anim_find_group(a->anim, pw_hash_name("idle"));
    }
    a->group_rec = g;
}

void actor_pose(Actor *a)
{
    const ZoneMesh *s = a->skel;
    float frame = 0.0f;
    uint32_t i;
    if (a->anim && a->group_rec)
        frame = anim_group_frame(a->anim, a->group_rec, a->time, a->loop);
    for (i = 0; i < s->hdr->node_count; ++i) {
        const DMeshNode *n = &s->nodes[i];
        Vec4 pos = vec4(n->pos[0], n->pos[1], n->pos[2], 1.0f);
        Quat rot = vec4(n->rot[0], n->rot[1], n->rot[2], n->rot[3]);
        Mat4 local;
        if (a->anim && a->group_rec && a->bone_track[i] >= 0)
            anim_sample(a->anim, &a->anim->tracks[a->bone_track[i]], frame, &rot, &pos);
        mat4_from_trs(&local, &pos, &rot, n->scale);
        if (n->parent < 0)
            a->pose[i] = local;
        else
            mat4_mul(&a->pose[i], &a->pose[n->parent], &local);
    }
    a->posed = 1;
}

void actors_update(ActorSet *s, float dt, const Vec4 *viewer, float max_dist)
{
    uint32_t i;
    for (i = 0; i < s->count; ++i) {
        Actor *a = &s->list[i];
        float dx = a->pos.x - viewer->x, dy = a->pos.y - viewer->y, dz = a->pos.z - viewer->z;
        a->time += dt;
        if (a->group_rec && !a->loop) {
            float len = (float)(a->group_rec->stop - a->group_rec->start) / a->anim->hdr->fps;
            if (a->time > len)
                a->time = len; /* hold the last frame */
        }
        if (dx * dx + dy * dy + dz * dz <= max_dist * max_dist)
            actor_pose(a);
        else
            a->posed = 0;
    }
}

static void dequant(Mat4 *out, const DMeshBatch *b)
{
    mat4_identity(out);
    out->x.x = b->pos_scale[0];
    out->y.y = b->pos_scale[1];
    out->z.z = b->pos_scale[2];
    out->w = vec4(b->pos_offset[0], b->pos_offset[1], b->pos_offset[2], 1.0f);
}

int actor_batch_matrix(const Actor *a, int part, uint32_t batch, Mat4 *out, int *mirrored)
{
    Mat4 m, dq;
    *mirrored = 0;
    if (part < 0) {
        const DMeshBatch *b = &a->skel->batches[batch];
        dequant(&dq, b);
        mat4_mul(&m, &a->pose[b->node], &dq);
        mat4_mul(out, &a->world, &m);
        return 1;
    } else {
        const ZoneMesh *pm = a->part_mesh[part];
        const DMeshBatch *b;
        if (!pm)
            return 0;
        b = &pm->batches[batch];
        if (b->flags & DMESH_BATCH_F_SEGMENT) {
            int16_t node = a->part_map[part][b->node];
            if (node < 0)
                return 0;
            dequant(&dq, b);
            mat4_mul(&m, &a->pose[node], &dq);
            mat4_mul(out, &a->world, &m);
            return 1;
        } else {
            int16_t bone = a->part_bone[part];
            Mat4 bm;
            if (bone < 0)
                return 0;
            bm = a->pose[bone];
            if (a->rec->part_flags & (1u << part)) {
                mat4_scale(&bm, -1.0f, 1.0f, 1.0f);
                *mirrored = 1;
            }
            mat4_mul(&m, &bm, &pm->batch_model[batch]);
            mat4_mul(out, &a->world, &m);
            return 1;
        }
    }
}
