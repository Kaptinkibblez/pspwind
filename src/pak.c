/*
 * pak.c - DPAK reader (see pak.h).
 */
#include "pak.h"

#include <stdio.h>
#include <string.h>

#if PW_PSP
#include <pspiofilemgr.h>
#endif

/* ------------------------------------------------------------------------ */
/* minimal file layer                                                       */
/* ------------------------------------------------------------------------ */
static intptr_t file_open(const char *path)
{
#if PW_PSP
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    return fd < 0 ? 0 : (intptr_t)fd + 1; /* +1 so fd 0 stays distinct from "closed" */
#else
    FILE *f = fopen(path, "rb");
    return (intptr_t)f;
#endif
}

static void file_close(intptr_t h)
{
#if PW_PSP
    sceIoClose((SceUID)(h - 1));
#else
    fclose((FILE *)h);
#endif
}

static int64_t file_size(intptr_t h)
{
#if PW_PSP
    SceOff size = sceIoLseek((SceUID)(h - 1), 0, PSP_SEEK_END);
    sceIoLseek((SceUID)(h - 1), 0, PSP_SEEK_SET);
    return (int64_t)size;
#else
    long size;
    if (fseek((FILE *)h, 0, SEEK_END) != 0)
        return -1;
    size = ftell((FILE *)h);
    fseek((FILE *)h, 0, SEEK_SET);
    return size;
#endif
}

static int file_read_at(intptr_t h, uint32_t ofs, void *dst, uint32_t size)
{
#if PW_PSP
    uint8_t *d = (uint8_t *)dst;
    if (sceIoLseek((SceUID)(h - 1), (SceOff)ofs, PSP_SEEK_SET) != (SceOff)ofs)
        return -1;
    while (size > 0) {
        /* large single reads are fine on the MS driver; chunk to stay polite
         * to the callback thread during long zone loads */
        uint32_t chunk = size > PW_KB(256) ? PW_KB(256) : size;
        int got = sceIoRead((SceUID)(h - 1), d, chunk);
        if (got <= 0)
            return -1;
        d += got;
        size -= (uint32_t)got;
    }
    return 0;
#else
    if (fseek((FILE *)h, (long)ofs, SEEK_SET) != 0)
        return -1;
    return fread(dst, 1, size, (FILE *)h) == size ? 0 : -1;
#endif
}

/* ------------------------------------------------------------------------ */
/* DPAK                                                                     */
/* ------------------------------------------------------------------------ */
static int fail(char *err, uint32_t errlen, const char *msg, const char *path)
{
    if (err && errlen)
        snprintf(err, errlen, "%s: %s", path, msg);
    return -1;
}

int pak_open(Pak *p, const char *path, MemRegionId region, char *err, uint32_t errlen)
{
    DPakHeader hdr;
    int64_t size;
    DPakEntry *dir;
    uint32_t i, dir_bytes;

    memset(p, 0, sizeof(*p));
    snprintf(p->path, sizeof(p->path), "%s", path);
    p->handle = file_open(path);
    if (!p->handle)
        return fail(err, errlen, "cannot open", path);
    size = file_size(p->handle);
    if (size < (int64_t)sizeof(hdr) || size > 0x7FFFFFFF) {
        pak_close(p);
        return fail(err, errlen, "bad file size", path);
    }
    p->file_size = (uint32_t)size;
    if (file_read_at(p->handle, 0, &hdr, sizeof(hdr)) != 0 || hdr.magic != DPAK_MAGIC ||
        hdr.version != DPAK_VERSION) {
        pak_close(p);
        return fail(err, errlen, "not a DPAK v1", path);
    }
    if (hdr.entry_count > 0x100000u || hdr.dir_ofs > p->file_size ||
        hdr.entry_count * (uint32_t)sizeof(DPakEntry) > p->file_size - hdr.dir_ofs) {
        pak_close(p);
        return fail(err, errlen, "corrupt directory", path);
    }
    dir_bytes = hdr.entry_count * (uint32_t)sizeof(DPakEntry);
    dir = (DPakEntry *)mem_try_alloc(region, dir_bytes ? dir_bytes : 16u, 16u);
    if (!dir) {
        pak_close(p);
        return fail(err, errlen, "directory does not fit in memory", path);
    }
    if (dir_bytes && file_read_at(p->handle, hdr.dir_ofs, dir, dir_bytes) != 0) {
        pak_close(p);
        return fail(err, errlen, "short directory read", path);
    }
    for (i = 0; i < hdr.entry_count; ++i) {
        if (dir[i].ofs > p->file_size || dir[i].size > p->file_size - dir[i].ofs ||
            (i > 0 && dir[i].hash < dir[i - 1].hash)) {
            pak_close(p);
            return fail(err, errlen, "corrupt entry", path);
        }
    }
    p->entries = dir;
    p->count = hdr.entry_count;
    p->kind = hdr.kind;
    return 0;
}

void pak_close(Pak *p)
{
    if (p->handle)
        file_close(p->handle);
    p->handle = 0;
    p->entries = NULL;
    p->count = 0;
}

const DPakEntry *pak_find(const Pak *p, uint32_t hash)
{
    uint32_t lo = 0, hi = p->count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) >> 1);
        uint32_t h = p->entries[mid].hash;
        if (h == hash)
            return &p->entries[mid];
        if (h < hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

int pak_read(const Pak *p, const DPakEntry *e, void *dst)
{
    if (!p->handle || !e)
        return -1;
    return file_read_at(p->handle, e->ofs, dst, e->size);
}

void *pak_load(const Pak *p, uint32_t hash, MemRegionId region, uint32_t align,
               uint32_t *size_out)
{
    const DPakEntry *e = pak_find(p, hash);
    MemMark m;
    void *dst;
    if (!e)
        return NULL;
    m = mem_mark(region);
    dst = mem_try_alloc(region, e->size ? e->size : 1u, align ? align : 16u);
    if (!dst)
        return NULL;
    if (pak_read(p, e, dst) != 0) {
        mem_release(region, m);
        return NULL;
    }
    if (size_out)
        *size_out = e->size;
    return dst;
}
