/*
 * rpgdb.c - rpg.dpak loader, lookups and the text/script caches.
 */
#include "rpgdb.h"

#include <stdio.h>
#include <string.h>

#include "inflate.h"

static void set_err(char *err, uint32_t errlen, const char *msg)
{
    if (err && errlen) {
        strncpy(err, msg, errlen - 1);
        err[errlen - 1] = '\0';
    }
}

static int table_ok(const DRpgTable *t, uint32_t elem, uint32_t size)
{
    uint64_t end = (uint64_t)t->ofs + (uint64_t)t->count * elem;
    if (t->count == 0)
        return 1;
    return (t->ofs & 3u) == 0 && t->ofs >= sizeof(DRpgHeader) && end <= size;
}

int rpgdb_validate(const uint8_t *base, uint32_t size)
{
    const DRpgHeader *h = (const DRpgHeader *)base;
    uint32_t i;
    const DRpgObject *objects;
    const DRpgTopic *topics;
    const DRpgInfo *infos;
    const DRpgScript *scripts;
    const DRpgFaction *factions;
    if (size < sizeof(DRpgHeader) || memcmp(h->magic, "DRPG", 4) != 0)
        return 1;
    if (h->version != DRPG_VERSION || h->file_size != size)
        return 2;
    if (!table_ok(&h->objects, sizeof(DRpgObject), size) ||
        !table_ok(&h->actors, sizeof(DRpgActor), size) ||
        !table_ok(&h->lists, sizeof(DRpgListItem), size) ||
        !table_ok(&h->effects, sizeof(DRpgEffect), size) ||
        !table_ok(&h->spells, sizeof(DRpgSpell), size) ||
        !table_ok(&h->factions, sizeof(DRpgFaction), size) ||
        !table_ok(&h->reactions, sizeof(DRpgReaction), size) ||
        !table_ok(&h->globals, sizeof(DRpgGlobal), size) ||
        !table_ok(&h->gmsts, sizeof(DRpgGmst), size) ||
        !table_ok(&h->scripts, sizeof(DRpgScript), size) ||
        !table_ok(&h->vars, sizeof(DRpgVar), size) ||
        !table_ok(&h->starts, sizeof(uint16_t), size) ||
        !table_ok(&h->topics, sizeof(DRpgTopic), size) ||
        !table_ok(&h->infos, sizeof(DRpgInfo), size) ||
        !table_ok(&h->conds, sizeof(DRpgCond), size) ||
        !table_ok(&h->blocks, sizeof(DRpgBlock), size) ||
        !table_ok(&h->skills, sizeof(DRpgSkill), size) ||
        !table_ok(&h->mgefs, sizeof(DRpgMgef), size) ||
        !table_ok(&h->races, sizeof(DRpgRace), size) ||
        !table_ok(&h->travel, sizeof(DRpgTravel), size) ||
        !table_ok(&h->markers, sizeof(DRpgMarker), size))
        return 3;
    if (h->skills.count != RPGDB_SKILLS || h->mgefs.count != RPGDB_EFFECTS)
        return 25;
    if (h->names_size == 0 || (uint64_t)h->names_ofs + h->names_size > size ||
        base[h->names_ofs + h->names_size - 1] != '\0')
        return 4;
    if (h->topics.count && ((h->topic_hash_ofs & 3u) ||
                            (uint64_t)h->topic_hash_ofs + 4ull * h->topics.count > size))
        return 5;
    objects = (const DRpgObject *)(base + h->objects.ofs);
    for (i = 0; i < h->objects.count; ++i) {
        const DRpgObject *o = &objects[i];
        if (i && objects[i - 1].hash >= o->hash)
            return 6;
        if (o->type >= DRPG_OT_COUNT)
            return 7;
        if (o->name != DRPG_NONE && o->name >= h->names_size)
            return 8;
        if (o->script != DRPG_NONE16 && o->script >= h->scripts.count)
            return 9;
        if ((uint64_t)o->list_first + o->list_count > h->lists.count && o->list_count)
            return 10;
        if (o->type == DRPG_OT_NPC && o->stat[2] &&
            ((uint32_t)o->stat[1] > h->travel.count || (uint32_t)o->stat[2] > h->travel.count - (uint32_t)o->stat[1]))
            return 26;
        if ((o->type == DRPG_OT_RACE || o->type == DRPG_OT_BSGN) && o->list2_count &&
            (uint64_t)o->list2_first + o->list2_count > h->lists.count)
            return 12;
        if (o->type == DRPG_OT_NPC || o->type == DRPG_OT_CREA) {
            if ((uint32_t)o->stat[0] >= h->actors.count)
                return 11;
            if (o->list2_count && (uint64_t)o->list2_first + o->list2_count > h->lists.count)
                return 12;
        } else if ((o->type == DRPG_OT_ALCH || o->type == DRPG_OT_INGR) && o->list2_count &&
                   (uint64_t)o->list2_first + o->list2_count > h->effects.count) {
            return 13;
        }
    }
    if (h->player != DRPG_NONE && h->player >= h->objects.count)
        return 14;
    scripts = (const DRpgScript *)(base + h->scripts.ofs);
    for (i = 0; i < h->scripts.count; ++i) {
        if (i && scripts[i - 1].hash >= scripts[i].hash)
            return 15;
        if ((uint64_t)scripts[i].var_first + scripts[i].var_count > h->vars.count)
            return 16;
    }
    for (i = 0; i < h->starts.count; ++i)
        if (((const uint16_t *)(base + h->starts.ofs))[i] >= h->scripts.count)
            return 17;
    topics = (const DRpgTopic *)(base + h->topics.ofs);
    for (i = 0; i < h->topics.count; ++i) {
        uint32_t ti = ((const uint32_t *)(base + h->topic_hash_ofs))[i];
        if (ti >= h->topics.count)
            return 18;
        if ((uint64_t)topics[i].info_first + topics[i].info_count > h->infos.count)
            return 19;
        if (topics[i].name >= h->names_size)
            return 20;
    }
    infos = (const DRpgInfo *)(base + h->infos.ofs);
    for (i = 0; i < h->infos.count; ++i)
        if ((uint64_t)infos[i].cond_first + infos[i].cond_count > h->conds.count)
            return 21;
    factions = (const DRpgFaction *)(base + h->factions.ofs);
    for (i = 0; i < h->factions.count; ++i) {
        if (i && factions[i - 1].hash >= factions[i].hash)
            return 22;
        if ((uint64_t)factions[i].reaction_first + factions[i].reaction_count > h->reactions.count)
            return 23;
    }
    {
        const DRpgSpell *sp = (const DRpgSpell *)(base + h->spells.ofs);
        for (i = 0; i < h->spells.count; ++i)
            if ((uint64_t)sp[i].effect_first + sp[i].effect_count > h->effects.count)
                return 24;
    }
    return 0;
}

static int attach(RpgDb *db, const uint8_t *core, uint32_t size, char *err, uint32_t errlen)
{
    const DRpgHeader *h = (const DRpgHeader *)core;
    char msg[64];
    uint32_t i, max_raw = 0, max_packed = 0;
    int v = rpgdb_validate(core, size);
    if (v) {
        snprintf(msg, sizeof(msg), "rpg/core invalid (check %d)", v);
        set_err(err, errlen, msg);
        return -2;
    }
    db->base = core;
    db->size = size;
    db->hdr = h;
    db->objects = (const DRpgObject *)(core + h->objects.ofs);
    db->actors = (const DRpgActor *)(core + h->actors.ofs);
    db->lists = (const DRpgListItem *)(core + h->lists.ofs);
    db->effects = (const DRpgEffect *)(core + h->effects.ofs);
    db->spells = (const DRpgSpell *)(core + h->spells.ofs);
    db->factions = (const DRpgFaction *)(core + h->factions.ofs);
    db->reactions = (const DRpgReaction *)(core + h->reactions.ofs);
    db->globals = (const DRpgGlobal *)(core + h->globals.ofs);
    db->gmsts = (const DRpgGmst *)(core + h->gmsts.ofs);
    db->scripts = (const DRpgScript *)(core + h->scripts.ofs);
    db->vars = (const DRpgVar *)(core + h->vars.ofs);
    db->starts = (const uint16_t *)(core + h->starts.ofs);
    db->topics = (const DRpgTopic *)(core + h->topics.ofs);
    db->topic_by_hash = (const uint32_t *)(core + h->topic_hash_ofs);
    db->infos = (const DRpgInfo *)(core + h->infos.ofs);
    db->conds = (const DRpgCond *)(core + h->conds.ofs);
    db->blocks = (const DRpgBlock *)(core + h->blocks.ofs);
    db->skills = (const DRpgSkill *)(core + h->skills.ofs);
    db->mgefs = (const DRpgMgef *)(core + h->mgefs.ofs);
    db->races = (const DRpgRace *)(core + h->races.ofs);
    db->travel = (const DRpgTravel *)(core + h->travel.ofs);
    db->markers = (const DRpgMarker *)(core + h->markers.ofs);
    db->names = (const char *)(core + h->names_ofs);
    db->names_size = h->names_size;
    for (i = 0; i < h->blocks.count; ++i) {
        if (db->blocks[i].raw_size > max_raw)
            max_raw = db->blocks[i].raw_size;
        if (db->blocks[i].packed_size > max_packed)
            max_packed = db->blocks[i].packed_size;
    }
    db->slot_cap = max_raw;
    db->packed_cap = max_packed;
    for (i = 0; i < RPGDB_TEXT_SLOTS; ++i) {
        db->slots[i].block = -1;
        db->slots[i].data = max_raw ? mem_try_alloc(MEM_REGION_SCRIPT, max_raw + 1u, 16) : NULL;
        if (max_raw && !db->slots[i].data) {
            set_err(err, errlen, "out of memory for the text cache");
            return -3;
        }
    }
    db->packed = max_packed ? mem_try_alloc(MEM_REGION_SCRIPT, max_packed, 16) : NULL;
    db->script_cache = mem_try_alloc(MEM_REGION_SCRIPT, RPGDB_SCRIPT_CACHE, 16);
    db->script_code = h->scripts.count
        ? mem_try_alloc(MEM_REGION_SCRIPT, h->scripts.count * (uint32_t)sizeof(uint8_t *), 16)
        : NULL;
    if ((max_packed && !db->packed) || !db->script_cache || (h->scripts.count && !db->script_code)) {
        set_err(err, errlen, "out of memory for the script cache");
        return -3;
    }
    if (db->script_code)
        memset((void *)db->script_code, 0, h->scripts.count * sizeof(uint8_t *));
    return 0;
}

int rpgdb_attach_core(RpgDb *db, const uint8_t *core, uint32_t size, char *err, uint32_t errlen)
{
    Pak keep = db->pak;
    memset(db, 0, sizeof(*db));
    db->pak = keep;
    return attach(db, core, size, err, errlen);
}

int rpgdb_open(RpgDb *db, const char *path, char *err, uint32_t errlen)
{
    uint32_t size = 0;
    const uint8_t *core;
    memset(db, 0, sizeof(*db));
    if (pak_open(&db->pak, path, MEM_REGION_DB, err, errlen) != 0)
        return -1;
    core = pak_load(&db->pak, pw_hash_name("rpg/core"), MEM_REGION_DB, 16, &size);
    if (!core) /* very large load orders: borrow the script region */
        core = pak_load(&db->pak, pw_hash_name("rpg/core"), MEM_REGION_SCRIPT, 16, &size);
    if (!core) {
        set_err(err, errlen, "rpg.dpak has no rpg/core (or the DB/script regions are full)");
        pak_close(&db->pak);
        return -1;
    }
    if (attach(db, core, size, err, errlen) != 0) {
        pak_close(&db->pak);
        return -2;
    }
    return 0;
}

void rpgdb_close(RpgDb *db)
{
    if (db->pak.handle)
        pak_close(&db->pak);
    memset(db, 0, sizeof(*db));
}

/* ------------------------------------------------------------------------ */
/* lookups                                                                  */
/* ------------------------------------------------------------------------ */
const char *rpgdb_name(const RpgDb *db, uint32_t ofs)
{
    if (!db->names || ofs == DRPG_NONE || ofs >= db->names_size)
        return "";
    return db->names + ofs;
}

#define BSEARCH_HASH(arr, count, h)                    \
    do {                                               \
        uint32_t lo_ = 0, hi_ = (count);               \
        while (lo_ < hi_) {                            \
            uint32_t mid_ = (lo_ + hi_) >> 1;          \
            if ((arr)[mid_].hash < (h))                \
                lo_ = mid_ + 1;                        \
            else                                       \
                hi_ = mid_;                            \
        }                                              \
        if (lo_ < (count) && (arr)[lo_].hash == (h))   \
            return (int)lo_;                           \
        return -1;                                     \
    } while (0)

static int object_index(const RpgDb *db, uint32_t hash)
{
    BSEARCH_HASH(db->objects, db->hdr->objects.count, hash);
}

const DRpgObject *rpgdb_object(const RpgDb *db, uint32_t hash)
{
    int i;
    if (!db->hdr)
        return NULL;
    i = object_index(db, hash);
    return i < 0 ? NULL : &db->objects[i];
}

const DRpgActor *rpgdb_actor(const RpgDb *db, const DRpgObject *o)
{
    if (!o || (o->type != DRPG_OT_NPC && o->type != DRPG_OT_CREA))
        return NULL;
    return &db->actors[o->stat[0]];
}

int rpgdb_faction_index(const RpgDb *db, uint32_t hash)
{
    if (!db->hdr)
        return -1;
    BSEARCH_HASH(db->factions, db->hdr->factions.count, hash);
}

const DRpgFaction *rpgdb_faction(const RpgDb *db, uint32_t hash)
{
    int i = rpgdb_faction_index(db, hash);
    return i < 0 ? NULL : &db->factions[i];
}

static int spell_index(const RpgDb *db, uint32_t hash)
{
    BSEARCH_HASH(db->spells, db->hdr->spells.count, hash);
}

const DRpgSpell *rpgdb_spell(const RpgDb *db, uint32_t hash)
{
    int i;
    if (!db->hdr)
        return NULL;
    i = spell_index(db, hash);
    return i < 0 ? NULL : &db->spells[i];
}

int rpgdb_global(const RpgDb *db, uint32_t hash)
{
    if (!db->hdr)
        return -1;
    BSEARCH_HASH(db->globals, db->hdr->globals.count, hash);
}

static int gmst_index(const RpgDb *db, uint32_t hash)
{
    BSEARCH_HASH(db->gmsts, db->hdr->gmsts.count, hash);
}

const DRpgGmst *rpgdb_gmst(const RpgDb *db, uint32_t hash)
{
    int i;
    if (!db->hdr)
        return NULL;
    i = gmst_index(db, hash);
    return i < 0 ? NULL : &db->gmsts[i];
}

const char *rpgdb_gmst_string(const RpgDb *db, const char *id, const char *fallback)
{
    const DRpgGmst *g = rpgdb_gmst(db, pw_hash_name(id));
    if (!g || g->type != DRPG_GMST_STRING)
        return fallback;
    {
        const char *s = rpgdb_name(db, g->value);
        return *s ? s : fallback;
    }
}

int32_t rpgdb_gmst_int(const RpgDb *db, const char *id, int32_t fallback)
{
    const DRpgGmst *g = rpgdb_gmst(db, pw_hash_name(id));
    if (!g)
        return fallback;
    if (g->type == DRPG_GMST_INT)
        return (int32_t)g->value;
    if (g->type == DRPG_GMST_FLOAT) {
        float f;
        memcpy(&f, &g->value, 4);
        return (int32_t)f;
    }
    return fallback;
}

float rpgdb_gmst_float(const RpgDb *db, const char *id, float fallback)
{
    const DRpgGmst *g = rpgdb_gmst(db, pw_hash_name(id));
    float f;
    if (!g)
        return fallback;
    if (g->type == DRPG_GMST_FLOAT) {
        memcpy(&f, &g->value, 4);
        return f;
    }
    if (g->type == DRPG_GMST_INT)
        return (float)(int32_t)g->value;
    return fallback;
}

int rpgdb_script(const RpgDb *db, uint32_t hash)
{
    if (!db->hdr)
        return -1;
    BSEARCH_HASH(db->scripts, db->hdr->scripts.count, hash);
}

const DRpgVar *rpgdb_script_var(const RpgDb *db, uint32_t script, uint32_t var_hash)
{
    const DRpgScript *s;
    const DRpgVar *v;
    uint32_t lo = 0, hi;
    if (!db->hdr || script >= db->hdr->scripts.count)
        return NULL;
    s = &db->scripts[script];
    v = db->vars + s->var_first;
    hi = s->var_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (v[mid].hash < var_hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < s->var_count && v[lo].hash == var_hash) ? &v[lo] : NULL;
}

int rpgdb_topic(const RpgDb *db, uint32_t hash)
{
    uint32_t lo = 0, hi;
    if (!db->hdr)
        return -1;
    hi = db->hdr->topics.count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (db->topics[db->topic_by_hash[mid]].hash < hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < db->hdr->topics.count && db->topics[db->topic_by_hash[lo]].hash == hash)
        return (int)db->topic_by_hash[lo];
    return -1;
}

static int race_index(const RpgDb *db, uint32_t hash)
{
    BSEARCH_HASH(db->races, db->hdr->races.count, hash);
}

const DRpgRace *rpgdb_race(const RpgDb *db, uint32_t hash)
{
    int i;
    if (!db->hdr)
        return NULL;
    i = race_index(db, hash);
    return i < 0 ? NULL : &db->races[i];
}

const DRpgMgef *rpgdb_mgef(const RpgDb *db, int effect)
{
    if (!db->hdr || effect < 0 || effect >= RPGDB_EFFECTS)
        return NULL;
    return &db->mgefs[effect];
}

const char *rpgdb_topic_name(const RpgDb *db, uint32_t topic)
{
    if (!db->hdr || topic >= db->hdr->topics.count)
        return "";
    return rpgdb_name(db, db->topics[topic].name);
}

/* ------------------------------------------------------------------------ */
/* text blocks                                                              */
/* ------------------------------------------------------------------------ */
static RpgTextSlot *load_block(RpgDb *db, uint32_t block)
{
    RpgTextSlot *slot = NULL;
    const DRpgBlock *b;
    const DPakEntry *e;
    uint32_t i, got = 0;
    if (!db->hdr || block >= db->hdr->blocks.count)
        return NULL;
    for (i = 0; i < RPGDB_TEXT_SLOTS; ++i) {
        if (db->slots[i].block == (int32_t)block) {
            db->slots[i].stamp = ++db->stamp;
            return &db->slots[i];
        }
    }
    for (i = 0; i < RPGDB_TEXT_SLOTS; ++i) {
        RpgTextSlot *c = &db->slots[i];
        if (c->block < 0) {
            slot = c;
            break;
        }
        if (!slot || c->stamp < slot->stamp)
            slot = c;
    }
    b = &db->blocks[block];
    if (!db->pak.handle || !slot || !slot->data)
        return NULL;
    e = pak_find(&db->pak, b->entry_hash);
    if (!e || e->size != b->packed_size || e->size > db->packed_cap)
        return NULL;
    slot->block = -1;
    if (pak_read(&db->pak, e, db->packed) != 0)
        return NULL;
    if (pw_inflate(db->packed, e->size, slot->data, db->slot_cap, &got) != PW_INFLATE_OK ||
        got != b->raw_size)
        return NULL;
    slot->data[got] = '\0'; /* guard so a corrupt text can never run off the slot */
    slot->size = got;
    slot->block = (int32_t)block;
    slot->stamp = ++db->stamp;
    db->text_loads++;
    return slot;
}

const uint8_t *rpgdb_blob(RpgDb *db, uint32_t sid, uint32_t *avail)
{
    RpgTextSlot *slot;
    uint32_t ofs = sid & 0xFFFFu;
    if (sid == DRPG_NONE)
        return NULL;
    slot = load_block(db, sid >> 16);
    if (!slot || ofs >= slot->size)
        return NULL;
    if (avail)
        *avail = slot->size - ofs;
    return slot->data + ofs;
}

uint32_t rpgdb_text(RpgDb *db, uint32_t sid, char *out, uint32_t cap)
{
    uint32_t avail = 0, n = 0;
    const uint8_t *p;
    if (!cap)
        return 0;
    out[0] = '\0';
    p = rpgdb_blob(db, sid, &avail);
    if (!p)
        return 0;
    while (n + 1 < cap && n < avail && p[n])
        n++;
    memcpy(out, p, n);
    out[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------------ */
/* script bytecode                                                          */
/* ------------------------------------------------------------------------ */
const uint8_t *rpgdb_script_code(RpgDb *db, uint32_t script)
{
    const DRpgScript *s;
    const DPakEntry *e;
    char key[160];
    uint32_t need;
    uint8_t *dst;
    const char *name;
    if (!db->hdr || script >= db->hdr->scripts.count)
        return NULL;
    if (db->script_code[script])
        return db->script_code[script];
    s = &db->scripts[script];
    need = s->code_size + s->strings_size;
    if (!db->pak.handle || need == 0 || need + 1u > RPGDB_SCRIPT_CACHE)
        return NULL;
    name = rpgdb_name(db, s->name);
    snprintf(key, sizeof(key), "script/%s", name);
    e = pak_find(&db->pak, pw_hash_name(key));
    if (!e || e->size != need)
        return NULL;
    if (db->script_cache_used + pw_align_up(need + 1u, 4) > RPGDB_SCRIPT_CACHE) {
        memset((void *)db->script_code, 0, db->hdr->scripts.count * sizeof(uint8_t *));
        db->script_cache_used = 0;
        db->script_flushes++;
    }
    dst = db->script_cache + db->script_cache_used;
    if (pak_read(&db->pak, e, dst) != 0)
        return NULL;
    dst[need] = '\0';
    db->script_cache_used += pw_align_up(need + 1u, 4);
    db->script_code[script] = dst;
    db->script_loads++;
    return dst;
}
