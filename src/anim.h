/*
 * anim.h - DANIM clips: validation, group lookup and sampling.
 *
 * Clips are resampled at a fixed rate by the cooker, so sampling is a
 * frame lookup plus nlerp/lerp between two neighbouring frames.
 */
#ifndef PSPWIND_ANIM_H
#define PSPWIND_ANIM_H

#include "common.h"
#include "fmt.h"
#include "vfpu.h"

typedef struct {
    const uint8_t *base;
    uint32_t size;
    const DAnimHeader *hdr;
    const DAnimTrack *tracks;
    const DAnimGroup *groups;
} Anim;

/* 0 when valid (structure, offsets, frame ranges). */
int danim_validate(const uint8_t *base, uint32_t size);
int anim_bind(Anim *a, const uint8_t *base, uint32_t size);
const DAnimGroup *anim_find_group(const Anim *a, uint32_t name_hash);
/* Fractional frame for a group played for `seconds`; looping groups wrap
 * between loop_start and loop_stop after reaching loop_stop. */
float anim_group_frame(const Anim *a, const DAnimGroup *g, float seconds, int loop);
/* Samples a track. Writes rot / trans only when the track has them;
 * returns DANIM_T_HAS_ROT | DANIM_T_HAS_TRANS bits for what was written. */
int anim_sample(const Anim *a, const DAnimTrack *t, float frame, Quat *rot, Vec4 *trans);

#endif /* PSPWIND_ANIM_H */
