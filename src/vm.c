/*
 * vm.c - MWScript interpreter, scripted game state and built-in verbs.
 */
#include "vm.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mem.h"

#define SPELL_SALT 0x5bd1e995u
#define VM_NOT_FOUND (-1)

static const int month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static const char *const month_names[12] = {
    "Morning Star", "Sun's Dawn", "First Seed", "Rain's Hand", "Second Seed", "Midyear",
    "Sun's Height", "Last Seed", "Hearthfire", "Frostfall", "Sun's Dusk", "Evening Star"
};

/* ------------------------------------------------------------------------ */
/* small helpers                                                            */
/* ------------------------------------------------------------------------ */
void vm_error(Vm *vm, const char *msg)
{
    vm->errors++;
    strncpy(vm->last_error, msg, sizeof(vm->last_error) - 1);
    vm->last_error[sizeof(vm->last_error) - 1] = '\0';
}

static uint32_t rng_next(Vm *vm)
{
    uint32_t x = vm->rng ? vm->rng : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vm->rng = x;
    return x;
}

static int ci_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        int a = (unsigned char)*s, b = (unsigned char)*prefix;
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (b >= 'A' && b <= 'Z')
            b += 32;
        if (a != b)
            return 0;
        ++s;
        ++prefix;
    }
    return 1;
}

static void copy_str(char *dst, uint32_t cap, const char *src)
{
    uint32_t n = 0;
    if (!cap)
        return;
    while (src[n] && n + 1 < cap) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

const char *vm_arg_string(const VmCtx *ctx, VmValue v)
{
    if (!ctx || !ctx->strings || v.i < 0 || (uint32_t)v.i >= ctx->strings_size)
        return "";
    return ctx->strings + v.i;
}

static int bit_get(const uint8_t *bits, uint32_t i)
{
    return (bits[i >> 3] >> (i & 7)) & 1;
}

static void bit_set(uint8_t *bits, uint32_t i)
{
    bits[i >> 3] = (uint8_t)(bits[i >> 3] | (1u << (i & 7)));
}

/* ------------------------------------------------------------------------ */
/* references                                                               */
/* ------------------------------------------------------------------------ */
VmRef vm_player_ref(const Vm *vm)
{
    VmRef r;
    r.kind = VM_REF_PLAYER;
    r.pad = 0;
    r.vmi = -1;
    r.inst = -1;
    r.object = vm->player_hash;
    r.key = vm->player_hash;
    return r;
}

static int find_instance_by_object(const Vm *vm, uint32_t object)
{
    uint32_t i;
    for (i = 0; i < vm->inst_count; ++i)
        if (vm->inst[i].object == object && !(vm->inst[i].flags & VMI_GLOBAL))
            return (int)i;
    return VM_NOT_FOUND;
}

static int find_instance_by_key(const Vm *vm, uint32_t key, int global)
{
    uint32_t i;
    for (i = 0; i < vm->inst_count; ++i)
        if (vm->inst[i].key == key && (!(vm->inst[i].flags & VMI_GLOBAL)) == !global)
            return (int)i;
    return VM_NOT_FOUND;
}

VmRef vm_resolve(Vm *vm, uint32_t object_hash)
{
    VmRef r;
    if (object_hash == vm->player_hash)
        return vm_player_ref(vm);
    memset(&r, 0, sizeof(r));
    r.kind = VM_REF_OBJECT;
    r.inst = -1;
    r.vmi = -1;
    r.object = object_hash;
    r.key = object_hash;
    if (vm->host.find_ref && vm->host.find_ref(vm->host.user, object_hash, &r)) {
        r.kind = VM_REF_OBJECT;
        r.object = object_hash;
    }
    if (r.vmi < 0)
        r.vmi = (int16_t)find_instance_by_object(vm, object_hash);
    return r;
}

const DRpgObject *vm_ref_object(const Vm *vm, const VmRef *ref)
{
    if (!ref || ref->kind == VM_REF_NONE)
        return NULL;
    return rpgdb_object(vm->db, ref->object);
}

uint32_t vm_reference_key(const Vm *vm, uint32_t object, uint32_t zone_hash, uint32_t refnum)
{
    const DRpgObject *o = rpgdb_object(vm->db, object);
    if (o && (o->flags & DRPG_OF_UNIQUE))
        return object;
    return (zone_hash * 0x9E3779B1u) ^ (refnum * 0x85EBCA77u) ^ 0xC2B2AE3Du;
}

/* ------------------------------------------------------------------------ */
/* stat store                                                               */
/* ------------------------------------------------------------------------ */
static uint32_t prop_slot(const Vm *vm, uint32_t key, uint32_t prop, int *found)
{
    uint32_t h = (key * 0x9E3779B1u) ^ (prop * 0x85EBCA77u);
    uint32_t i, mask = VM_PROP_SLOTS - 1u;
    for (i = 0; i < VM_PROP_SLOTS; ++i) {
        uint32_t s = (h + i) & mask;
        const VmProp *p = &vm->props[s];
        if (!p->used) {
            *found = 0;
            return s;
        }
        if (p->key == key && p->prop == prop) {
            *found = 1;
            return s;
        }
    }
    *found = 0;
    return VM_PROP_SLOTS;
}

static float actor_default(const Vm *vm, uint32_t object, int prop)
{
    const DRpgObject *o = rpgdb_object(vm->db, object);
    const DRpgActor *a;
    int is_player = object == vm->player_hash;
    if (!o && is_player && vm->db->hdr && vm->db->hdr->player != DRPG_NONE)
        o = &vm->db->objects[vm->db->hdr->player];
    a = rpgdb_actor(vm->db, o);
    if (prop >= MWP_ATTR_STRENGTH && prop < MWP_ATTR_STRENGTH + 8)
        return a ? (float)a->attributes[prop - MWP_ATTR_STRENGTH] : (is_player ? 40.0f : 0.0f);
    if (prop >= MWP_DYN_HEALTH && prop < MWP_DYN_HEALTH + 3) {
        int k = prop - MWP_DYN_HEALTH;
        float v = a ? (float)(k == 0 ? a->health : k == 1 ? a->magicka : a->fatigue) : 0.0f;
        return (v <= 0.0f && is_player) ? 100.0f : v;
    }
    if (prop >= MWP_DYNBASE_HEALTH && prop < MWP_DYNBASE_HEALTH + 3) {
        int k = prop - MWP_DYNBASE_HEALTH;
        float v = a ? (float)(k == 0 ? a->health : k == 1 ? a->magicka : a->fatigue) : 0.0f;
        return (v <= 0.0f && is_player) ? 100.0f : v;
    }
    if (prop >= MWP_SKILL_BLOCK && prop < MWP_SKILL_BLOCK + 27) {
        if (o && o->type == DRPG_OT_NPC && a)
            return (float)a->skills[prop - MWP_SKILL_BLOCK];
        return is_player ? 5.0f : 0.0f;
    }
    switch (prop) {
    case MWP_AI_HELLO: return a ? (float)a->hello : 0.0f;
    case MWP_AI_FIGHT: return a ? (float)a->fight : 0.0f;
    case MWP_AI_FLEE: return a ? (float)a->flee : 0.0f;
    case MWP_AI_ALARM: return a ? (float)a->alarm : 0.0f;
    case MWP_DISPOSITION: return a ? (float)a->disposition : 50.0f;
    case MWP_REPUTATION: return a ? (float)a->reputation : 0.0f;
    case MWP_LEVEL: return a ? (float)a->level : 1.0f;
    case MWP_RANK: return a ? (float)a->rank : -1.0f;
    default: return 0.0f;
    }
}

float vm_prop_key(Vm *vm, uint32_t key, uint32_t object, int prop)
{
    int found;
    uint32_t s = prop_slot(vm, key, (uint32_t)prop, &found);
    if (found)
        return vm->props[s].value;
    return actor_default(vm, object, prop);
}

void vm_set_prop_key(Vm *vm, uint32_t key, int prop, float v)
{
    int found;
    uint32_t s = prop_slot(vm, key, (uint32_t)prop, &found);
    if (s >= VM_PROP_SLOTS) {
        vm_error(vm, "stat store full");
        return;
    }
    if (!found) {
        vm->props[s].used = 1;
        vm->props[s].key = key;
        vm->props[s].prop = (uint16_t)prop;
        vm->prop_count++;
    }
    vm->props[s].value = v;
}

float vm_prop(Vm *vm, const VmRef *ref, int prop)
{
    if (!ref || ref->kind == VM_REF_NONE)
        return 0.0f;
    return vm_prop_key(vm, ref->key, ref->object, prop);
}

void vm_set_prop(Vm *vm, const VmRef *ref, int prop, float v)
{
    if (!ref || ref->kind == VM_REF_NONE)
        return;
    vm_set_prop_key(vm, ref->key, prop, v);
}

int vm_disabled(Vm *vm, uint32_t key)
{
    int found;
    uint32_t s = prop_slot(vm, key, MWP_DISABLED, &found);
    return found && vm->props[s].value != 0.0f;
}

int vm_disposition(Vm *vm, const VmRef *ref)
{
    float d = vm_prop(vm, ref, MWP_DISPOSITION);
    return (int)PW_CLAMP(d, 0.0f, 100.0f);
}

/* ------------------------------------------------------------------------ */
/* inventories                                                              */
/* ------------------------------------------------------------------------ */
static uint32_t item_slot(const Vm *vm, uint32_t owner, uint32_t item, int *found)
{
    uint32_t h = (owner * 0x9E3779B1u) ^ (item * 0x27D4EB2Fu);
    uint32_t i, mask = VM_ITEM_SLOTS - 1u;
    for (i = 0; i < VM_ITEM_SLOTS; ++i) {
        uint32_t s = (h + i) & mask;
        const VmItem *it = &vm->items[s];
        if (it->owner == 0 && it->item == 0) {
            *found = 0;
            return s;
        }
        if (it->owner == owner && it->item == item) {
            *found = 1;
            return s;
        }
    }
    *found = 0;
    return VM_ITEM_SLOTS;
}

int32_t vm_item_count(Vm *vm, uint32_t owner, uint32_t item)
{
    int found;
    uint32_t s = item_slot(vm, owner, item, &found);
    return found ? vm->items[s].count : 0;
}

void vm_item_add(Vm *vm, uint32_t owner, uint32_t item, int32_t delta)
{
    int found;
    uint32_t s = item_slot(vm, owner, item, &found);
    if (s >= VM_ITEM_SLOTS) {
        vm_error(vm, "inventory store full");
        return;
    }
    if (!found) {
        if (delta <= 0)
            return;
        vm->items[s].owner = owner;
        vm->items[s].item = item;
        vm->items[s].count = 0;
        vm->item_count++;
    }
    vm->items[s].count += delta;
    if (vm->items[s].count < 0)
        vm->items[s].count = 0;
}

int vm_item_next(Vm *vm, uint32_t owner, uint32_t *cursor, uint32_t *item, int32_t *count)
{
    uint32_t i;
    for (i = *cursor; i < VM_ITEM_SLOTS; ++i) {
        const VmItem *it = &vm->items[i];
        if (it->owner == owner && it->count > 0) {
            *cursor = i + 1;
            *item = it->item;
            *count = it->count;
            return (int)i;
        }
    }
    *cursor = VM_ITEM_SLOTS;
    return -1;
}

/* Copies an actor's/container's record inventory and spells into the
 * stores the first time scripts look at them. */
static void seed_reference(Vm *vm, uint32_t key, uint32_t object)
{
    const DRpgObject *o;
    uint32_t i;
    int found;
    uint32_t s = prop_slot(vm, key, MWP_SEEDED, &found);
    if (found || s >= VM_PROP_SLOTS)
        return;
    vm_set_prop_key(vm, key, MWP_SEEDED, 1.0f);
    o = rpgdb_object(vm->db, object);
    if (!o && object == vm->player_hash && vm->db->hdr && vm->db->hdr->player != DRPG_NONE)
        o = &vm->db->objects[vm->db->hdr->player];
    if (!o)
        return;
    if (o->type == DRPG_OT_NPC || o->type == DRPG_OT_CREA || o->type == DRPG_OT_CONT) {
        for (i = 0; i < o->list_count; ++i) {
            const DRpgListItem *li = &vm->db->lists[o->list_first + i];
            int32_t n = li->count < 0 ? -li->count : li->count;
            vm_item_add(vm, key, li->hash, n);
        }
    }
    if (o->type == DRPG_OT_NPC || o->type == DRPG_OT_CREA) {
        for (i = 0; i < o->list2_count; ++i)
            vm_item_add(vm, key ^ SPELL_SALT, vm->db->lists[o->list2_first + i].hash, 1);
    }
}

static void seed_ref(Vm *vm, const VmRef *ref)
{
    if (ref && ref->kind != VM_REF_NONE)
        seed_reference(vm, ref->key, ref->object);
}

/* ------------------------------------------------------------------------ */
/* instances                                                                */
/* ------------------------------------------------------------------------ */
static int new_instance(Vm *vm, uint32_t key, uint32_t object, uint32_t script, uint16_t flags)
{
    const DRpgScript *s = &vm->db->scripts[script];
    uint32_t n = (uint32_t)s->num_shorts + s->num_longs + s->num_floats;
    VmInstance *in;
    if (vm->inst_count >= VM_MAX_INSTANCES || vm->locals_used + n > VM_MAX_LOCALS) {
        vm_error(vm, "script instance pool full");
        return VM_NOT_FOUND;
    }
    in = &vm->inst[vm->inst_count];
    memset(in, 0, sizeof(*in));
    in->key = key;
    in->object = object;
    in->script = (uint16_t)script;
    in->flags = flags;
    in->locals = vm->locals_used;
    in->inst = -1;
    memset(&vm->locals[vm->locals_used], 0, n * sizeof(VmValue));
    vm->locals_used += n;
    return (int)vm->inst_count++;
}

static uint32_t instance_nlocals(const Vm *vm, const VmInstance *in)
{
    const DRpgScript *s = &vm->db->scripts[in->script];
    return (uint32_t)s->num_shorts + s->num_longs + s->num_floats;
}

/* global script instance (created stopped when missing) */
static int global_instance(Vm *vm, uint32_t script_hash)
{
    int si, vmi = find_instance_by_key(vm, script_hash, 1);
    if (vmi >= 0)
        return vmi;
    si = rpgdb_script(vm->db, script_hash);
    if (si < 0)
        return VM_NOT_FOUND;
    return new_instance(vm, script_hash, 0, (uint32_t)si, VMI_GLOBAL);
}

/* local script instance of an object (lazy, for member access) */
static int object_instance(Vm *vm, uint32_t object)
{
    const DRpgObject *o;
    int vmi = find_instance_by_object(vm, object);
    if (vmi >= 0)
        return vmi;
    o = rpgdb_object(vm->db, object);
    if (!o || o->script == DRPG_NONE16)
        return VM_NOT_FOUND;
    return new_instance(vm, object, object, o->script, 0);
}

void vm_unload_zone(Vm *vm)
{
    uint32_t i;
    for (i = 0; i < vm->inst_count; ++i) {
        vm->inst[i].flags &= (uint16_t)~VMI_LOADED;
        vm->inst[i].inst = -1;
    }
}

int vm_bind_reference(Vm *vm, uint32_t key, uint32_t object, int32_t inst)
{
    const DRpgObject *o = rpgdb_object(vm->db, object);
    int vmi;
    if (!o || o->script == DRPG_NONE16)
        return VM_NOT_FOUND;
    vmi = find_instance_by_key(vm, key, 0);
    if (vmi < 0 && key != object) {
        /* a lazily created instance keyed by object id (member access before
         * the reference was ever loaded) adopts the first placed copy */
        int lazy = find_instance_by_key(vm, object, 0);
        if (lazy >= 0 && !(vm->inst[lazy].flags & VMI_LOADED)) {
            vm->inst[lazy].key = key;
            vmi = lazy;
        }
    }
    if (vmi < 0)
        vmi = new_instance(vm, key, object, o->script, 0);
    if (vmi < 0)
        return VM_NOT_FOUND;
    vm->inst[vmi].flags |= VMI_LOADED;
    vm->inst[vmi].inst = inst;
    return vmi;
}

/* ------------------------------------------------------------------------ */
/* journal, topics, factions                                                */
/* ------------------------------------------------------------------------ */
static int journal_topic(const Vm *vm, uint32_t quest_hash)
{
    int t = rpgdb_topic(vm->db, quest_hash);
    if (t < 0 || vm->db->topics[t].type != DRPG_TT_JOURNAL)
        return VM_NOT_FOUND;
    return t;
}

int vm_journal_index(const Vm *vm, uint32_t quest_hash)
{
    int t = journal_topic(vm, quest_hash);
    return t < 0 ? 0 : vm->quest_index[t];
}

void vm_set_journal_index(Vm *vm, uint32_t quest_hash, int index)
{
    int t = journal_topic(vm, quest_hash);
    if (t < 0) {
        vm_error(vm, "SetJournalIndex: unknown quest");
        return;
    }
    vm->quest_index[t] = (int16_t)index;
}

int vm_journal(Vm *vm, uint32_t quest_hash, int index)
{
    const DRpgTopic *tp;
    int t = journal_topic(vm, quest_hash);
    uint32_t i, info = DRPG_NONE;
    if (t < 0) {
        vm_error(vm, "Journal: unknown quest");
        return -1;
    }
    tp = &vm->db->topics[t];
    for (i = 0; i < tp->info_count; ++i) {
        const DRpgInfo *in = &vm->db->infos[tp->info_first + i];
        if (in->data == index && !(in->flags & DRPG_IF_QUEST_NAME)) {
            info = tp->info_first + i;
            break;
        }
    }
    if (info == DRPG_NONE) {
        vm->quest_index[t] = (int16_t)index;
        vm_error(vm, "Journal: no entry for that index");
        return -1;
    }
    for (i = 0; i < vm->journal_count; ++i) {
        if (vm->journal[i].topic == (uint16_t)t && vm->journal[i].info == info) {
            if (vm->quest_index[t] < index) {
                vm->quest_index[t] = (int16_t)index;
                vm_notify(vm, rpgdb_gmst_string(vm->db, "sJournalEntry", "Your journal has been updated."));
            }
            return 0;
        }
    }
    if (vm->journal_count < VM_MAX_JOURNAL) {
        VmJournalEntry *e = &vm->journal[vm->journal_count++];
        e->topic = (uint16_t)t;
        e->index = (int16_t)index;
        e->info = info;
        e->day = (uint16_t)vm->day;
        e->month = (uint8_t)vm->month;
        e->pad = 0;
        e->days_passed = (uint32_t)vm->days_passed;
    } else {
        vm_error(vm, "journal full");
    }
    vm->quest_index[t] = (int16_t)index;
    vm->quest_flags[t] |= VMQ_STARTED;
    if (vm->db->infos[info].flags & DRPG_IF_QUEST_FINISHED)
        vm->quest_flags[t] |= VMQ_FINISHED;
    else if (vm->db->infos[info].flags & DRPG_IF_QUEST_RESTART)
        vm->quest_flags[t] &= (uint8_t)~VMQ_FINISHED;
    vm->journal_updated = 1;
    vm_notify(vm, rpgdb_gmst_string(vm->db, "sJournalEntry", "Your journal has been updated."));
    return 1;
}

void vm_add_topic(Vm *vm, uint32_t topic_hash)
{
    int t = rpgdb_topic(vm->db, topic_hash);
    if (t < 0 || vm->db->topics[t].type != DRPG_TT_TOPIC) {
        vm_error(vm, "AddTopic: unknown topic");
        return;
    }
    bit_set(vm->known_topics, (uint32_t)t);
}

int vm_topic_known(const Vm *vm, uint32_t topic)
{
    if (!vm->db->hdr || topic >= vm->db->hdr->topics.count)
        return 0;
    return bit_get(vm->known_topics, topic);
}

void vm_mark_heard(Vm *vm, uint32_t info)
{
    if (vm->db->hdr && info < vm->db->hdr->infos.count)
        bit_set(vm->heard_infos, info);
}

int vm_heard(const Vm *vm, uint32_t info)
{
    if (!vm->db->hdr || info >= vm->db->hdr->infos.count)
        return 0;
    return bit_get(vm->heard_infos, info);
}

int vm_pc_rank(const Vm *vm, uint32_t faction_hash)
{
    int f = rpgdb_faction_index(vm->db, faction_hash);
    return f < 0 ? -1 : vm->pc_rank[f];
}

void vm_pc_set_rank(Vm *vm, uint32_t faction_hash, int rank)
{
    int f = rpgdb_faction_index(vm->db, faction_hash);
    if (f < 0) {
        vm_error(vm, "unknown faction");
        return;
    }
    vm->pc_rank[f] = (int8_t)PW_CLAMP(rank, -1, 9);
}

int vm_pc_expelled(const Vm *vm, uint32_t faction_hash)
{
    int f = rpgdb_faction_index(vm->db, faction_hash);
    return f < 0 ? 0 : vm->pc_expelled[f];
}

int vm_pc_facrep(const Vm *vm, uint32_t faction_hash)
{
    int f = rpgdb_faction_index(vm->db, faction_hash);
    return f < 0 ? 0 : vm->pc_facrep[f];
}

int vm_faction_reaction(Vm *vm, uint32_t f1, uint32_t f2)
{
    const DRpgFaction *fa = rpgdb_faction(vm->db, f1);
    int f2i = rpgdb_faction_index(vm->db, f2);
    int found;
    uint32_t i, s;
    int base = 0;
    if (!fa || f2i < 0)
        return 0;
    for (i = 0; i < fa->reaction_count; ++i)
        if (vm->db->reactions[fa->reaction_first + i].faction == f2)
            base = vm->db->reactions[fa->reaction_first + i].value;
    s = prop_slot(vm, f1, 0x8000u | (uint32_t)f2i, &found);
    return found ? (int)vm->props[s].value : base;
}

static void set_faction_reaction(Vm *vm, uint32_t f1, uint32_t f2, int v)
{
    int f2i = rpgdb_faction_index(vm->db, f2);
    if (f2i < 0 || !rpgdb_faction(vm->db, f1)) {
        vm_error(vm, "unknown faction");
        return;
    }
    vm_set_prop_key(vm, f1, 0x8000 | f2i, (float)v);
}

uint32_t vm_ref_faction(Vm *vm, const VmRef *ref)
{
    const DRpgObject *o = vm_ref_object(vm, ref);
    if (!o || o->type != DRPG_OT_NPC)
        return 0;
    return o->link[2];
}

int vm_ref_rank(Vm *vm, const VmRef *ref)
{
    if (!vm_ref_faction(vm, ref))
        return -1;
    return (int)vm_prop(vm, ref, MWP_RANK);
}

/* ------------------------------------------------------------------------ */
/* messages and notifications                                               */
/* ------------------------------------------------------------------------ */
void vm_message(Vm *vm, const char *text, const char (*buttons)[48], int nbuttons)
{
    VmMessage *m;
    int i;
    if (vm->msg_count >= VM_MAX_MESSAGES) {
        vm_error(vm, "message box queue full");
        return;
    }
    m = &vm->msg[(vm->msg_head + vm->msg_count) % VM_MAX_MESSAGES];
    copy_str(m->text, sizeof(m->text), text);
    if (nbuttons > VM_MAX_BUTTONS)
        nbuttons = VM_MAX_BUTTONS;
    for (i = 0; i < nbuttons; ++i)
        copy_str(m->buttons[i], sizeof(m->buttons[i]), buttons[i]);
    m->nbuttons = (uint8_t)(nbuttons < 0 ? 0 : nbuttons);
    vm->msg_count++;
}

const VmMessage *vm_message_front(const Vm *vm)
{
    return vm->msg_count ? &vm->msg[vm->msg_head] : NULL;
}

void vm_message_answer(Vm *vm, int button)
{
    if (!vm->msg_count)
        return;
    if (vm->msg[vm->msg_head].nbuttons)
        vm->button = button;
    vm->msg_head = (vm->msg_head + 1) % VM_MAX_MESSAGES;
    vm->msg_count--;
}

void vm_notify(Vm *vm, const char *text)
{
    uint32_t i;
    if (vm->notify_count == VM_MAX_NOTIFY) {
        for (i = 1; i < VM_MAX_NOTIFY; ++i)
            memcpy(vm->notify[i - 1], vm->notify[i], sizeof(vm->notify[0]));
        vm->notify_count--;
    }
    copy_str(vm->notify[vm->notify_count++], sizeof(vm->notify[0]), text);
}

/* ------------------------------------------------------------------------ */
/* game time                                                                */
/* ------------------------------------------------------------------------ */
static float gfloat(const Vm *vm, int idx, float fallback)
{
    if (idx < 0)
        return fallback;
    return vm->db->globals[idx].type == DRPG_VT_FLOAT ? vm->globals[idx].f
                                                       : (float)vm->globals[idx].i;
}

static void gset(Vm *vm, int idx, float v)
{
    if (idx < 0)
        return;
    if (vm->db->globals[idx].type == DRPG_VT_FLOAT)
        vm->globals[idx].f = v;
    else
        vm->globals[idx].i = (int32_t)v;
}

static void pull_time(Vm *vm)
{
    vm->hour = gfloat(vm, vm->g_hour, vm->hour);
    vm->day = (int32_t)gfloat(vm, vm->g_day, (float)vm->day);
    vm->month = (int32_t)gfloat(vm, vm->g_month, (float)vm->month);
    vm->year = (int32_t)gfloat(vm, vm->g_year, (float)vm->year);
    vm->days_passed = (int32_t)gfloat(vm, vm->g_dayspassed, (float)vm->days_passed);
    vm->timescale = gfloat(vm, vm->g_timescale, vm->timescale);
}

static void push_time(Vm *vm)
{
    gset(vm, vm->g_hour, vm->hour);
    gset(vm, vm->g_day, (float)vm->day);
    gset(vm, vm->g_month, (float)vm->month);
    gset(vm, vm->g_year, (float)vm->year);
    gset(vm, vm->g_dayspassed, (float)vm->days_passed);
}

static void advance_time(Vm *vm, float dt)
{
    pull_time(vm);
    vm->hour += dt * vm->timescale / 3600.0f;
    while (vm->hour >= 24.0f) {
        vm->hour -= 24.0f;
        vm->days_passed++;
        vm->day++;
        if (vm->month < 0 || vm->month > 11)
            vm->month = 0;
        if (vm->day > month_days[vm->month]) {
            vm->day = 1;
            vm->month++;
            if (vm->month > 11) {
                vm->month = 0;
                vm->year++;
            }
        }
    }
    push_time(vm);
}

const char *vm_month_name(const Vm *vm, int month)
{
    static const char *const ids[12] = { "sMonthMorningstar", "sMonthSunsdawn", "sMonthFirstseed",
                                         "sMonthRainshand", "sMonthSecondseed", "sMonthMidyear",
                                         "sMonthSunsheight", "sMonthLastseed", "sMonthHeartfire",
                                         "sMonthFrostfall", "sMonthSunsdusk", "sMonthEveningstar" };
    if (month < 0 || month > 11)
        month = 0;
    return rpgdb_gmst_string(vm->db, ids[month], month_names[month]);
}

void vm_date_string(const Vm *vm, uint16_t day, uint8_t month, char *out, uint32_t cap)
{
    snprintf(out, cap, "%u %s", (unsigned)day, vm_month_name(vm, month));
}

/* ------------------------------------------------------------------------ */
/* globals                                                                  */
/* ------------------------------------------------------------------------ */
int vm_global_index(const Vm *vm, const char *name)
{
    return rpgdb_global(vm->db, pw_hash_name(name));
}

float vm_global_float(const Vm *vm, int index)
{
    if (index < 0 || (uint32_t)index >= vm->global_count)
        return 0.0f;
    return gfloat(vm, index, 0.0f);
}

void vm_set_global_float(Vm *vm, int index, float v)
{
    if (index < 0 || (uint32_t)index >= vm->global_count)
        return;
    gset(vm, index, v);
}

/* ------------------------------------------------------------------------ */
/* text defines                                                             */
/* ------------------------------------------------------------------------ */
static const char *class_name(Vm *vm, uint32_t hash)
{
    const DRpgObject *o = rpgdb_object(vm->db, hash);
    return o ? rpgdb_name(vm->db, o->name) : "";
}

static const char *rank_name(Vm *vm, uint32_t faction, int rank)
{
    const DRpgFaction *f = rpgdb_faction(vm->db, faction);
    if (!f || rank < 0 || rank > 9)
        return "";
    return rpgdb_name(vm->db, f->rank_name[rank]);
}

static const DRpgObject *player_object(const Vm *vm)
{
    if (vm->db->hdr && vm->db->hdr->player != DRPG_NONE)
        return &vm->db->objects[vm->db->hdr->player];
    return NULL;
}

void vm_fix_defines(Vm *vm, char sigil, const char *in, char *out, uint32_t cap,
                    const VmRef *speaker)
{
    uint32_t n = 0;
    const DRpgObject *pc = player_object(vm);
    const DRpgObject *sp = speaker ? vm_ref_object(vm, speaker) : NULL;
    uint32_t sp_faction = speaker ? vm_ref_faction(vm, speaker) : 0;
    if (!cap)
        return;
    while (*in && n + 1 < cap) {
        char buf[64];
        const char *rep = NULL;
        uint32_t skip = 0;
        if (*in == sigil) {
            const char *k = in + 1;
            buf[0] = '\0';
            if (ci_starts_with(k, "pcname")) {
                rep = vm->pc_name, skip = 6;
            } else if (ci_starts_with(k, "pcrace")) {
                rep = pc ? class_name(vm, pc->link[0]) : "", skip = 6;
            } else if (ci_starts_with(k, "pcclass")) {
                rep = pc ? class_name(vm, pc->link[1]) : "", skip = 7;
            } else if (ci_starts_with(k, "pcrank")) {
                rep = rank_name(vm, sp_faction, vm_pc_rank(vm, sp_faction)), skip = 6;
            } else if (ci_starts_with(k, "nextpcrank")) {
                rep = rank_name(vm, sp_faction, vm_pc_rank(vm, sp_faction) + 1), skip = 10;
            } else if (ci_starts_with(k, "pccrimelevel")) {
                VmRef p = vm_player_ref(vm);
                snprintf(buf, sizeof(buf), "%d", (int)vm_prop(vm, &p, MWP_CRIME));
                rep = buf, skip = 12;
            } else if (ci_starts_with(k, "name")) {
                rep = sp ? rpgdb_name(vm->db, sp->name) : "", skip = 4;
            } else if (ci_starts_with(k, "race")) {
                rep = (sp && sp->type == DRPG_OT_NPC) ? class_name(vm, sp->link[0]) : "", skip = 4;
            } else if (ci_starts_with(k, "class")) {
                rep = (sp && sp->type == DRPG_OT_NPC) ? class_name(vm, sp->link[1]) : "", skip = 5;
            } else if (ci_starts_with(k, "faction")) {
                const DRpgFaction *f = rpgdb_faction(vm->db, sp_faction);
                rep = f ? rpgdb_name(vm->db, f->name) : "", skip = 7;
            } else if (ci_starts_with(k, "rank")) {
                rep = speaker ? rank_name(vm, sp_faction, vm_ref_rank(vm, speaker)) : "", skip = 4;
            } else if (ci_starts_with(k, "cell")) {
                rep = vm->host.cell_name ? vm->host.cell_name(vm->host.user) : "", skip = 4;
            } else {
                /* global variable names: longest match wins */
                char word[48];
                uint32_t w = 0, best = 0;
                int best_idx = -1;
                while (w + 1 < sizeof(word) &&
                       ((k[w] >= 'a' && k[w] <= 'z') || (k[w] >= 'A' && k[w] <= 'Z') ||
                        (k[w] >= '0' && k[w] <= '9') || k[w] == '_')) {
                    int g;
                    word[w] = k[w];
                    word[++w] = '\0';
                    g = rpgdb_global(vm->db, pw_hash_name(word));
                    if (g >= 0) {
                        best = w;
                        best_idx = g;
                    }
                }
                if (best_idx >= 0) {
                    float v = gfloat(vm, best_idx, 0.0f);
                    if (vm->db->globals[best_idx].type == DRPG_VT_FLOAT)
                        snprintf(buf, sizeof(buf), "%g", (double)v);
                    else
                        snprintf(buf, sizeof(buf), "%d", (int)v);
                    rep = buf;
                    skip = best;
                }
            }
        }
        if (rep) {
            while (*rep && n + 1 < cap)
                out[n++] = *rep++;
            in += 1 + skip;
        } else {
            out[n++] = *in++;
        }
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------------------ */
/* message box formatting                                                   */
/* ------------------------------------------------------------------------ */
static void format_message(const char *fmt, const VmValue *args, const uint8_t *types, int nargs,
                           const VmCtx *ctx, char *out, uint32_t cap)
{
    uint32_t n = 0;
    int ai = 0;
    while (*fmt && n + 1 < cap) {
        char spec[16], piece[96];
        uint32_t sl = 0;
        const char *p;
        char conv;
        if (*fmt != '%') {
            out[n++] = *fmt++;
            continue;
        }
        if (fmt[1] == '%') {
            out[n++] = '%';
            fmt += 2;
            continue;
        }
        p = fmt + 1;
        spec[sl++] = '%';
        while (*p && strchr("-+ #0", *p) && sl < 6)
            spec[sl++] = *p++;
        while (*p >= '0' && *p <= '9' && sl < 9)
            spec[sl++] = *p++;
        if (*p == '.') {
            spec[sl++] = *p++;
            while (*p >= '0' && *p <= '9' && sl < 12)
                spec[sl++] = *p++;
        }
        conv = *p;
        if (!conv || !strchr("SsdifeEgGF", conv)) {
            out[n++] = *fmt++;
            continue;
        }
        piece[0] = '\0';
        if (ai < nargs) {
            VmValue v = args[ai];
            uint8_t t = types[ai];
            ai++;
            if (conv == 'S' || conv == 's') {
                spec[sl++] = 's';
                spec[sl] = '\0';
                snprintf(piece, sizeof(piece), spec, t == MWBC_ARG_STRING ? vm_arg_string(ctx, v) : "");
            } else if (conv == 'd' || conv == 'i') {
                spec[sl++] = 'd';
                spec[sl] = '\0';
                snprintf(piece, sizeof(piece), spec, t == MWBC_ARG_FLOAT ? (int)v.f : (int)v.i);
            } else {
                spec[sl++] = conv;
                spec[sl] = '\0';
                snprintf(piece, sizeof(piece), spec, t == MWBC_ARG_FLOAT ? (double)v.f : (double)v.i);
            }
        }
        {
            const char *q = piece;
            while (*q && n + 1 < cap)
                out[n++] = *q++;
        }
        fmt = p + 1;
    }
    out[n] = '\0';
}

/* ------------------------------------------------------------------------ */
/* built-in verbs                                                           */
/* ------------------------------------------------------------------------ */
static float argf(const VmValue *args, int argc, int i, char type)
{
    if (i >= argc)
        return 0.0f;
    return type == 'f' ? args[i].f : (float)args[i].i;
}

static int argi(const VmValue *args, int argc, int i)
{
    return i < argc ? args[i].i : 0;
}

static uint32_t arg_hash(const VmCtx *ctx, const VmValue *args, int argc, int i)
{
    if (i >= argc)
        return 0;
    return pw_hash_name(vm_arg_string(ctx, args[i]));
}

static void start_script(Vm *vm, uint32_t script_hash, const VmRef *target)
{
    int vmi = global_instance(vm, script_hash);
    if (vmi < 0) {
        vm_error(vm, "StartScript: unknown script");
        return;
    }
    vm->inst[vmi].flags |= VMI_RUNNING;
    if (target && target->kind == VM_REF_OBJECT) {
        vm->inst[vmi].flags |= VMI_TARGETED;
        vm->inst[vmi].target = target->object;
    }
}

static void player_item_notice(Vm *vm, uint32_t item, int32_t count, int added)
{
    const DRpgObject *o = rpgdb_object(vm->db, item);
    char text[96];
    const char *name = o ? rpgdb_name(vm->db, o->name) : "";
    if (!*name)
        return;
    if (count > 1)
        snprintf(text, sizeof(text), "%d %s %s", (int)count, name,
                 added ? "have been added to your inventory." : "have been removed from your inventory.");
    else
        snprintf(text, sizeof(text), "%s %s", name,
                 added ? "has been added to your inventory." : "has been removed from your inventory.");
    vm_notify(vm, text);
}

/* family verbs: GetStrength / SetHealth / ModDisposition / ... */
static int family_verb(Vm *vm, uint16_t func, const VmRef *ref, const VmValue *args, int argc,
                       VmValue *ret)
{
    int fam = mw_func_fam[func], prop = mw_func_prop[func];
    const MwFuncInfo *fi = &mw_funcs[func];
    char at = fi->args[0];
    VmRef pc = vm_player_ref(vm);
    float v;
    if (fam == MWFAM_GET_PC || fam == MWFAM_SET_PC || fam == MWFAM_MOD_PC)
        ref = &pc;
    switch (fam) {
    case MWFAM_GET:
    case MWFAM_GET_PC:
        v = prop == MWP_DISPOSITION ? (float)vm_disposition(vm, ref) : vm_prop(vm, ref, prop);
        if (fi->ret == 'f')
            ret->f = v;
        else
            ret->i = (int32_t)v;
        return 1;
    case MWFAM_SET:
    case MWFAM_SET_PC:
        vm_set_prop(vm, ref, prop, argf(args, argc, 0, at));
        return 1;
    case MWFAM_MOD:
    case MWFAM_MOD_PC:
        vm_set_prop(vm, ref, prop, vm_prop(vm, ref, prop) + argf(args, argc, 0, at));
        return 1;
    case MWFAM_SETDYN: {
        float x = argf(args, argc, 0, at);
        vm_set_prop(vm, ref, prop - MWP_DYN_HEALTH + MWP_DYNBASE_HEALTH, x);
        vm_set_prop(vm, ref, prop, x);
        return 1;
    }
    case MWFAM_MODDYN: {
        float x = argf(args, argc, 0, at);
        int base = prop - MWP_DYN_HEALTH + MWP_DYNBASE_HEALTH;
        vm_set_prop(vm, ref, base, vm_prop(vm, ref, base) + x);
        vm_set_prop(vm, ref, prop, vm_prop(vm, ref, prop) + x);
        return 1;
    }
    case MWFAM_RATIO: {
        float base = vm_prop(vm, ref, prop - MWP_DYN_HEALTH + MWP_DYNBASE_HEALTH);
        ret->f = base > 0.0f ? vm_prop(vm, ref, prop) / base : 0.0f;
        return 1;
    }
    default:
        return 0;
    }
}

static int builtin(Vm *vm, VmCtx *ctx, uint16_t func, const VmRef *ref, const VmValue *args,
                   int argc, VmValue *ret)
{
    if (mw_func_fam[func] != MWFAM_NONE)
        return family_verb(vm, func, ref, args, argc, ret);
    switch (func) {
    /* --- dialogue and journal --------------------------------------- */
    case MWF_JOURNAL:
        vm_journal(vm, arg_hash(ctx, args, argc, 0), argi(args, argc, 1));
        return 1;
    case MWF_SETJOURNALINDEX:
        vm_set_journal_index(vm, arg_hash(ctx, args, argc, 0), argi(args, argc, 1));
        return 1;
    case MWF_GETJOURNALINDEX:
        ret->i = vm_journal_index(vm, arg_hash(ctx, args, argc, 0));
        return 1;
    case MWF_ADDTOPIC:
        vm_add_topic(vm, arg_hash(ctx, args, argc, 0));
        return 1;
    case MWF_CHOICE: {
        int i;
        for (i = 0; i + 1 < argc; i += 2) {
            if (vm->choice_count < VM_MAX_CHOICES) {
                VmChoice *c = &vm->choices[vm->choice_count++];
                copy_str(c->text, sizeof(c->text), vm_arg_string(ctx, args[i]));
                c->value = args[i + 1].i;
            }
        }
        return 1;
    }
    case MWF_GOODBYE:
        vm->goodbye = 1;
        return 1;
    case MWF_FORCEGREETING:
        if (ref->kind == VM_REF_OBJECT) {
            vm->force_greeting = *ref;
            vm->has_force_greeting = 1;
        }
        return 1;
    case MWF_CLEARINFOACTOR:
        /* the journal topic list stores no speaker names, nothing to clear */
        return 1;
    /* --- messages, time, randomness ----------------------------------- */
    case MWF_GETBUTTONPRESSED:
        ret->i = vm->button;
        vm->button = -1;
        return 1;
    case MWF_MENUMODE:
        ret->i = vm->menu_mode;
        return 1;
    case MWF_GETSECONDSPASSED:
        ret->f = vm->frame_dt;
        return 1;
    case MWF_RANDOM: {
        int n = argi(args, argc, 0);
        ret->f = n > 0 ? (float)(rng_next(vm) % (uint32_t)n) : 0.0f;
        return 1;
    }
    case MWF_GETCURRENTTIME:
        ret->f = vm->hour;
        return 1;
    case MWF_GETSQUAREROOT: {
        float x = argf(args, argc, 0, 'f');
        ret->f = x > 0.0f ? sqrtf(x) : 0.0f;
        return 1;
    }
    /* --- scripts --------------------------------------------------------- */
    case MWF_STARTSCRIPT:
        start_script(vm, arg_hash(ctx, args, argc, 0), ref == &ctx->self ? NULL : ref);
        return 1;
    case MWF_STOPSCRIPT: {
        int vmi = find_instance_by_key(vm, arg_hash(ctx, args, argc, 0), 1);
        if (vmi >= 0)
            vm->inst[vmi].flags &= (uint16_t)~(VMI_RUNNING | VMI_TARGETED);
        return 1;
    }
    case MWF_SCRIPTRUNNING: {
        int vmi = find_instance_by_key(vm, arg_hash(ctx, args, argc, 0), 1);
        ret->i = vmi >= 0 && (vm->inst[vmi].flags & VMI_RUNNING) ? 1 : 0;
        return 1;
    }
    /* --- events ---------------------------------------------------------- */
    case MWF_ONACTIVATE:
    case MWF_ONDEATH:
    case MWF_ONKNOCKOUT:
    case MWF_ONMURDER: {
        /* (OnPCAdd, OnPCEquip, ... are script locals the engine sets) */
        uint16_t bit = func == MWF_ONACTIVATE ? VMEV_ACTIVATE : func == MWF_ONDEATH ? VMEV_DEATH
                     : func == MWF_ONKNOCKOUT ? VMEV_KNOCKOUT : VMEV_MURDER;
        ret->i = (ctx->vmi >= 0 && (vm->inst[ctx->vmi].events & bit)) ? 1 : 0;
        return 1;
    }
    /* --- enable state ---------------------------------------------------- */
    case MWF_ENABLE:
    case MWF_DISABLE:
        if (ref->kind == VM_REF_OBJECT) {
            vm_set_prop(vm, ref, MWP_DISABLED, func == MWF_DISABLE ? 1.0f : 0.0f);
            if (vm->host.set_disabled)
                vm->host.set_disabled(vm->host.user, ref, func == MWF_DISABLE);
        }
        return 1;
    case MWF_GETDISABLED:
        ret->i = ref->kind == VM_REF_OBJECT ? vm_disabled(vm, ref->key) : 0;
        return 1;
    /* --- items and spells ------------------------------------------------ */
    case MWF_ADDITEM:
    case MWF_REMOVEITEM: {
        uint32_t item = arg_hash(ctx, args, argc, 0);
        int32_t n = argi(args, argc, 1);
        if (ref->kind == VM_REF_NONE || n <= 0)
            return 1;
        seed_ref(vm, ref);
        if (func == MWF_REMOVEITEM) {
            int32_t have = vm_item_count(vm, ref->key, item);
            if (n > have)
                n = have;
            vm_item_add(vm, ref->key, item, -n);
        } else {
            vm_item_add(vm, ref->key, item, n);
        }
        if (ref->kind == VM_REF_PLAYER && n > 0)
            player_item_notice(vm, item, n, func == MWF_ADDITEM);
        return 1;
    }
    case MWF_GETITEMCOUNT:
        seed_ref(vm, ref);
        ret->i = ref->kind == VM_REF_NONE ? 0 : vm_item_count(vm, ref->key, arg_hash(ctx, args, argc, 0));
        return 1;
    case MWF_ADDSPELL:
    case MWF_REMOVESPELL:
        if (ref->kind != VM_REF_NONE) {
            uint32_t sp = arg_hash(ctx, args, argc, 0);
            seed_ref(vm, ref);
            if (func == MWF_ADDSPELL) {
                if (!vm_item_count(vm, ref->key ^ SPELL_SALT, sp))
                    vm_item_add(vm, ref->key ^ SPELL_SALT, sp, 1);
            } else {
                vm_item_add(vm, ref->key ^ SPELL_SALT, sp, -1);
            }
        }
        return 1;
    case MWF_GETSPELL:
        seed_ref(vm, ref);
        ret->i = ref->kind == VM_REF_NONE ? 0
                 : vm_item_count(vm, ref->key ^ SPELL_SALT, arg_hash(ctx, args, argc, 0)) > 0;
        return 1;
    /* --- factions -------------------------------------------------------- */
    case MWF_PCJOINFACTION:
    case MWF_PCRAISERANK:
    case MWF_PCLOWERRANK:
    case MWF_PCEXPELL:
    case MWF_PCCLEAREXPELLED:
    case MWF_GETPCRANK:
    case MWF_PCEXPELLED:
    case MWF_GETPCFACREP:
    case MWF_SETPCFACREP:
    case MWF_MODPCFACREP: {
        /* signatures "/S", "/c" or "l/c": the optional faction defaults to
         * the faction of the reference (the speaker in dialogue) */
        int fidx = mw_funcs[func].args[0] == 'l' ? 1 : 0;
        uint32_t fac = argc > fidx ? arg_hash(ctx, args, argc, fidx) : vm_ref_faction(vm, ref);
        int fi = rpgdb_faction_index(vm->db, fac), rank;
        if (fi < 0) {
            if (func == MWF_GETPCRANK)
                ret->i = -1;
            else if (func == MWF_PCEXPELLED || func == MWF_GETPCFACREP)
                ret->i = 0;
            return 1;
        }
        rank = vm->pc_rank[fi];
        switch (func) {
        case MWF_PCJOINFACTION:
            if (rank < 0)
                vm->pc_rank[fi] = 0;
            break;
        case MWF_PCRAISERANK:
            vm->pc_rank[fi] = (int8_t)(rank < 0 ? 0 : PW_MIN(rank + 1, 9));
            break;
        case MWF_PCLOWERRANK:
            if (rank > 0)
                vm->pc_rank[fi] = (int8_t)(rank - 1);
            break;
        case MWF_PCEXPELL:
            vm->pc_expelled[fi] = 1;
            break;
        case MWF_PCCLEAREXPELLED:
            vm->pc_expelled[fi] = 0;
            break;
        case MWF_GETPCRANK:
            ret->i = rank;
            break;
        case MWF_PCEXPELLED:
            ret->i = vm->pc_expelled[fi];
            break;
        case MWF_GETPCFACREP:
            ret->i = vm->pc_facrep[fi];
            break;
        case MWF_SETPCFACREP:
            vm->pc_facrep[fi] = (int16_t)argi(args, argc, 0);
            break;
        default: /* MWF_MODPCFACREP */
            vm->pc_facrep[fi] = (int16_t)(vm->pc_facrep[fi] + argi(args, argc, 0));
            break;
        }
        return 1;
    }
    case MWF_GETFACTIONREACTION:
        ret->i = vm_faction_reaction(vm, arg_hash(ctx, args, argc, 0), arg_hash(ctx, args, argc, 1));
        return 1;
    case MWF_SETFACTIONREACTION:
        set_faction_reaction(vm, arg_hash(ctx, args, argc, 0), arg_hash(ctx, args, argc, 1),
                             argi(args, argc, 2));
        return 1;
    case MWF_MODFACTIONREACTION: {
        uint32_t a = arg_hash(ctx, args, argc, 0), b = arg_hash(ctx, args, argc, 1);
        set_faction_reaction(vm, a, b, vm_faction_reaction(vm, a, b) + argi(args, argc, 2));
        return 1;
    }
    case MWF_RAISERANK:
    case MWF_LOWERRANK:
        if (vm_ref_faction(vm, ref)) {
            int r = vm_ref_rank(vm, ref) + (func == MWF_RAISERANK ? 1 : -1);
            vm_set_prop(vm, ref, MWP_RANK, (float)PW_CLAMP(r, 0, 9));
        }
        return 1;
    case MWF_SAMEFACTION: {
        uint32_t f = vm_ref_faction(vm, ref);
        ret->i = f && vm_pc_rank(vm, f) >= 0;
        return 1;
    }
    case MWF_GETRACE: {
        const DRpgObject *o = vm_ref_object(vm, ref);
        if (ref->kind == VM_REF_PLAYER)
            o = player_object(vm);
        ret->i = (o && o->type == DRPG_OT_NPC && o->link[0] == arg_hash(ctx, args, argc, 0));
        return 1;
    }
    case MWF_GETDEADCOUNT: {
        uint32_t id = arg_hash(ctx, args, argc, 0);
        ret->i = (int32_t)vm_prop_key(vm, id, id, MWP_DEATHS);
        return 1;
    }
    case MWF_GETPCCELL: {
        const char *cell = vm->host.cell_name ? vm->host.cell_name(vm->host.user) : "";
        ret->i = ci_starts_with(cell, vm_arg_string(ctx, args[0]));
        return 1;
    }
    case MWF_SAY: {
        VmValue dummy;
        if (vm->host.call && vm->host.call(vm->host.user, vm, func, ref, ctx, args, argc, &dummy))
            return 1;
        if (argc > 1 && *vm_arg_string(ctx, args[1]))
            vm_notify(vm, vm_arg_string(ctx, args[1]));
        return 1;
    }
    default:
        return 0;
    }
}

/* neutral results for world functions the host does not implement */
static void neutral_result(uint16_t func, VmValue *ret)
{
    switch (func) {
    case MWF_SAYDONE:
        ret->i = 1;
        break;
    case MWF_GETDISTANCE:
        ret->f = 1.0e6f;
        break;
    case MWF_GETSCALE:
        ret->f = 1.0f;
        break;
    case MWF_GETCURRENTAIPACKAGE:
        ret->i = -1;
        break;
    default:
        if (mw_funcs[func].ret == 'f')
            ret->f = 0.0f;
        else
            ret->i = 0;
        break;
    }
}

static void call_verb(Vm *vm, VmCtx *ctx, uint16_t func, const VmRef *ref, const VmValue *args,
                      int argc, VmValue *ret)
{
    ret->i = 0;
    if (func >= MWF_COUNT) {
        vm_error(vm, "bad function id");
        return;
    }
    if (builtin(vm, ctx, func, ref, args, argc, ret))
        return;
    if (vm->host.call && vm->host.call(vm->host.user, vm, func, ref, ctx, args, argc, ret))
        return;
    vm->unhandled++;
    neutral_result(func, ret);
}

/* ------------------------------------------------------------------------ */
/* interpreter                                                              */
/* ------------------------------------------------------------------------ */
#define NEED(n)                                       \
    do {                                              \
        if (pc + (n) > size) {                        \
            vm_error(vm, "bytecode truncated");       \
            return -1;                                \
        }                                             \
    } while (0)
#define POP(v)                                        \
    do {                                              \
        if (sp <= 0) {                                \
            vm_error(vm, "stack underflow");          \
            return -1;                                \
        }                                             \
        (v) = st[--sp];                               \
    } while (0)
#define PUSH(v)                                       \
    do {                                              \
        if (sp >= VM_STACK) {                         \
            vm_error(vm, "stack overflow");           \
            return -1;                                \
        }                                             \
        st[sp++] = (v);                               \
    } while (0)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static VmValue *member_slot(Vm *vm, uint32_t hash, uint32_t slot, int global, int *type_out,
                            uint8_t flags)
{
    int vmi = global ? global_instance(vm, hash) : object_instance(vm, hash);
    *type_out = (flags >> 1) & 3;
    if (vmi < 0) {
        vm_error(vm, "member access: no such script instance");
        return NULL;
    }
    if (slot >= instance_nlocals(vm, &vm->inst[vmi])) {
        vm_error(vm, "member access: bad slot");
        return NULL;
    }
    return &vm->locals[vm->inst[vmi].locals + slot];
}

static void store_typed(VmValue *dst, int type, VmValue v)
{
    if (type == DRPG_VT_SHORT)
        dst->i = (int16_t)v.i;
    else
        *dst = v;
}

static int run(Vm *vm, VmCtx *ctx, const uint8_t *code, uint32_t size)
{
    VmValue st[VM_STACK];
    int sp = 0;
    uint32_t pc = 0, budget = VM_OP_BUDGET;
    vm->runs++;
    while (pc < size) {
        uint8_t op = code[pc++];
        VmValue a, b;
        if (!--budget) {
            vm_error(vm, "script exceeded its instruction budget");
            return -1;
        }
        if (op >= MWBC_CMPI && op < MWBC_CMPI + 6) {
            POP(b);
            POP(a);
            switch (op - MWBC_CMPI) {
            case 0: a.i = a.i == b.i; break;
            case 1: a.i = a.i != b.i; break;
            case 2: a.i = a.i < b.i; break;
            case 3: a.i = a.i <= b.i; break;
            case 4: a.i = a.i > b.i; break;
            default: a.i = a.i >= b.i; break;
            }
            PUSH(a);
            continue;
        }
        if (op >= MWBC_CMPF && op < MWBC_CMPF + 6) {
            int r;
            POP(b);
            POP(a);
            switch (op - MWBC_CMPF) {
            case 0: r = a.f == b.f; break;
            case 1: r = a.f != b.f; break;
            case 2: r = a.f < b.f; break;
            case 3: r = a.f <= b.f; break;
            case 4: r = a.f > b.f; break;
            default: r = a.f >= b.f; break;
            }
            a.i = r;
            PUSH(a);
            continue;
        }
        switch (op) {
        case MWBC_RET:
            return 0;
        case MWBC_PUSHI8:
            NEED(1);
            a.i = (int8_t)code[pc];
            pc += 1;
            PUSH(a);
            break;
        case MWBC_PUSHI16:
            NEED(2);
            a.i = (int16_t)rd16(code + pc);
            pc += 2;
            PUSH(a);
            break;
        case MWBC_PUSHI32:
        case MWBC_PUSHF: {
            uint32_t raw;
            NEED(4);
            raw = rd32(code + pc);
            pc += 4;
            memcpy(&a, &raw, 4);
            PUSH(a);
            break;
        }
        case MWBC_PUSHS:
            NEED(2);
            a.i = rd16(code + pc);
            pc += 2;
            PUSH(a);
            break;
        case MWBC_LOADL:
        case MWBC_STOREL_S:
        case MWBC_STOREL_L:
        case MWBC_STOREL_F: {
            uint32_t slot;
            NEED(2);
            slot = rd16(code + pc);
            pc += 2;
            if (slot >= ctx->nlocals) {
                vm_error(vm, "local slot out of range");
                return -1;
            }
            if (op == MWBC_LOADL) {
                PUSH(ctx->locals[slot]);
            } else {
                POP(a);
                store_typed(&ctx->locals[slot], op == MWBC_STOREL_S ? DRPG_VT_SHORT : DRPG_VT_LONG, a);
            }
            break;
        }
        case MWBC_LOADG:
        case MWBC_STOREG_S:
        case MWBC_STOREG_L:
        case MWBC_STOREG_F: {
            uint32_t g;
            NEED(2);
            g = rd16(code + pc);
            pc += 2;
            if (g >= vm->global_count) {
                vm_error(vm, "global index out of range");
                return -1;
            }
            if (op == MWBC_LOADG) {
                PUSH(vm->globals[g]);
            } else {
                POP(a);
                store_typed(&vm->globals[g], op == MWBC_STOREG_S ? DRPG_VT_SHORT : DRPG_VT_LONG, a);
            }
            break;
        }
        case MWBC_LOADM:
        case MWBC_STOREM: {
            uint32_t hash, slot;
            uint8_t flags;
            int type;
            VmValue *dst;
            NEED(7);
            hash = rd32(code + pc);
            slot = rd16(code + pc + 4);
            flags = code[pc + 6];
            pc += 7;
            dst = member_slot(vm, hash, slot, flags & 1, &type, flags);
            if (op == MWBC_LOADM) {
                if (dst) {
                    PUSH(*dst);
                } else {
                    a.i = 0;
                    PUSH(a);
                }
            } else {
                POP(a);
                if (dst)
                    store_typed(dst, type, a);
            }
            break;
        }
        case MWBC_LOADD:
        case MWBC_STORED: {
            uint32_t hash;
            const DRpgVar *var = NULL;
            NEED(4);
            hash = rd32(code + pc);
            pc += 4;
            if (ctx->script >= 0)
                var = rpgdb_script_var(vm->db, (uint32_t)ctx->script, hash);
            if (var && var->slot >= ctx->nlocals)
                var = NULL;
            if (op == MWBC_LOADD) {
                a.f = 0.0f;
                if (var)
                    a.f = var->type == DRPG_VT_FLOAT ? ctx->locals[var->slot].f
                                                     : (float)ctx->locals[var->slot].i;
                else
                    vm_error(vm, "speaker has no such local variable");
                PUSH(a);
            } else {
                POP(a);
                if (var) {
                    VmValue v;
                    if (var->type == DRPG_VT_FLOAT)
                        v.f = a.f;
                    else
                        v.i = (int32_t)a.f;
                    store_typed(&ctx->locals[var->slot], var->type, v);
                } else {
                    vm_error(vm, "speaker has no such local variable");
                }
            }
            break;
        }
        case MWBC_I2F:
            POP(a);
            a.f = (float)a.i;
            PUSH(a);
            break;
        case MWBC_F2I:
            POP(a);
            a.i = (int32_t)a.f;
            PUSH(a);
            break;
        case MWBC_NEGI:
            POP(a);
            a.i = -a.i;
            PUSH(a);
            break;
        case MWBC_NEGF:
            POP(a);
            a.f = -a.f;
            PUSH(a);
            break;
        case MWBC_ADDI: POP(b); POP(a); a.i += b.i; PUSH(a); break;
        case MWBC_SUBI: POP(b); POP(a); a.i -= b.i; PUSH(a); break;
        case MWBC_MULI: POP(b); POP(a); a.i *= b.i; PUSH(a); break;
        case MWBC_DIVI:
            POP(b);
            POP(a);
            if (b.i == 0) {
                vm_error(vm, "integer division by zero");
                return -1;
            }
            a.i /= b.i;
            PUSH(a);
            break;
        case MWBC_ADDF: POP(b); POP(a); a.f += b.f; PUSH(a); break;
        case MWBC_SUBF: POP(b); POP(a); a.f -= b.f; PUSH(a); break;
        case MWBC_MULF: POP(b); POP(a); a.f *= b.f; PUSH(a); break;
        case MWBC_DIVF:
            POP(b);
            POP(a);
            if (b.f == 0.0f) {
                vm_error(vm, "division by zero");
                return -1;
            }
            a.f /= b.f;
            PUSH(a);
            break;
        case MWBC_JMP:
        case MWBC_JZ: {
            int16_t rel;
            int take = 1;
            NEED(2);
            rel = (int16_t)rd16(code + pc);
            pc += 2;
            if (op == MWBC_JZ) {
                POP(a);
                take = a.i == 0;
            }
            if (take) {
                int32_t to = (int32_t)pc + rel;
                if (to < 0 || (uint32_t)to > size) {
                    vm_error(vm, "jump out of range");
                    return -1;
                }
                pc = (uint32_t)to;
            }
            break;
        }
        case MWBC_POP:
            POP(a);
            break;
        case MWBC_CALL:
        case MWBC_CALLX: {
            uint16_t func;
            uint8_t argc;
            VmRef ref;
            VmValue ret;
            NEED(op == MWBC_CALL ? 3 : 7);
            func = rd16(code + pc);
            argc = code[pc + 2];
            if (op == MWBC_CALLX) {
                ref = vm_resolve(vm, rd32(code + pc + 3));
                pc += 7;
            } else {
                ref = ctx->self;
                pc += 3;
            }
            if (argc > sp) {
                vm_error(vm, "stack underflow in call");
                return -1;
            }
            sp -= argc;
            call_verb(vm, ctx, func, op == MWBC_CALL ? &ctx->self : &ref, st + sp, argc, &ret);
            if (func < MWF_COUNT && mw_funcs[func].kind == 'f')
                PUSH(ret);
            break;
        }
        case MWBC_MSGBOX: {
            uint16_t fmt;
            uint8_t nargs, nbtn, types[64];
            char text[320], fixed[320];
            char buttons[VM_MAX_BUTTONS][48];
            int i;
            NEED(4);
            fmt = rd16(code + pc);
            nargs = code[pc + 2];
            nbtn = code[pc + 3];
            pc += 4;
            NEED((uint32_t)nargs + 2u * nbtn);
            if (nargs > 64 || nargs > sp) {
                vm_error(vm, "bad message box");
                return -1;
            }
            memcpy(types, code + pc, nargs);
            pc += nargs;
            sp -= nargs;
            {
                VmValue fv;
                fv.i = fmt;
                format_message(vm_arg_string(ctx, fv), st + sp, types, nargs, ctx, text, sizeof(text));
            }
            vm_fix_defines(vm, '^', text, fixed, sizeof(fixed), &ctx->self);
            for (i = 0; i < nbtn; ++i) {
                VmValue bv;
                bv.i = rd16(code + pc + 2u * (uint32_t)i);
                if (i < VM_MAX_BUTTONS)
                    copy_str(buttons[i], sizeof(buttons[i]), vm_arg_string(ctx, bv));
            }
            pc += 2u * nbtn;
            vm_message(vm, fixed, (const char (*)[48])buttons, nbtn);
            break;
        }
        default:
            vm_error(vm, "bad opcode");
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* running scripts                                                          */
/* ------------------------------------------------------------------------ */
static VmRef instance_ref(Vm *vm, uint32_t vmi)
{
    VmInstance *in = &vm->inst[vmi];
    VmRef r;
    memset(&r, 0, sizeof(r));
    if (in->flags & VMI_GLOBAL) {
        if (in->flags & VMI_TARGETED)
            return vm_resolve(vm, in->target);
        r.kind = VM_REF_NONE;
        r.vmi = -1;
        r.inst = -1;
        return r;
    }
    r.kind = VM_REF_OBJECT;
    r.vmi = (int16_t)vmi;
    r.inst = (in->flags & VMI_LOADED) ? in->inst : -1;
    r.object = in->object;
    r.key = in->key;
    return r;
}

int vm_run_instance(Vm *vm, uint32_t vmi)
{
    VmInstance *in;
    const DRpgScript *s;
    const uint8_t *code;
    VmCtx ctx;
    int rc;
    if (vmi >= vm->inst_count)
        return -1;
    in = &vm->inst[vmi];
    s = &vm->db->scripts[in->script];
    code = rpgdb_script_code(vm->db, in->script);
    if (!code) {
        vm_error(vm, "script bytecode unavailable");
        return -1;
    }
    ctx.locals = &vm->locals[in->locals];
    ctx.nlocals = instance_nlocals(vm, in);
    ctx.script = in->script;
    ctx.strings = (const char *)code + s->code_size;
    ctx.strings_size = s->strings_size;
    ctx.self = instance_ref(vm, vmi);
    ctx.vmi = (int32_t)vmi;
    rc = run(vm, &ctx, code, s->code_size);
    vm->inst[vmi].events = 0;
    return rc;
}

int vm_run_snippet(Vm *vm, const uint8_t *blob, uint32_t avail, const VmRef *speaker)
{
    static uint8_t copy[VM_SNIPPET_MAX];
    DRpgSnippet h;
    VmCtx ctx;
    VmValue none;
    uint32_t total;
    if (!blob || avail < sizeof(h))
        return -1;
    memcpy(&h, blob, sizeof(h));
    total = (uint32_t)sizeof(h) + h.code_size + h.strings_size;
    if (total > avail || total > sizeof(copy)) {
        vm_error(vm, "result script blob too large");
        return -1;
    }
    memcpy(copy, blob, total);
    memset(&ctx, 0, sizeof(ctx));
    none.i = 0;
    ctx.locals = &none;
    ctx.nlocals = 0;
    ctx.script = -1;
    ctx.vmi = -1;
    ctx.self = *speaker;
    if (speaker->kind == VM_REF_OBJECT) {
        int vmi = speaker->vmi >= 0 ? speaker->vmi : find_instance_by_object(vm, speaker->object);
        if (vmi < 0)
            vmi = object_instance(vm, speaker->object);
        if (vmi >= 0) {
            ctx.locals = &vm->locals[vm->inst[vmi].locals];
            ctx.nlocals = instance_nlocals(vm, &vm->inst[vmi]);
            ctx.script = vm->inst[vmi].script;
            ctx.self.vmi = (int16_t)vmi;
        }
    }
    ctx.strings = (const char *)copy + sizeof(h) + h.code_size;
    ctx.strings_size = h.strings_size;
    return run(vm, &ctx, copy + sizeof(h), h.code_size);
}

int vm_activate(Vm *vm, const VmRef *ref)
{
    int vmi;
    if (!ref || ref->kind != VM_REF_OBJECT)
        return 0;
    vmi = ref->vmi >= 0 ? ref->vmi : find_instance_by_key(vm, ref->key, 0);
    if (vmi < 0)
        return 0;
    if (!(vm->db->scripts[vm->inst[vmi].script].flags & DRPG_SF_ONACTIVATE))
        return 0;
    vm->inst[vmi].events |= VMEV_ACTIVATE;
    return 1;
}

void vm_post_event(Vm *vm, const VmRef *ref, uint16_t events)
{
    int vmi;
    if (!ref || ref->kind != VM_REF_OBJECT)
        return;
    vmi = ref->vmi >= 0 ? ref->vmi : find_instance_by_key(vm, ref->key, 0);
    if (vmi >= 0)
        vm->inst[vmi].events |= events;
}

void vm_tick(Vm *vm, float dt)
{
    uint32_t i, n;
    vm->frame_dt = dt;
    advance_time(vm, dt);
    n = vm->inst_count; /* instances created during the tick run next frame */
    for (i = 0; i < n; ++i) {
        VmInstance *in = &vm->inst[i];
        if (in->flags & VMI_GLOBAL) {
            if (in->flags & VMI_RUNNING)
                vm_run_instance(vm, i);
        } else if ((in->flags & VMI_LOADED) && !vm_disabled(vm, in->key)) {
            vm_run_instance(vm, i);
        }
    }
    pull_time(vm);
}

/* ------------------------------------------------------------------------ */
/* setup                                                                    */
/* ------------------------------------------------------------------------ */
static void *alloc_zero(uint32_t bytes)
{
    void *p = mem_try_alloc(MEM_REGION_SCRIPT, bytes ? bytes : 4u, 16);
    if (p)
        memset(p, 0, bytes ? bytes : 4u);
    return p;
}

int vm_init(Vm *vm, RpgDb *db, const VmHost *host)
{
    const DRpgHeader *h = db->hdr;
    uint32_t topics = h->topics.count, infos = h->infos.count, factions = h->factions.count;
    memset(vm, 0, sizeof(*vm));
    vm->db = db;
    if (host)
        vm->host = *host;
    vm->player_hash = pw_hash_name("player");
    vm->global_count = h->globals.count;
    vm->globals = alloc_zero(vm->global_count * (uint32_t)sizeof(VmValue));
    vm->inst = alloc_zero(VM_MAX_INSTANCES * (uint32_t)sizeof(VmInstance));
    vm->locals = alloc_zero(VM_MAX_LOCALS * (uint32_t)sizeof(VmValue));
    vm->props = alloc_zero(VM_PROP_SLOTS * (uint32_t)sizeof(VmProp));
    vm->items = alloc_zero(VM_ITEM_SLOTS * (uint32_t)sizeof(VmItem));
    vm->quest_index = alloc_zero(topics * (uint32_t)sizeof(int16_t));
    vm->quest_flags = alloc_zero(topics);
    vm->journal = alloc_zero(VM_MAX_JOURNAL * (uint32_t)sizeof(VmJournalEntry));
    vm->known_topics = alloc_zero((topics + 7u) / 8u);
    vm->heard_infos = alloc_zero((infos + 7u) / 8u);
    vm->pc_rank = alloc_zero(factions);
    vm->pc_expelled = alloc_zero(factions);
    vm->pc_facrep = alloc_zero(factions * (uint32_t)sizeof(int16_t));
    if (!vm->globals || !vm->inst || !vm->locals || !vm->props || !vm->items || !vm->quest_index ||
        !vm->quest_flags || !vm->journal || !vm->known_topics || !vm->heard_infos ||
        !vm->pc_rank || !vm->pc_expelled || !vm->pc_facrep)
        return -1;
    vm_new_game(vm);
    return 0;
}

void vm_new_game(Vm *vm)
{
    const DRpgHeader *h = vm->db->hdr;
    const DRpgObject *pc;
    uint32_t i;
    for (i = 0; i < vm->global_count; ++i)
        vm->globals[i].i = (int32_t)vm->db->globals[i].value;
    vm->inst_count = 0;
    vm->locals_used = 0;
    memset(vm->props, 0, VM_PROP_SLOTS * sizeof(VmProp));
    memset(vm->items, 0, VM_ITEM_SLOTS * sizeof(VmItem));
    vm->prop_count = vm->item_count = 0;
    memset(vm->quest_index, 0, h->topics.count * sizeof(int16_t));
    memset(vm->quest_flags, 0, h->topics.count);
    vm->journal_count = 0;
    memset(vm->known_topics, 0, (h->topics.count + 7u) / 8u);
    memset(vm->heard_infos, 0, (h->infos.count + 7u) / 8u);
    memset(vm->pc_rank, 0xFF, h->factions.count);
    memset(vm->pc_expelled, 0, h->factions.count);
    memset(vm->pc_facrep, 0, h->factions.count * sizeof(int16_t));
    vm->msg_head = vm->msg_count = 0;
    vm->button = -1;
    vm->choice_count = 0;
    vm->goodbye = 0;
    vm->has_force_greeting = 0;
    vm->notify_count = 0;
    vm->journal_updated = 0;
    vm->errors = vm->unhandled = vm->runs = 0;
    vm->last_error[0] = '\0';
    vm->rng = 0x2545F491u;
    vm->g_hour = vm_global_index(vm, "gamehour");
    vm->g_day = vm_global_index(vm, "day");
    vm->g_month = vm_global_index(vm, "month");
    vm->g_year = vm_global_index(vm, "year");
    vm->g_dayspassed = vm_global_index(vm, "dayspassed");
    vm->g_timescale = vm_global_index(vm, "timescale");
    vm->hour = 9.0f;
    vm->day = 16;
    vm->month = 7;
    vm->year = 427;
    vm->days_passed = 1;
    vm->timescale = 30.0f;
    pull_time(vm);
    if (vm->day < 1)
        vm->day = 1;
    pc = player_object(vm);
    copy_str(vm->pc_name, sizeof(vm->pc_name), "Outlander");
    if (pc) {
        const char *nm = rpgdb_name(vm->db, pc->name);
        if (*nm && !ci_starts_with(nm, "player"))
            copy_str(vm->pc_name, sizeof(vm->pc_name), nm);
    }
    seed_reference(vm, vm->player_hash, vm->player_hash);
    for (i = 0; i < h->starts.count; ++i) {
        uint32_t si = vm->db->starts[i];
        start_script(vm, vm->db->scripts[si].hash, NULL);
    }
}
