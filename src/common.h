/*
 * common.h - project-wide types and helpers (C99, host + PSP).
 *
 * Everything in src/ that is not explicitly PSP-only compiles with a plain
 * host C99 compiler as well, so allocator and math logic can be unit-tested
 * on the build machine (tests/host) before it ever runs on hardware.
 */
#ifndef PSPWIND_COMMON_H
#define PSPWIND_COMMON_H

#include <stddef.h>
#include <stdint.h>

#if defined(__PSP__) || defined(__psp__)
#define PW_PSP 1
#else
#define PW_PSP 0
#endif

#define PW_ALIGN(n) __attribute__((aligned(n)))
#define PW_UNUSED(x) ((void)(x))
#define PW_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))
#define PW_MIN(a, b) ((a) < (b) ? (a) : (b))
#define PW_MAX(a, b) ((a) > (b) ? (a) : (b))
#define PW_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define PW_KB(n) ((uint32_t)(n) * 1024u)
#define PW_MB(n) ((uint32_t)(n) * 1024u * 1024u)

/* C99 has no _Static_assert; a negative array size is a portable stand-in. */
#define PW_CAT_(a, b) a##b
#define PW_CAT(a, b) PW_CAT_(a, b)
#define PW_STATIC_ASSERT(cond, tag) \
    typedef char PW_CAT(pw_static_assert_##tag##_, __LINE__)[(cond) ? 1 : -1]

static inline uint32_t pw_align_up(uint32_t v, uint32_t a)
{
    return (v + (a - 1u)) & ~(a - 1u);
}

static inline int pw_is_pow2(uint32_t v)
{
    return v != 0u && (v & (v - 1u)) == 0u;
}

/*
 * FNV-1a 32-bit over a normalised asset name. Asset names are normalised by
 * the cookers (lower case, '/' separators, no extension, no leading
 * "textures/" or "meshes/"), so the runtime only ever hashes clean keys.
 * The same function lives in tools/demake_cooker/dmcommon.py (fnv1a32).
 */
static inline uint32_t pw_hash_name(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        else if (c == '\\')
            c = '/';
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

#endif /* PSPWIND_COMMON_H */
