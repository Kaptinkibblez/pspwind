/*
 * mem.c - strict 32 MB static arena (see mem.h).
 */
#include "mem.h"

#include <stdio.h>
#include <string.h>

#if PW_PSP
#include <pspdebug.h>
#include <pspthreadman.h>
#else
#include <stdlib.h>
#endif

#ifndef MEM_DEBUG
#define MEM_DEBUG 1
#endif

#define MEM_FILL_FREED 0xDDu

typedef struct {
    const char *name;
    uint8_t *base;
    uint32_t size;
    uint32_t lo;      /* next free byte from the bottom (offset) */
    uint32_t hi;      /* lowest used byte from the top (offset), == size when empty */
    uint32_t peak;
    uint32_t allocs;
    uint32_t gen_lo;  /* bumped on low-end reset: invalidates old marks */
    uint32_t gen_hi;
} Region;

static const struct {
    const char *name;
    uint32_t size;
} k_budgets[MEM_REGION_COUNT] = {
    { "DB", PW_MB(6) },
    { "ZONE", PW_MB(8) },
    { "TEXTURE", PW_MB(8) },
    { "SCRIPT", PW_MB(4) },
    { "AUDIO_UI", PW_MB(6) },
};

static uint8_t g_arena[MEM_ARENA_SIZE] PW_ALIGN(64);
static Region g_regions[MEM_REGION_COUNT];
static int g_initialized;
static MemOomHandler g_oom_handler;

/* ------------------------------------------------------------------------ */
/* failure paths                                                            */
/* ------------------------------------------------------------------------ */
static void mem_halt(const char *msg)
{
#if PW_PSP
    pspDebugScreenInit();
    pspDebugScreenPrintf("FATAL (mem): %s\n", msg);
    printf("FATAL (mem): %s\n", msg);
    for (;;)
        sceKernelSleepThread();
#else
    fprintf(stderr, "FATAL (mem): %s\n", msg);
    abort();
#endif
}

static void mem_default_oom(MemRegionId region, uint32_t requested,
                            uint32_t available)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "region %s exhausted: need %lu, have %lu",
             mem_region_name(region), (unsigned long)requested,
             (unsigned long)available);
    mem_halt(buf);
}

static void mem_misuse(const char *what, MemRegionId region)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s (region %s)", what,
             (unsigned)region < MEM_REGION_COUNT ? k_budgets[region].name : "?");
    mem_halt(buf);
}

static Region *region_get(MemRegionId id)
{
    if (!g_initialized)
        mem_halt("mem_init() not called");
    if ((unsigned)id >= MEM_REGION_COUNT)
        mem_halt("bad region id");
    return &g_regions[id];
}

static void region_update_peak(Region *r)
{
    uint32_t used = r->lo + (r->size - r->hi);
    if (used > r->peak)
        r->peak = used;
}

/* ------------------------------------------------------------------------ */
/* arena                                                                    */
/* ------------------------------------------------------------------------ */
void mem_init(void)
{
    uint32_t ofs = 0;
    int i;
    for (i = 0; i < MEM_REGION_COUNT; ++i) {
        Region *r = &g_regions[i];
        r->name = k_budgets[i].name;
        r->base = g_arena + ofs;
        r->size = k_budgets[i].size;
        r->lo = 0;
        r->hi = r->size;
        r->peak = 0;
        r->allocs = 0;
        r->gen_lo = 1;
        r->gen_hi = 1;
        ofs += r->size;
    }
    if (ofs != MEM_ARENA_SIZE)
        mem_halt("region budgets do not sum to arena size");
    if (!g_oom_handler)
        g_oom_handler = mem_default_oom;
    g_initialized = 1;
}

int mem_is_initialized(void) { return g_initialized; }

void mem_set_oom_handler(MemOomHandler handler)
{
    g_oom_handler = handler ? handler : mem_default_oom;
}

uint32_t mem_region_size(MemRegionId region) { return region_get(region)->size; }

const char *mem_region_name(MemRegionId region)
{
    if ((unsigned)region >= MEM_REGION_COUNT)
        return "?";
    return k_budgets[region].name;
}

uint8_t *mem_region_base(MemRegionId region) { return region_get(region)->base; }

int mem_owns(MemRegionId region, const void *ptr)
{
    const Region *r = region_get(region);
    const uint8_t *p = (const uint8_t *)ptr;
    return p >= r->base && p < r->base + r->size;
}

/* ------------------------------------------------------------------------ */
/* bump allocation                                                          */
/* ------------------------------------------------------------------------ */
static void *alloc_lo(Region *r, uint32_t size, uint32_t align)
{
    uintptr_t base = (uintptr_t)r->base;
    uintptr_t p;
    if (align == 0)
        align = MEM_DEFAULT_ALIGN;
    if (!pw_is_pow2(align))
        return NULL;
    p = (base + r->lo + (align - 1u)) & ~(uintptr_t)(align - 1u);
    if (p + size > base + r->hi || p + size < p)
        return NULL;
    r->lo = (uint32_t)(p + size - base);
    r->allocs++;
    region_update_peak(r);
    return (void *)p;
}

static void *alloc_hi(Region *r, uint32_t size, uint32_t align)
{
    uintptr_t base = (uintptr_t)r->base;
    uintptr_t top = base + r->hi;
    uintptr_t p;
    if (align == 0)
        align = MEM_DEFAULT_ALIGN;
    if (!pw_is_pow2(align) || size > r->hi)
        return NULL;
    p = (top - size) & ~(uintptr_t)(align - 1u);
    if (p < base + r->lo)
        return NULL;
    r->hi = (uint32_t)(p - base);
    r->allocs++;
    region_update_peak(r);
    return (void *)p;
}

void *mem_try_alloc(MemRegionId region, uint32_t size, uint32_t align)
{
    return alloc_lo(region_get(region), size, align);
}

void *mem_try_alloc_hi(MemRegionId region, uint32_t size, uint32_t align)
{
    return alloc_hi(region_get(region), size, align);
}

void *mem_alloc(MemRegionId region, uint32_t size, uint32_t align)
{
    Region *r = region_get(region);
    void *p = alloc_lo(r, size, align);
    if (!p) {
        if (align != 0 && !pw_is_pow2(align))
            mem_misuse("alignment is not a power of two", region);
        g_oom_handler(region, size, r->hi - r->lo);
    }
    return p;
}

void *mem_alloc_hi(MemRegionId region, uint32_t size, uint32_t align)
{
    Region *r = region_get(region);
    void *p = alloc_hi(r, size, align);
    if (!p) {
        if (align != 0 && !pw_is_pow2(align))
            mem_misuse("alignment is not a power of two", region);
        g_oom_handler(region, size, r->hi - r->lo);
    }
    return p;
}

void *mem_calloc(MemRegionId region, uint32_t size, uint32_t align)
{
    void *p = mem_alloc(region, size, align);
    memset(p, 0, size);
    return p;
}

char *mem_strdup(MemRegionId region, const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1u;
    char *d = (char *)mem_alloc(region, n, 1);
    memcpy(d, s, n);
    return d;
}

MemMark mem_mark(MemRegionId region)
{
    Region *r = region_get(region);
    MemMark m;
    m.offset = r->lo;
    m.serial = r->gen_lo;
    return m;
}

MemMark mem_mark_hi(MemRegionId region)
{
    Region *r = region_get(region);
    MemMark m;
    m.offset = r->hi;
    m.serial = r->gen_hi;
    return m;
}

void mem_release(MemRegionId region, MemMark mark)
{
    Region *r = region_get(region);
    if (mark.serial != r->gen_lo)
        mem_misuse("stale mark released (region was reset)", region);
    if (mark.offset > r->lo)
        mem_misuse("mark released twice or out of order", region);
#if MEM_DEBUG
    memset(r->base + mark.offset, MEM_FILL_FREED, r->lo - mark.offset);
#endif
    r->lo = mark.offset;
}

void mem_release_hi(MemRegionId region, MemMark mark)
{
    Region *r = region_get(region);
    if (mark.serial != r->gen_hi)
        mem_misuse("stale high mark released (region was reset)", region);
    if (mark.offset < r->hi || mark.offset > r->size)
        mem_misuse("high mark released twice or out of order", region);
#if MEM_DEBUG
    memset(r->base + r->hi, MEM_FILL_FREED, mark.offset - r->hi);
#endif
    r->hi = mark.offset;
}

void mem_reset_hi(MemRegionId region)
{
    Region *r = region_get(region);
#if MEM_DEBUG
    memset(r->base + r->hi, MEM_FILL_FREED, r->size - r->hi);
#endif
    r->hi = r->size;
    r->gen_hi++;
}

void mem_reset(MemRegionId region)
{
    Region *r = region_get(region);
#if MEM_DEBUG
    memset(r->base, MEM_FILL_FREED, r->lo);
#endif
    r->lo = 0;
    r->allocs = 0;
    r->gen_lo++;
    mem_reset_hi(region);
}

uint32_t mem_available(MemRegionId region)
{
    Region *r = region_get(region);
    return r->hi - r->lo;
}

void mem_stats(MemRegionId region, MemStats *out)
{
    Region *r = region_get(region);
    out->name = r->name;
    out->size = r->size;
    out->used_lo = r->lo;
    out->used_hi = r->size - r->hi;
    out->peak = r->peak;
    out->alloc_count = r->allocs;
}

void mem_dump(void (*print)(const char *line, void *user), void *user)
{
    char line[96];
    uint32_t total_used = 0, total_peak = 0;
    int i;
    for (i = 0; i < MEM_REGION_COUNT; ++i) {
        MemStats s;
        mem_stats((MemRegionId)i, &s);
        snprintf(line, sizeof(line), "%-8s %5luK used (%5luK+%4luK) peak %5luK / %5luK",
                 s.name, (unsigned long)((s.used_lo + s.used_hi) >> 10),
                 (unsigned long)(s.used_lo >> 10), (unsigned long)(s.used_hi >> 10),
                 (unsigned long)(s.peak >> 10), (unsigned long)(s.size >> 10));
        print(line, user);
        total_used += s.used_lo + s.used_hi;
        total_peak += s.peak;
    }
    snprintf(line, sizeof(line), "TOTAL    %5luK used, peak %5luK / %5luK",
             (unsigned long)(total_used >> 10), (unsigned long)(total_peak >> 10),
             (unsigned long)(MEM_ARENA_SIZE >> 10));
    print(line, user);
}

/* ------------------------------------------------------------------------ */
/* fixed-size pool                                                          */
/* ------------------------------------------------------------------------ */
void mem_pool_init(MemPool *pool, MemRegionId region, uint32_t block_size,
                   uint32_t capacity, uint32_t align)
{
    if (align == 0)
        align = MEM_DEFAULT_ALIGN;
    if (!pw_is_pow2(align) || capacity == 0)
        mem_misuse("bad pool parameters", region);
    if (block_size < sizeof(MemPoolBlock))
        block_size = sizeof(MemPoolBlock);
    block_size = pw_align_up(block_size, align < sizeof(void *) ? sizeof(void *) : align);
    pool->base = (uint8_t *)mem_alloc(region, block_size * capacity, align);
    pool->block_size = block_size;
    pool->capacity = capacity;
    mem_pool_reset(pool);
}

void mem_pool_reset(MemPool *pool)
{
    uint32_t i;
    pool->free_list = NULL;
    /* push in reverse so allocation order is ascending (cache friendly) */
    for (i = pool->capacity; i-- > 0;) {
        MemPoolBlock *b = (MemPoolBlock *)(pool->base + i * pool->block_size);
        b->next = pool->free_list;
        pool->free_list = b;
    }
    pool->in_use = 0;
    pool->peak = 0;
}

void *mem_pool_alloc(MemPool *pool)
{
    MemPoolBlock *b = pool->free_list;
    if (!b)
        return NULL;
    pool->free_list = b->next;
    pool->in_use++;
    if (pool->in_use > pool->peak)
        pool->peak = pool->in_use;
    return b;
}

int mem_pool_owns(const MemPool *pool, const void *block)
{
    const uint8_t *p = (const uint8_t *)block;
    if (p < pool->base || p >= pool->base + pool->block_size * pool->capacity)
        return 0;
    return ((uint32_t)(p - pool->base) % pool->block_size) == 0;
}

void mem_pool_free(MemPool *pool, void *block)
{
    MemPoolBlock *b = (MemPoolBlock *)block;
    if (!block)
        return;
    if (!mem_pool_owns(pool, block) || pool->in_use == 0)
        mem_halt("mem_pool_free: foreign or misaligned block");
#if MEM_DEBUG
    memset(block, MEM_FILL_FREED, pool->block_size);
#endif
    b->next = pool->free_list;
    pool->free_list = b;
    pool->in_use--;
}

uint32_t mem_pool_index(const MemPool *pool, const void *block)
{
    return (uint32_t)((const uint8_t *)block - pool->base) / pool->block_size;
}

/* ------------------------------------------------------------------------ */
/* buddy allocator                                                          */
/* ------------------------------------------------------------------------ */
#define BUDDY_FREE 0x80u
#define BUDDY_ORDER_MASK 0x7Fu

static void buddy_list_init(MemBuddyFree *s) { s->next = s->prev = s; }

static void buddy_list_push(MemBuddyFree *s, MemBuddyFree *n)
{
    n->next = s->next;
    n->prev = s;
    s->next->prev = n;
    s->next = n;
}

static void buddy_list_remove(MemBuddyFree *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->next = n->prev = n;
}

static MemBuddyFree *buddy_node(const MemBuddy *b, uint32_t idx)
{
    return (MemBuddyFree *)(b->base + ((uintptr_t)idx << b->min_shift));
}

static void buddy_push_free(MemBuddy *b, uint32_t idx, uint32_t order)
{
    b->meta[idx] = (uint8_t)(BUDDY_FREE | order);
    buddy_list_push(&b->free_lists[order], buddy_node(b, idx));
}

void mem_buddy_init(MemBuddy *b, MemRegionId region, uint32_t bytes,
                    uint32_t min_block)
{
    uint32_t shift = 0, meta_size, data_bytes, max_order, idx, i;
    if (!pw_is_pow2(min_block) || min_block < sizeof(MemBuddyFree) || min_block < 16)
        mem_misuse("buddy min_block must be a power of two >= 16", region);
    while ((1u << shift) < min_block)
        ++shift;
    if (bytes < min_block * 4u)
        mem_misuse("buddy span too small", region);

    /* metadata upper bound: one byte per min block of the whole request */
    meta_size = pw_align_up(bytes / min_block, 16u);
    if (meta_size + 64u + min_block > bytes)
        mem_misuse("buddy span too small for metadata", region);
    data_bytes = (bytes - meta_size - 64u) & ~(min_block - 1u);

    memset(b, 0, sizeof(*b));
    b->meta = (uint8_t *)mem_calloc(region, meta_size, 16u);
    b->base = (uint8_t *)mem_alloc(region, data_bytes, 64u);
    b->span = data_bytes;
    b->min_block = min_block;
    b->min_shift = shift;
    b->block_count = data_bytes >> shift;

    max_order = 0;
    while (max_order + 1u < MEM_BUDDY_MAX_ORDERS &&
           (1u << (max_order + 1u)) <= b->block_count)
        ++max_order;
    b->order_count = max_order + 1u;
    for (i = 0; i < MEM_BUDDY_MAX_ORDERS; ++i)
        buddy_list_init(&b->free_lists[i]);

    /* Decompose the span into maximal naturally aligned blocks. */
    idx = 0;
    while (idx < b->block_count) {
        uint32_t order = max_order;
        while (order > 0 && ((idx & ((1u << order) - 1u)) != 0 ||
                             idx + (1u << order) > b->block_count))
            --order;
        buddy_push_free(b, idx, order);
        idx += 1u << order;
    }
    b->free_bytes = b->block_count << shift;
    b->used_bytes = 0;
    b->peak_used = 0;
}

void *mem_buddy_alloc(MemBuddy *b, uint32_t size)
{
    uint32_t need_order = 0, order, idx;
    MemBuddyFree *n;
    if (size == 0)
        size = 1;
    while (need_order < b->order_count && (b->min_block << need_order) < size)
        ++need_order;
    if (need_order >= b->order_count)
        return NULL;
    order = need_order;
    while (order < b->order_count && b->free_lists[order].next == &b->free_lists[order])
        ++order;
    if (order >= b->order_count)
        return NULL;

    n = b->free_lists[order].next;
    buddy_list_remove(n);
    idx = (uint32_t)(((uint8_t *)n - b->base) >> b->min_shift);
    while (order > need_order) {
        --order;
        buddy_push_free(b, idx + (1u << order), order);
    }
    b->meta[idx] = (uint8_t)need_order; /* allocated head, free bit clear */
    b->free_bytes -= b->min_block << need_order;
    b->used_bytes += b->min_block << need_order;
    if (b->used_bytes > b->peak_used)
        b->peak_used = b->used_bytes;
    return b->base + ((uintptr_t)idx << b->min_shift);
}

static uint32_t buddy_index_checked(const MemBuddy *b, const void *ptr)
{
    const uint8_t *p = (const uint8_t *)ptr;
    uint32_t ofs, idx;
    if (p < b->base || p >= b->base + b->span)
        mem_halt("mem_buddy: pointer outside allocator");
    ofs = (uint32_t)(p - b->base);
    if (ofs & (b->min_block - 1u))
        mem_halt("mem_buddy: misaligned pointer");
    idx = ofs >> b->min_shift;
    if (b->meta[idx] & BUDDY_FREE)
        mem_halt("mem_buddy: double free");
    if ((b->meta[idx] & BUDDY_ORDER_MASK) >= b->order_count)
        mem_halt("mem_buddy: corrupt metadata");
    return idx;
}

uint32_t mem_buddy_block_size(const MemBuddy *b, const void *ptr)
{
    uint32_t idx = buddy_index_checked(b, ptr);
    return b->min_block << (b->meta[idx] & BUDDY_ORDER_MASK);
}

void mem_buddy_free(MemBuddy *b, void *ptr)
{
    uint32_t idx, order, bytes;
    if (!ptr)
        return;
    idx = buddy_index_checked(b, ptr);
    order = b->meta[idx] & BUDDY_ORDER_MASK;
    bytes = b->min_block << order;
#if MEM_DEBUG
    memset(ptr, MEM_FILL_FREED, bytes);
#endif
    b->free_bytes += bytes;
    b->used_bytes -= bytes;
    b->meta[idx] = 0;

    while (order + 1u < b->order_count) {
        uint32_t buddy = idx ^ (1u << order);
        if (buddy + (1u << order) > b->block_count)
            break;
        if (b->meta[buddy] != (uint8_t)(BUDDY_FREE | order))
            break;
        buddy_list_remove(buddy_node(b, buddy));
        b->meta[buddy] = 0;
        idx = PW_MIN(idx, buddy);
        ++order;
    }
    buddy_push_free(b, idx, order);
}

uint32_t mem_buddy_largest_free(const MemBuddy *b)
{
    uint32_t order = b->order_count;
    while (order-- > 0) {
        if (b->free_lists[order].next != &b->free_lists[order])
            return b->min_block << order;
    }
    return 0;
}

int mem_buddy_check(const MemBuddy *b)
{
    uint32_t order, listed_bytes = 0, listed_heads = 0, meta_heads = 0, i;
    for (order = 0; order < b->order_count; ++order) {
        const MemBuddyFree *s = &b->free_lists[order];
        const MemBuddyFree *n;
        for (n = s->next; n != s; n = n->next) {
            uint32_t idx;
            if (n->next->prev != n)
                return 1; /* broken links */
            idx = (uint32_t)(((const uint8_t *)n - b->base) >> b->min_shift);
            if (idx >= b->block_count || (idx & ((1u << order) - 1u)))
                return 2; /* misaligned free block */
            if (idx + (1u << order) > b->block_count)
                return 3; /* runs past span */
            if (b->meta[idx] != (uint8_t)(BUDDY_FREE | order))
                return 4; /* metadata disagrees */
            listed_bytes += b->min_block << order;
            listed_heads++;
        }
    }
    for (i = 0; i < b->block_count; ++i)
        if (b->meta[i] & BUDDY_FREE)
            meta_heads++;
    if (meta_heads != listed_heads)
        return 5; /* stale free flag */
    if (listed_bytes != b->free_bytes)
        return 6;
    if (b->free_bytes + b->used_bytes != (b->block_count << b->min_shift))
        return 7;
    return 0;
}
