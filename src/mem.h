/*
 * mem.h - strict 32 MB static arena.
 *
 * The whole game runs out of one statically reserved block that is carved
 * into fixed budget regions at boot. No malloc/free is ever called during
 * gameplay; every subsystem allocates from its region with one of:
 *
 *   - double-ended bump allocation (low end = long-lived data, high end =
 *     transient scratch such as decompression buffers), rewound with marks
 *   - fixed-size block pools (entities, sound voices, UI widgets)
 *   - a buddy allocator (power-of-two texture cache with eviction)
 *
 * Budgets (sum = 32 MB):
 *   DB        6 MB  permanent RPG / quest / dialogue database
 *   ZONE      8 MB  active zone geometry, collision, actors
 *   TEXTURE   8 MB  swizzled texture + CLUT cache
 *   SCRIPT    4 MB  MWScript VM state, globals, save game state
 *   AUDIO_UI  6 MB  audio buffers, UI, per-frame scratch
 */
#ifndef PSPWIND_MEM_H
#define PSPWIND_MEM_H

#include "common.h"

#define MEM_ARENA_SIZE PW_MB(32)
#define MEM_DEFAULT_ALIGN 16u

typedef enum {
    MEM_REGION_DB = 0,
    MEM_REGION_ZONE,
    MEM_REGION_TEXTURE,
    MEM_REGION_SCRIPT,
    MEM_REGION_AUDIO_UI,
    MEM_REGION_COUNT
} MemRegionId;

/* Rewind point for one end of a region. */
typedef struct {
    uint32_t offset;
    uint32_t serial; /* guards against releasing marks out of order */
} MemMark;

typedef struct {
    const char *name;
    uint32_t size;
    uint32_t used_lo;     /* bytes used by the low (persistent) end */
    uint32_t used_hi;     /* bytes used by the high (scratch) end */
    uint32_t peak;        /* high-water mark of used_lo + used_hi */
    uint32_t alloc_count; /* live allocations since last reset */
} MemStats;

/* Called on exhaustion by the non-try allocators. Must not return. */
typedef void (*MemOomHandler)(MemRegionId region, uint32_t requested,
                              uint32_t available);

/* --- arena ------------------------------------------------------------- */
void mem_init(void);
int mem_is_initialized(void);
void mem_set_oom_handler(MemOomHandler handler);
uint32_t mem_region_size(MemRegionId region);
const char *mem_region_name(MemRegionId region);
uint8_t *mem_region_base(MemRegionId region);
int mem_owns(MemRegionId region, const void *ptr);

/* --- double-ended bump allocation ------------------------------------- */
void *mem_alloc(MemRegionId region, uint32_t size, uint32_t align);
void *mem_alloc_hi(MemRegionId region, uint32_t size, uint32_t align);
void *mem_try_alloc(MemRegionId region, uint32_t size, uint32_t align);
void *mem_try_alloc_hi(MemRegionId region, uint32_t size, uint32_t align);
void *mem_calloc(MemRegionId region, uint32_t size, uint32_t align);
char *mem_strdup(MemRegionId region, const char *s);

MemMark mem_mark(MemRegionId region);
MemMark mem_mark_hi(MemRegionId region);
void mem_release(MemRegionId region, MemMark mark);
void mem_release_hi(MemRegionId region, MemMark mark);
void mem_reset(MemRegionId region);    /* both ends */
void mem_reset_hi(MemRegionId region); /* scratch end only */

uint32_t mem_available(MemRegionId region);
void mem_stats(MemRegionId region, MemStats *out);

/* Formats one line per region into the callback (debug overlay / log). */
void mem_dump(void (*print)(const char *line, void *user), void *user);

/* --- fixed-size block pool -------------------------------------------- */
typedef struct MemPoolBlock MemPoolBlock;
struct MemPoolBlock {
    MemPoolBlock *next;
};

typedef struct {
    uint8_t *base;
    uint32_t block_size;
    uint32_t capacity;
    uint32_t in_use;
    uint32_t peak;
    MemPoolBlock *free_list;
} MemPool;

/* Carves block_size*capacity bytes from the low end of region. */
void mem_pool_init(MemPool *pool, MemRegionId region, uint32_t block_size,
                   uint32_t capacity, uint32_t align);
void *mem_pool_alloc(MemPool *pool);      /* NULL when exhausted */
void mem_pool_free(MemPool *pool, void *block);
void mem_pool_reset(MemPool *pool);
uint32_t mem_pool_index(const MemPool *pool, const void *block);
int mem_pool_owns(const MemPool *pool, const void *block);

/* --- buddy allocator ---------------------------------------------------- */
#define MEM_BUDDY_MAX_ORDERS 20

typedef struct MemBuddyFree MemBuddyFree;
struct MemBuddyFree {
    MemBuddyFree *next;
    MemBuddyFree *prev;
};

typedef struct {
    uint8_t *base;      /* start of managed span */
    uint32_t span;      /* bytes actually backed by memory */
    uint32_t min_block; /* power of two, >= sizeof(MemBuddyFree) */
    uint32_t min_shift;
    uint32_t order_count;
    uint32_t block_count; /* span / min_block */
    uint8_t *meta;        /* one byte per min block: free flag | order */
    MemBuddyFree free_lists[MEM_BUDDY_MAX_ORDERS];
    uint32_t free_bytes;
    uint32_t used_bytes;
    uint32_t peak_used;
} MemBuddy;

/*
 * Takes the rest of `bytes` from the low end of the region (metadata is
 * allocated first, the remainder is managed). min_block must be a power of
 * two >= 16. Non power-of-two spans are fully usable.
 */
void mem_buddy_init(MemBuddy *b, MemRegionId region, uint32_t bytes,
                    uint32_t min_block);
void *mem_buddy_alloc(MemBuddy *b, uint32_t size); /* NULL when full */
void mem_buddy_free(MemBuddy *b, void *ptr);
uint32_t mem_buddy_block_size(const MemBuddy *b, const void *ptr);
uint32_t mem_buddy_largest_free(const MemBuddy *b);
/* Verifies free lists and metadata agree. Returns 0 when consistent. */
int mem_buddy_check(const MemBuddy *b);

#endif /* PSPWIND_MEM_H */
