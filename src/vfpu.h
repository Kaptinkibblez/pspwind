/*
 * vfpu.h - 3D math for the Allegrex VFPU.
 *
 * On PSP every routine below is implemented with inline VFPU assembly
 * (lv.q / vmmul.q / vtfm4.q / vdot.t / vcrsp.t / vqmul.q / sv.q ...).
 * The ref_* twins are plain C99 scalar implementations; they are what the
 * public API calls on host builds, and on PSP they are kept as an oracle
 * for the boot-time self-test (selftest.c) that checks VFPU == scalar.
 *
 * Conventions (identical to pspgum / ScePspFMatrix4):
 *   - Mat4 is column-major: x, y, z are basis columns, w is translation
 *   - column vectors: p' = M * p, so mat4_mul(out, a, b) = a * b applies b
 *     first
 *   - right-handed, angles in radians
 *   - Quat is (x, y, z, w), Hamilton product
 *   - planes are (nx, ny, nz, d) with inside where dot(n, p) + d >= 0
 *
 * Every Vec4 / Mat4 / Quat must be 16-byte aligned (lv.q requirement); the
 * types carry the alignment so locals and arena allocations are fine.
 * Thread note: the calling thread must be created with
 * PSP_THREAD_ATTR_VFPU so the VFPU context is saved across switches.
 */
#ifndef PSPWIND_VFPU_H
#define PSPWIND_VFPU_H

#include "common.h"

#define PW_PI 3.14159265358979323846f
#define PW_TWO_PI 6.28318530717958647692f
#define PW_HALF_PI 1.57079632679489661923f
#define PW_DEG2RAD (PW_PI / 180.0f)

typedef struct {
    float x, y, z, w;
} PW_ALIGN(16) Vec4;

typedef Vec4 Quat;

typedef struct {
    Vec4 x, y, z, w;
} PW_ALIGN(64) Mat4;

/* Six clip planes, laid out for two 4-wide VFPU passes (slots 6,7 repeat
 * plane 5 so they never reject). abs_n holds |n| with w = 0 for AABBs. */
enum { FRUSTUM_LEFT, FRUSTUM_RIGHT, FRUSTUM_BOTTOM, FRUSTUM_TOP, FRUSTUM_NEAR, FRUSTUM_FAR };
typedef struct {
    Vec4 plane[8];
    Vec4 abs_n[8];
} PW_ALIGN(64) Frustum;

/* Results of frustum tests. */
enum { CULL_OUTSIDE = 0, CULL_INTERSECT = 1, CULL_INSIDE = 2 };

/* --- scalar ------------------------------------------------------------ */
float pw_sinf(float rad);
float pw_cosf(float rad);
void pw_sincosf(float rad, float *s, float *c);
float pw_sqrtf(float v);
float pw_rsqrtf(float v); /* 1/sqrt(v), v > 0 */
float pw_wrap_angle(float rad); /* to [-pi, pi) */

static inline float pw_absf(float v) { return v < 0.0f ? -v : v; }
static inline float pw_lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* --- Vec4 (xyz used as 3-vectors where the name says *3) ---------------- */
static inline Vec4 vec4(float x, float y, float z, float w)
{
    Vec4 v;
    v.x = x; v.y = y; v.z = z; v.w = w;
    return v;
}

void vec4_add(Vec4 *out, const Vec4 *a, const Vec4 *b);
void vec4_sub(Vec4 *out, const Vec4 *a, const Vec4 *b);
void vec4_scale(Vec4 *out, const Vec4 *a, float s);
void vec4_lerp(Vec4 *out, const Vec4 *a, const Vec4 *b, float t);
float vec4_dot3(const Vec4 *a, const Vec4 *b);
float vec4_dot4(const Vec4 *a, const Vec4 *b);
void vec4_cross3(Vec4 *out, const Vec4 *a, const Vec4 *b); /* out.w = 0 */
float vec4_length3(const Vec4 *a);
float vec4_normalize3(Vec4 *out, const Vec4 *a); /* returns old length; zero stays zero */

/* --- Mat4 --------------------------------------------------------------- */
void mat4_identity(Mat4 *out);
void mat4_copy(Mat4 *out, const Mat4 *m);
void mat4_mul(Mat4 *out, const Mat4 *a, const Mat4 *b);
void mat4_transpose(Mat4 *out, const Mat4 *m);
void mat4_translation(Mat4 *out, float x, float y, float z);
void mat4_scaling(Mat4 *out, float x, float y, float z);
void mat4_rotation_x(Mat4 *out, float rad);
void mat4_rotation_y(Mat4 *out, float rad);
void mat4_rotation_z(Mat4 *out, float rad);
/* In-place post-multiplication, m = m * op (gum semantics). */
void mat4_translate(Mat4 *m, float x, float y, float z);
void mat4_scale(Mat4 *m, float x, float y, float z);
void mat4_rotate_x(Mat4 *m, float rad);
void mat4_rotate_y(Mat4 *m, float rad);
void mat4_rotate_z(Mat4 *m, float rad);
/* GL-style projection (clip z in [-w, w]), as consumed by sceGuSetMatrix. */
void mat4_perspective(Mat4 *out, float fovy_rad, float aspect, float znear, float zfar);
void mat4_ortho(Mat4 *out, float l, float r, float b, float t, float n, float f);
void mat4_look_at(Mat4 *out, const Vec4 *eye, const Vec4 *center, const Vec4 *up);
/* Inverse of a rotation+translation matrix (orthonormal 3x3). */
void mat4_inverse_orthonormal(Mat4 *out, const Mat4 *m);
/* General inverse; returns 0 and writes identity if singular. */
int mat4_inverse(Mat4 *out, const Mat4 *m);
/* T * R(q) * S(uniform s) - node transform from a cooked DMeshNode. */
void mat4_from_trs(Mat4 *out, const Vec4 *pos, const Quat *rot, float scale);
void mat4_transform(Vec4 *out, const Mat4 *m, const Vec4 *v);      /* full 4D */
void mat4_transform_point(Vec4 *out, const Mat4 *m, const Vec4 *p); /* w = 1 */
void mat4_transform_dir(Vec4 *out, const Mat4 *m, const Vec4 *d);   /* w = 0 */
/* Batch transform of packed float[3] points (unaligned arrays). out may == in. */
void mat4_transform_points(float *out, const Mat4 *m, const float *in, uint32_t count);

/* --- Quat ---------------------------------------------------------------- */
static inline void quat_identity(Quat *q) { *q = vec4(0.0f, 0.0f, 0.0f, 1.0f); }
void quat_mul(Quat *out, const Quat *a, const Quat *b);
void quat_normalize(Quat *out, const Quat *q);
void quat_from_axis_angle(Quat *out, const Vec4 *axis, float rad);
void quat_rotate(Vec4 *out, const Quat *q, const Vec4 *v);
void quat_nlerp(Quat *out, const Quat *a, const Quat *b, float t);
void quat_slerp(Quat *out, const Quat *a, const Quat *b, float t);

/* --- culling ------------------------------------------------------------- */
void frustum_from_matrix(Frustum *f, const Mat4 *view_proj);
int frustum_test_sphere(const Frustum *f, const Vec4 *center, float radius);
int frustum_test_aabb(const Frustum *f, const Vec4 *bmin, const Vec4 *bmax);
/* spheres[i] = (cx, cy, cz, r). Writes 1/0 per sphere, returns visible count. */
uint32_t frustum_cull_spheres(const Frustum *f, const Vec4 *spheres, uint32_t count,
                              uint8_t *visible);

/* --- intersection -------------------------------------------------------- */
/* dir must be normalised. On hit *t is the entry distance (0 if inside). */
int ray_sphere(const Vec4 *origin, const Vec4 *dir, const Vec4 *center, float radius,
               float *t);
/* Two-sided Moller-Trumbore. On hit writes t (>= 0) and barycentrics u, v. */
int ray_triangle(const Vec4 *origin, const Vec4 *dir, const Vec4 *v0, const Vec4 *v1,
                 const Vec4 *v2, float *t, float *u, float *v);
int sphere_sphere(const Vec4 *ca, float ra, const Vec4 *cb, float rb);
/* Closest point on triangle abc to p (Ericson, Real-Time Collision Detection). */
void closest_point_triangle(Vec4 *out, const Vec4 *p, const Vec4 *a, const Vec4 *b,
                            const Vec4 *c);

/* --- scalar reference implementations (always available) ---------------- */
float ref_sinf(float rad);
float ref_cosf(float rad);
float ref_sqrtf(float v);
float ref_rsqrtf(float v);
float ref_vec4_dot3(const Vec4 *a, const Vec4 *b);
void ref_vec4_cross3(Vec4 *out, const Vec4 *a, const Vec4 *b);
float ref_vec4_normalize3(Vec4 *out, const Vec4 *a);
void ref_mat4_identity(Mat4 *out);
void ref_mat4_mul(Mat4 *out, const Mat4 *a, const Mat4 *b);
void ref_mat4_transpose(Mat4 *out, const Mat4 *m);
void ref_mat4_rotation_x(Mat4 *out, float rad);
void ref_mat4_rotation_y(Mat4 *out, float rad);
void ref_mat4_rotation_z(Mat4 *out, float rad);
void ref_mat4_inverse_orthonormal(Mat4 *out, const Mat4 *m);
void ref_mat4_transform(Vec4 *out, const Mat4 *m, const Vec4 *v);
void ref_mat4_transform_points(float *out, const Mat4 *m, const float *in, uint32_t count);
void ref_quat_mul(Quat *out, const Quat *a, const Quat *b);
void ref_quat_normalize(Quat *out, const Quat *q);
int ref_frustum_test_sphere(const Frustum *f, const Vec4 *center, float radius);
int ref_frustum_test_aabb(const Frustum *f, const Vec4 *bmin, const Vec4 *bmax);
int ref_ray_sphere(const Vec4 *origin, const Vec4 *dir, const Vec4 *center, float radius,
                   float *t);
int ref_ray_triangle(const Vec4 *origin, const Vec4 *dir, const Vec4 *v0, const Vec4 *v1,
                     const Vec4 *v2, float *t, float *u, float *v);

#endif /* PSPWIND_VFPU_H */
