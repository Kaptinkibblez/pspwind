/*
 * selftest.c - allocator and math self-tests (see selftest.h).
 */
#include "selftest.h"

#include <math.h>
#include <string.h>

#include "mem.h"
#include "vfpu.h"

/* ------------------------------------------------------------------------ */
/* helpers                                                                  */
/* ------------------------------------------------------------------------ */
typedef struct {
    SelftestPrintf log;
    const char *group;
    SelftestResult r;
} Ctx;

static void check(Ctx *c, int ok, const char *what)
{
    c->r.checks++;
    if (!ok) {
        c->r.failures++;
        if (c->r.failures <= 12)
            c->log("  FAIL [%s] %s\n", c->group, what);
    }
}

static int near_f(float a, float b, float tol)
{
    float d = a - b;
    if (d != d) /* NaN */
        return 0;
    if (d < 0.0f)
        d = -d;
    return d <= tol * (1.0f + pw_absf(b));
}

static int near_v4(const Vec4 *a, const Vec4 *b, float tol)
{
    return near_f(a->x, b->x, tol) && near_f(a->y, b->y, tol) &&
           near_f(a->z, b->z, tol) && near_f(a->w, b->w, tol);
}

static int near_v3(const Vec4 *a, const Vec4 *b, float tol)
{
    return near_f(a->x, b->x, tol) && near_f(a->y, b->y, tol) && near_f(a->z, b->z, tol);
}

static int near_m4(const Mat4 *a, const Mat4 *b, float tol)
{
    return near_v4(&a->x, &b->x, tol) && near_v4(&a->y, &b->y, tol) &&
           near_v4(&a->z, &b->z, tol) && near_v4(&a->w, &b->w, tol);
}

static uint32_t g_rng = 0x1234567u;

static uint32_t rng_u32(void)
{
    /* xorshift32 - deterministic across host and PSP */
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

static float rng_f(float lo, float hi)
{
    return lo + (hi - lo) * (float)(rng_u32() & 0xFFFFFFu) / 16777215.0f;
}

static Vec4 rng_v4(float range)
{
    return vec4(rng_f(-range, range), rng_f(-range, range), rng_f(-range, range),
                rng_f(-range, range));
}

static void rng_m4(Mat4 *m, float range)
{
    m->x = rng_v4(range);
    m->y = rng_v4(range);
    m->z = rng_v4(range);
    m->w = rng_v4(range);
}

static void rng_rigid(Mat4 *m)
{
    Quat q;
    Vec4 axis = rng_v4(1.0f), pos = rng_v4(100.0f);
    axis.x += 0.01f; /* never zero */
    quat_from_axis_angle(&q, &axis, rng_f(-PW_PI, PW_PI));
    mat4_from_trs(m, &pos, &q, 1.0f);
}

/* ------------------------------------------------------------------------ */
/* memory                                                                   */
/* ------------------------------------------------------------------------ */
static void test_regions(Ctx *c)
{
    uint32_t total = 0;
    int i;
    for (i = 0; i < MEM_REGION_COUNT; ++i) {
        total += mem_region_size((MemRegionId)i);
        mem_reset((MemRegionId)i);
    }
    check(c, total == MEM_ARENA_SIZE, "region budgets sum to 32 MB");
    check(c, mem_region_size(MEM_REGION_DB) == PW_MB(6), "DB budget 6 MB");
    check(c, mem_region_size(MEM_REGION_ZONE) == PW_MB(8), "ZONE budget 8 MB");
    check(c, mem_region_size(MEM_REGION_TEXTURE) == PW_MB(8), "TEXTURE budget 8 MB");
    check(c, mem_region_size(MEM_REGION_SCRIPT) == PW_MB(4), "SCRIPT budget 4 MB");
    check(c, mem_region_size(MEM_REGION_AUDIO_UI) == PW_MB(6), "AUDIO_UI budget 6 MB");
    check(c, ((uintptr_t)mem_region_base(MEM_REGION_DB) & 63u) == 0, "arena 64-byte aligned");
    check(c, mem_region_base(MEM_REGION_ZONE) ==
                 mem_region_base(MEM_REGION_DB) + PW_MB(6), "regions are contiguous");
}

static void test_bump(Ctx *c)
{
    const MemRegionId R = MEM_REGION_SCRIPT;
    uint8_t *a, *b, *h, *p;
    MemMark m, mh;
    MemStats s;
    uint32_t avail;

    mem_reset(R);
    a = (uint8_t *)mem_alloc(R, 3, 1);
    b = (uint8_t *)mem_alloc(R, 100, 64);
    check(c, a == mem_region_base(R), "first low alloc at region base");
    check(c, ((uintptr_t)b & 63u) == 0, "64-byte alignment honoured");
    check(c, b > a, "low end grows upwards");

    h = (uint8_t *)mem_alloc_hi(R, 1000, 16);
    check(c, ((uintptr_t)h & 15u) == 0, "high alloc aligned");
    check(c, h + 1000 <= mem_region_base(R) + mem_region_size(R), "high alloc inside region");
    check(c, h > b + 100, "high end below top, above low end");

    m = mem_mark(R);
    p = (uint8_t *)mem_alloc(R, 4096, 16);
    memset(p, 0xAB, 4096);
    mem_release(R, m);
    check(c, mem_alloc(R, 16, 16) == p, "mark/release rewinds low end");

    mh = mem_mark_hi(R);
    mem_alloc_hi(R, 50000, 16);
    mem_release_hi(R, mh);
    check(c, (uint8_t *)mem_alloc_hi(R, 16, 16) == h - 16, "high mark rewinds");

    avail = mem_available(R);
    check(c, mem_try_alloc(R, avail + 1u, 1) == NULL, "try_alloc fails past capacity");
    check(c, mem_try_alloc_hi(R, avail + 1u, 1) == NULL, "try_alloc_hi fails past capacity");
    check(c, mem_try_alloc(R, 16, 3) == NULL, "non power-of-two alignment rejected");
    p = (uint8_t *)mem_try_alloc(R, avail & ~15u, 16);
    check(c, p != NULL, "exact remaining capacity allocatable");
    check(c, mem_available(R) < 16u, "region now full");

    mem_stats(R, &s);
    check(c, s.peak >= s.used_lo + s.used_hi, "peak tracks usage");
    mem_reset(R);
    mem_stats(R, &s);
    check(c, s.used_lo == 0 && s.used_hi == 0, "reset clears both ends");
    check(c, mem_owns(R, mem_region_base(R)) && !mem_owns(R, mem_region_base(R) - 1),
          "ownership test");
    {
        char *d = mem_strdup(R, "Seyda Neen");
        check(c, strcmp(d, "Seyda Neen") == 0, "strdup copies");
    }
    mem_reset(R);
}

static void test_pool(Ctx *c)
{
    const MemRegionId R = MEM_REGION_ZONE;
    MemPool pool;
    void *blocks[64];
    uint32_t i;
    int distinct = 1;

    mem_reset(R);
    mem_pool_init(&pool, R, 40, 64, 16);
    check(c, pool.block_size == 48, "block size rounded to alignment");
    for (i = 0; i < 64; ++i) {
        blocks[i] = mem_pool_alloc(&pool);
        if (!blocks[i] || ((uintptr_t)blocks[i] & 15u))
            distinct = 0;
        else
            memset(blocks[i], (int)i, 40);
    }
    check(c, distinct, "64 aligned blocks allocated");
    check(c, mem_pool_alloc(&pool) == NULL, "pool exhaustion returns NULL");
    for (i = 0; i < 64; ++i)
        if (((uint8_t *)blocks[i])[39] != (uint8_t)i)
            distinct = 0;
    check(c, distinct, "blocks do not overlap");
    check(c, mem_pool_index(&pool, blocks[10]) == 10, "block index");
    mem_pool_free(&pool, blocks[10]);
    mem_pool_free(&pool, blocks[20]);
    check(c, pool.in_use == 62, "in_use after frees");
    check(c, mem_pool_alloc(&pool) == blocks[20], "LIFO reuse");
    check(c, mem_pool_alloc(&pool) == blocks[10], "LIFO reuse 2");
    check(c, pool.peak == 64, "peak tracked");
    check(c, !mem_pool_owns(&pool, (uint8_t *)blocks[0] + 1), "misaligned pointer not owned");
    mem_pool_reset(&pool);
    check(c, pool.in_use == 0 && mem_pool_alloc(&pool) == blocks[0], "reset restores order");
    mem_reset(R);
}

#define BUDDY_SLOTS 256

static void test_buddy(Ctx *c)
{
    const MemRegionId R = MEM_REGION_TEXTURE;
    static MemBuddy buddy; /* large struct, keep off the stack */
    static uint8_t *ptr[BUDDY_SLOTS];
    static uint32_t len[BUDDY_SLOTS];
    uint32_t initial_free, initial_largest, i, step, bad = 0, check_err = 0;

    mem_reset(R);
    mem_buddy_init(&buddy, R, mem_available(R), 256);
    initial_free = buddy.free_bytes;
    initial_largest = mem_buddy_largest_free(&buddy);
    check(c, mem_buddy_check(&buddy) == 0, "fresh buddy consistent");
    check(c, initial_free > PW_MB(7), "buddy manages most of the texture region");
    check(c, initial_largest == PW_MB(4), "non-pow2 span decomposed (4 MB top block)");
    check(c, buddy.order_count >= 15, "orders up to 4 MB");

    memset(ptr, 0, sizeof(ptr));
    g_rng = 0xC0FFEEu;
    for (step = 0; step < 4000; ++step) {
        uint32_t slot = rng_u32() % BUDDY_SLOTS;
        if (ptr[slot]) {
            uint32_t k;
            for (k = 0; k < len[slot]; ++k)
                if (ptr[slot][k] != (uint8_t)slot) {
                    bad++;
                    break;
                }
            mem_buddy_free(&buddy, ptr[slot]);
            ptr[slot] = NULL;
        } else {
            /* texture-like sizes: 512 B (32x32 T4) .. 64 KB, sometimes odd */
            uint32_t sz = 256u << (rng_u32() % 9);
            if (rng_u32() & 1u)
                sz -= rng_u32() % (sz / 2);
            ptr[slot] = (uint8_t *)mem_buddy_alloc(&buddy, sz);
            if (ptr[slot]) {
                len[slot] = sz;
                if (mem_buddy_block_size(&buddy, ptr[slot]) < sz)
                    bad++;
                memset(ptr[slot], (int)slot, sz);
            }
        }
        if ((step & 63u) == 0 && mem_buddy_check(&buddy) != 0)
            check_err++;
    }
    check(c, bad == 0, "buddy blocks keep their contents");
    check(c, check_err == 0, "buddy invariants hold during stress");
    for (i = 0; i < BUDDY_SLOTS; ++i)
        if (ptr[i])
            mem_buddy_free(&buddy, ptr[i]);
    check(c, mem_buddy_check(&buddy) == 0, "buddy consistent after drain");
    check(c, buddy.free_bytes == initial_free, "all bytes returned");
    check(c, mem_buddy_largest_free(&buddy) == initial_largest, "blocks fully coalesced");
    check(c, mem_buddy_alloc(&buddy, PW_MB(8)) == NULL, "oversized request rejected");
    {
        void *big = mem_buddy_alloc(&buddy, PW_MB(4));
        check(c, big != NULL && mem_buddy_block_size(&buddy, big) == PW_MB(4),
              "largest block allocatable");
        mem_buddy_free(&buddy, big);
    }
    check(c, buddy.peak_used > 0, "peak usage tracked");
    mem_reset(R);
}

SelftestResult selftest_mem(SelftestPrintf log)
{
    Ctx c;
    c.log = log;
    c.r.checks = c.r.failures = 0;
    c.group = "regions"; test_regions(&c);
    c.group = "bump"; test_bump(&c);
    c.group = "pool"; test_pool(&c);
    c.group = "buddy"; test_buddy(&c);
    return c.r;
}

/* ------------------------------------------------------------------------ */
/* math: implementation vs scalar oracle                                    */
/* ------------------------------------------------------------------------ */
#define TOL 2e-5f
#define TRIG_TOL 2e-4f /* VFPU transcendental units are ~1e-6..1e-5 accurate */

static void test_scalar(Ctx *c)
{
    int i, ok_s = 1, ok_c = 1, ok_sc = 1, ok_sqrt = 1, ok_rsq = 1;
    for (i = 0; i < 200; ++i) {
        float a = rng_f(-20.0f, 20.0f), s, co;
        float v = rng_f(0.001f, 1000.0f);
        if (!near_f(pw_sinf(a), ref_sinf(a), TRIG_TOL)) ok_s = 0;
        if (!near_f(pw_cosf(a), ref_cosf(a), TRIG_TOL)) ok_c = 0;
        pw_sincosf(a, &s, &co);
        if (!near_f(s, ref_sinf(a), TRIG_TOL) || !near_f(co, ref_cosf(a), TRIG_TOL)) ok_sc = 0;
        if (!near_f(pw_sqrtf(v), ref_sqrtf(v), TOL)) ok_sqrt = 0;
        if (!near_f(pw_rsqrtf(v), ref_rsqrtf(v), 1e-4f)) ok_rsq = 0;
    }
    check(c, ok_s, "sin matches reference");
    check(c, ok_c, "cos matches reference");
    check(c, ok_sc, "sincos matches reference");
    check(c, ok_sqrt, "sqrt matches reference");
    check(c, ok_rsq, "rsqrt matches reference");
    check(c, near_f(pw_wrap_angle(3.0f * PW_PI), -PW_PI, 1e-5f) ||
              near_f(pw_wrap_angle(3.0f * PW_PI), PW_PI, 1e-5f), "wrap 3pi");
    check(c, near_f(pw_wrap_angle(-0.5f), -0.5f, 0.0f), "wrap passthrough");
}

static void test_vec(Ctx *c)
{
    int i, ok_dot = 1, ok_cross = 1, ok_norm = 1, ok_misc = 1;
    Vec4 a, b, r, e;
    for (i = 0; i < 200; ++i) {
        a = rng_v4(50.0f);
        b = rng_v4(50.0f);
        if (!near_f(vec4_dot3(&a, &b), ref_vec4_dot3(&a, &b), 1e-4f)) ok_dot = 0;
        vec4_cross3(&r, &a, &b);
        ref_vec4_cross3(&e, &a, &b);
        if (!near_v4(&r, &e, 1e-4f)) ok_cross = 0;
        {
            float l1 = vec4_normalize3(&r, &a), l2 = ref_vec4_normalize3(&e, &a);
            if (!near_f(l1, l2, 1e-4f) || !near_v4(&r, &e, 1e-4f)) ok_norm = 0;
        }
        vec4_add(&r, &a, &b);
        if (!near_f(r.w, a.w + b.w, TOL) || !near_f(r.x, a.x + b.x, TOL)) ok_misc = 0;
        vec4_sub(&r, &a, &b);
        if (!near_f(r.z, a.z - b.z, TOL)) ok_misc = 0;
        vec4_scale(&r, &a, 0.25f);
        if (!near_f(r.y, a.y * 0.25f, TOL)) ok_misc = 0;
        vec4_lerp(&r, &a, &b, 0.5f);
        if (!near_f(r.w, (a.w + b.w) * 0.5f, 1e-4f)) ok_misc = 0;
        if (!near_f(vec4_dot4(&a, &b), a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w, 1e-4f))
            ok_misc = 0;
        if (!near_f(vec4_length3(&a), sqrtf(ref_vec4_dot3(&a, &a)), 1e-4f)) ok_misc = 0;
    }
    check(c, ok_dot, "vdot.t dot3");
    check(c, ok_cross, "vcrsp.t cross3");
    check(c, ok_norm, "normalize3");
    check(c, ok_misc, "add/sub/scale/lerp/dot4/length");
    {
        Vec4 z = vec4(0.0f, 0.0f, 0.0f, 7.0f);
        float l = vec4_normalize3(&r, &z);
        check(c, l == 0.0f && r.x == 0.0f && r.w == 7.0f, "normalize zero vector stays zero");
    }
    {
        Vec4 x = vec4(1, 0, 0, 0), y = vec4(0, 1, 0, 0);
        vec4_cross3(&r, &x, &y);
        check(c, near_v4(&r, &(Vec4){0, 0, 1, 0}, 0.0f), "x cross y = z, w = 0");
    }
}

static void test_mat(Ctx *c)
{
    int i, ok_mul = 1, ok_tr = 1, ok_rot = 1, ok_post = 1, ok_inv = 1, ok_xf = 1, ok_ginv = 1;
    Mat4 a, b, r, e, t;
    Vec4 v, rv, ev;
    for (i = 0; i < 100; ++i) {
        float ang = rng_f(-10.0f, 10.0f);
        rng_m4(&a, 10.0f);
        rng_m4(&b, 10.0f);
        mat4_mul(&r, &a, &b);
        ref_mat4_mul(&e, &a, &b);
        if (!near_m4(&r, &e, 1e-4f)) ok_mul = 0;
        r = a;
        mat4_mul(&r, &r, &b); /* aliasing */
        if (!near_m4(&r, &e, 1e-4f)) ok_mul = 0;

        mat4_transpose(&r, &a);
        ref_mat4_transpose(&e, &a);
        if (!near_m4(&r, &e, 0.0f)) ok_tr = 0;

        mat4_rotation_x(&r, ang); ref_mat4_rotation_x(&e, pw_wrap_angle(ang));
        if (!near_m4(&r, &e, TRIG_TOL)) ok_rot = 0;
        mat4_rotation_y(&r, ang); ref_mat4_rotation_y(&e, pw_wrap_angle(ang));
        if (!near_m4(&r, &e, TRIG_TOL)) ok_rot = 0;
        mat4_rotation_z(&r, ang); ref_mat4_rotation_z(&e, pw_wrap_angle(ang));
        if (!near_m4(&r, &e, TRIG_TOL)) ok_rot = 0;

        /* post-multiplication helpers: m = m * op */
        r = a;
        mat4_rotate_y(&r, ang);
        ref_mat4_rotation_y(&t, pw_wrap_angle(ang));
        ref_mat4_mul(&e, &a, &t);
        if (!near_m4(&r, &e, 1e-3f)) ok_post = 0;
        r = a;
        mat4_rotate_x(&r, ang);
        ref_mat4_rotation_x(&t, pw_wrap_angle(ang));
        ref_mat4_mul(&e, &a, &t);
        if (!near_m4(&r, &e, 1e-3f)) ok_post = 0;
        r = a;
        mat4_rotate_z(&r, ang);
        ref_mat4_rotation_z(&t, pw_wrap_angle(ang));
        ref_mat4_mul(&e, &a, &t);
        if (!near_m4(&r, &e, 1e-3f)) ok_post = 0;
        r = a;
        mat4_translate(&r, 1.5f, -2.0f, 3.0f);
        mat4_translation(&t, 1.5f, -2.0f, 3.0f);
        ref_mat4_mul(&e, &a, &t);
        if (!near_m4(&r, &e, 1e-4f)) ok_post = 0;
        r = a;
        mat4_scale(&r, 2.0f, 0.5f, -1.0f);
        mat4_scaling(&t, 2.0f, 0.5f, -1.0f);
        ref_mat4_mul(&e, &a, &t);
        if (!near_m4(&r, &e, 1e-4f)) ok_post = 0;

        rng_rigid(&a);
        mat4_inverse_orthonormal(&r, &a);
        ref_mat4_inverse_orthonormal(&e, &a);
        if (!near_m4(&r, &e, 1e-4f)) ok_inv = 0;
        ref_mat4_mul(&t, &r, &a);
        ref_mat4_identity(&e);
        if (!near_m4(&t, &e, 1e-4f)) ok_inv = 0;

        rng_m4(&b, 3.0f);
        b.x.x += 4.0f; b.y.y += 4.0f; b.z.z += 4.0f; b.w.w += 4.0f; /* well conditioned */
        if (mat4_inverse(&r, &b)) {
            ref_mat4_mul(&t, &b, &r);
            if (!near_m4(&t, &e, 1e-3f)) ok_ginv = 0;
        } else {
            ok_ginv = 0;
        }

        rng_m4(&a, 10.0f);
        v = rng_v4(10.0f);
        mat4_transform(&rv, &a, &v);
        ref_mat4_transform(&ev, &a, &v);
        if (!near_v4(&rv, &ev, 1e-4f)) ok_xf = 0;
        mat4_transform_point(&rv, &a, &v);
        v.w = 1.0f;
        ref_mat4_transform(&ev, &a, &v);
        if (!near_v4(&rv, &ev, 1e-4f)) ok_xf = 0;
        mat4_transform_dir(&rv, &a, &v);
        v.w = 0.0f;
        ref_mat4_transform(&ev, &a, &v);
        if (!near_v4(&rv, &ev, 1e-4f)) ok_xf = 0;
    }
    check(c, ok_mul, "vmmul.q mat4_mul (incl. aliasing)");
    check(c, ok_tr, "mat4_transpose");
    check(c, ok_rot, "vrot.q rotation x/y/z");
    check(c, ok_post, "post-multiply rotate/translate/scale");
    check(c, ok_inv, "orthonormal inverse");
    check(c, ok_ginv, "general inverse");
    check(c, ok_xf, "vtfm4.q transform / point / dir");

    {
        static float in[3 * 37], out[3 * 37], exp[3 * 37];
        int k, ok = 1;
        rng_m4(&a, 5.0f);
        for (k = 0; k < 3 * 37; ++k)
            in[k] = rng_f(-100.0f, 100.0f);
        mat4_transform_points(out, &a, in, 37);
        ref_mat4_transform_points(exp, &a, in, 37);
        for (k = 0; k < 3 * 37; ++k)
            if (!near_f(out[k], exp[k], 1e-4f))
                ok = 0;
        mat4_transform_points(in, &a, in, 37); /* in place */
        for (k = 0; k < 3 * 37; ++k)
            if (!near_f(in[k], exp[k], 1e-4f))
                ok = 0;
        check(c, ok, "batched transform_points (incl. in place)");
    }
    {
        /* gluPerspective(90, 1, 1, 100) reference values */
        mat4_perspective(&r, 90.0f * PW_DEG2RAD, 1.0f, 1.0f, 100.0f);
        check(c, near_f(r.x.x, 1.0f, 1e-4f) && near_f(r.y.y, 1.0f, 1e-4f) &&
                     near_f(r.z.z, -101.0f / 99.0f, 1e-4f) && r.z.w == -1.0f &&
                     near_f(r.w.z, -200.0f / 99.0f, 1e-4f) && r.w.w == 0.0f,
              "perspective matches gluPerspective");
    }
    {
        Vec4 eye = vec4(10, 5, -3, 1), at = vec4(0, 0, 0, 1), up = vec4(0, 0, 1, 0);
        mat4_look_at(&r, &eye, &at, &up);
        mat4_transform_point(&rv, &r, &eye);
        check(c, near_v3(&rv, &(Vec4){0, 0, 0, 0}, 1e-4f), "look_at maps eye to origin");
        mat4_transform_point(&rv, &r, &at);
        check(c, near_f(rv.x, 0.0f, 1e-4f) && near_f(rv.y, 0.0f, 1e-4f) && rv.z < 0.0f,
              "look_at target on -Z");
    }
}

static void test_quat(Ctx *c)
{
    int i, ok_mul = 1, ok_norm = 1, ok_rot = 1, ok_trs = 1;
    Quat a, b, r, e;
    Vec4 axis = vec4(0, 0, 1, 0), v, rv, ev;
    Mat4 m;
    for (i = 0; i < 100; ++i) {
        a = rng_v4(1.0f);
        b = rng_v4(1.0f);
        quat_mul(&r, &a, &b);
        ref_quat_mul(&e, &a, &b);
        if (!near_v4(&r, &e, 1e-4f)) ok_mul = 0;
        quat_normalize(&r, &a);
        ref_quat_normalize(&e, &a);
        if (!near_v4(&r, &e, 1e-4f)) ok_norm = 0;

        /* rotating via quaternion == rotating via the TRS matrix */
        axis = rng_v4(1.0f);
        axis.y += 0.01f;
        quat_from_axis_angle(&a, &axis, rng_f(-PW_PI, PW_PI));
        v = rng_v4(10.0f);
        v.w = 0.0f;
        quat_rotate(&rv, &a, &v);
        {
            Vec4 zero = vec4(0, 0, 0, 1);
            mat4_from_trs(&m, &zero, &a, 1.0f);
        }
        ref_mat4_transform(&ev, &m, &v);
        if (!near_v3(&rv, &ev, 1e-3f)) ok_rot = 0;
        /* composition: (a*b) applied == a applied after b */
        quat_from_axis_angle(&b, &axis, 0.7f);
        quat_mul(&r, &a, &b);
        quat_rotate(&ev, &b, &v);
        quat_rotate(&ev, &a, &ev);
        quat_rotate(&rv, &r, &v);
        if (!near_v3(&rv, &ev, 1e-3f)) ok_trs = 0;
    }
    check(c, ok_mul, "vqmul.q quaternion product");
    check(c, ok_norm, "quat_normalize");
    check(c, ok_rot, "quat_rotate == from_trs matrix");
    check(c, ok_trs, "quaternion composition order");
    {
        Vec4 z = vec4(0, 0, 1, 0), x = vec4(1, 0, 0, 0);
        quat_from_axis_angle(&a, &z, PW_HALF_PI);
        quat_rotate(&rv, &a, &x);
        check(c, near_v3(&rv, &(Vec4){0, 1, 0, 0}, 1e-4f), "90 deg about Z maps X to Y");
        quat_identity(&b);
        quat_slerp(&r, &b, &a, 0.0f);
        check(c, near_v4(&r, &b, 1e-4f), "slerp t=0");
        quat_slerp(&r, &b, &a, 1.0f);
        check(c, near_v4(&r, &a, 1e-4f), "slerp t=1");
        quat_slerp(&r, &b, &a, 0.5f);
        quat_from_axis_angle(&e, &z, PW_HALF_PI * 0.5f);
        check(c, near_v4(&r, &e, 1e-4f), "slerp midpoint = 45 deg");
        quat_nlerp(&r, &b, &a, 0.5f);
        check(c, near_v4(&r, &e, 1e-4f), "nlerp midpoint = 45 deg");
    }
}

static void test_cull(Ctx *c)
{
    Mat4 proj, view, vp;
    Frustum f;
    Vec4 eye = vec4(0, 0, 0, 1), at = vec4(0, 1, 0, 1), up = vec4(0, 0, 1, 0);
    int i, mismatch_s = 0, mismatch_b = 0;
    static Vec4 spheres[64];
    static uint8_t vis[64];
    uint32_t nvis, nexp = 0;

    check(c, ((uintptr_t)&f & 63u) == 0 && ((uintptr_t)&vp & 63u) == 0,
          "stack Mat4/Frustum are 64-byte aligned");
    mat4_perspective(&proj, 60.0f * PW_DEG2RAD, 480.0f / 272.0f, 1.0f, 1000.0f);
    mat4_look_at(&view, &eye, &at, &up); /* Morrowind: +Y forward, +Z up */
    mat4_mul(&vp, &proj, &view);
    frustum_from_matrix(&f, &vp);

    {
        Vec4 ahead = vec4(0, 100, 0, 1), behind = vec4(0, -100, 0, 1), far = vec4(0, 2000, 0, 1);
        Vec4 edge = vec4(0, 0.5f, 0, 1);
        check(c, frustum_test_sphere(&f, &ahead, 5.0f) == CULL_INSIDE, "sphere ahead inside");
        check(c, frustum_test_sphere(&f, &behind, 5.0f) == CULL_OUTSIDE, "sphere behind culled");
        check(c, frustum_test_sphere(&f, &far, 5.0f) == CULL_OUTSIDE, "sphere past far culled");
        check(c, frustum_test_sphere(&f, &edge, 2.0f) == CULL_INTERSECT, "near-plane straddle");
    }
    {
        Vec4 mn = vec4(-5, 95, -5, 1), mx = vec4(5, 105, 5, 1);
        Vec4 mn2 = vec4(-5, -105, -5, 1), mx2 = vec4(5, -95, 5, 1);
        check(c, frustum_test_aabb(&f, &mn, &mx) == CULL_INSIDE, "aabb ahead inside");
        check(c, frustum_test_aabb(&f, &mn2, &mx2) == CULL_OUTSIDE, "aabb behind culled");
    }
    for (i = 0; i < 300; ++i) {
        Vec4 ctr = vec4(rng_f(-800, 800), rng_f(-200, 1200), rng_f(-400, 400), 1.0f);
        Vec4 ext = vec4(rng_f(1, 80), rng_f(1, 80), rng_f(1, 80), 0.0f), mn, mx;
        float rad = rng_f(1.0f, 100.0f);
        if (frustum_test_sphere(&f, &ctr, rad) != ref_frustum_test_sphere(&f, &ctr, rad))
            mismatch_s++;
        vec4_sub(&mn, &ctr, &ext);
        vec4_add(&mx, &ctr, &ext);
        if (frustum_test_aabb(&f, &mn, &mx) != ref_frustum_test_aabb(&f, &mn, &mx))
            mismatch_b++;
    }
    /* touching cases may flip on the last ulp; allow 1% */
    check(c, mismatch_s <= 3, "sphere culling agrees with reference");
    check(c, mismatch_b <= 3, "aabb culling agrees with reference");
    for (i = 0; i < 64; ++i) {
        spheres[i] = vec4(rng_f(-800, 800), rng_f(-200, 1200), rng_f(-400, 400), rng_f(1, 50));
        nexp += ref_frustum_test_sphere(&f, &spheres[i], spheres[i].w) != CULL_OUTSIDE;
    }
    nvis = frustum_cull_spheres(&f, spheres, 64, vis);
    check(c, nvis == nexp, "batched sphere culling count");
}

static void test_intersect(Ctx *c)
{
    Vec4 o = vec4(0, -10, 0, 1), d = vec4(0, 1, 0, 0), ctr = vec4(0, 0, 0, 1);
    Vec4 v0 = vec4(-1, 0, -1, 1), v1 = vec4(1, 0, -1, 1), v2 = vec4(0, 0, 1, 1);
    float t = -1.0f, u = 0, v = 0, t2, u2, vv2;
    int i, mismatch = 0, mismatch_s = 0;

    check(c, ray_sphere(&o, &d, &ctr, 2.0f, &t) && near_f(t, 8.0f, 1e-4f), "ray hits sphere at 8");
    {
        Vec4 away = vec4(0, -1, 0, 0);
        check(c, !ray_sphere(&o, &away, &ctr, 2.0f, &t), "ray pointing away misses");
        check(c, ray_sphere(&ctr, &d, &ctr, 2.0f, &t) && t == 0.0f, "ray from inside t=0");
    }
    check(c, ray_triangle(&o, &d, &v0, &v1, &v2, &t, &u, &v) && near_f(t, 10.0f, 1e-4f),
          "ray hits triangle at 10");
    {
        Vec4 off = vec4(5, -10, 0, 1);
        check(c, !ray_triangle(&off, &d, &v0, &v1, &v2, &t, &u, &v), "offset ray misses");
    }
    for (i = 0; i < 300; ++i) {
        Vec4 ro = rng_v4(20.0f), rd = rng_v4(1.0f), a = rng_v4(10.0f), b = rng_v4(10.0f),
             cc = rng_v4(10.0f);
        int h1, h2;
        float rr = rng_f(0.5f, 8.0f);
        rd.w = 0.0f;
        ro.w = 1.0f;
        vec4_normalize3(&rd, &rd);
        h1 = ray_triangle(&ro, &rd, &a, &b, &cc, &t, &u, &v);
        h2 = ref_ray_triangle(&ro, &rd, &a, &b, &cc, &t2, &u2, &vv2);
        if (h1 != h2 || (h1 && !near_f(t, t2, 1e-3f)))
            mismatch++;
        h1 = ray_sphere(&ro, &rd, &a, rr, &t);
        h2 = ref_ray_sphere(&ro, &rd, &a, rr, &t2);
        if (h1 != h2 || (h1 && !near_f(t, t2, 1e-3f)))
            mismatch_s++;
    }
    /* grazing rays can legitimately differ by an ulp; demand >= 99% */
    check(c, mismatch <= 3, "ray_triangle agrees with reference");
    check(c, mismatch_s <= 3, "ray_sphere agrees with reference");
    {
        Vec4 a = vec4(0, 0, 0, 1), b = vec4(3, 0, 0, 1);
        check(c, sphere_sphere(&a, 1.0f, &b, 2.0f) && !sphere_sphere(&a, 1.0f, &b, 1.9f),
              "sphere_sphere touching / apart");
    }
    {
        Vec4 p = vec4(0, 5, 0, 1), q, a2 = vec4(-1, 0, -1, 1);
        closest_point_triangle(&q, &p, &v0, &v1, &v2);
        check(c, near_v3(&q, &(Vec4){0, 0, 0, 0}, 1e-5f), "closest point interior projection");
        p = vec4(-5, 0, -5, 1);
        closest_point_triangle(&q, &p, &v0, &v1, &v2);
        check(c, near_v3(&q, &a2, 1e-5f), "closest point vertex region");
        p = vec4(0, 0, -5, 1);
        closest_point_triangle(&q, &p, &v0, &v1, &v2);
        check(c, near_v3(&q, &(Vec4){0, 0, -1, 0}, 1e-5f), "closest point edge region");
    }
}

SelftestResult selftest_vfpu(SelftestPrintf log)
{
    Ctx c;
    c.log = log;
    c.r.checks = c.r.failures = 0;
    g_rng = 0x1234567u;
    c.group = "scalar"; test_scalar(&c);
    c.group = "vec"; test_vec(&c);
    c.group = "mat"; test_mat(&c);
    c.group = "quat"; test_quat(&c);
    c.group = "cull"; test_cull(&c);
    c.group = "isect"; test_intersect(&c);
    return c.r;
}

/* ------------------------------------------------------------------------ */
/* benchmarks                                                               */
/* ------------------------------------------------------------------------ */
void selftest_bench(SelftestPrintf log, SelftestClock clock_us)
{
    static Mat4 a, b, r;
    static float pts_in[3 * 1024], pts_out[3 * 1024];
    uint64_t t0, t1, t2;
    int i;
    volatile float sink = 0.0f;

    rng_m4(&a, 1.0f);
    rng_m4(&b, 1.0f);
    for (i = 0; i < 3 * 1024; ++i)
        pts_in[i] = rng_f(-100.0f, 100.0f);

    t0 = clock_us();
    for (i = 0; i < 20000; ++i)
        mat4_mul(&r, &a, &b);
    t1 = clock_us();
    for (i = 0; i < 20000; ++i)
        ref_mat4_mul(&r, &a, &b);
    t2 = clock_us();
    sink += r.x.x;
    log("  mat4_mul x20000: %lu us (ref %lu us)\n", (unsigned long)(t1 - t0),
        (unsigned long)(t2 - t1));

    t0 = clock_us();
    for (i = 0; i < 20; ++i)
        mat4_transform_points(pts_out, &a, pts_in, 1024);
    t1 = clock_us();
    for (i = 0; i < 20; ++i)
        ref_mat4_transform_points(pts_out, &a, pts_in, 1024);
    t2 = clock_us();
    sink += pts_out[5];
    log("  transform 20480 pts: %lu us (ref %lu us)\n", (unsigned long)(t1 - t0),
        (unsigned long)(t2 - t1));
    (void)sink;
}
