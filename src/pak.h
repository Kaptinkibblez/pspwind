/*
 * pak.h - DPAK asset packs on the Memory Stick (or the host file system).
 *
 * The directory (16 bytes per entry, sorted by name hash) is loaded once
 * into a persistent arena region; lookups are binary searches; payloads are
 * read on demand straight into caller-provided arena memory. The file stays
 * open for the whole session so zone loads do no directory walks.
 */
#ifndef PSPWIND_PAK_H
#define PSPWIND_PAK_H

#include "common.h"
#include "fmt.h"
#include "mem.h"

typedef struct {
    intptr_t handle; /* SceUID on PSP, FILE* on host; 0 when closed */
    uint32_t file_size;
    uint32_t count;
    uint16_t kind;
    const DPakEntry *entries;
    char path[160];
} Pak;

/* Opens path and loads its directory into region. Returns 0 or a negative
 * error; err (if given) receives a message. */
int pak_open(Pak *p, const char *path, MemRegionId region, char *err, uint32_t errlen);
void pak_close(Pak *p);
const DPakEntry *pak_find(const Pak *p, uint32_t hash);
/* Reads a whole entry into dst (entry->size bytes). Returns 0 on success. */
int pak_read(const Pak *p, const DPakEntry *e, void *dst);
/* Finds + allocates (mem_try_alloc, low end) + reads. NULL if missing or the
 * region is full; *size_out receives the size when non-NULL. */
void *pak_load(const Pak *p, uint32_t hash, MemRegionId region, uint32_t align,
               uint32_t *size_out);

#endif /* PSPWIND_PAK_H */
