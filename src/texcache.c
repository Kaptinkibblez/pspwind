/*
 * texcache.c - swizzled texture cache (see texcache.h).
 */
#include "texcache.h"

#include <string.h>

int dtex_validate(const DTexHeader *t, uint32_t size)
{
    uint32_t i, bpp, entries;
    if (size < sizeof(DTexHeader) || t->magic != DTEX_MAGIC || t->version != DTEX_VERSION)
        return 1;
    if (t->file_size != size)
        return 2;
    if (t->psm == DTEX_PSM_T4) {
        bpp = 4;
        entries = 16;
    } else if (t->psm == DTEX_PSM_T8) {
        bpp = 8;
        entries = 256;
    } else {
        return 3;
    }
    if (t->clut_entries != entries || (t->clut_ofs & 15u) ||
        t->clut_ofs > size || entries * 4u > size - t->clut_ofs)
        return 4;
    if (t->level_count < 1 || t->level_count > DTEX_MAX_LEVELS)
        return 5;
    for (i = 0; i < t->level_count; ++i) {
        const DTexLevel *l = &t->level[i];
        uint32_t need;
        if (!pw_is_pow2(l->width) || !pw_is_pow2(l->height) || l->width > 512 ||
            l->height > 512 || l->buf_width < l->width)
            return 6;
        if ((l->ofs & 15u) || l->ofs > size || l->size > size - l->ofs)
            return 7;
        need = (uint32_t)l->buf_width * l->height * bpp / 8u;
        if (l->size < need)
            return 8;
    }
    if (t->level[0].width != t->width || t->level[0].height != t->height)
        return 9;
    return 0;
}

/* 32x32 T4 magenta/black checker, unswizzled. */
static DTexHeader *make_fallback(MemBuddy *b)
{
    const uint32_t clut_ofs = 96, data_ofs = 96 + 64, data_size = 32 * 32 / 2;
    const uint32_t total = data_ofs + data_size;
    uint8_t *mem = (uint8_t *)mem_buddy_alloc(b, total);
    DTexHeader *t = (DTexHeader *)mem;
    uint32_t *clut;
    uint32_t x, y;
    if (!mem)
        return NULL;
    memset(mem, 0, total);
    t->magic = DTEX_MAGIC;
    t->version = DTEX_VERSION;
    t->psm = DTEX_PSM_T4;
    t->flags = 0;
    t->width = t->height = 32;
    t->level_count = 1;
    t->alpha_ref = 0;
    t->clut_entries = 16;
    t->clut_ofs = clut_ofs;
    t->file_size = total;
    t->level[0].width = t->level[0].height = t->level[0].buf_width = 32;
    t->level[0].ofs = data_ofs;
    t->level[0].size = data_size;
    clut = (uint32_t *)(mem + clut_ofs);
    clut[0] = 0xFF000000u;
    clut[1] = 0xFFFF00FFu;
    for (y = 0; y < 32; ++y) {
        for (x = 0; x < 32; x += 2) {
            uint8_t a = (uint8_t)(((x / 8) + (y / 8)) & 1);
            uint8_t b2 = (uint8_t)((((x + 1) / 8) + (y / 8)) & 1);
            mem[data_ofs + (y * 32 + x) / 2] = (uint8_t)(a | (b2 << 4));
        }
    }
    return t;
}

void texcache_init(TexCache *c, const Pak *pak)
{
    memset(c, 0, sizeof(*c));
    mem_reset(MEM_REGION_TEXTURE);
    mem_buddy_init(&c->buddy, MEM_REGION_TEXTURE, mem_available(MEM_REGION_TEXTURE), 256);
    c->pak = pak;
    c->fallback = make_fallback(&c->buddy);
}

void texcache_begin(TexCache *c)
{
    c->gen++;
}

uint32_t texcache_bytes_used(const TexCache *c)
{
    return c->buddy.used_bytes;
}

static int evict_one(TexCache *c)
{
    uint32_t i, best = c->count;
    for (i = 0; i < c->count; ++i) {
        if (c->slots[i].gen != c->gen && (best == c->count || c->slots[i].gen < c->slots[best].gen))
            best = i;
    }
    if (best == c->count)
        return 0;
    mem_buddy_free(&c->buddy, c->slots[best].tex);
    c->slots[best] = c->slots[--c->count];
    c->evictions++;
    return 1;
}

const DTexHeader *texcache_get(TexCache *c, uint32_t hash)
{
    const DPakEntry *e;
    DTexHeader *t = NULL;
    uint32_t i;

    for (i = 0; i < c->count; ++i) {
        if (c->slots[i].hash == hash) {
            c->slots[i].gen = c->gen;
            c->hits++;
            return c->slots[i].tex;
        }
    }
    if (!c->pak || !(e = pak_find(c->pak, hash))) {
        c->misses++;
        return c->fallback;
    }
    if (c->count == TEXCACHE_MAX_SLOTS && !evict_one(c))
        return c->fallback;
    while (!(t = (DTexHeader *)mem_buddy_alloc(&c->buddy, e->size))) {
        if (!evict_one(c))
            return c->fallback;
    }
    if (pak_read(c->pak, e, t) != 0 || dtex_validate(t, e->size) != 0) {
        mem_buddy_free(&c->buddy, t);
        c->corrupt++;
        return c->fallback;
    }
    c->slots[c->count].hash = hash;
    c->slots[c->count].gen = c->gen;
    c->slots[c->count].tex = t;
    c->count++;
    c->loads++;
    return t;
}
