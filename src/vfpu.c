/*
 * vfpu.c - VFPU math (see vfpu.h for conventions).
 *
 * VFPU register conventions used here (verified against the SDK's
 * libpspgum_vfpu, see docs/VFPU.md):
 *   - lv.q Cm<i>0 loads memory vector i into column i of matrix m, so a
 *     column-major Mat4 maps directly onto Mm00.
 *   - vmmul.q Md, Ms, Mt  computes  D = S * T.
 *   - vtfm4.q Cd, Ms, Cv  computes  d[i] = dot(column i of S, v) = S^T v,
 *     so a regular M * v uses the transposed view: vtfm4.q Cd, E000, Cv.
 *   - vsin/vcos/vrot take angles in quarter turns (x * 2/pi).
 * GCC never allocates VFPU registers, so values may stay in VFPU registers
 * between consecutive asm statements of one function (used by the batch
 * loops to load a matrix once).
 */
#include "vfpu.h"

#include <math.h>
#include <string.h>

/* ======================================================================== */
/* Scalar reference implementations                                         */
/* ======================================================================== */
float ref_sinf(float rad) { return sinf(rad); }
float ref_cosf(float rad) { return cosf(rad); }
float ref_sqrtf(float v) { return sqrtf(v); }
float ref_rsqrtf(float v) { return 1.0f / sqrtf(v); }

float ref_vec4_dot3(const Vec4 *a, const Vec4 *b)
{
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

void ref_vec4_cross3(Vec4 *out, const Vec4 *a, const Vec4 *b)
{
    Vec4 r;
    r.x = a->y * b->z - a->z * b->y;
    r.y = a->z * b->x - a->x * b->z;
    r.z = a->x * b->y - a->y * b->x;
    r.w = 0.0f;
    *out = r;
}

float ref_vec4_normalize3(Vec4 *out, const Vec4 *a)
{
    float len2 = ref_vec4_dot3(a, a);
    float len, inv;
    if (len2 <= 1e-30f) {
        out->x = out->y = out->z = 0.0f;
        out->w = a->w;
        return 0.0f;
    }
    len = sqrtf(len2);
    inv = 1.0f / len;
    out->x = a->x * inv;
    out->y = a->y * inv;
    out->z = a->z * inv;
    out->w = a->w;
    return len;
}

void ref_mat4_identity(Mat4 *out)
{
    memset(out, 0, sizeof(*out));
    out->x.x = out->y.y = out->z.z = out->w.w = 1.0f;
}

void ref_mat4_mul(Mat4 *out, const Mat4 *a, const Mat4 *b)
{
    const float *A = &a->x.x;
    const float *B = &b->x.x;
    float R[16];
    int c, r, k;
    for (c = 0; c < 4; ++c) {
        for (r = 0; r < 4; ++r) {
            float s = 0.0f;
            for (k = 0; k < 4; ++k)
                s += A[k * 4 + r] * B[c * 4 + k];
            R[c * 4 + r] = s;
        }
    }
    memcpy(&out->x.x, R, sizeof(R));
}

void ref_mat4_transpose(Mat4 *out, const Mat4 *m)
{
    const float *M = &m->x.x;
    float R[16];
    int c, r;
    for (c = 0; c < 4; ++c)
        for (r = 0; r < 4; ++r)
            R[c * 4 + r] = M[r * 4 + c];
    memcpy(&out->x.x, R, sizeof(R));
}

void ref_mat4_rotation_x(Mat4 *out, float rad)
{
    float s = sinf(rad), c = cosf(rad);
    ref_mat4_identity(out);
    out->y.y = c;  out->y.z = s;
    out->z.y = -s; out->z.z = c;
}

void ref_mat4_rotation_y(Mat4 *out, float rad)
{
    float s = sinf(rad), c = cosf(rad);
    ref_mat4_identity(out);
    out->x.x = c; out->x.z = -s;
    out->z.x = s; out->z.z = c;
}

void ref_mat4_rotation_z(Mat4 *out, float rad)
{
    float s = sinf(rad), c = cosf(rad);
    ref_mat4_identity(out);
    out->x.x = c;  out->x.y = s;
    out->y.x = -s; out->y.y = c;
}

void ref_mat4_inverse_orthonormal(Mat4 *out, const Mat4 *m)
{
    Mat4 r;
    const Vec4 t = m->w;
    r.x = vec4(m->x.x, m->y.x, m->z.x, 0.0f);
    r.y = vec4(m->x.y, m->y.y, m->z.y, 0.0f);
    r.z = vec4(m->x.z, m->y.z, m->z.z, 0.0f);
    r.w.x = -(m->x.x * t.x + m->x.y * t.y + m->x.z * t.z);
    r.w.y = -(m->y.x * t.x + m->y.y * t.y + m->y.z * t.z);
    r.w.z = -(m->z.x * t.x + m->z.y * t.y + m->z.z * t.z);
    r.w.w = 1.0f;
    *out = r;
}

void ref_mat4_transform(Vec4 *out, const Mat4 *m, const Vec4 *v)
{
    Vec4 r;
    r.x = m->x.x * v->x + m->y.x * v->y + m->z.x * v->z + m->w.x * v->w;
    r.y = m->x.y * v->x + m->y.y * v->y + m->z.y * v->z + m->w.y * v->w;
    r.z = m->x.z * v->x + m->y.z * v->y + m->z.z * v->z + m->w.z * v->w;
    r.w = m->x.w * v->x + m->y.w * v->y + m->z.w * v->z + m->w.w * v->w;
    *out = r;
}

void ref_mat4_transform_points(float *out, const Mat4 *m, const float *in, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; ++i) {
        float x = in[0], y = in[1], z = in[2];
        out[0] = m->x.x * x + m->y.x * y + m->z.x * z + m->w.x;
        out[1] = m->x.y * x + m->y.y * y + m->z.y * z + m->w.y;
        out[2] = m->x.z * x + m->y.z * y + m->z.z * z + m->w.z;
        in += 3;
        out += 3;
    }
}

void ref_quat_mul(Quat *out, const Quat *a, const Quat *b)
{
    Quat r;
    r.x = a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y;
    r.y = a->w * b->y - a->x * b->z + a->y * b->w + a->z * b->x;
    r.z = a->w * b->z + a->x * b->y - a->y * b->x + a->z * b->w;
    r.w = a->w * b->w - a->x * b->x - a->y * b->y - a->z * b->z;
    *out = r;
}

void ref_quat_normalize(Quat *out, const Quat *q)
{
    float len2 = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    float inv;
    if (len2 <= 1e-30f) {
        quat_identity(out);
        return;
    }
    inv = 1.0f / sqrtf(len2);
    *out = vec4(q->x * inv, q->y * inv, q->z * inv, q->w * inv);
}

static int classify_sphere(const float *d, float radius)
{
    int i, result = CULL_INSIDE;
    for (i = 0; i < 6; ++i) {
        if (d[i] < -radius)
            return CULL_OUTSIDE;
        if (d[i] < radius)
            result = CULL_INTERSECT;
    }
    return result;
}

static int classify_box(const float *d, const float *e)
{
    int i, result = CULL_INSIDE;
    for (i = 0; i < 6; ++i) {
        if (d[i] + e[i] < 0.0f)
            return CULL_OUTSIDE;
        if (d[i] - e[i] < 0.0f)
            result = CULL_INTERSECT;
    }
    return result;
}

int ref_frustum_test_sphere(const Frustum *f, const Vec4 *center, float radius)
{
    float d[6];
    int i;
    for (i = 0; i < 6; ++i)
        d[i] = ref_vec4_dot3(&f->plane[i], center) + f->plane[i].w;
    return classify_sphere(d, radius);
}

int ref_frustum_test_aabb(const Frustum *f, const Vec4 *bmin, const Vec4 *bmax)
{
    Vec4 c, e;
    float d[6], r[6];
    int i;
    c = vec4((bmin->x + bmax->x) * 0.5f, (bmin->y + bmax->y) * 0.5f,
             (bmin->z + bmax->z) * 0.5f, 1.0f);
    e = vec4((bmax->x - bmin->x) * 0.5f, (bmax->y - bmin->y) * 0.5f,
             (bmax->z - bmin->z) * 0.5f, 0.0f);
    for (i = 0; i < 6; ++i) {
        d[i] = ref_vec4_dot3(&f->plane[i], &c) + f->plane[i].w;
        r[i] = ref_vec4_dot3(&f->abs_n[i], &e);
    }
    return classify_box(d, r);
}

static int finish_ray_sphere(float b, float mm, float radius, float *t)
{
    float c = mm - radius * radius;
    float disc, s;
    if (c > 0.0f && b > 0.0f)
        return 0; /* outside and pointing away */
    disc = b * b - c;
    if (disc < 0.0f)
        return 0;
    s = -b - sqrtf(disc);
    *t = s < 0.0f ? 0.0f : s;
    return 1;
}

int ref_ray_sphere(const Vec4 *origin, const Vec4 *dir, const Vec4 *center, float radius,
                   float *t)
{
    Vec4 m = vec4(origin->x - center->x, origin->y - center->y, origin->z - center->z, 0.0f);
    return finish_ray_sphere(ref_vec4_dot3(&m, dir), ref_vec4_dot3(&m, &m), radius, t);
}

/* r = (det, u*det, v*det, t*det) */
static int finish_ray_triangle(const Vec4 *r, float *t, float *u, float *v)
{
    float inv, uu, vv, tt;
    if (pw_absf(r->x) < 1e-12f)
        return 0;
    inv = 1.0f / r->x;
    uu = r->y * inv;
    if (uu < 0.0f || uu > 1.0f)
        return 0;
    vv = r->z * inv;
    if (vv < 0.0f || uu + vv > 1.0f)
        return 0;
    tt = r->w * inv;
    if (tt < 0.0f)
        return 0;
    *t = tt;
    if (u)
        *u = uu;
    if (v)
        *v = vv;
    return 1;
}

int ref_ray_triangle(const Vec4 *origin, const Vec4 *dir, const Vec4 *v0, const Vec4 *v1,
                     const Vec4 *v2, float *t, float *u, float *v)
{
    Vec4 e1, e2, p, tv, q, r;
    e1 = vec4(v1->x - v0->x, v1->y - v0->y, v1->z - v0->z, 0.0f);
    e2 = vec4(v2->x - v0->x, v2->y - v0->y, v2->z - v0->z, 0.0f);
    ref_vec4_cross3(&p, dir, &e2);
    tv = vec4(origin->x - v0->x, origin->y - v0->y, origin->z - v0->z, 0.0f);
    ref_vec4_cross3(&q, &tv, &e1);
    r.x = ref_vec4_dot3(&e1, &p);
    r.y = ref_vec4_dot3(&tv, &p);
    r.z = ref_vec4_dot3(dir, &q);
    r.w = ref_vec4_dot3(&e2, &q);
    return finish_ray_triangle(&r, t, u, v);
}

/* ======================================================================== */
/* Shared helpers                                                           */
/* ======================================================================== */
float pw_wrap_angle(float rad)
{
    if (rad >= -PW_PI && rad < PW_PI)
        return rad;
    return rad - PW_TWO_PI * floorf((rad + PW_PI) / PW_TWO_PI);
}

/* ======================================================================== */
/* Public API - VFPU (PSP)                                                  */
/* ======================================================================== */
#if PW_PSP

static const float k_half = 0.5f;

float pw_sinf(float rad)
{
    float r;
    rad = pw_wrap_angle(rad);
    __asm__ volatile(
        "mtv     %1, S000\n"
        "vcst.s  S001, VFPU_2_PI\n"
        "vmul.s  S000, S000, S001\n"
        "vsin.s  S002, S000\n"
        "mfv     %0, S002\n"
        : "=r"(r) : "r"(rad));
    return r;
}

float pw_cosf(float rad)
{
    float r;
    rad = pw_wrap_angle(rad);
    __asm__ volatile(
        "mtv     %1, S000\n"
        "vcst.s  S001, VFPU_2_PI\n"
        "vmul.s  S000, S000, S001\n"
        "vcos.s  S002, S000\n"
        "mfv     %0, S002\n"
        : "=r"(r) : "r"(rad));
    return r;
}

void pw_sincosf(float rad, float *s, float *c)
{
    float rs, rc;
    rad = pw_wrap_angle(rad);
    __asm__ volatile(
        "mtv     %2, S000\n"
        "vcst.s  S001, VFPU_2_PI\n"
        "vmul.s  S000, S000, S001\n"
        "vsin.s  S002, S000\n"
        "vcos.s  S003, S000\n"
        "mfv     %0, S002\n"
        "mfv     %1, S003\n"
        : "=r"(rs), "=r"(rc) : "r"(rad));
    *s = rs;
    *c = rc;
}

float pw_sqrtf(float v)
{
    float r;
    __asm__ volatile(
        "mtv     %1, S000\n"
        "vsqrt.s S000, S000\n"
        "mfv     %0, S000\n"
        : "=r"(r) : "r"(v));
    return r;
}

float pw_rsqrtf(float v)
{
    float r;
    __asm__ volatile(
        "mtv     %1, S000\n"
        "vrsq.s  S000, S000\n"
        "mfv     %0, S000\n"
        : "=r"(r) : "r"(v));
    return r;
}

void vec4_add(Vec4 *out, const Vec4 *a, const Vec4 *b)
{
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "lv.q    C010, 0(%2)\n"
        "vadd.q  C020, C000, C010\n"
        "sv.q    C020, 0(%0)\n"
        : : "r"(out), "r"(a), "r"(b) : "memory");
}

void vec4_sub(Vec4 *out, const Vec4 *a, const Vec4 *b)
{
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "lv.q    C010, 0(%2)\n"
        "vsub.q  C020, C000, C010\n"
        "sv.q    C020, 0(%0)\n"
        : : "r"(out), "r"(a), "r"(b) : "memory");
}

void vec4_scale(Vec4 *out, const Vec4 *a, float s)
{
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "mtv     %2, S010\n"
        "vscl.q  C020, C000, S010\n"
        "sv.q    C020, 0(%0)\n"
        : : "r"(out), "r"(a), "r"(s) : "memory");
}

void vec4_lerp(Vec4 *out, const Vec4 *a, const Vec4 *b, float t)
{
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "lv.q    C010, 0(%2)\n"
        "mtv     %3, S030\n"
        "vsub.q  C020, C010, C000\n"
        "vscl.q  C020, C020, S030\n"
        "vadd.q  C020, C000, C020\n"
        "sv.q    C020, 0(%0)\n"
        : : "r"(out), "r"(a), "r"(b), "r"(t) : "memory");
}

float vec4_dot3(const Vec4 *a, const Vec4 *b)
{
    float r;
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "lv.q    C010, 0(%2)\n"
        "vdot.t  S020, C000, C010\n"
        "mfv     %0, S020\n"
        : "=r"(r) : "r"(a), "r"(b) : "memory");
    return r;
}

float vec4_dot4(const Vec4 *a, const Vec4 *b)
{
    float r;
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "lv.q    C010, 0(%2)\n"
        "vdot.q  S020, C000, C010\n"
        "mfv     %0, S020\n"
        : "=r"(r) : "r"(a), "r"(b) : "memory");
    return r;
}

void vec4_cross3(Vec4 *out, const Vec4 *a, const Vec4 *b)
{
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "lv.q    C010, 0(%2)\n"
        "vcrsp.t C020, C000, C010\n"
        "vzero.s S023\n"
        "sv.q    C020, 0(%0)\n"
        : : "r"(out), "r"(a), "r"(b) : "memory");
}

float vec4_length3(const Vec4 *a)
{
    float r;
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "vdot.t  S010, C000, C000\n"
        "vsqrt.s S010, S010\n"
        "mfv     %0, S010\n"
        : "=r"(r) : "r"(a) : "memory");
    return r;
}

float vec4_normalize3(Vec4 *out, const Vec4 *a)
{
    float len2, len;
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "vdot.t  S010, C000, C000\n"
        "mfv     %0, S010\n"
        : "=r"(len2) : "r"(a) : "memory");
    if (len2 <= 1e-30f) {
        float w = a->w;
        out->x = out->y = out->z = 0.0f;
        out->w = w;
        return 0.0f;
    }
    /* C000 and S010 are still live from the block above. */
    __asm__ volatile(
        "vrsq.s  S011, S010\n"
        "vsqrt.s S012, S010\n"
        "vscl.t  C000, C000, S011\n"
        "sv.q    C000, 0(%1)\n"
        "mfv     %0, S012\n"
        : "=r"(len) : "r"(out) : "memory");
    return len;
}

void mat4_identity(Mat4 *out)
{
    __asm__ volatile(
        "vmidt.q M000\n"
        "sv.q    C000,  0(%0)\n"
        "sv.q    C010, 16(%0)\n"
        "sv.q    C020, 32(%0)\n"
        "sv.q    C030, 48(%0)\n"
        : : "r"(out) : "memory");
}

void mat4_mul(Mat4 *out, const Mat4 *a, const Mat4 *b)
{
    __asm__ volatile(
        "lv.q    C000,  0(%1)\n"
        "lv.q    C010, 16(%1)\n"
        "lv.q    C020, 32(%1)\n"
        "lv.q    C030, 48(%1)\n"
        "lv.q    C100,  0(%2)\n"
        "lv.q    C110, 16(%2)\n"
        "lv.q    C120, 32(%2)\n"
        "lv.q    C130, 48(%2)\n"
        "vmmul.q M200, M000, M100\n"
        "sv.q    C200,  0(%0)\n"
        "sv.q    C210, 16(%0)\n"
        "sv.q    C220, 32(%0)\n"
        "sv.q    C230, 48(%0)\n"
        : : "r"(out), "r"(a), "r"(b) : "memory");
}

void mat4_transpose(Mat4 *out, const Mat4 *m)
{
    __asm__ volatile(
        "lv.q    C000,  0(%1)\n"
        "lv.q    C010, 16(%1)\n"
        "lv.q    C020, 32(%1)\n"
        "lv.q    C030, 48(%1)\n"
        "vmmov.q M100, E000\n"
        "sv.q    C100,  0(%0)\n"
        "sv.q    C110, 16(%0)\n"
        "sv.q    C120, 32(%0)\n"
        "sv.q    C130, 48(%0)\n"
        : : "r"(out), "r"(m) : "memory");
}

void mat4_rotation_x(Mat4 *out, float rad)
{
    rad = pw_wrap_angle(rad);
    __asm__ volatile(
        "vmidt.q M000\n"
        "mtv     %1, S100\n"
        "vcst.s  S101, VFPU_2_PI\n"
        "vmul.s  S100, S101, S100\n"
        "vrot.q  C010, S100, [0, c, s, 0]\n"
        "vrot.q  C020, S100, [0, -s, c, 0]\n"
        "sv.q    C000,  0(%0)\n"
        "sv.q    C010, 16(%0)\n"
        "sv.q    C020, 32(%0)\n"
        "sv.q    C030, 48(%0)\n"
        : : "r"(out), "r"(rad) : "memory");
}

void mat4_rotation_y(Mat4 *out, float rad)
{
    rad = pw_wrap_angle(rad);
    __asm__ volatile(
        "vmidt.q M000\n"
        "mtv     %1, S100\n"
        "vcst.s  S101, VFPU_2_PI\n"
        "vmul.s  S100, S101, S100\n"
        "vrot.q  C000, S100, [c, 0, -s, 0]\n"
        "vrot.q  C020, S100, [s, 0, c, 0]\n"
        "sv.q    C000,  0(%0)\n"
        "sv.q    C010, 16(%0)\n"
        "sv.q    C020, 32(%0)\n"
        "sv.q    C030, 48(%0)\n"
        : : "r"(out), "r"(rad) : "memory");
}

void mat4_rotation_z(Mat4 *out, float rad)
{
    rad = pw_wrap_angle(rad);
    __asm__ volatile(
        "vmidt.q M000\n"
        "mtv     %1, S100\n"
        "vcst.s  S101, VFPU_2_PI\n"
        "vmul.s  S100, S101, S100\n"
        "vrot.q  C000, S100, [c, s, 0, 0]\n"
        "vrot.q  C010, S100, [-s, c, 0, 0]\n"
        "sv.q    C000,  0(%0)\n"
        "sv.q    C010, 16(%0)\n"
        "sv.q    C020, 32(%0)\n"
        "sv.q    C030, 48(%0)\n"
        : : "r"(out), "r"(rad) : "memory");
}

/* m = m * R: rotation built in M000, m in M200, product in M100. */
#define ROTATE_POST(m, rad, ROT_ASM)                                          \
    __asm__ volatile(                                                         \
        "lv.q    C200,  0(%0)\n"                                              \
        "lv.q    C210, 16(%0)\n"                                              \
        "lv.q    C220, 32(%0)\n"                                              \
        "lv.q    C230, 48(%0)\n"                                              \
        "vmidt.q M000\n"                                                      \
        "mtv     %1, S100\n"                                                  \
        "vcst.s  S101, VFPU_2_PI\n"                                           \
        "vmul.s  S100, S101, S100\n"                                          \
        ROT_ASM                                                               \
        "vmmul.q M100, M200, M000\n"                                          \
        "sv.q    C100,  0(%0)\n"                                              \
        "sv.q    C110, 16(%0)\n"                                              \
        "sv.q    C120, 32(%0)\n"                                              \
        "sv.q    C130, 48(%0)\n"                                              \
        : : "r"(m), "r"(rad) : "memory")

void mat4_rotate_x(Mat4 *m, float rad)
{
    rad = pw_wrap_angle(rad);
    ROTATE_POST(m, rad, "vrot.q  C010, S100, [0, c, s, 0]\n"
                        "vrot.q  C020, S100, [0, -s, c, 0]\n");
}

void mat4_rotate_y(Mat4 *m, float rad)
{
    rad = pw_wrap_angle(rad);
    ROTATE_POST(m, rad, "vrot.q  C000, S100, [c, 0, -s, 0]\n"
                        "vrot.q  C020, S100, [s, 0, c, 0]\n");
}

void mat4_rotate_z(Mat4 *m, float rad)
{
    rad = pw_wrap_angle(rad);
    ROTATE_POST(m, rad, "vrot.q  C000, S100, [c, s, 0, 0]\n"
                        "vrot.q  C010, S100, [-s, c, 0, 0]\n");
}

void mat4_translate(Mat4 *m, float x, float y, float z)
{
    Vec4 t = vec4(x, y, z, 0.0f);
    __asm__ volatile(
        "lv.q    C030,  0(%1)\n"
        "lv.q    C100,  0(%0)\n"
        "lv.q    C110, 16(%0)\n"
        "lv.q    C120, 32(%0)\n"
        "lv.q    C130, 48(%0)\n"
        "vscl.q  C000, C100, S030\n"
        "vscl.q  C010, C110, S031\n"
        "vscl.q  C020, C120, S032\n"
        "vadd.q  C130, C130, C000\n"
        "vadd.q  C130, C130, C010\n"
        "vadd.q  C130, C130, C020\n"
        "sv.q    C130, 48(%0)\n"
        : : "r"(m), "r"(&t) : "memory");
}

void mat4_scale(Mat4 *m, float x, float y, float z)
{
    Vec4 s = vec4(x, y, z, 1.0f);
    __asm__ volatile(
        "lv.q    C030,  0(%1)\n"
        "lv.q    C100,  0(%0)\n"
        "lv.q    C110, 16(%0)\n"
        "lv.q    C120, 32(%0)\n"
        "vscl.q  C100, C100, S030\n"
        "vscl.q  C110, C110, S031\n"
        "vscl.q  C120, C120, S032\n"
        "sv.q    C100,  0(%0)\n"
        "sv.q    C110, 16(%0)\n"
        "sv.q    C120, 32(%0)\n"
        : : "r"(m), "r"(&s) : "memory");
}

void mat4_inverse_orthonormal(Mat4 *out, const Mat4 *m)
{
    __asm__ volatile(
        "lv.q    C200,  0(%1)\n"
        "lv.q    C210, 16(%1)\n"
        "lv.q    C220, 32(%1)\n"
        "lv.q    C230, 48(%1)\n"
        "vmidt.q M000\n"
        "vmmov.t M000, E200\n"
        "vneg.t  C100, C230\n"
        "vtfm3.t C030, M200, C100\n"
        "sv.q    C000,  0(%0)\n"
        "sv.q    C010, 16(%0)\n"
        "sv.q    C020, 32(%0)\n"
        "sv.q    C030, 48(%0)\n"
        : : "r"(out), "r"(m) : "memory");
}

void mat4_transform(Vec4 *out, const Mat4 *m, const Vec4 *v)
{
    __asm__ volatile(
        "lv.q    C000,  0(%1)\n"
        "lv.q    C010, 16(%1)\n"
        "lv.q    C020, 32(%1)\n"
        "lv.q    C030, 48(%1)\n"
        "lv.q    C100,  0(%2)\n"
        "vtfm4.q C110, E000, C100\n"
        "sv.q    C110, 0(%0)\n"
        : : "r"(out), "r"(m), "r"(v) : "memory");
}

void mat4_transform_point(Vec4 *out, const Mat4 *m, const Vec4 *p)
{
    __asm__ volatile(
        "lv.q    C000,  0(%1)\n"
        "lv.q    C010, 16(%1)\n"
        "lv.q    C020, 32(%1)\n"
        "lv.q    C030, 48(%1)\n"
        "lv.q    C100,  0(%2)\n"
        "vone.s  S103\n"
        "vtfm4.q C110, E000, C100\n"
        "sv.q    C110, 0(%0)\n"
        : : "r"(out), "r"(m), "r"(p) : "memory");
}

void mat4_transform_dir(Vec4 *out, const Mat4 *m, const Vec4 *d)
{
    __asm__ volatile(
        "lv.q    C000,  0(%1)\n"
        "lv.q    C010, 16(%1)\n"
        "lv.q    C020, 32(%1)\n"
        "lv.q    C030, 48(%1)\n"
        "lv.q    C100,  0(%2)\n"
        "vzero.s S103\n"
        "vtfm4.q C110, E000, C100\n"
        "sv.q    C110, 0(%0)\n"
        : : "r"(out), "r"(m), "r"(d) : "memory");
}

void mat4_transform_points(float *out, const Mat4 *m, const float *in, uint32_t count)
{
    uint32_t i;
    __asm__ volatile(
        "lv.q    C000,  0(%0)\n"
        "lv.q    C010, 16(%0)\n"
        "lv.q    C020, 32(%0)\n"
        "lv.q    C030, 48(%0)\n"
        : : "r"(m) : "memory");
    for (i = 0; i < count; ++i) {
        __asm__ volatile(
            "lv.s    S100, 0(%1)\n"
            "lv.s    S101, 4(%1)\n"
            "lv.s    S102, 8(%1)\n"
            "vone.s  S103\n"
            "vtfm4.q C110, E000, C100\n"
            "sv.s    S110, 0(%0)\n"
            "sv.s    S111, 4(%0)\n"
            "sv.s    S112, 8(%0)\n"
            : : "r"(out), "r"(in) : "memory");
        in += 3;
        out += 3;
    }
}

void quat_mul(Quat *out, const Quat *a, const Quat *b)
{
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "lv.q    C010, 0(%2)\n"
        "vqmul.q C020, C000, C010\n"
        "sv.q    C020, 0(%0)\n"
        : : "r"(out), "r"(a), "r"(b) : "memory");
}

void quat_normalize(Quat *out, const Quat *q)
{
    float len2;
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "vdot.q  S010, C000, C000\n"
        "mfv     %0, S010\n"
        : "=r"(len2) : "r"(q) : "memory");
    if (len2 <= 1e-30f) {
        quat_identity(out);
        return;
    }
    __asm__ volatile(
        "vrsq.s  S010, S010\n"
        "vscl.q  C000, C000, S010\n"
        "sv.q    C000, 0(%0)\n"
        : : "r"(out) : "memory");
}

static void frustum_load_planes(const Frustum *f)
{
    __asm__ volatile(
        "lv.q    C100,   0(%0)\n"
        "lv.q    C110,  16(%0)\n"
        "lv.q    C120,  32(%0)\n"
        "lv.q    C130,  48(%0)\n"
        "lv.q    C200,  64(%0)\n"
        "lv.q    C210,  80(%0)\n"
        "lv.q    C220,  96(%0)\n"
        "lv.q    C230, 112(%0)\n"
        : : "r"(f->plane) : "memory");
}

/* Planes must already be in M100/M200. d receives 8 signed distances. */
static void frustum_distances(Vec4 *d /* [2] */, const Vec4 *p)
{
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"
        "vone.s  S003\n"
        "vtfm4.q C010, M100, C000\n"
        "vtfm4.q C020, M200, C000\n"
        "sv.q    C010,  0(%0)\n"
        "sv.q    C020, 16(%0)\n"
        : : "r"(d), "r"(p) : "memory");
}

int frustum_test_sphere(const Frustum *f, const Vec4 *center, float radius)
{
    Vec4 d[2];
    frustum_load_planes(f);
    frustum_distances(d, center);
    return classify_sphere(&d[0].x, radius);
}

uint32_t frustum_cull_spheres(const Frustum *f, const Vec4 *spheres, uint32_t count,
                              uint8_t *visible)
{
    Vec4 d[2];
    uint32_t i, n = 0;
    frustum_load_planes(f);
    for (i = 0; i < count; ++i) {
        frustum_distances(d, &spheres[i]);
        visible[i] = (uint8_t)(classify_sphere(&d[0].x, spheres[i].w) != CULL_OUTSIDE);
        n += visible[i];
    }
    return n;
}

int frustum_test_aabb(const Frustum *f, const Vec4 *bmin, const Vec4 *bmax)
{
    Vec4 r[4]; /* d0-3, e0-3, d4-7, e4-7 */
    frustum_load_planes(f);
    __asm__ volatile(
        "lv.q    C300,   0(%1)\n"
        "lv.q    C310,  16(%1)\n"
        "lv.q    C320,  32(%1)\n"
        "lv.q    C330,  48(%1)\n"
        "lv.q    C400,  64(%1)\n"
        "lv.q    C410,  80(%1)\n"
        "lv.q    C420,  96(%1)\n"
        "lv.q    C430, 112(%1)\n"
        "lv.q    C000, 0(%2)\n"
        "lv.q    C010, 0(%3)\n"
        "lv.s    S600, 0(%4)\n"
        "vadd.q  C020, C000, C010\n"
        "vsub.q  C030, C010, C000\n"
        "vscl.q  C020, C020, S600\n"
        "vscl.q  C030, C030, S600\n"
        "vone.s  S023\n"
        "vzero.s S033\n"
        "vtfm4.q C510, M100, C020\n"
        "vtfm4.q C520, M300, C030\n"
        "vtfm4.q C710, M200, C020\n"
        "vtfm4.q C720, M400, C030\n"
        "sv.q    C510,  0(%0)\n"
        "sv.q    C520, 16(%0)\n"
        "sv.q    C710, 32(%0)\n"
        "sv.q    C720, 48(%0)\n"
        : : "r"(r), "r"(f->abs_n), "r"(bmin), "r"(bmax), "r"(&k_half) : "memory");
    {
        float d[6], e[6];
        d[0] = r[0].x; d[1] = r[0].y; d[2] = r[0].z; d[3] = r[0].w;
        d[4] = r[2].x; d[5] = r[2].y;
        e[0] = r[1].x; e[1] = r[1].y; e[2] = r[1].z; e[3] = r[1].w;
        e[4] = r[3].x; e[5] = r[3].y;
        return classify_box(d, e);
    }
}

int ray_sphere(const Vec4 *origin, const Vec4 *dir, const Vec4 *center, float radius,
               float *t)
{
    float b, mm;
    __asm__ volatile(
        "lv.q    C000, 0(%2)\n"
        "lv.q    C010, 0(%3)\n"
        "lv.q    C020, 0(%4)\n"
        "vsub.t  C030, C000, C020\n"
        "vdot.t  S100, C030, C010\n"
        "vdot.t  S101, C030, C030\n"
        "mfv     %0, S100\n"
        "mfv     %1, S101\n"
        : "=r"(b), "=r"(mm) : "r"(origin), "r"(dir), "r"(center) : "memory");
    return finish_ray_sphere(b, mm, radius, t);
}

int ray_triangle(const Vec4 *origin, const Vec4 *dir, const Vec4 *v0, const Vec4 *v1,
                 const Vec4 *v2, float *t, float *u, float *v)
{
    Vec4 r;
    __asm__ volatile(
        "lv.q    C000, 0(%1)\n"           /* origin */
        "lv.q    C010, 0(%2)\n"           /* dir */
        "lv.q    C020, 0(%3)\n"           /* v0 */
        "lv.q    C030, 0(%4)\n"           /* v1 */
        "lv.q    C100, 0(%5)\n"           /* v2 */
        "vsub.t  C110, C030, C020\n"      /* e1 */
        "vsub.t  C120, C100, C020\n"      /* e2 */
        "vcrsp.t C130, C010, C120\n"      /* p = dir x e2 */
        "vsub.t  C210, C000, C020\n"      /* tvec */
        "vcrsp.t C220, C210, C110\n"      /* q = tvec x e1 */
        "vdot.t  S200, C110, C130\n"      /* det */
        "vdot.t  S201, C210, C130\n"      /* u * det */
        "vdot.t  S202, C010, C220\n"      /* v * det */
        "vdot.t  S203, C120, C220\n"      /* t * det */
        "sv.q    C200, 0(%0)\n"
        : : "r"(&r), "r"(origin), "r"(dir), "r"(v0), "r"(v1), "r"(v2) : "memory");
    return finish_ray_triangle(&r, t, u, v);
}

#else /* ---------------------------------------------------------- host -- */

float pw_sinf(float rad) { return ref_sinf(pw_wrap_angle(rad)); }
float pw_cosf(float rad) { return ref_cosf(pw_wrap_angle(rad)); }
void pw_sincosf(float rad, float *s, float *c)
{
    rad = pw_wrap_angle(rad);
    *s = sinf(rad);
    *c = cosf(rad);
}
float pw_sqrtf(float v) { return ref_sqrtf(v); }
float pw_rsqrtf(float v) { return ref_rsqrtf(v); }

void vec4_add(Vec4 *out, const Vec4 *a, const Vec4 *b)
{
    *out = vec4(a->x + b->x, a->y + b->y, a->z + b->z, a->w + b->w);
}
void vec4_sub(Vec4 *out, const Vec4 *a, const Vec4 *b)
{
    *out = vec4(a->x - b->x, a->y - b->y, a->z - b->z, a->w - b->w);
}
void vec4_scale(Vec4 *out, const Vec4 *a, float s)
{
    *out = vec4(a->x * s, a->y * s, a->z * s, a->w * s);
}
void vec4_lerp(Vec4 *out, const Vec4 *a, const Vec4 *b, float t)
{
    *out = vec4(a->x + (b->x - a->x) * t, a->y + (b->y - a->y) * t,
                a->z + (b->z - a->z) * t, a->w + (b->w - a->w) * t);
}
float vec4_dot3(const Vec4 *a, const Vec4 *b) { return ref_vec4_dot3(a, b); }
float vec4_dot4(const Vec4 *a, const Vec4 *b)
{
    return a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w;
}
void vec4_cross3(Vec4 *out, const Vec4 *a, const Vec4 *b) { ref_vec4_cross3(out, a, b); }
float vec4_length3(const Vec4 *a) { return sqrtf(ref_vec4_dot3(a, a)); }
float vec4_normalize3(Vec4 *out, const Vec4 *a) { return ref_vec4_normalize3(out, a); }

void mat4_identity(Mat4 *out) { ref_mat4_identity(out); }
void mat4_mul(Mat4 *out, const Mat4 *a, const Mat4 *b) { ref_mat4_mul(out, a, b); }
void mat4_transpose(Mat4 *out, const Mat4 *m) { ref_mat4_transpose(out, m); }
void mat4_rotation_x(Mat4 *out, float rad) { ref_mat4_rotation_x(out, pw_wrap_angle(rad)); }
void mat4_rotation_y(Mat4 *out, float rad) { ref_mat4_rotation_y(out, pw_wrap_angle(rad)); }
void mat4_rotation_z(Mat4 *out, float rad) { ref_mat4_rotation_z(out, pw_wrap_angle(rad)); }

void mat4_rotate_x(Mat4 *m, float rad)
{
    Mat4 r;
    ref_mat4_rotation_x(&r, rad);
    ref_mat4_mul(m, m, &r);
}
void mat4_rotate_y(Mat4 *m, float rad)
{
    Mat4 r;
    ref_mat4_rotation_y(&r, rad);
    ref_mat4_mul(m, m, &r);
}
void mat4_rotate_z(Mat4 *m, float rad)
{
    Mat4 r;
    ref_mat4_rotation_z(&r, rad);
    ref_mat4_mul(m, m, &r);
}

void mat4_translate(Mat4 *m, float x, float y, float z)
{
    m->w.x += m->x.x * x + m->y.x * y + m->z.x * z;
    m->w.y += m->x.y * x + m->y.y * y + m->z.y * z;
    m->w.z += m->x.z * x + m->y.z * y + m->z.z * z;
    m->w.w += m->x.w * x + m->y.w * y + m->z.w * z;
}

void mat4_scale(Mat4 *m, float x, float y, float z)
{
    vec4_scale(&m->x, &m->x, x);
    vec4_scale(&m->y, &m->y, y);
    vec4_scale(&m->z, &m->z, z);
}

void mat4_inverse_orthonormal(Mat4 *out, const Mat4 *m) { ref_mat4_inverse_orthonormal(out, m); }
void mat4_transform(Vec4 *out, const Mat4 *m, const Vec4 *v) { ref_mat4_transform(out, m, v); }
void mat4_transform_point(Vec4 *out, const Mat4 *m, const Vec4 *p)
{
    Vec4 q = *p;
    q.w = 1.0f;
    ref_mat4_transform(out, m, &q);
}
void mat4_transform_dir(Vec4 *out, const Mat4 *m, const Vec4 *d)
{
    Vec4 q = *d;
    q.w = 0.0f;
    ref_mat4_transform(out, m, &q);
}
void mat4_transform_points(float *out, const Mat4 *m, const float *in, uint32_t count)
{
    ref_mat4_transform_points(out, m, in, count);
}

void quat_mul(Quat *out, const Quat *a, const Quat *b) { ref_quat_mul(out, a, b); }
void quat_normalize(Quat *out, const Quat *q) { ref_quat_normalize(out, q); }

int frustum_test_sphere(const Frustum *f, const Vec4 *center, float radius)
{
    return ref_frustum_test_sphere(f, center, radius);
}

uint32_t frustum_cull_spheres(const Frustum *f, const Vec4 *spheres, uint32_t count,
                              uint8_t *visible)
{
    uint32_t i, n = 0;
    for (i = 0; i < count; ++i) {
        visible[i] = (uint8_t)(ref_frustum_test_sphere(f, &spheres[i], spheres[i].w) !=
                               CULL_OUTSIDE);
        n += visible[i];
    }
    return n;
}

int frustum_test_aabb(const Frustum *f, const Vec4 *bmin, const Vec4 *bmax)
{
    return ref_frustum_test_aabb(f, bmin, bmax);
}

int ray_sphere(const Vec4 *origin, const Vec4 *dir, const Vec4 *center, float radius,
               float *t)
{
    return ref_ray_sphere(origin, dir, center, radius, t);
}

int ray_triangle(const Vec4 *origin, const Vec4 *dir, const Vec4 *v0, const Vec4 *v1,
                 const Vec4 *v2, float *t, float *u, float *v)
{
    return ref_ray_triangle(origin, dir, v0, v1, v2, t, u, v);
}

#endif /* PW_PSP */

/* ======================================================================== */
/* Platform independent (built on the primitives above)                     */
/* ======================================================================== */
void mat4_copy(Mat4 *out, const Mat4 *m) { *out = *m; }

void mat4_translation(Mat4 *out, float x, float y, float z)
{
    mat4_identity(out);
    out->w = vec4(x, y, z, 1.0f);
}

void mat4_scaling(Mat4 *out, float x, float y, float z)
{
    mat4_identity(out);
    out->x.x = x;
    out->y.y = y;
    out->z.z = z;
}

void mat4_perspective(Mat4 *out, float fovy_rad, float aspect, float znear, float zfar)
{
    float s, c, f, nf;
    pw_sincosf(fovy_rad * 0.5f, &s, &c);
    f = c / s;
    nf = 1.0f / (znear - zfar);
    memset(out, 0, sizeof(*out));
    out->x.x = f / aspect;
    out->y.y = f;
    out->z.z = (zfar + znear) * nf;
    out->z.w = -1.0f;
    out->w.z = 2.0f * zfar * znear * nf;
}

void mat4_ortho(Mat4 *out, float l, float r, float b, float t, float n, float f)
{
    memset(out, 0, sizeof(*out));
    out->x.x = 2.0f / (r - l);
    out->y.y = 2.0f / (t - b);
    out->z.z = -2.0f / (f - n);
    out->w.x = -(r + l) / (r - l);
    out->w.y = -(t + b) / (t - b);
    out->w.z = -(f + n) / (f - n);
    out->w.w = 1.0f;
}

void mat4_look_at(Mat4 *out, const Vec4 *eye, const Vec4 *center, const Vec4 *up)
{
    Vec4 f, s, u;
    vec4_sub(&f, center, eye);
    f.w = 0.0f;
    vec4_normalize3(&f, &f);
    vec4_cross3(&s, &f, up);
    vec4_normalize3(&s, &s);
    vec4_cross3(&u, &s, &f);
    out->x = vec4(s.x, u.x, -f.x, 0.0f);
    out->y = vec4(s.y, u.y, -f.y, 0.0f);
    out->z = vec4(s.z, u.z, -f.z, 0.0f);
    out->w = vec4(-vec4_dot3(&s, eye), -vec4_dot3(&u, eye), vec4_dot3(&f, eye), 1.0f);
}

int mat4_inverse(Mat4 *out, const Mat4 *m)
{
    const float *a = &m->x.x;
    float inv[16], det;
    int i;
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] +
             a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] -
             a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] +
             a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] -
              a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] -
             a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] +
             a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] -
             a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] +
              a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] +
             a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] -
             a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] +
              a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] -
              a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] -
             a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] +
             a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] -
              a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] +
              a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (pw_absf(det) < 1e-20f) {
        mat4_identity(out);
        return 0;
    }
    det = 1.0f / det;
    for (i = 0; i < 16; ++i)
        inv[i] *= det;
    memcpy(&out->x.x, inv, sizeof(inv));
    return 1;
}

void mat4_from_trs(Mat4 *out, const Vec4 *pos, const Quat *q, float s)
{
    float xx = q->x * q->x, yy = q->y * q->y, zz = q->z * q->z;
    float xy = q->x * q->y, xz = q->x * q->z, yz = q->y * q->z;
    float wx = q->w * q->x, wy = q->w * q->y, wz = q->w * q->z;
    out->x = vec4((1.0f - 2.0f * (yy + zz)) * s, 2.0f * (xy + wz) * s, 2.0f * (xz - wy) * s, 0.0f);
    out->y = vec4(2.0f * (xy - wz) * s, (1.0f - 2.0f * (xx + zz)) * s, 2.0f * (yz + wx) * s, 0.0f);
    out->z = vec4(2.0f * (xz + wy) * s, 2.0f * (yz - wx) * s, (1.0f - 2.0f * (xx + yy)) * s, 0.0f);
    out->w = vec4(pos->x, pos->y, pos->z, 1.0f);
}

void quat_from_axis_angle(Quat *out, const Vec4 *axis, float rad)
{
    Vec4 n;
    float s, c;
    vec4_normalize3(&n, axis);
    pw_sincosf(rad * 0.5f, &s, &c);
    *out = vec4(n.x * s, n.y * s, n.z * s, c);
}

void quat_rotate(Vec4 *out, const Quat *q, const Vec4 *v)
{
    /* v' = v + w*t + q.xyz x t, with t = 2 * (q.xyz x v) */
    Vec4 qv = vec4(q->x, q->y, q->z, 0.0f), t, qt;
    float w = v->w;
    vec4_cross3(&t, &qv, v);
    vec4_scale(&t, &t, 2.0f);
    vec4_cross3(&qt, &qv, &t);
    *out = vec4(v->x + q->w * t.x + qt.x, v->y + q->w * t.y + qt.y,
                v->z + q->w * t.z + qt.z, w);
}

void quat_nlerp(Quat *out, const Quat *a, const Quat *b, float t)
{
    Quat bb = *b;
    if (vec4_dot4(a, b) < 0.0f)
        bb = vec4(-b->x, -b->y, -b->z, -b->w);
    vec4_lerp(out, a, &bb, t);
    quat_normalize(out, out);
}

void quat_slerp(Quat *out, const Quat *a, const Quat *b, float t)
{
    Quat bb = *b;
    float cosom = vec4_dot4(a, b), omega, sinom, s0, s1;
    if (cosom < 0.0f) {
        cosom = -cosom;
        bb = vec4(-b->x, -b->y, -b->z, -b->w);
    }
    if (cosom > 0.9995f) {
        quat_nlerp(out, a, &bb, t);
        return;
    }
    omega = acosf(cosom);
    sinom = pw_sinf(omega);
    s0 = pw_sinf((1.0f - t) * omega) / sinom;
    s1 = pw_sinf(t * omega) / sinom;
    *out = vec4(a->x * s0 + bb.x * s1, a->y * s0 + bb.y * s1, a->z * s0 + bb.z * s1,
                a->w * s0 + bb.w * s1);
}

void frustum_from_matrix(Frustum *f, const Mat4 *vp)
{
    const float *m = &vp->x.x; /* element (row r, col c) = m[c*4 + r] */
    int i, sign;
    for (i = 0; i < 6; ++i) {
        int axis = i >> 1;
        Vec4 p;
        float len;
        sign = (i & 1) ? -1 : 1;
        p.x = m[0 * 4 + 3] + (float)sign * m[0 * 4 + axis];
        p.y = m[1 * 4 + 3] + (float)sign * m[1 * 4 + axis];
        p.z = m[2 * 4 + 3] + (float)sign * m[2 * 4 + axis];
        p.w = m[3 * 4 + 3] + (float)sign * m[3 * 4 + axis];
        len = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len > 1e-20f) {
            float inv = 1.0f / len;
            p.x *= inv; p.y *= inv; p.z *= inv; p.w *= inv;
        }
        f->plane[i] = p;
        f->abs_n[i] = vec4(pw_absf(p.x), pw_absf(p.y), pw_absf(p.z), 0.0f);
    }
    f->plane[6] = f->plane[7] = f->plane[5];
    f->abs_n[6] = f->abs_n[7] = f->abs_n[5];
}

int sphere_sphere(const Vec4 *ca, float ra, const Vec4 *cb, float rb)
{
    Vec4 d = vec4(ca->x - cb->x, ca->y - cb->y, ca->z - cb->z, 0.0f);
    float r = ra + rb;
    return vec4_dot3(&d, &d) <= r * r;
}

void closest_point_triangle(Vec4 *out, const Vec4 *p, const Vec4 *a, const Vec4 *b,
                            const Vec4 *c)
{
    Vec4 ab, ac, ap, bp, cp;
    float d1, d2, d3, d4, d5, d6, va, vb, vc, denom, v, w;
    ab = vec4(b->x - a->x, b->y - a->y, b->z - a->z, 0.0f);
    ac = vec4(c->x - a->x, c->y - a->y, c->z - a->z, 0.0f);
    ap = vec4(p->x - a->x, p->y - a->y, p->z - a->z, 0.0f);
    d1 = ref_vec4_dot3(&ab, &ap);
    d2 = ref_vec4_dot3(&ac, &ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        *out = vec4(a->x, a->y, a->z, 1.0f);
        return;
    }
    bp = vec4(p->x - b->x, p->y - b->y, p->z - b->z, 0.0f);
    d3 = ref_vec4_dot3(&ab, &bp);
    d4 = ref_vec4_dot3(&ac, &bp);
    if (d3 >= 0.0f && d4 <= d3) {
        *out = vec4(b->x, b->y, b->z, 1.0f);
        return;
    }
    vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        v = d1 / (d1 - d3);
        *out = vec4(a->x + v * ab.x, a->y + v * ab.y, a->z + v * ab.z, 1.0f);
        return;
    }
    cp = vec4(p->x - c->x, p->y - c->y, p->z - c->z, 0.0f);
    d5 = ref_vec4_dot3(&ab, &cp);
    d6 = ref_vec4_dot3(&ac, &cp);
    if (d6 >= 0.0f && d5 <= d6) {
        *out = vec4(c->x, c->y, c->z, 1.0f);
        return;
    }
    vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        w = d2 / (d2 - d6);
        *out = vec4(a->x + w * ac.x, a->y + w * ac.y, a->z + w * ac.z, 1.0f);
        return;
    }
    va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        *out = vec4(b->x + w * (c->x - b->x), b->y + w * (c->y - b->y),
                    b->z + w * (c->z - b->z), 1.0f);
        return;
    }
    denom = 1.0f / (va + vb + vc);
    v = vb * denom;
    w = vc * denom;
    *out = vec4(a->x + ab.x * v + ac.x * w, a->y + ab.y * v + ac.y * w,
                a->z + ab.z * v + ac.z * w, 1.0f);
}
