/*
 * rpgdb.h - the cooked RPG database (rpg.dpak).
 *
 * rpg/core stays resident in the DB region: object, faction, spell, global,
 * GMST, script, topic, INFO and condition tables plus the name pool, all
 * validated once at load and then accessed by binary search on hashes.
 * Dialogue/book text and result scripts live in ~32 KB raw-DEFLATE blocks
 * that are read from the Memory Stick and inflated into a small LRU cache
 * on demand. Script bytecode is paged into a bump cache that is flushed
 * wholesale when full (scripts are re-read on their next run).
 */
#ifndef PSPWIND_RPGDB_H
#define PSPWIND_RPGDB_H

#include "common.h"
#include "fmt.h"
#include "mem.h"
#include "pak.h"

#define RPGDB_TEXT_SLOTS 4
#define RPGDB_SCRIPT_CACHE PW_KB(384)
#define RPGDB_SKILLS 27
#define RPGDB_EFFECTS 143

typedef struct {
    int32_t block;      /* -1 = empty */
    uint32_t stamp;
    uint8_t *data;
    uint32_t size;
} RpgTextSlot;

typedef struct {
    Pak pak;
    const uint8_t *base;
    uint32_t size;
    const DRpgHeader *hdr;
    const DRpgObject *objects;
    const DRpgActor *actors;
    const DRpgListItem *lists;
    const DRpgEffect *effects;
    const DRpgSpell *spells;
    const DRpgFaction *factions;
    const DRpgReaction *reactions;
    const DRpgGlobal *globals;
    const DRpgGmst *gmsts;
    const DRpgScript *scripts;
    const DRpgVar *vars;
    const uint16_t *starts;
    const DRpgTopic *topics;
    const uint32_t *topic_by_hash;
    const DRpgInfo *infos;
    const DRpgCond *conds;
    const DRpgBlock *blocks;
    const DRpgSkill *skills;     /* 27 entries (NULL in a pre-v2 core) */
    const DRpgMgef *mgefs;       /* RPGDB_EFFECTS entries */
    const DRpgRace *races;
    const DRpgTravel *travel;
    const DRpgMarker *markers;
    const char *names;
    uint32_t names_size;
    /* text cache */
    RpgTextSlot slots[RPGDB_TEXT_SLOTS];
    uint8_t *packed;
    uint32_t packed_cap, slot_cap;
    uint32_t stamp;
    uint32_t text_loads;
    /* script cache */
    uint8_t *script_cache;
    uint32_t script_cache_used;
    const uint8_t **script_code; /* per script, NULL when not cached */
    uint32_t script_loads, script_flushes;
} RpgDb;

/* Opens path (rpg.dpak): core into the DB region, caches into SCRIPT. */
int rpgdb_open(RpgDb *db, const char *path, char *err, uint32_t errlen);
/* Same, from an in-memory core (tests); text/scripts come from pak if open. */
int rpgdb_attach_core(RpgDb *db, const uint8_t *core, uint32_t size, char *err, uint32_t errlen);
void rpgdb_close(RpgDb *db);

const char *rpgdb_name(const RpgDb *db, uint32_t ofs); /* "" for DRPG_NONE / bad */
const DRpgObject *rpgdb_object(const RpgDb *db, uint32_t hash);
const DRpgActor *rpgdb_actor(const RpgDb *db, const DRpgObject *o); /* NPC/CREA or NULL */
const DRpgFaction *rpgdb_faction(const RpgDb *db, uint32_t hash);
int rpgdb_faction_index(const RpgDb *db, uint32_t hash); /* -1 if missing */
const DRpgSpell *rpgdb_spell(const RpgDb *db, uint32_t hash);
int rpgdb_global(const RpgDb *db, uint32_t hash);       /* index or -1 */
const DRpgGmst *rpgdb_gmst(const RpgDb *db, uint32_t hash);
const char *rpgdb_gmst_string(const RpgDb *db, const char *id, const char *fallback);
int32_t rpgdb_gmst_int(const RpgDb *db, const char *id, int32_t fallback);
float rpgdb_gmst_float(const RpgDb *db, const char *id, float fallback);
int rpgdb_script(const RpgDb *db, uint32_t hash);       /* index or -1 */
const DRpgVar *rpgdb_script_var(const RpgDb *db, uint32_t script, uint32_t var_hash);
int rpgdb_topic(const RpgDb *db, uint32_t hash);        /* index or -1 */
const DRpgRace *rpgdb_race(const RpgDb *db, uint32_t hash);
const DRpgMgef *rpgdb_mgef(const RpgDb *db, int effect); /* NULL when out of range */
const char *rpgdb_topic_name(const RpgDb *db, uint32_t topic);

/* Blob access: returns a pointer valid until the next blob/text call (the
 * cache slot may be reused), *avail = bytes from there to the block end. */
const uint8_t *rpgdb_blob(RpgDb *db, uint32_t sid, uint32_t *avail);
/* Copies a NUL-terminated text blob into out (truncated to cap). Returns the
 * copied length, or 0 (out = "") when unavailable. */
uint32_t rpgdb_text(RpgDb *db, uint32_t sid, char *out, uint32_t cap);
/* Script code (code_size bytes) followed by its string pool; valid until the
 * next rpgdb_script_code() call. NULL when missing or unreadable. */
const uint8_t *rpgdb_script_code(RpgDb *db, uint32_t script);

/* Structural validation of a core blob; 0 = valid, else a check id. */
int rpgdb_validate(const uint8_t *base, uint32_t size);

#endif /* PSPWIND_RPGDB_H */
