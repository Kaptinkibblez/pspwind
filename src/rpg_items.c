/*
 * rpg_items.c - equipment, item use, casting, combat, lock picking.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rpg_internal.h"

/* ------------------------------------------------------------------------ */
/* stacks                                                                   */
/* ------------------------------------------------------------------------ */
int rpg_find_stack(Rpg *r, uint32_t owner, uint32_t item)
{
    uint32_t cursor = 0, it;
    int32_t count;
    int s;
    while ((s = vm_item_next(r->vm, owner, &cursor, &it, &count)) >= 0)
        if (it == item)
            return s;
    return -1;
}

static void retarget_equipment(Rpg *r, int from, int to)
{
    uint32_t i;
    for (i = 0; i < r->eq_count; ++i)
        if (r->eq[i].stack == from)
            r->eq[i].stack = to;
}

int rpg_set_condition(Rpg *r, int stack, int health)
{
    VmItem *it = vm_item_at(r->vm, stack);
    int ns;
    if (!it || it->count <= 0)
        return -1;
    if (health < 0)
        health = 0;
    if (it->count == 1 && it->variant != 0) {
        it->health = (uint16_t)health;
        return stack;
    }
    /* split one unit off with its new state; the equipped unit follows it */
    ns = vm_item_add_state(r->vm, it->owner, it->item, 1, (uint16_t)health, it->charge, it->soul);
    it = vm_item_at(r->vm, stack);
    if (ns < 0)
        return stack;
    it->count--;
    if (rpg_is_equipped_any(r, stack))
        retarget_equipment(r, stack, ns);
    return ns;
}

void rpg_set_charge(Rpg *r, int stack, float charge)
{
    VmItem *it = vm_item_at(r->vm, stack);
    int ns;
    if (!it || it->count <= 0)
        return;
    if (it->count == 1 && it->variant != 0) {
        it->charge = charge;
        return;
    }
    ns = vm_item_add_state(r->vm, it->owner, it->item, 1, it->health, charge, it->soul);
    it = vm_item_at(r->vm, stack);
    if (ns < 0)
        return;
    it->count--;
    if (rpg_is_equipped_any(r, stack))
        retarget_equipment(r, stack, ns);
}

int rpg_move_stack(Rpg *r, int stack, uint32_t to_owner, int32_t count)
{
    VmItem *it = vm_item_at(r->vm, stack);
    uint32_t item, owner;
    uint16_t health;
    float charge;
    uint32_t soul;
    int ns;
    if (!it || count <= 0 || it->count < count)
        return -1;
    item = it->item;
    owner = it->owner;
    health = it->health;
    charge = it->charge;
    soul = it->soul;
    it->count -= count;
    if (it->count == 0) {
        uint32_t i = 0;
        while (i < r->eq_count) {
            if (r->eq[i].stack == stack)
                r->eq[i] = r->eq[--r->eq_count];
            else
                ++i;
        }
    }
    ns = vm_item_add_state(r->vm, to_owner, item, count, health, charge, soul);
    /* the source owner's scripts / constants learn about the change */
    vm_item_add(r->vm, owner, item, 0);
    return ns;
}

int rpg_item_value(Rpg *r, const VmItem *it)
{
    const DRpgObject *o = rpg_object(r, it->item);
    int mx;
    float v;
    if (!o)
        return 0;
    v = (float)o->value;
    mx = rpg_item_health_max(r, o);
    if (mx > 0 && it->health != VM_FULL)
        v *= (float)it->health / (float)mx;
    if (it->soul) {
        const DRpgObject *c = rpgdb_object(r->db, it->soul);
        const DRpgActor *a = rpgdb_actor(r->db, c);
        if (a)
            v += (float)a->soul * rpg_gmst_f(r, "fSoulGemMult", 3.0f);
    }
    return (int)v;
}

float rpg_item_charge_max(Rpg *r, const DRpgObject *o)
{
    const DRpgSpell *en;
    if (!o || !o->link[0] || o->type == DRPG_OT_NPC || o->type == DRPG_OT_RACE || o->type == DRPG_OT_CLAS)
        return 0.0f;
    en = rpg_spell(r, o->link[0]);
    if (!en || en->kind != 1)
        return 0.0f;
    return (float)en->charge;
}

static float stack_charge(Rpg *r, const VmItem *it)
{
    return it->charge < 0.0f ? rpg_item_charge_max(r, rpg_object(r, it->item)) : it->charge;
}

/* ------------------------------------------------------------------------ */
/* equipment                                                                */
/* ------------------------------------------------------------------------ */
static int two_handed(const DRpgObject *o)
{
    if (!o || o->type != DRPG_OT_WEAP)
        return 0;
    switch (o->stat[0]) {
    case 2: case 4: case 5: case 6: case 8: case 9: case 10:
        return 1;
    default:
        return 0;
    }
}

static int is_beast(Rpg *r, const VmRef *ref)
{
    const DRpgObject *o = rpg_actor_object(r, ref);
    uint32_t race = rpg_is_player(r, ref) ? vm_pc_race(r->vm) : (o ? o->link[0] : 0);
    const DRpgRace *rc = rpgdb_race(r->db, race);
    return rc && (rc->flags & 2);
}

int rpg_slot_for(Rpg *r, const VmRef *ref, const DRpgObject *o, int *second)
{
    static const int armor_slots[11] = { RPG_SLOT_HELMET, RPG_SLOT_CUIRASS, RPG_SLOT_LPAULDRON,
                                         RPG_SLOT_RPAULDRON, RPG_SLOT_GREAVES, RPG_SLOT_BOOTS,
                                         RPG_SLOT_LGAUNTLET, RPG_SLOT_RGAUNTLET, RPG_SLOT_SHIELD,
                                         RPG_SLOT_LGAUNTLET, RPG_SLOT_RGAUNTLET };
    static const int cloth_slots[10] = { RPG_SLOT_PANTS, RPG_SLOT_BOOTS, RPG_SLOT_SHIRT, RPG_SLOT_BELT,
                                         RPG_SLOT_ROBE, RPG_SLOT_RGAUNTLET, RPG_SLOT_LGAUNTLET,
                                         RPG_SLOT_SKIRT, RPG_SLOT_RRING, RPG_SLOT_AMULET };
    *second = -1;
    if (!o)
        return -1;
    switch (o->type) {
    case DRPG_OT_ARMO: {
        int s = armor_slots[PW_CLAMP(o->stat[0], 0, 10)];
        if (s == RPG_SLOT_BOOTS && is_beast(r, ref))
            return -2;
        return s;
    }
    case DRPG_OT_CLOT: {
        int s = cloth_slots[PW_CLAMP(o->stat[0], 0, 9)];
        if (s == RPG_SLOT_BOOTS && is_beast(r, ref))
            return -2;
        if (s == RPG_SLOT_RRING)
            *second = RPG_SLOT_LRING;
        return s;
    }
    case DRPG_OT_WEAP:
        return (o->stat[0] == 12 || o->stat[0] == 13) ? RPG_SLOT_AMMO : RPG_SLOT_WEAPON;
    case DRPG_OT_LOCK:
    case DRPG_OT_PROB:
        return RPG_SLOT_WEAPON;
    case DRPG_OT_LIGH:
        return (o->flags & DRPG_OF_CARRY) ? RPG_SLOT_SHIELD : -1;
    default:
        return -1;
    }
}

static int eq_index(Rpg *r, uint32_t key, int slot)
{
    uint32_t i;
    for (i = 0; i < r->eq_count; ++i)
        if (r->eq[i].key == key && r->eq[i].slot == slot)
            return (int)i;
    return -1;
}

int rpg_equipped(Rpg *r, const VmRef *ref, int slot)
{
    int i = eq_index(r, ref->key, slot);
    const VmItem *it;
    if (i < 0)
        return -1;
    it = vm_item_at(r->vm, r->eq[i].stack);
    if (!it || it->owner != ref->key || it->count <= 0) {
        r->eq[i] = r->eq[--r->eq_count];
        return -1;
    }
    return r->eq[i].stack;
}

int rpg_is_equipped_any(Rpg *r, int stack)
{
    uint32_t i;
    for (i = 0; i < r->eq_count; ++i)
        if (r->eq[i].stack == stack && r->eq[i].slot < RPG_SLOT_COUNT)
            return 1;
    return 0;
}

int rpg_is_equipped(Rpg *r, const VmRef *ref, int stack)
{
    uint32_t i;
    for (i = 0; i < r->eq_count; ++i)
        if (r->eq[i].key == ref->key && r->eq[i].stack == stack && r->eq[i].slot < RPG_SLOT_COUNT)
            return 1;
    return 0;
}

static void item_local(Rpg *r, uint32_t item, const char *name)
{
    const DRpgObject *o = rpgdb_object(r->db, item);
    if (o && o->script != DRPG_NONE16)
        vm_set_local(r->vm, item, name, 1.0f);
}

void rpg_unequip_slot(Rpg *r, const VmRef *ref, int slot)
{
    int i = eq_index(r, ref->key, slot);
    if (i < 0)
        return;
    r->eq[i] = r->eq[--r->eq_count];
    if (rpg_is_player(r, ref))
        r->dirty_constants = 1;
    else
        rpg_refresh_constants(r, ref);
}

static int set_slot(Rpg *r, const VmRef *ref, int slot, int stack)
{
    int i = eq_index(r, ref->key, slot);
    if (i < 0) {
        if (r->eq_count >= RPG_MAX_EQUIP) {
            vm_error(r->vm, "equipment table full");
            return 0;
        }
        i = (int)r->eq_count++;
        r->eq[i].key = ref->key;
        r->eq[i].slot = (uint8_t)slot;
    }
    r->eq[i].stack = stack;
    return 1;
}

int rpg_equip(Rpg *r, const VmRef *ref, int stack)
{
    VmItem *it = vm_item_at(r->vm, stack);
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    int second, slot;
    if (!it || it->owner != ref->key || it->count <= 0)
        return 0;
    slot = rpg_slot_for(r, ref, o, &second);
    if (slot == -2) {
        if (rpg_is_player(r, ref))
            rpg_notify(r, "sNotifyMessage13", "Your race cannot wear that.");
        return -1;
    }
    if (slot < 0)
        return 0;
    if (second >= 0 && rpg_equipped(r, ref, slot) >= 0 && rpg_equipped(r, ref, second) < 0)
        slot = second;
    if (slot == RPG_SLOT_WEAPON && two_handed(o))
        rpg_unequip_slot(r, ref, RPG_SLOT_SHIELD);
    if (slot == RPG_SLOT_SHIELD) {
        int w = rpg_equipped(r, ref, RPG_SLOT_WEAPON);
        const VmItem *wi = w >= 0 ? vm_item_at(r->vm, w) : NULL;
        if (wi && two_handed(rpg_object(r, wi->item)))
            rpg_unequip_slot(r, ref, RPG_SLOT_WEAPON);
    }
    /* a robe covers the shirt etc. only visually; slots stay independent */
    if (!set_slot(r, ref, slot, stack))
        return 0;
    if (rpg_is_player(r, ref)) {
        item_local(r, it->item, "onpcequip");
        r->dirty_constants = 1;
    } else {
        rpg_refresh_constants(r, ref);
    }
    return 1;
}

static float weapon_score(const DRpgObject *o)
{
    float best = 0.0f;
    int k;
    for (k = 0; k < 3; ++k) {
        uint32_t packed = k < 2 ? o->link[1] : o->link[2];
        int mx = k == 0 ? (int)((packed >> 8) & 0xFF) : k == 1 ? (int)((packed >> 24) & 0xFF)
                                                               : (int)((packed >> 8) & 0xFF);
        if ((float)mx > best)
            best = (float)mx;
    }
    return best;
}

void rpg_autoequip(Rpg *r, const VmRef *ref)
{
    const DRpgObject *ao = rpg_actor_object(r, ref);
    uint32_t cursor = 0, item;
    int32_t count;
    int s, slot;
    int best[RPG_SLOT_COUNT];
    float score[RPG_SLOT_COUNT];
    if (!ao || rpg_is_player(r, ref) || eq_index(r, ref->key, 0xFF) >= 0)
        return;
    /* marker entry: this actor has been dressed */
    if (r->eq_count < RPG_MAX_EQUIP) {
        r->eq[r->eq_count].key = ref->key;
        r->eq[r->eq_count].slot = 0xFF;
        r->eq[r->eq_count].stack = -1;
        r->eq_count++;
    }
    vm_seed(r->vm, ref);
    for (slot = 0; slot < RPG_SLOT_COUNT; ++slot) {
        best[slot] = -1;
        score[slot] = -1.0f;
    }
    while ((s = vm_item_next(r->vm, ref->key, &cursor, &item, &count)) >= 0) {
        const DRpgObject *o = rpg_object(r, item);
        int second;
        float sc;
        slot = rpg_slot_for(r, ref, o, &second);
        if (slot < 0 || o->type == DRPG_OT_LOCK || o->type == DRPG_OT_PROB || o->type == DRPG_OT_LIGH)
            continue;
        if (o->type == DRPG_OT_CREA)
            continue;
        sc = o->type == DRPG_OT_ARMO ? (float)o->stat[3] : o->type == DRPG_OT_WEAP ? weapon_score(o)
                                                                                    : (float)o->value;
        if (sc > score[slot]) {
            score[slot] = sc;
            best[slot] = s;
        }
    }
    for (slot = 0; slot < RPG_SLOT_COUNT; ++slot)
        if (best[slot] >= 0)
            set_slot(r, ref, slot, best[slot]);
    if (best[RPG_SLOT_WEAPON] >= 0 && best[RPG_SLOT_SHIELD] >= 0) {
        const VmItem *w = vm_item_at(r->vm, best[RPG_SLOT_WEAPON]);
        if (two_handed(rpg_object(r, w->item)))
            rpg_unequip_slot(r, ref, RPG_SLOT_SHIELD);
    }
    rpg_refresh_constants(r, ref);
}

/* ------------------------------------------------------------------------ */
/* bound items, disintegration                                              */
/* ------------------------------------------------------------------------ */
static uint32_t bound_item(Rpg *r, int effect, int index)
{
    static const char *const gmsts[12] = { "sMagicBoundDaggerID", "sMagicBoundLongswordID",
                                           "sMagicBoundMaceID", "sMagicBoundBattleAxeID",
                                           "sMagicBoundSpearID", "sMagicBoundLongbowID", "",
                                           "sMagicBoundCuirassID", "sMagicBoundHelmID",
                                           "sMagicBoundBootsID", "sMagicBoundShieldID",
                                           "sMagicBoundLeftGauntletID" };
    static const char *const ids[12] = { "bound_dagger", "bound_longsword", "bound_mace",
                                         "bound_battle_axe", "bound_spear", "bound_longbow", "",
                                         "bound_cuirass", "bound_helm", "bound_boots", "bound_shield",
                                         "bound_gauntlet_left" };
    int k = effect - MGEF_BOUND_DAGGER;
    if (k < 0 || k > 11 || k == 6)
        return 0;
    if (k == 11 && index == 1)
        return pw_hash_name(rpgdb_gmst_string(r->db, "sMagicBoundRightGauntletID", "bound_gauntlet_right"));
    return pw_hash_name(rpgdb_gmst_string(r->db, gmsts[k], ids[k]));
}

void rpg_bound_start(Rpg *r, RpgFx *f)
{
    VmRef t;
    int i;
    memset(&t, 0, sizeof(t));
    t = f->key == r->vm->player_hash ? vm_player_ref(r->vm) : t;
    if (t.kind == VM_REF_NONE) {
        t.kind = VM_REF_OBJECT;
        t.key = f->key;
        t.object = f->object;
        t.vmi = -1;
        t.inst = -1;
    }
    f->applied = -1.0f;
    for (i = 0; i < (f->effect == MGEF_BOUND_GLOVES ? 2 : 1); ++i) {
        uint32_t item = bound_item(r, f->effect, i);
        const DRpgObject *o = item ? rpgdb_object(r->db, item) : NULL;
        int s, second, slot;
        if (!o)
            continue;
        /* remember what the bound item displaces (re-equipped afterwards) */
        slot = rpg_slot_for(r, &t, o, &second);
        if (i == 0 && slot >= 0)
            f->applied = (float)rpg_equipped(r, &t, slot);
        s = vm_item_add_state(r->vm, t.key, item, 1, VM_FULL, -1.0f, 0);
        if (s >= 0)
            rpg_equip(r, &t, s);
    }
}

void rpg_bound_end(Rpg *r, RpgFx *f)
{
    int i;
    for (i = 0; i < (f->effect == MGEF_BOUND_GLOVES ? 2 : 1); ++i) {
        uint32_t item = bound_item(r, f->effect, i);
        if (item)
            vm_item_add(r->vm, f->key, item, -1);
    }
    if (f->applied >= 0.0f) {
        const VmItem *prev = vm_item_at(r->vm, (int)f->applied);
        if (prev && prev->owner == f->key && prev->count > 0) {
            VmRef t;
            memset(&t, 0, sizeof(t));
            t = f->key == r->vm->player_hash ? vm_player_ref(r->vm) : t;
            if (t.kind == VM_REF_NONE) {
                t.kind = VM_REF_OBJECT;
                t.key = f->key;
                t.object = f->object;
            }
            rpg_equip(r, &t, (int)f->applied);
        }
    }
}

void rpg_disintegrate(Rpg *r, const VmRef *ref, int weapon, float amount)
{
    static const int armor[] = { RPG_SLOT_CUIRASS, RPG_SLOT_HELMET, RPG_SLOT_GREAVES, RPG_SLOT_BOOTS,
                                 RPG_SLOT_LPAULDRON, RPG_SLOT_RPAULDRON, RPG_SLOT_SHIELD };
    int slot = weapon ? RPG_SLOT_WEAPON : armor[vm_random(r->vm) % 7u];
    int st = rpg_equipped(r, ref, slot);
    const VmItem *it = st >= 0 ? vm_item_at(r->vm, st) : NULL;
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    int mx = rpg_item_health_max(r, o), h;
    if (!it || mx <= 0)
        return;
    h = (it->health == VM_FULL ? mx : it->health) - (int)ceilf(amount);
    st = rpg_set_condition(r, st, h);
    if (h <= 0)
        rpg_unequip_slot(r, ref, slot);
}

/* ------------------------------------------------------------------------ */
/* spells                                                                   */
/* ------------------------------------------------------------------------ */
static float effect_cost(Rpg *r, const DRpgEffect *e, int player_spell)
{
    const DRpgMgef *m = rpgdb_mgef(r->db, e->effect);
    int has_mag, has_dur, once, mn, mx, dur, min_area = 0, dur_off = 0;
    float x;
    if (!m)
        return 0.0f;
    has_mag = !(m->flags & DRPG_MF_NO_MAGNITUDE);
    has_dur = !(m->flags & DRPG_MF_NO_DURATION);
    once = (m->flags & DRPG_MF_APPLIED_ONCE) != 0;
    mn = has_mag ? PW_MAX(1, (int)e->mag_min) : 1;
    mx = has_mag ? PW_MAX(1, (int)e->mag_max) : 1;
    dur = has_dur ? e->duration : 1;
    if (!once)
        dur = PW_MAX(1, dur);
    if (player_spell) {
        dur_off = 1;
        min_area = 1;
    }
    x = 0.5f * (float)(mn + mx);
    x *= 0.1f * m->base_cost;
    x *= (float)(dur_off + dur);
    x += 0.05f * (float)PW_MAX(min_area, (int)e->area) * m->base_cost;
    x *= rpg_gmst_f(r, "fEffectCostMult", 0.5f);
    if (e->range == RPG_RANGE_TARGET)
        x *= 1.5f;
    return PW_MAX(0.0f, x);
}

float rpg_effects_cost(Rpg *r, const DRpgEffect *e, int n, int player_spell)
{
    float cost = 0.0f;
    int i;
    for (i = 0; i < n; ++i)
        cost += effect_cost(r, &e[i], player_spell);
    return cost;
}

int rpg_spell_cost(Rpg *r, const DRpgSpell *s)
{
    int n;
    const DRpgEffect *e;
    if (!s)
        return 0;
    if (!(s->flags & 1))
        return s->cost;
    e = rpg_spell_effects(r, s, &n);
    return (int)roundf(rpg_effects_cost(r, e, n, 0));
}

float rpg_spell_base_chance(Rpg *r, const VmRef *caster, const DRpgSpell *s)
{
    int n, i;
    const DRpgEffect *e = rpg_spell_effects(r, s, &n);
    float y = 1e30f, lowest = 0.0f;
    for (i = 0; i < n; ++i) {
        const DRpgMgef *m = rpgdb_mgef(r->db, e[i].effect);
        float x, sk;
        if (!m)
            continue;
        x = (float)e[i].duration;
        if (!(m->flags & DRPG_MF_APPLIED_ONCE))
            x = PW_MAX(1.0f, x);
        x *= 0.1f * m->base_cost;
        x *= 0.5f * (float)(e[i].mag_min + e[i].mag_max);
        x += (float)e[i].area * 0.05f * m->base_cost;
        if (e[i].range == RPG_RANGE_TARGET)
            x *= 1.5f;
        x *= rpg_gmst_f(r, "fEffectCostMult", 0.5f);
        sk = 2.0f * rpg_skill(r, caster, school_skill(m->school));
        if (sk - x < y) {
            y = sk - x;
            lowest = sk;
        }
    }
    return lowest - (float)rpg_spell_cost(r, s) + 0.2f * rpg_attr(r, caster, ATTR_WILLPOWER) +
           0.1f * rpg_attr(r, caster, ATTR_LUCK);
}

int school_skill(int school)
{
    static const int skills[6] = { SKILL_ALTERATION, SKILL_CONJURATION, SKILL_DESTRUCTION,
                                   SKILL_ILLUSION, SKILL_MYSTICISM, SKILL_RESTORATION };
    return skills[PW_CLAMP(school, 0, 5)];
}

static int spell_school(Rpg *r, const VmRef *caster, const DRpgSpell *s)
{
    int n, i, best = -1;
    float y = 1e30f;
    const DRpgEffect *e = rpg_spell_effects(r, s, &n);
    for (i = 0; i < n; ++i) {
        const DRpgMgef *m = rpgdb_mgef(r->db, e[i].effect);
        float v;
        if (!m)
            continue;
        v = 2.0f * rpg_skill(r, caster, school_skill(m->school)) - effect_cost(r, &e[i], 0);
        if (v < y) {
            y = v;
            best = school_skill(m->school);
        }
    }
    return best;
}

static int power_used_today(Rpg *r, uint32_t key, uint32_t spell)
{
    return vm_item_count(r->vm, key ^ RPG_POWER_SALT, spell) == r->vm->days_passed + 1;
}

float rpg_spell_chance(Rpg *r, const VmRef *caster, const DRpgSpell *s)
{
    float chance;
    if (!s)
        return 0.0f;
    if (s->type == 5)
        return power_used_today(r, caster->key, s->hash) ? 0.0f : 100.0f;
    if (rpg_effect(r, caster, MGEF_SILENCE, -1) > 0.0f)
        return 0.0f;
    if (s->type != 0)
        return 100.0f;
    if (s->flags & 4)
        return 100.0f;
    chance = rpg_spell_base_chance(r, caster, s) - rpg_effect(r, caster, MGEF_SOUND, -1);
    chance *= rpg_fatigue_term(r, caster);
    return PW_MAX(0.0f, chance);
}

int rpg_select_spell(Rpg *r, uint32_t spell)
{
    VmRef pc = vm_player_ref(r->vm);
    if (!vm_item_count(r->vm, pc.key ^ VM_SPELL_SALT, spell))
        return 0;
    r->sel_spell = spell;
    r->sel_item = -1;
    return 1;
}

int rpg_select_item(Rpg *r, int stack)
{
    const VmItem *it = vm_item_at(r->vm, stack);
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    const DRpgSpell *en;
    if (!o || it->owner != r->vm->player_hash || it->count <= 0 || !o->link[0])
        return 0;
    en = rpg_spell(r, o->link[0]);
    if (!en || en->kind != 1 || (en->type != 0 && en->type != 2))
        return 0;
    r->sel_item = stack;
    r->sel_spell = 0;
    return 1;
}

static void fill_cast(RpgCast *c, const DRpgEffect *e, int n)
{
    int i;
    c->effects = e;
    c->count = n;
    c->area = 0.0f;
    c->has[0] = c->has[1] = c->has[2] = 0;
    for (i = 0; i < n; ++i) {
        c->has[e[i].range % 3] = 1;
        if (e[i].range == RPG_RANGE_TARGET && (float)e[i].area > c->area)
            c->area = (float)e[i].area;
    }
}

int rpg_begin_cast(Rpg *r, const VmRef *caster, RpgCast *out)
{
    memset(out, 0, sizeof(*out));
    out->school = -1;
    if (rpg_effect(r, caster, MGEF_PARALYZE, -1) > 0.0f) {
        snprintf(out->message, sizeof(out->message), "You are paralyzed.");
        return 0;
    }
    if (r->sel_item >= 0) {
        VmItem *it = vm_item_at(r->vm, r->sel_item);
        const DRpgObject *o = it && it->count > 0 && it->owner == caster->key ? rpg_object(r, it->item) : NULL;
        const DRpgSpell *en = o ? rpg_spell(r, o->link[0]) : NULL;
        float cost, charge;
        int n;
        if (!en) {
            r->sel_item = -1;
            snprintf(out->message, sizeof(out->message), "Nothing is selected.");
            return 0;
        }
        {
            const DRpgEffect *ee = rpg_spell_effects(r, en, &n);
            cost = (float)((en->flags & 1) ? (int)roundf(rpg_effects_cost(r, ee, n, 0)) : en->cost);
        }
        cost = PW_MAX(1.0f, cost - cost / 100.0f * (rpg_skill(r, caster, SKILL_ENCHANT) - 10.0f));
        if (en->type == 0) {
            /* scrolls: read once, consumed */
            {
            const DRpgEffect *ce = rpg_spell_effects(r, en, &n);
            fill_cast(out, ce, n);
        }
            out->source = en->hash;
            out->chance = 100.0f;
            out->success = 1;
            vm_item_add(r->vm, caster->key, it->item, -1);
            if (vm_item_count(r->vm, caster->key, o->hash) == 0)
                r->sel_item = -1;
            return 1;
        }
        charge = stack_charge(r, it);
        if (charge < cost) {
            snprintf(out->message, sizeof(out->message), "%s",
                     rpgdb_gmst_string(r->db, "sMagicInsufficientCharge", "This item does not have enough charge."));
            return 0;
        }
        rpg_set_charge(r, r->sel_item, charge - cost);
        if (rpg_is_player(r, caster))
            rpg_skill_use(r, SKILL_ENCHANT, 1, 1.0f);
        {
            const DRpgEffect *ce = rpg_spell_effects(r, en, &n);
            fill_cast(out, ce, n);
        }
        out->source = en->hash;
        out->chance = 100.0f;
        out->success = 1;
        return 1;
    }
    {
        const DRpgSpell *s = r->sel_spell ? rpg_spell(r, r->sel_spell) : NULL;
        int n, cost;
        float chance;
        if (!s || !vm_item_count(r->vm, caster->key ^ VM_SPELL_SALT, s->hash)) {
            snprintf(out->message, sizeof(out->message), "No spell is selected.");
            return 0;
        }
        cost = s->type == 0 ? rpg_spell_cost(r, s) : 0;
        if (s->type == 5 && power_used_today(r, caster->key, s->hash)) {
            snprintf(out->message, sizeof(out->message), "%s",
                     rpgdb_gmst_string(r->db, "sPowerAlreadyUsed", "You have already used that power today."));
            return 0;
        }
        if ((float)cost > rpg_dyn(r, caster, 1)) {
            snprintf(out->message, sizeof(out->message), "%s",
                     rpgdb_gmst_string(r->db, "sMagicInsufficientSP", "You do not have enough Magicka."));
            return 0;
        }
        chance = rpg_spell_chance(r, caster, s);
        rpg_dyn_add(r, caster, 1, -(float)cost);
        out->school = spell_school(r, caster, s);
        out->chance = s->type == 0 ? rpg_spell_base_chance(r, caster, s) : 100.0f;
        if (s->type == 5) {
            vm_item_add(r->vm, caster->key ^ RPG_POWER_SALT, s->hash,
                        r->vm->days_passed + 1 - vm_item_count(r->vm, caster->key ^ RPG_POWER_SALT, s->hash));
        }
        if ((float)rpg_roll100(r) >= chance) {
            snprintf(out->message, sizeof(out->message), "%s",
                     rpgdb_gmst_string(r->db, "sMagicSkillFail", "You failed casting the spell."));
            return 0;
        }
        if (rpg_is_player(r, caster) && out->school >= 0 && s->type == 0)
            rpg_skill_use(r, out->school, 0, 1.0f);
        {
            const DRpgEffect *ce = rpg_spell_effects(r, s, &n);
            fill_cast(out, ce, n);
        }
        out->source = s->hash;
        out->success = 1;
        return 1;
    }
}

/* ------------------------------------------------------------------------ */
/* item use                                                                 */
/* ------------------------------------------------------------------------ */
int rpg_ingredient_known_effects(Rpg *r)
{
    VmRef pc = vm_player_ref(r->vm);
    float wort = rpg_gmst_f(r, "fWortChanceValue", 15.0f);
    return wort > 0.0f ? (int)(rpg_skill(r, &pc, SKILL_ALCHEMY) / wort) : 4;
}

static int eat_ingredient(Rpg *r, const DRpgObject *o, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    int n;
    const DRpgEffect *e = rpg_object_effects(r, o, &n);
    const DRpgMgef *m;
    float x, y;
    int roll;
    DRpgEffect eff;
    rpg_skill_use(r, SKILL_ALCHEMY, 1, 1.0f);
    if (n <= 0)
        return 0;
    m = rpgdb_mgef(r->db, e[0].effect);
    x = (rpg_skill(r, &pc, SKILL_ALCHEMY) + 0.2f * rpg_attr(r, &pc, ATTR_INTELLIGENCE) +
         0.1f * rpg_attr(r, &pc, ATTR_LUCK)) * rpg_fatigue_term(r, &pc);
    roll = rpg_roll100(r);
    if ((float)roll > x || !m) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sNotifyMessage45", "That tastes awful."));
        return 0;
    }
    y = (float)roll / PW_MIN(x, 100.0f) * 0.25f * x;
    eff = e[0];
    eff.range = RPG_RANGE_SELF;
    eff.mag_min = eff.mag_max = (uint16_t)((m->flags & DRPG_MF_NO_MAGNITUDE) ? 1 : PW_MAX(1, (int)y));
    eff.duration = (uint16_t)((m->flags & DRPG_MF_NO_DURATION) ? 1 : PW_MAX(1, (int)y));
    rpg_apply(r, &pc, &pc, &eff, 1, RPG_RANGE_SELF, o->hash, 100.0f, 1);
    return 1;
}

int rpg_read_book(Rpg *r, uint32_t book)
{
    const DRpgObject *o = rpg_object(r, book);
    if (!o || o->type != DRPG_OT_BOOK || o->stat[0] < 0 || o->stat[0] >= SKILL_COUNT)
        return -1;
    if (vm_prop_key(r->vm, book, 0, MWP_BOOKREAD) != 0.0f)
        return -1;
    vm_set_prop_key(r->vm, book, MWP_BOOKREAD, 1.0f);
    rpg_skill_raise(r, o->stat[0], 0);
    return o->stat[0];
}

int rpg_use(Rpg *r, int stack, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    VmItem *it = vm_item_at(r->vm, stack);
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    int second;
    msg[0] = '\0';
    if (!o || it->owner != pc.key || it->count <= 0)
        return RPG_USE_NONE;
    switch (o->type) {
    case DRPG_OT_ALCH: {
        int n;
        const DRpgEffect *e = rpg_object_effects(r, o, &n);
        uint32_t item = it->item;
        vm_item_add(r->vm, pc.key, item, -1);
        rpg_apply(r, &pc, &pc, e, n, RPG_RANGE_SELF, item, 100.0f, 1);
        return RPG_USE_DRINK;
    }
    case DRPG_OT_INGR: {
        uint32_t item = it->item;
        vm_item_add(r->vm, pc.key, item, -1);
        eat_ingredient(r, o, msg, cap);
        return RPG_USE_EAT;
    }
    case DRPG_OT_BOOK:
        return RPG_USE_BOOK;
    case DRPG_OT_APPA:
        return RPG_USE_ALCHEMY;
    case DRPG_OT_REPA:
        return RPG_USE_REPAIR;
    default:
        break;
    }
    if (o->link[0] && o->type != DRPG_OT_LIGH) {
        const DRpgSpell *en = rpg_spell(r, o->link[0]);
        if (en && en->kind == 1 && en->type == 2 && rpg_slot_for(r, &pc, o, &second) < 0) {
            rpg_select_item(r, stack);
            snprintf(msg, cap, "%s selected.", rpg_name(r, o));
            return RPG_USE_CAST;
        }
    }
    if (rpg_slot_for(r, &pc, o, &second) == -1)
        return RPG_USE_NONE;
    if (rpg_is_equipped(r, &pc, stack)) {
        int slot;
        for (slot = 0; slot < RPG_SLOT_COUNT; ++slot)
            if (rpg_equipped(r, &pc, slot) == stack)
                rpg_unequip_slot(r, &pc, slot);
        return RPG_USE_UNEQUIP;
    }
    if (rpg_equip(r, &pc, stack) > 0) {
        const DRpgSpell *en = o->link[0] ? rpg_spell(r, o->link[0]) : NULL;
        if (en && en->kind == 1 && en->type == 2)
            rpg_select_item(r, stack);
        return RPG_USE_EQUIP;
    }
    return RPG_USE_FAILED;
}

/* ------------------------------------------------------------------------ */
/* combat                                                                   */
/* ------------------------------------------------------------------------ */
static const DRpgObject *weapon_of(Rpg *r, const VmRef *ref, int *stack)
{
    int st;
    const VmItem *it;
    const DRpgObject *o;
    rpg_autoequip(r, ref);
    st = rpg_equipped(r, ref, RPG_SLOT_WEAPON);
    it = st >= 0 ? vm_item_at(r->vm, st) : NULL;
    o = it ? rpg_object(r, it->item) : NULL;
    if (stack)
        *stack = st;
    return (o && o->type == DRPG_OT_WEAP) ? o : NULL;
}

static int weapon_type_skill(int type)
{
    static const int skills[14] = { SKILL_SHORTBLADE, SKILL_LONGBLADE, SKILL_LONGBLADE,
                                    SKILL_BLUNTWEAPON, SKILL_BLUNTWEAPON, SKILL_BLUNTWEAPON, SKILL_SPEAR,
                                    SKILL_AXE, SKILL_AXE, SKILL_MARKSMAN, SKILL_MARKSMAN,
                                    SKILL_MARKSMAN, SKILL_MARKSMAN, SKILL_MARKSMAN };
    return skills[PW_CLAMP(type, 0, 13)];
}

int rpg_weapon_skill(Rpg *r, const VmRef *ref)
{
    const DRpgObject *w = weapon_of(r, ref, NULL);
    return w ? weapon_type_skill(w->stat[0]) : SKILL_HANDTOHAND;
}

int rpg_weapon_ranged(Rpg *r, const VmRef *ref)
{
    const DRpgObject *w = weapon_of(r, ref, NULL);
    return w && w->stat[0] >= 9 && w->stat[0] <= 11;
}

float rpg_weapon_reach(Rpg *r, const VmRef *ref)
{
    const DRpgObject *w = weapon_of(r, ref, NULL);
    float reach = 1.0f;
    if (w) {
        memcpy(&reach, &w->stat[3], 4);
        if (reach <= 0.0f)
            reach = 1.0f;
    } else {
        reach = rpg_gmst_f(r, "fHandToHandReach", 1.0f);
    }
    return reach * rpg_gmst_f(r, "fCombatDistance", 128.0f);
}

int rpg_consume_ammo(Rpg *r, const VmRef *ref)
{
    int st;
    const DRpgObject *w = weapon_of(r, ref, &st);
    if (!w)
        return 0;
    if (w->stat[0] == 11) {
        const VmItem *it = vm_item_at(r->vm, st);
        vm_item_add_state(r->vm, ref->key, it->item, -1, it->health, it->charge, it->soul);
        return 1;
    }
    if (w->stat[0] == 9 || w->stat[0] == 10) {
        int ammo = rpg_equipped(r, ref, RPG_SLOT_AMMO);
        const VmItem *a = ammo >= 0 ? vm_item_at(r->vm, ammo) : NULL;
        const DRpgObject *ao = a ? rpg_object(r, a->item) : NULL;
        if (!ao || ao->stat[0] != (w->stat[0] == 9 ? 12 : 13))
            return 0;
        vm_item_add_state(r->vm, ref->key, a->item, -1, a->health, a->charge, a->soul);
        return 1;
    }
    return 1;
}

static float hit_chance(Rpg *r, const VmRef *att, const VmRef *vic, float skill)
{
    float defense = 0.0f, attack;
    float invis = rpg_gmst_f(r, "fCombatInvisoMult", 0.2f);
    if (rpg_dyn(r, vic, 2) >= 0.0f) {
        if (rpg_effect(r, vic, MGEF_PARALYZE, -1) <= 0.0f)
            defense = (rpg_attr(r, vic, ATTR_AGILITY) / 5.0f + rpg_attr(r, vic, ATTR_LUCK) / 10.0f) *
                          rpg_fatigue_term(r, vic) +
                      PW_MIN(100.0f, rpg_effect(r, vic, MGEF_SANCTUARY, -1));
        defense += PW_MIN(100.0f, invis * rpg_effect(r, vic, MGEF_CHAMELEON, -1));
        defense += PW_MIN(100.0f, invis * rpg_effect(r, vic, MGEF_INVISIBILITY, -1));
    }
    attack = (skill + rpg_attr(r, att, ATTR_AGILITY) / 5.0f + rpg_attr(r, att, ATTR_LUCK) / 10.0f) *
             rpg_fatigue_term(r, att);
    attack += rpg_effect(r, att, MGEF_FORTIFY_ATTACK, -1) - rpg_effect(r, att, MGEF_BLIND, -1);
    return roundf(attack - defense);
}

static float attacker_skill(Rpg *r, const VmRef *ref, int skill)
{
    const DRpgObject *o = rpg_actor_object(r, ref);
    if (o && o->type == DRPG_OT_CREA && !rpg_is_player(r, ref)) {
        const DRpgActor *a = rpgdb_actor(r->db, o);
        return a ? (float)a->skills[0] : 0.0f; /* combat skill */
    }
    return rpg_skill(r, ref, skill);
}

static int try_block(Rpg *r, const VmRef *att, const VmRef *vic, float att_skill, float strength, float damage)
{
    int sh = rpg_equipped(r, vic, RPG_SLOT_SHIELD);
    const VmItem *it = sh >= 0 ? vm_item_at(r->vm, sh) : NULL;
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    float block_term, att_term;
    int x, mx, h;
    if (!o || o->type != DRPG_OT_ARMO || rpg_effect(r, vic, MGEF_PARALYZE, -1) > 0.0f)
        return 0;
    if (!rpg_is_player(r, vic) && vm_prop(r->vm, vic, MWP_HOSTILE) == 0.0f)
        return 0; /* not ready to block */
    if (rpg_is_player(r, vic) && r->draw_state != 1)
        return 0;
    block_term = rpg_skill(r, vic, SKILL_BLOCK) + 0.2f * rpg_attr(r, vic, ATTR_AGILITY) +
                 0.1f * rpg_attr(r, vic, ATTR_LUCK);
    block_term *= strength * rpg_gmst_f(r, "fSwingBlockMult", 1.0f) + rpg_gmst_f(r, "fSwingBlockBase", 1.0f);
    block_term *= rpg_gmst_f(r, "fBlockStillBonus", 1.25f);
    block_term *= rpg_fatigue_term(r, vic);
    att_term = (att_skill + 0.2f * rpg_attr(r, att, ATTR_AGILITY) + 0.1f * rpg_attr(r, att, ATTR_LUCK)) *
               rpg_fatigue_term(r, att);
    x = PW_CLAMP((int)(block_term - att_term), rpg_gmst_i(r, "iBlockMinChance", 10),
                 rpg_gmst_i(r, "iBlockMaxChance", 50));
    if (rpg_roll100(r) >= x)
        return 0;
    mx = rpg_item_health_max(r, o);
    h = (it->health == VM_FULL ? mx : it->health) - (int)damage;
    rpg_set_condition(r, sh, h);
    if (h <= 0)
        rpg_unequip_slot(r, vic, RPG_SLOT_SHIELD);
    rpg_dyn_add(r, vic, 2, -(rpg_gmst_f(r, "fFatigueBlockBase", 4.0f) +
                             PW_MIN(1.0f, rpg_encumbrance(r, vic) / PW_MAX(1.0f, rpg_capacity(r, vic))) *
                                 rpg_gmst_f(r, "fFatigueBlockMult", 0.0f)));
    if (rpg_is_player(r, vic))
        rpg_skill_use(r, SKILL_BLOCK, 0, 1.0f);
    return 1;
}

static void armor_hit(Rpg *r, const VmRef *vic, float damage)
{
    /* the piece that was hit, weighted like the original slot table */
    static const int slots[10] = { RPG_SLOT_CUIRASS, RPG_SLOT_CUIRASS, RPG_SLOT_CUIRASS, RPG_SLOT_HELMET,
                                   RPG_SLOT_GREAVES, RPG_SLOT_BOOTS, RPG_SLOT_LPAULDRON, RPG_SLOT_RPAULDRON,
                                   RPG_SLOT_LGAUNTLET, RPG_SLOT_RGAUNTLET };
    int slot = slots[vm_random(r->vm) % 10u];
    int st = rpg_equipped(r, vic, slot);
    const VmItem *it = st >= 0 ? vm_item_at(r->vm, st) : NULL;
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    if (o && o->type == DRPG_OT_ARMO) {
        int mx = rpg_item_health_max(r, o), h = (it->health == VM_FULL ? mx : it->health) - (int)damage;
        rpg_set_condition(r, st, h);
        if (h <= 0)
            rpg_unequip_slot(r, vic, slot);
        if (rpg_is_player(r, vic))
            rpg_skill_use(r, armor_skill_of(r, o), 0, 1.0f);
    } else if (rpg_is_player(r, vic)) {
        rpg_skill_use(r, SKILL_UNARMORED, 0, 1.0f);
    }
}

int armor_skill_of(Rpg *r, const DRpgObject *o)
{
    static const char *const weights[11] = { "iHelmWeight", "iCuirassWeight", "iPauldronWeight",
                                             "iPauldronWeight", "iGreavesWeight", "iBootsWeight",
                                             "iGauntletWeight", "iGauntletWeight", "iShieldWeight",
                                             "iGauntletWeight", "iGauntletWeight" };
    static const int defaults[11] = { 5, 30, 10, 10, 15, 20, 5, 5, 15, 5, 5 };
    int t = PW_CLAMP(o->stat[0], 0, 10);
    float base = (float)rpg_gmst_i(r, weights[t], defaults[t]);
    if (o->weight <= base * rpg_gmst_f(r, "fLightMaxMod", 0.6f))
        return SKILL_LIGHTARMOR;
    if (o->weight <= base * rpg_gmst_f(r, "fMedMaxMod", 0.9f))
        return SKILL_MEDIUMARMOR;
    return SKILL_HEAVYARMOR;
}

static float weapon_damage(const DRpgObject *w, float strength, const DRpgObject *ammo)
{
    float best = 0.0f;
    int k;
    if (w->stat[0] >= 9 && w->stat[0] <= 11) {
        float lo = (float)(w->link[1] & 0xFF), hi = (float)((w->link[1] >> 8) & 0xFF);
        float d = lo + (hi - lo) * strength;
        if (ammo)
            d += (float)(ammo->link[1] & 0xFF) +
                 ((float)((ammo->link[1] >> 8) & 0xFF) - (float)(ammo->link[1] & 0xFF)) * strength;
        return d;
    }
    /* chop, slash, thrust: the original's "always use best attack" */
    for (k = 0; k < 3; ++k) {
        uint32_t packed = k < 2 ? w->link[1] : w->link[2];
        int shift = k == 1 ? 16 : 0;
        float lo = (float)((packed >> shift) & 0xFF), hi = (float)((packed >> (shift + 8)) & 0xFF);
        float d = lo + (hi - lo) * strength;
        if (d > best)
            best = d;
    }
    return best;
}

RpgHit rpg_attack(Rpg *r, const VmRef *att, const VmRef *vic, float strength)
{
    RpgHit hit;
    int wstack, skill;
    const DRpgObject *w;
    const DRpgObject *ao = rpg_actor_object(r, att);
    float skill_value, damage = 0.0f, chance;
    int health_damage = 1;
    memset(&hit, 0, sizeof(hit));
    strength = PW_CLAMP(strength, 0.0f, 1.0f);
    if (rpg_is_dead(r, att) || rpg_is_dead(r, vic))
        return hit;
    w = weapon_of(r, att, &wstack);
    skill = w ? weapon_type_skill(w->stat[0]) : SKILL_HANDTOHAND;
    skill_value = attacker_skill(r, att, skill);
    /* the attack costs fatigue */
    rpg_dyn_add(r, att, 2, -(rpg_gmst_f(r, "fFatigueAttackBase", 2.0f) +
                             PW_MIN(1.0f, rpg_encumbrance(r, att) / PW_MAX(1.0f, rpg_capacity(r, att))) *
                                 rpg_gmst_f(r, "fFatigueAttackMult", 0.0f) +
                             (w ? w->weight * strength * rpg_gmst_f(r, "fWeaponFatigueMult", 0.25f) : 0.0f)));
    vm_set_prop(r->vm, vic, MWP_ATTACKED, 1.0f);
    if (rpg_is_player(r, att))
        vm_set_prop(r->vm, vic, MWP_HOSTILE, 1.0f);
    chance = hit_chance(r, att, vic, skill_value);
    if ((float)rpg_roll100(r) >= chance) {
        if (w)
            rpg_set_condition(r, wstack, (vm_item_at(r->vm, wstack)->health == VM_FULL
                                              ? rpg_item_health_max(r, w)
                                              : vm_item_at(r->vm, wstack)->health) - 1);
        return hit;
    }
    hit.hit = 1;
    if (w) {
        const VmItem *wi = vm_item_at(r->vm, wstack);
        const DRpgObject *ammo = NULL;
        int mx = rpg_item_health_max(r, w), h;
        if (w->stat[0] == 9 || w->stat[0] == 10) {
            int a = rpg_equipped(r, att, RPG_SLOT_AMMO);
            const VmItem *ai = a >= 0 ? vm_item_at(r->vm, a) : NULL;
            ammo = ai ? rpg_object(r, ai->item) : NULL;
        }
        damage = weapon_damage(w, strength, ammo);
        if (mx > 0 && wi->health != VM_FULL)
            damage *= (float)wi->health / (float)mx;
        damage *= rpg_gmst_f(r, "fDamageStrengthBase", 0.5f) +
                  rpg_attr(r, att, ATTR_STRENGTH) * rpg_gmst_f(r, "fDamageStrengthMult", 0.1f) * 0.1f;
        h = (wi->health == VM_FULL ? mx : wi->health) -
            (int)PW_MAX(1.0f, rpg_gmst_f(r, "fWeaponDamageMult", 0.1f) * damage);
        wstack = rpg_set_condition(r, wstack, h);
        if (h <= 0)
            rpg_unequip_slot(r, att, RPG_SLOT_WEAPON);
        /* normal weapons against resistant targets */
        if (!w->link[0] && !(w->stat[5] & 0x3)) {
            float res = rpg_effect(r, vic, MGEF_RESIST_NORMAL_WEAPONS, -1) -
                        rpg_effect(r, vic, MGEF_WEAKNESS_TO_NORMAL_WEAPONS, -1);
            damage *= 1.0f - PW_MIN(res, 100.0f) / 100.0f;
        }
    } else if (ao && ao->type == DRPG_OT_CREA && !rpg_is_player(r, att)) {
        const DRpgActor *a = rpgdb_actor(r->db, ao);
        if (a)
            damage = (float)a->attack[0] + ((float)a->attack[1] - (float)a->attack[0]) * strength;
    } else {
        float mn = rpg_gmst_f(r, "fMinHandToHandMult", 0.1f), mx = rpg_gmst_f(r, "fMaxHandToHandMult", 0.5f);
        damage = skill_value * (mn + (mx - mn) * strength);
        health_damage = rpg_effect(r, vic, MGEF_PARALYZE, -1) > 0.0f || rpg_dyn(r, vic, 2) < 0.0f;
        if (health_damage)
            damage *= rpg_gmst_f(r, "fHandtoHandHealthPer", 0.1f);
    }
    if (rpg_is_player(r, att))
        rpg_skill_use(r, skill, 0, 1.0f);
    /* sneak attacks on unaware targets */
    if (rpg_is_player(r, att) && r->sneaking && vm_prop(r->vm, vic, MWP_HOSTILE) == 0.0f) {
        damage *= rpg_gmst_f(r, "fCombatCriticalStrikeMult", 4.0f);
        hit.critical = 1;
        rpg_notify(r, "sTargetCriticalStrike", "Critical strike!");
    }
    if (try_block(r, att, vic, skill_value, strength, damage)) {
        hit.blocked = 1;
        damage = 0.0f;
    }
    if (damage > 0.0f && health_damage) {
        float ar = rpg_armor_rating(r, vic);
        armor_hit(r, vic, damage);
        damage *= PW_MAX(rpg_gmst_f(r, "fCombatArmorMinMult", 0.25f), damage / (damage + PW_MAX(ar, 0.0f)));
    }
    hit.damage = damage;
    if (health_damage)
        rpg_damage(r, vic, 0, damage, att);
    else
        rpg_dyn_add(r, vic, 2, -damage);
    /* cast-when-strikes enchantments */
    if (w && w->link[0] && !rpg_is_dead(r, vic)) {
        const DRpgSpell *en = rpg_spell(r, w->link[0]);
        VmItem *wi = wstack >= 0 ? vm_item_at(r->vm, wstack) : NULL;
        if (en && en->kind == 1 && en->type == 1 && wi && wi->count > 0) {
            int n;
            const DRpgEffect *e = rpg_spell_effects(r, en, &n);
            float cost = (float)((en->flags & 1) ? (int)roundf(rpg_effects_cost(r, e, n, 0)) : en->cost);
            float charge = stack_charge(r, wi);
            cost = PW_MAX(1.0f, cost - cost / 100.0f * (rpg_skill(r, att, SKILL_ENCHANT) - 10.0f));
            if (charge >= cost) {
                rpg_set_charge(r, wstack, charge - cost);
                rpg_apply(r, att, vic, e, n, RPG_RANGE_TOUCH, en->hash, 100.0f, 1);
            }
        }
    }
    hit.killed = rpg_is_dead(r, vic);
    return hit;
}

int rpg_pick_lock(Rpg *r, const VmRef *target, int lock_level, int *new_level, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    int st = rpg_equipped(r, &pc, RPG_SLOT_WEAPON);
    const VmItem *it = st >= 0 ? vm_item_at(r->vm, st) : NULL;
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    float quality, x;
    int uses;
    PW_UNUSED(target);
    *new_level = lock_level;
    if (!o || o->type != DRPG_OT_LOCK) {
        snprintf(msg, cap, "Equip a lockpick first.");
        return 0;
    }
    if (lock_level <= 0) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sLockImpossible", "This lock is not locked."));
        return 0;
    }
    memcpy(&quality, &o->stat[1], 4);
    x = (0.2f * rpg_attr(r, &pc, ATTR_AGILITY) + 0.1f * rpg_attr(r, &pc, ATTR_LUCK) +
         rpg_skill(r, &pc, SKILL_SECURITY)) * quality * rpg_fatigue_term(r, &pc);
    x += rpg_gmst_f(r, "fPickLockMult", -1.0f) * (float)lock_level;
    uses = (it->health == VM_FULL ? o->stat[0] : it->health) - 1;
    st = rpg_set_condition(r, st, uses);
    if (uses <= 0) {
        rpg_unequip_slot(r, &pc, RPG_SLOT_WEAPON);
        vm_item_add_state(r->vm, pc.key, o->hash, -1, 0, vm_item_at(r->vm, st)->charge, 0);
    }
    if (x <= 0.0f) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sLockImpossible", "This lock is too complex."));
        return 0;
    }
    if ((float)rpg_roll100(r) < x) {
        *new_level = 0;
        rpg_skill_use(r, SKILL_SECURITY, 0, 1.0f);
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sLockSuccess", "You have picked the lock."));
        return 1;
    }
    snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sLockFail", "You failed to pick the lock."));
    return 0;
}
