/*
 * texcache.h - swizzled texture cache over the TEXTURE region.
 *
 * DTEX blobs are loaded from textures.dpak into a buddy allocator spanning
 * the whole 8 MB TEXTURE region and used by the GE in place. Textures are
 * resolved to pointers when a zone loads (never per frame). Each zone load
 * starts a new generation; when the region is full, textures not used by
 * the current generation are evicted oldest-first. Missing or corrupt
 * textures resolve to a built-in magenta checker so nothing ever draws
 * garbage memory.
 */
#ifndef PSPWIND_TEXCACHE_H
#define PSPWIND_TEXCACHE_H

#include "common.h"
#include "fmt.h"
#include "mem.h"
#include "pak.h"

#define TEXCACHE_MAX_SLOTS 2048

typedef struct {
    uint32_t hash;
    uint32_t gen;
    DTexHeader *tex;
} TexSlot;

typedef struct {
    MemBuddy buddy;
    const Pak *pak;
    TexSlot slots[TEXCACHE_MAX_SLOTS];
    uint32_t count;
    uint32_t gen;
    DTexHeader *fallback;
    uint32_t loads, hits, misses, evictions, corrupt;
} TexCache;

/* Resets the TEXTURE region and takes all of it. pak may be NULL (fallback only). */
void texcache_init(TexCache *c, const Pak *pak);
/* Starts a new generation (call before resolving a zone's textures). */
void texcache_begin(TexCache *c);
/* Never NULL: returns the fallback texture when missing/corrupt/full. */
const DTexHeader *texcache_get(TexCache *c, uint32_t hash);
uint32_t texcache_bytes_used(const TexCache *c);

/* Structural validation of a DTEX blob of `size` bytes. 0 = valid. */
int dtex_validate(const DTexHeader *t, uint32_t size);

#endif /* PSPWIND_TEXCACHE_H */
