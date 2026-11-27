/*
 * save.c - saved games (see save.h).
 */
#if !defined(__PSP__) && !defined(__psp__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* mkdir */
#endif

#include "save.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#if PW_PSP
#include <pspiofilemgr.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define SAVE_MAX_ACTORS 512
#define SAVE_MAX_BYTES PW_MB(2)

typedef struct {
    float hour, timescale;
    int32_t day, month, year, days_passed;
    uint32_t rng;
    char pc_name[32];
    uint32_t pc_race, pc_class;
    int32_t pc_female;
    int32_t journal_updated;
} SaveTime;

typedef struct {
    uint32_t zone_entry;
    float pos[3];
    float yaw, pitch;
    uint8_t running, sneaking, flying, controls_disabled;
    uint32_t menus_enabled;
} SaveGameState;

typedef struct {
    uint32_t object, refnum;
    float pos[3];
    float yaw;
    float home[3];
    DRpgAiPackage pkg;
    int32_t target;
    float life;
    uint32_t summoner;
    int32_t summon_effect;
    uint8_t state, pkg_done, ally, fled, has_pkg, hidden, pad[2];
} SaveActor;

/* ------------------------------------------------------------------------ */
/* CRC-32 (IEEE, reflected)                                                 */
/* ------------------------------------------------------------------------ */
uint32_t save_crc32(uint32_t crc, const void *data, uint32_t size)
{
    static uint32_t table[256];
    static int ready;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;
    if (!ready) {
        for (i = 0; i < 256; ++i) {
            uint32_t c = i, k;
            for (k = 0; k < 8; ++k)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = 1;
    }
    crc = ~crc;
    for (i = 0; i < size; ++i)
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------------------------ */
/* files                                                                    */
/* ------------------------------------------------------------------------ */
typedef struct {
    intptr_t h;
    uint32_t crc, size;
    int err;
} SaveFile;

static void slot_path(char *out, size_t n, const char *dir, int slot, const char *ext)
{
    snprintf(out, n, "%s/slot%d.%s", dir, slot, ext);
}

static void make_dir(const char *dir)
{
#if PW_PSP
    sceIoMkdir(dir, 0777);
#else
    mkdir(dir, 0777);
#endif
}

static int sf_create(SaveFile *f, const char *path)
{
    memset(f, 0, sizeof(*f));
#if PW_PSP
    {
        SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        f->h = fd < 0 ? 0 : (intptr_t)fd + 1;
    }
#else
    f->h = (intptr_t)fopen(path, "wb");
#endif
    return f->h ? 0 : -1;
}

static void sf_raw(SaveFile *f, const void *data, uint32_t n)
{
    if (f->err || !n)
        return;
#if PW_PSP
    if (sceIoWrite((SceUID)(f->h - 1), data, n) != (int)n)
        f->err = 1;
#else
    if (fwrite(data, 1, n, (FILE *)f->h) != n)
        f->err = 1;
#endif
}

static void sf_write(SaveFile *f, const void *data, uint32_t n)
{
    sf_raw(f, data, n);
    f->crc = save_crc32(f->crc, data, n);
    f->size += n;
}

static int sf_finish(SaveFile *f, const DSaveHeader *h)
{
#if PW_PSP
    SceUID fd = (SceUID)(f->h - 1);
    if (!f->err && sceIoLseek(fd, 0, PSP_SEEK_SET) != 0)
        f->err = 1;
    sf_raw(f, h, sizeof(*h));
    sceIoClose(fd);
#else
    FILE *fp = (FILE *)f->h;
    if (!f->err && fseek(fp, 0, SEEK_SET) != 0)
        f->err = 1;
    sf_raw(f, h, sizeof(*h));
    if (fclose(fp) != 0)
        f->err = 1;
#endif
    return f->err ? -1 : 0;
}

static int replace_file(const char *tmp, const char *path)
{
#if PW_PSP
    sceIoRemove(path);
    return sceIoRename(tmp, path) < 0 ? -1 : 0;
#else
    return rename(tmp, path);
#endif
}

static int32_t read_file(const char *path, uint8_t *dst, uint32_t cap)
{
#if PW_PSP
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    int n;
    if (fd < 0)
        return -1;
    n = sceIoRead(fd, dst, cap);
    sceIoClose(fd);
    return n;
#else
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return -1;
    n = fread(dst, 1, cap, f);
    fclose(f);
    return (int32_t)n;
#endif
}

/* ------------------------------------------------------------------------ */
/* saving                                                                   */
/* ------------------------------------------------------------------------ */
static void section(SaveFile *f, uint32_t tag, uint32_t size)
{
    uint32_t hdr[2];
    hdr[0] = tag;
    hdr[1] = size;
    sf_write(f, hdr, sizeof(hdr));
}

static void fingerprint(const Game *g, DSaveHeader *h)
{
    const DRpgHeader *d = g->rpg->db.hdr;
    h->db_size = d->file_size;
    h->db_objects = d->objects.count;
    h->db_infos = d->infos.count;
    h->struct_sizes = (uint32_t)sizeof(Rpg) ^ ((uint32_t)sizeof(AiActor) << 16);
}

static uint32_t count_used_props(const Vm *vm)
{
    uint32_t i, n = 0;
    for (i = 0; i < VM_PROP_SLOTS; ++i)
        n += vm->props[i].used != 0;
    return n;
}

static int item_used(const VmItem *it)
{
    return it->owner != 0 || it->item != 0;
}

static uint32_t count_used_items(const Vm *vm)
{
    uint32_t i, n = 0;
    for (i = 0; i < VM_ITEM_SLOTS; ++i)
        n += (uint32_t)item_used(&vm->items[i]);
    return n;
}

int save_game(Game *g, const char *dir, int slot, float playtime, char *err, uint32_t errlen)
{
    static SaveActor actors[SAVE_MAX_ACTORS];
    static Rpg rpg_copy;
    char tmp[220], path[220];
    SaveFile f;
    DSaveHeader h;
    Vm *vm;
    VmRef pc;
    uint32_t i, n, topics, infos, factions;
    if (!g->rpg || !g->zone.hdr) {
        snprintf(err, errlen, "Nothing to save.");
        return -1;
    }
    vm = &g->rpg->vm;
    if (slot < 0 || slot >= SAVE_SLOTS) {
        snprintf(err, errlen, "No such slot.");
        return -1;
    }
    if (g->player_dead || g->rpg->dlg.active || vm_message_front(vm)) {
        snprintf(err, errlen, "You cannot save right now.");
        return -1;
    }
    pc = vm_player_ref(vm);
    make_dir(dir);
    slot_path(tmp, sizeof(tmp), dir, slot, "tmp");
    slot_path(path, sizeof(path), dir, slot, "sav");
    if (sf_create(&f, tmp) != 0) {
        snprintf(err, errlen, "Cannot write %s.", tmp);
        return -1;
    }
    memset(&h, 0, sizeof(h));
    sf_raw(&f, &h, sizeof(h)); /* placeholder */

    /* VM: globals, time */
    section(&f, TAG('V', 'M', 'G', 'L'), 4u + vm->global_count * (uint32_t)sizeof(VmValue));
    sf_write(&f, &vm->global_count, 4);
    sf_write(&f, vm->globals, vm->global_count * (uint32_t)sizeof(VmValue));
    {
        SaveTime t;
        memset(&t, 0, sizeof(t));
        t.hour = vm->hour;
        t.timescale = vm->timescale;
        t.day = vm->day;
        t.month = vm->month;
        t.year = vm->year;
        t.days_passed = vm->days_passed;
        t.rng = vm->rng;
        memcpy(t.pc_name, vm->pc_name, sizeof(t.pc_name));
        t.pc_race = vm->pc_race;
        t.pc_class = vm->pc_class;
        t.pc_female = vm->pc_female;
        t.journal_updated = vm->journal_updated;
        section(&f, TAG('V', 'M', 'T', 'I'), sizeof(t));
        sf_write(&f, &t, sizeof(t));
    }
    /* script instances */
    section(&f, TAG('V', 'M', 'I', 'N'),
            8u + vm->inst_count * (uint32_t)sizeof(VmInstance) + vm->locals_used * (uint32_t)sizeof(VmValue));
    sf_write(&f, &vm->inst_count, 4);
    sf_write(&f, &vm->locals_used, 4);
    sf_write(&f, vm->inst, vm->inst_count * (uint32_t)sizeof(VmInstance));
    sf_write(&f, vm->locals, vm->locals_used * (uint32_t)sizeof(VmValue));
    /* stat store and inventories, slot exact */
    n = count_used_props(vm);
    section(&f, TAG('V', 'M', 'P', 'R'), 4u + n * (4u + (uint32_t)sizeof(VmProp)));
    sf_write(&f, &n, 4);
    for (i = 0; i < VM_PROP_SLOTS; ++i)
        if (vm->props[i].used) {
            sf_write(&f, &i, 4);
            sf_write(&f, &vm->props[i], sizeof(VmProp));
        }
    n = count_used_items(vm);
    section(&f, TAG('V', 'M', 'I', 'T'), 4u + n * (4u + (uint32_t)sizeof(VmItem)));
    sf_write(&f, &n, 4);
    for (i = 0; i < VM_ITEM_SLOTS; ++i)
        if (item_used(&vm->items[i])) {
            sf_write(&f, &i, 4);
            sf_write(&f, &vm->items[i], sizeof(VmItem));
        }
    /* journal, topics, factions */
    topics = g->rpg->db.hdr->topics.count;
    infos = g->rpg->db.hdr->infos.count;
    factions = g->rpg->db.hdr->factions.count;
    section(&f, TAG('V', 'M', 'J', 'R'),
            16u + topics * 3u + vm->journal_count * (uint32_t)sizeof(VmJournalEntry) + (topics + 7u) / 8u +
                (infos + 7u) / 8u + factions * 4u);
    sf_write(&f, &topics, 4);
    sf_write(&f, &infos, 4);
    sf_write(&f, &factions, 4);
    sf_write(&f, &vm->journal_count, 4);
    sf_write(&f, vm->quest_index, topics * 2u);
    sf_write(&f, vm->quest_flags, topics);
    sf_write(&f, vm->journal, vm->journal_count * (uint32_t)sizeof(VmJournalEntry));
    sf_write(&f, vm->known_topics, (topics + 7u) / 8u);
    sf_write(&f, vm->heard_infos, (infos + 7u) / 8u);
    sf_write(&f, vm->pc_rank, factions);
    sf_write(&f, vm->pc_expelled, factions);
    sf_write(&f, vm->pc_facrep, factions * 2u);
    /* mechanics */
    rpg_copy = g->rpg->rpg;
    rpg_copy.vm = NULL;
    rpg_copy.db = NULL;
    memset(&rpg_copy.host, 0, sizeof(rpg_copy.host));
    section(&f, TAG('R', 'P', 'G', 'S'), (uint32_t)sizeof(Rpg));
    sf_write(&f, &rpg_copy, sizeof(Rpg));
    /* game */
    {
        SaveGameState s;
        memset(&s, 0, sizeof(s));
        s.zone_entry = g->world->zone_list[g->zone.index].entry_hash;
        s.pos[0] = g->body.pos.x;
        s.pos[1] = g->body.pos.y;
        s.pos[2] = g->body.pos.z;
        s.yaw = g->yaw;
        s.pitch = g->pitch;
        s.running = (uint8_t)g->running;
        s.sneaking = (uint8_t)g->sneaking;
        s.flying = (uint8_t)g->flying;
        s.controls_disabled = (uint8_t)g->controls_disabled;
        s.menus_enabled = g->menus_enabled;
        section(&f, TAG('G', 'A', 'M', 'E'), sizeof(s));
        sf_write(&f, &s, sizeof(s));
    }
    /* AI */
    n = PW_MIN(g->actors.count, (uint32_t)SAVE_MAX_ACTORS);
    for (i = 0; i < n; ++i) {
        const Actor *a = &g->actors.list[i];
        const DZoneInstance *in = &g->zone.instances[a->instance];
        SaveActor *sa = &actors[i];
        memset(sa, 0, sizeof(*sa));
        sa->object = in->object_hash;
        sa->refnum = in->refnum;
        sa->pos[0] = a->pos.x;
        sa->pos[1] = a->pos.y;
        sa->pos[2] = a->pos.z;
        sa->yaw = a->yaw;
        sa->hidden = g->zone.inst_hidden[a->instance];
        if (g->ai.actors) {
            const AiActor *s = &g->ai.actors[i];
            memcpy(sa->home, s->home, sizeof(sa->home));
            sa->pkg = s->pkg;
            sa->target = s->target;
            sa->life = s->life;
            sa->summoner = s->summoner;
            sa->summon_effect = s->summon_effect;
            sa->state = s->state == AI_WALK ? AI_IDLE : s->state;
            sa->pkg_done = s->pkg_done;
            sa->ally = s->ally;
            sa->fled = s->fled;
            sa->has_pkg = s->has_pkg;
        }
    }
    section(&f, TAG('A', 'I', 'S', 'T'), 12u + AI_STOLEN_MAX * 8u + n * (uint32_t)sizeof(SaveActor));
    sf_write(&f, &g->ai.stolen_used, 4);
    sf_write(&f, g->ai.stolen_item, AI_STOLEN_MAX * 4u);
    sf_write(&f, g->ai.stolen_count, AI_STOLEN_MAX * 4u);
    sf_write(&f, &g->ai.spawn_serial, 4);
    sf_write(&f, &n, 4);
    sf_write(&f, actors, n * (uint32_t)sizeof(SaveActor));

    /* header */
    h.magic = SAVE_MAGIC;
    h.version = SAVE_VERSION;
    h.size = (uint32_t)sizeof(h) + f.size;
    h.crc = f.crc;
    fingerprint(g, &h);
    snprintf(h.name, sizeof(h.name), "%s", vm->pc_name[0] ? vm->pc_name : "Player");
    snprintf(h.cell, sizeof(h.cell), "%s", game_cell_name(g));
    h.level = (int16_t)rpg_level(&g->rpg->rpg, &pc);
    h.month = (uint16_t)vm->month;
    h.day = (uint16_t)vm->day;
    h.hour = vm->hour;
    h.playtime = playtime;
    h.real_time = (int64_t)time(NULL);
    if (sf_finish(&f, &h) != 0 || replace_file(tmp, path) != 0) {
        snprintf(err, errlen, "Writing the save failed.");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* loading                                                                  */
/* ------------------------------------------------------------------------ */
typedef struct {
    const uint8_t *p;
    uint32_t left;
    int bad;
} Reader;

static const void *take(Reader *r, uint32_t n)
{
    const void *p = r->p;
    if (r->bad || n > r->left) {
        r->bad = 1;
        return NULL;
    }
    r->p += n;
    r->left -= n;
    return p;
}

static uint32_t take_u32(Reader *r)
{
    uint32_t v;
    if (r->bad || !r->p || r->left < 4) {
        r->bad = 1;
        return 0;
    }
    v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) | ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    r->left -= 4;
    return v;
}

typedef struct {
    const uint8_t *sec[8];
    uint32_t size[8];
} Sections;

enum { S_VMGL, S_VMTI, S_VMIN, S_VMPR, S_VMIT, S_VMJR, S_RPGS, S_GAME };
static const uint32_t sec_tags[8] = { TAG('V', 'M', 'G', 'L'), TAG('V', 'M', 'T', 'I'), TAG('V', 'M', 'I', 'N'),
                                      TAG('V', 'M', 'P', 'R'), TAG('V', 'M', 'I', 'T'), TAG('V', 'M', 'J', 'R'),
                                      TAG('R', 'P', 'G', 'S'), TAG('G', 'A', 'M', 'E') };

/* checks every count and index; 0 when the payload is consistent */
static int validate(Game *g, const Sections *s, const uint8_t *ai_sec, uint32_t ai_size)
{
    Vm *vm = &g->rpg->vm;
    Reader r;
    uint32_t n, i, k;
    /* globals */
    r.p = s->sec[S_VMGL];
    r.left = s->size[S_VMGL];
    r.bad = 0;
    if (take_u32(&r) != vm->global_count || r.left != vm->global_count * sizeof(VmValue))
        return 10;
    if (s->size[S_VMTI] != sizeof(SaveTime))
        return 11;
    /* instances */
    r.p = s->sec[S_VMIN];
    r.left = s->size[S_VMIN];
    n = take_u32(&r);
    k = take_u32(&r);
    if (r.bad || n > VM_MAX_INSTANCES || k > VM_MAX_LOCALS ||
        r.left != n * sizeof(VmInstance) + k * sizeof(VmValue))
        return 12;
    for (i = 0; i < n; ++i) {
        VmInstance in;
        memcpy(&in, r.p + i * sizeof(VmInstance), sizeof(in));
        if (in.script >= g->rpg->db.hdr->scripts.count || in.locals > k)
            return 13;
    }
    /* props, items */
    r.p = s->sec[S_VMPR];
    r.left = s->size[S_VMPR];
    n = take_u32(&r);
    if (r.bad || n > VM_PROP_SLOTS || r.left != n * (4u + sizeof(VmProp)))
        return 14;
    for (i = 0; i < n; ++i) {
        uint32_t slot;
        memcpy(&slot, r.p + i * (4u + sizeof(VmProp)), 4);
        if (slot >= VM_PROP_SLOTS)
            return 15;
    }
    r.p = s->sec[S_VMIT];
    r.left = s->size[S_VMIT];
    n = take_u32(&r);
    if (r.bad || n > VM_ITEM_SLOTS || r.left != n * (4u + sizeof(VmItem)))
        return 16;
    for (i = 0; i < n; ++i) {
        uint32_t slot;
        memcpy(&slot, r.p + i * (4u + sizeof(VmItem)), 4);
        if (slot >= VM_ITEM_SLOTS)
            return 17;
    }
    /* journal */
    {
        uint32_t topics, infos, factions, jc;
        r.p = s->sec[S_VMJR];
        r.left = s->size[S_VMJR];
        topics = take_u32(&r);
        infos = take_u32(&r);
        factions = take_u32(&r);
        jc = take_u32(&r);
        if (r.bad || topics != g->rpg->db.hdr->topics.count || infos != g->rpg->db.hdr->infos.count ||
            factions != g->rpg->db.hdr->factions.count || jc > VM_MAX_JOURNAL ||
            r.left != topics * 3u + jc * sizeof(VmJournalEntry) + (topics + 7u) / 8u + (infos + 7u) / 8u +
                          factions * 4u)
            return 18;
        for (i = 0; i < jc; ++i) {
            VmJournalEntry e;
            memcpy(&e, r.p + topics * 3u + i * sizeof(VmJournalEntry), sizeof(e));
            if (e.topic >= topics || e.info >= infos)
                return 19;
        }
    }
    /* mechanics */
    if (s->size[S_RPGS] != sizeof(Rpg))
        return 20;
    {
        static Rpg tmp;
        memcpy(&tmp, s->sec[S_RPGS], sizeof(Rpg));
        if (tmp.fx_count > RPG_MAX_FX || tmp.eq_count > RPG_MAX_EQUIP || tmp.custom_count > RPG_MAX_CUSTOM ||
            tmp.visited_count > RPG_MAX_VISITED)
            return 21;
        for (i = 0; i < tmp.eq_count; ++i) /* -1: the "dressed" marker entries */
            if (tmp.eq[i].stack < -1 || tmp.eq[i].stack >= (int32_t)VM_ITEM_SLOTS)
                return 22;
        if (tmp.sel_item >= (int32_t)VM_ITEM_SLOTS)
            return 22;
        for (i = 0; i < tmp.custom_count; ++i)
            if (tmp.custom[i].spell.effect_count > RPG_CUSTOM_EFFECTS)
                return 23;
    }
    if (s->size[S_GAME] != sizeof(SaveGameState))
        return 24;
    {
        SaveGameState gs;
        uint32_t z, found = 0;
        memcpy(&gs, s->sec[S_GAME], sizeof(gs));
        for (z = 0; z < g->world->hdr->zone_count; ++z)
            found |= g->world->zone_list[z].entry_hash == gs.zone_entry;
        if (!found)
            return 25;
    }
    /* AI */
    r.p = ai_sec;
    r.left = ai_size;
    n = take_u32(&r);
    take(&r, AI_STOLEN_MAX * 8u);
    take_u32(&r);
    k = take_u32(&r);
    if (r.bad || n > AI_STOLEN_MAX || k > SAVE_MAX_ACTORS || r.left != k * sizeof(SaveActor))
        return 26;
    return 0;
}

static void apply_vm(Game *g, const Sections *s)
{
    Vm *vm = &g->rpg->vm;
    Reader r;
    uint32_t n, k, i;
    memcpy(vm->globals, s->sec[S_VMGL] + 4, vm->global_count * sizeof(VmValue));
    {
        SaveTime t;
        memcpy(&t, s->sec[S_VMTI], sizeof(t));
        vm->hour = t.hour;
        vm->timescale = t.timescale;
        vm->day = t.day;
        vm->month = t.month;
        vm->year = t.year;
        vm->days_passed = t.days_passed;
        vm->rng = t.rng;
        memcpy(vm->pc_name, t.pc_name, sizeof(vm->pc_name));
        vm->pc_name[sizeof(vm->pc_name) - 1] = '\0';
        vm->pc_race = t.pc_race;
        vm->pc_class = t.pc_class;
        vm->pc_female = t.pc_female;
        vm->journal_updated = t.journal_updated;
    }
    r.p = s->sec[S_VMIN];
    r.left = s->size[S_VMIN];
    r.bad = 0;
    n = take_u32(&r);
    k = take_u32(&r);
    memset(vm->inst, 0, VM_MAX_INSTANCES * sizeof(VmInstance));
    memcpy(vm->inst, take(&r, n * (uint32_t)sizeof(VmInstance)), n * sizeof(VmInstance));
    vm->inst_count = n;
    memset(vm->locals, 0, VM_MAX_LOCALS * sizeof(VmValue));
    memcpy(vm->locals, take(&r, k * (uint32_t)sizeof(VmValue)), k * sizeof(VmValue));
    vm->locals_used = k;
    r.p = s->sec[S_VMPR];
    r.left = s->size[S_VMPR];
    n = take_u32(&r);
    memset(vm->props, 0, VM_PROP_SLOTS * sizeof(VmProp));
    vm->prop_count = n;
    for (i = 0; i < n; ++i) {
        uint32_t slot = take_u32(&r);
        memcpy(&vm->props[slot], take(&r, sizeof(VmProp)), sizeof(VmProp));
    }
    r.p = s->sec[S_VMIT];
    r.left = s->size[S_VMIT];
    n = take_u32(&r);
    memset(vm->items, 0, VM_ITEM_SLOTS * sizeof(VmItem));
    vm->item_count = n;
    for (i = 0; i < n; ++i) {
        uint32_t slot = take_u32(&r);
        memcpy(&vm->items[slot], take(&r, sizeof(VmItem)), sizeof(VmItem));
    }
    {
        uint32_t topics, infos, factions, jc;
        r.p = s->sec[S_VMJR];
        r.left = s->size[S_VMJR];
        topics = take_u32(&r);
        infos = take_u32(&r);
        factions = take_u32(&r);
        jc = take_u32(&r);
        memcpy(vm->quest_index, take(&r, topics * 2u), topics * 2u);
        memcpy(vm->quest_flags, take(&r, topics), topics);
        memcpy(vm->journal, take(&r, jc * (uint32_t)sizeof(VmJournalEntry)), jc * sizeof(VmJournalEntry));
        vm->journal_count = jc;
        memcpy(vm->known_topics, take(&r, (topics + 7u) / 8u), (topics + 7u) / 8u);
        memcpy(vm->heard_infos, take(&r, (infos + 7u) / 8u), (infos + 7u) / 8u);
        memcpy(vm->pc_rank, take(&r, factions), factions);
        memcpy(vm->pc_expelled, take(&r, factions), factions);
        memcpy(vm->pc_facrep, take(&r, factions * 2u), factions * 2u);
    }
    /* transient state of the old game */
    while (vm_message_front(vm))
        vm_message_answer(vm, 0);
    vm->notify_count = 0;
    vm->has_force_greeting = 0;
    vm->button = -1;
    {
        Rpg *rp = &g->rpg->rpg;
        Vm *keep_vm = rp->vm;
        RpgDb *keep_db = rp->db;
        RpgHost keep_host = rp->host;
        memcpy(rp, s->sec[S_RPGS], sizeof(Rpg));
        rp->vm = keep_vm;
        rp->db = keep_db;
        rp->host = keep_host;
    }
}

void save_slot_info(const char *dir, int slot, SaveSlotInfo *out)
{
    char path[220];
    DSaveHeader h;
    memset(out, 0, sizeof(*out));
    if (slot < 0 || slot >= SAVE_SLOTS)
        return;
    slot_path(path, sizeof(path), dir, slot, "sav");
    if (read_file(path, (uint8_t *)&h, sizeof(h)) != (int32_t)sizeof(h) || h.magic != SAVE_MAGIC ||
        h.version != SAVE_VERSION)
        return;
    h.name[sizeof(h.name) - 1] = '\0';
    h.cell[sizeof(h.cell) - 1] = '\0';
    out->used = 1;
    out->hdr = h;
}

int save_latest(const char *dir)
{
    int slot, best = -1;
    int64_t when = 0;
    for (slot = 0; slot < SAVE_SLOTS; ++slot) {
        SaveSlotInfo info;
        save_slot_info(dir, slot, &info);
        if (info.used && (best < 0 || info.hdr.real_time >= when)) {
            best = slot;
            when = info.hdr.real_time;
        }
    }
    return best;
}

int load_game(Game *g, const char *dir, int slot, float *playtime, char *err, uint32_t errlen)
{
    static SaveActor actors[SAVE_MAX_ACTORS];
    char path[220];
    MemRegionId region = mem_available(MEM_REGION_ZONE) > mem_available(MEM_REGION_AUDIO_UI) ? MEM_REGION_ZONE
                                                                                              : MEM_REGION_AUDIO_UI;
    MemMark mark;
    uint8_t *buf;
    int32_t len;
    DSaveHeader h, now;
    Sections s;
    const uint8_t *ai_sec = NULL;
    uint32_t ai_size = 0, stolen_used, spawn_serial, actor_count, i;
    uint32_t stolen_item[AI_STOLEN_MAX];
    int32_t stolen_count[AI_STOLEN_MAX];
    SaveGameState gs;
    int rc, zi = -1;
    Reader r;
    if (!g->rpg) {
        snprintf(err, errlen, "No game data.");
        return -1;
    }
    slot_path(path, sizeof(path), dir, slot, "sav");
    mark = mem_mark_hi(region);
    buf = (uint8_t *)mem_try_alloc_hi(region, PW_MIN(mem_available(region) - PW_KB(64), (uint32_t)SAVE_MAX_BYTES), 16);
    if (!buf) {
        snprintf(err, errlen, "Out of memory for loading.");
        return -1;
    }
    len = read_file(path, buf, PW_MIN(mem_available(region), (uint32_t)SAVE_MAX_BYTES));
#define LOAD_FAIL(...)                                                       \
    do {                                                                     \
        snprintf(err, errlen, __VA_ARGS__);                                  \
        mem_release_hi(region, mark);                                        \
        return -1;                                                           \
    } while (0)
    if (len < (int32_t)sizeof(DSaveHeader))
        LOAD_FAIL("Slot %d is empty.", slot);
    memcpy(&h, buf, sizeof(h));
    memset(&now, 0, sizeof(now));
    fingerprint(g, &now);
    if (h.magic != SAVE_MAGIC || h.version != SAVE_VERSION || h.size != (uint32_t)len)
        LOAD_FAIL("Slot %d is not a save of this version.", slot);
    if (save_crc32(0, buf + sizeof(h), h.size - (uint32_t)sizeof(h)) != h.crc)
        LOAD_FAIL("Slot %d is damaged.", slot);
    if (h.db_size != now.db_size || h.db_objects != now.db_objects || h.db_infos != now.db_infos ||
        h.struct_sizes != now.struct_sizes)
        LOAD_FAIL("Slot %d was made with different game data.", slot);
    /* sections */
    memset(&s, 0, sizeof(s));
    r.p = buf + sizeof(h);
    r.left = h.size - (uint32_t)sizeof(h);
    r.bad = 0;
    while (r.left && !r.bad) {
        uint32_t tag = take_u32(&r), size = take_u32(&r), k;
        const uint8_t *data = (const uint8_t *)take(&r, size);
        if (r.bad)
            break;
        if (tag == TAG('A', 'I', 'S', 'T')) {
            ai_sec = data;
            ai_size = size;
            continue;
        }
        for (k = 0; k < 8; ++k)
            if (sec_tags[k] == tag) {
                s.sec[k] = data;
                s.size[k] = size;
            }
    }
    for (i = 0; i < 8; ++i)
        if (!s.sec[i])
            r.bad = 1;
    if (r.bad || !ai_sec)
        LOAD_FAIL("Slot %d is incomplete.", slot);
    rc = validate(g, &s, ai_sec, ai_size);
    if (rc)
        LOAD_FAIL("Slot %d is inconsistent (check %d).", slot, rc);

    /* ---- from here on the running game is replaced */
    memcpy(&gs, s.sec[S_GAME], sizeof(gs));
    r.p = ai_sec;
    r.left = ai_size;
    r.bad = 0;
    stolen_used = take_u32(&r);
    memcpy(stolen_item, take(&r, AI_STOLEN_MAX * 4u), sizeof(stolen_item));
    memcpy(stolen_count, take(&r, AI_STOLEN_MAX * 4u), sizeof(stolen_count));
    spawn_serial = take_u32(&r);
    actor_count = take_u32(&r);
    memcpy(actors, take(&r, actor_count * (uint32_t)sizeof(SaveActor)), actor_count * sizeof(SaveActor));
    apply_vm(g, &s);
    mem_release_hi(region, mark); /* the zone load below reuses the scratch */
#undef LOAD_FAIL

    for (i = 0; i < g->world->hdr->zone_count; ++i)
        if (g->world->zone_list[i].entry_hash == gs.zone_entry)
            zi = (int)i;
    {
        float rot[3] = { 0.0f, 0.0f, gs.yaw };
        char zerr[120];
        dlg_end(&g->rpg->dlg);
        g->player_dead = 0;
        g->ui_event = GAME_UI_NONE;
        g->menu_mode = 0;
        g->request.pending = 0;
        if (game_enter_zone(g, (uint32_t)zi, gs.pos, rot, zerr, sizeof(zerr)) != 0) {
            snprintf(err, errlen, "Zone of slot %d failed to load: %s", slot, zerr);
            return -1;
        }
    }
    g->body.pos = vec4(gs.pos[0], gs.pos[1], gs.pos[2], 1.0f);
    g->yaw = gs.yaw;
    g->pitch = gs.pitch;
    g->running = gs.running;
    g->sneaking = gs.sneaking;
    g->flying = gs.flying;
    g->controls_disabled = gs.controls_disabled;
    g->menus_enabled = gs.menus_enabled;
    g->ai.stolen_used = stolen_used;
    memcpy(g->ai.stolen_item, stolen_item, sizeof(stolen_item));
    memcpy(g->ai.stolen_count, stolen_count, sizeof(stolen_count));
    /* spawned creatures come back first, then everyone takes their place */
    for (i = 0; i < actor_count; ++i) {
        const SaveActor *sa = &actors[i];
        if ((sa->refnum & 0xFF000000u) == ZONE_SPAWN_REFNUM && !sa->hidden && i >= g->actors.count) {
            int a = ai_spawn(g, sa->object, sa->pos, sa->yaw, sa->ally, sa->life);
            if (a >= 0)
                g->zone.inst_rw[g->actors.list[a].instance].refnum = sa->refnum;
        }
    }
    g->ai.spawn_serial = spawn_serial;
    for (i = 0; i < actor_count && i < g->actors.count; ++i) {
        const SaveActor *sa = &actors[i];
        Actor *a = &g->actors.list[i];
        const DZoneInstance *in = &g->zone.instances[a->instance];
        Vec4 p;
        if (in->object_hash != sa->object || in->refnum != sa->refnum)
            continue; /* the cooked zone changed; keep the fresh actor */
        p = vec4(sa->pos[0], sa->pos[1], sa->pos[2], 1.0f);
        actor_set_placement(a, &p, sa->yaw);
        if (sa->hidden)
            g->zone.inst_hidden[a->instance] = 1;
        if (g->ai.actors) {
            AiActor *st = &g->ai.actors[i];
            memcpy(st->home, sa->home, sizeof(st->home));
            st->pkg = sa->pkg;
            st->has_pkg = sa->has_pkg;
            st->pkg_done = sa->pkg_done;
            st->target = sa->target;
            st->life = sa->life;
            st->summoner = sa->summoner;
            st->summon_effect = sa->summon_effect;
            st->state = sa->state > AI_GONE ? AI_IDLE : sa->state;
            st->ally = sa->ally;
            st->fled = sa->fled;
            st->body.pos = p;
        }
    }
    if (playtime)
        *playtime = h.playtime;
    return 0;
}
