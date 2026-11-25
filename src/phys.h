/*
 * phys.h - zone collision and character movement.
 *
 * Collision world (built at zone load, ZONE region):
 *   - static triangles: every instance's DMESH collision mesh (merged
 *     statics included) transformed to world space, indexed by a uniform
 *     2D grid (256-unit cells, CSR layout)
 *   - dynamic triangles: doors keep their model-space collision and are
 *     transformed with the door's current matrix when tested (they swing)
 *   - terrain: sampled straight from the zone's 17x17 height patches
 *   - obstacles: vertical cylinders supplied per frame (actors)
 *
 * Characters are upright cylinders. Horizontal motion is resolved by
 * clipping each nearby triangle to the cylinder's vertical slab
 * [feet + step_up, head] and pushing the cylinder's axis out of the 2D
 * footprint of what remains - steps lower than step_up and gentle slopes
 * never enter the slab, walls and slopes steeper than ~46 degrees do.
 * Vertical motion: gravity (627.2 units/s^2, Morrowind/OpenMW), jumping,
 * snapping down steps up to step_down, ceilings, swimming below the
 * water surface.
 */
#ifndef PSPWIND_PHYS_H
#define PSPWIND_PHYS_H

#include "common.h"
#include "vfpu.h"
#include "zone.h"

#define PHYS_GRAVITY 627.2f
#define PHYS_MAX_FALL_SPEED 4005.5f
#define PHYS_STEP_UP 34.0f
#define PHYS_STEP_DOWN 62.0f
#define PHYS_WALKABLE_NZ 0.6947f /* cos(46 degrees) */
#define PHYS_MAX_OBSTACLES 64

typedef struct {
    float v[3][3];
    float n[3];
    float zmin, zmax;
    float xmin, xmax, ymin, ymax;
    uint32_t inst;
} PhysTri;

typedef struct {
    uint32_t inst;
    uint32_t first; /* into dyn_model */
    uint32_t count;
    float bound[4]; /* model-space sphere */
} PhysDynamic;

typedef struct {
    float x, y, z, radius, height;
    uint32_t id;
} PhysObstacle;

typedef struct {
    const Zone *zone;
    PhysTri *tris;
    uint32_t tri_count;
    uint32_t *cell_start; /* gw*gh + 1 */
    uint32_t *cell_items;
    uint16_t *stamp;      /* per tri, query de-duplication */
    uint16_t stamp_value;
    int gw, gh;
    float x0, y0;
    PhysDynamic *dyn;
    uint32_t dyn_count;
    float (*dyn_model)[9];
    PhysObstacle obstacles[PHYS_MAX_OBSTACLES];
    uint32_t obstacle_count;
    int has_water;
    float water_z;
    uint32_t tests; /* triangle tests in the last move (stats) */
} PhysWorld;

typedef struct {
    Vec4 pos;      /* centre of the feet */
    Vec4 vel;      /* only z is persistent (gravity / jumps) */
    float radius;
    float height;
    float ground_z;
    uint32_t ignore_obstacle; /* obstacle id to skip (self) */
    uint8_t on_ground;
    uint8_t swimming;
    uint8_t noclip;
    uint8_t steep;  /* standing on a slope too steep to climb */
} PhysBody;

int phys_build(PhysWorld *pw, const Zone *z);
void phys_body_init(PhysBody *b, const Vec4 *feet, float radius, float height);
/* wish: desired horizontal velocity (x, y) and swim/fly vertical (z), units/s */
void phys_move(PhysWorld *pw, PhysBody *b, const Vec4 *wish, int jump, float jump_speed, float dt);
/* Highest walkable support at (x, y) at or below z_top: terrain or
 * triangles. Returns 0 when nothing supports the point. */
int phys_ground(PhysWorld *pw, float x, float y, float z_top, float *ground);
/* First triangle hit along a ray (static, doors, terrain). Returns 1 on hit
 * with distance and the instance (0xFFFFFFFF for terrain). */
int phys_raycast(PhysWorld *pw, const Vec4 *origin, const Vec4 *dir, float max_dist, float *t,
                 uint32_t *inst);

/* Exposed for unit tests: push needed to move a cylinder (axis cx, cy,
 * slab z0..z1, radius r) out of triangle tri. Returns 1 when overlapping. */
int phys_cylinder_triangle(float cx, float cy, float r, float z0, float z1, float v[3][3],
                           const float n[3], float *push_x, float *push_y);

#endif /* PSPWIND_PHYS_H */
