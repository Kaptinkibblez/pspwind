/*
 * actor.h - animated NPCs and creatures (segmented rigid-limb characters).
 *
 * Binding at zone load (no string work per frame):
 *   skeleton node -> animation track   (by node name hash)
 *   part segment node -> skeleton node (by node name hash)
 *   rigid part -> skeleton attachment bone (by DZoneActor part bone hash)
 * Per frame each nearby actor samples its clip and rebuilds node world
 * matrices (one VFPU mat4_mul per bone). Drawing a limb is then a single
 * matrix: actor world * bone pose * [mirror] * batch dequantisation.
 */
#ifndef PSPWIND_ACTOR_H
#define PSPWIND_ACTOR_H

#include "common.h"
#include "fmt.h"
#include "vfpu.h"
#include "zone.h"

#define ACTOR_MAX_BONES 160

typedef struct {
    const DZoneActor *rec;
    uint32_t instance;
    const ZoneMesh *skel;
    const Anim *anim;      /* NULL: bind pose */
    int16_t *bone_track;   /* skeleton node -> track index or -1 */
    int16_t *part_map[DZONE_ACTOR_MAX_PARTS]; /* part mesh node -> skeleton node */
    int16_t part_bone[DZONE_ACTOR_MAX_PARTS]; /* rigid part attachment node or -1 */
    const ZoneMesh *part_mesh[DZONE_ACTOR_MAX_PARTS];
    Mat4 *pose;            /* skeleton node worlds, actor space */
    Mat4 world;            /* placement: T(pos) Rz(-yaw) S(scale) */
    Vec4 pos;
    float yaw;             /* Morrowind z rotation (clockwise, radians) */
    float scale;
    uint32_t group;        /* current group name hash */
    const DAnimGroup *group_rec;
    float time;
    uint8_t loop;
    uint8_t posed;         /* pose valid this frame */
} Actor;

typedef struct {
    Actor *list;
    uint32_t count;
} ActorSet;

/* Builds the runtime actors of a loaded zone (ZONE region). */
int actors_init(ActorSet *s, const Zone *z);
/* Starts a group by name hash; unknown groups fall back to "idle". */
void actor_play(Actor *a, uint32_t group_hash, int loop);
void actor_set_placement(Actor *a, const Vec4 *pos, float yaw);
/* Advances clocks and poses actors within max_dist of the viewer. */
void actors_update(ActorSet *s, float dt, const Vec4 *viewer, float max_dist);
void actor_pose(Actor *a);
/* Final model matrix of batch `batch` of part `part` (-1 = skeleton mesh).
 * Returns 0 when the batch has no bone to attach to; *mirrored is set for
 * mirrored left-side parts (draw with reversed winding). */
int actor_batch_matrix(const Actor *a, int part, uint32_t batch, Mat4 *out, int *mirrored);

#endif /* PSPWIND_ACTOR_H */
