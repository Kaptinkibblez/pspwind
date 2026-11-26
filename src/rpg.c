/*
 * rpg.c - stats, progression, magic effects, equipment, items, casting,
 * combat, rest, character creation (see rpg.h). Trade, services and
 * crafting live in rpg_trade.c.
 */
#include "rpg.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rpg_internal.h"

static const char *const attr_names[8] = { "Strength", "Intelligence", "Willpower", "Agility",
                                            "Speed", "Endurance", "Personality", "Luck" };
static const char *const attr_gmst[8] = { "sAttributeStrength", "sAttributeIntelligence",
                                          "sAttributeWillpower", "sAttributeAgility",
                                          "sAttributeSpeed", "sAttributeEndurance",
                                          "sAttributePersonality", "sAttributeLuck" };

/* ------------------------------------------------------------------------ */
/* small helpers                                                            */
/* ------------------------------------------------------------------------ */
float rpg_gmst_f(const Rpg *r, const char *id, float fallback)
{
    return rpgdb_gmst_float(r->db, id, fallback);
}

int rpg_gmst_i(const Rpg *r, const char *id, int fallback)
{
    return (int)rpgdb_gmst_int(r->db, id, fallback);
}

void rpg_notify(Rpg *r, const char *gmst, const char *fallback)
{
    vm_notify(r->vm, rpgdb_gmst_string(r->db, gmst, fallback));
}

int rpg_is_player(const Rpg *r, const VmRef *ref)
{
    return ref && (ref->kind == VM_REF_PLAYER || ref->key == r->vm->player_hash);
}

int rpg_roll100(Rpg *r)
{
    return (int)(vm_random(r->vm) % 100u);
}

const DRpgObject *rpg_actor_object(const Rpg *r, const VmRef *ref)
{
    if (rpg_is_player(r, ref)) {
        if (r->db->hdr->player != DRPG_NONE)
            return &r->db->objects[r->db->hdr->player];
        return NULL;
    }
    return ref ? rpgdb_object(r->db, ref->object) : NULL;
}

/* ------------------------------------------------------------------------ */
/* lookups                                                                  */
/* ------------------------------------------------------------------------ */
const DRpgObject *rpg_object(const Rpg *r, uint32_t hash)
{
    uint32_t i;
    const DRpgObject *o = rpgdb_object(r->db, hash);
    if (o)
        return o;
    for (i = 0; i < r->custom_count; ++i)
        if (r->custom[i].used && r->custom[i].kind == 0 && r->custom[i].obj.hash == hash)
            return &r->custom[i].obj;
    return NULL;
}

const DRpgSpell *rpg_spell(const Rpg *r, uint32_t hash)
{
    uint32_t i;
    const DRpgSpell *s = rpgdb_spell(r->db, hash);
    if (s)
        return s;
    for (i = 0; i < r->custom_count; ++i)
        if (r->custom[i].used && r->custom[i].kind != 0 && r->custom[i].spell.hash == hash)
            return &r->custom[i].spell;
    return NULL;
}

static const RpgCustom *custom_of_object(const Rpg *r, const DRpgObject *o)
{
    const RpgCustom *c = (const RpgCustom *)((const uint8_t *)o - offsetof(RpgCustom, obj));
    if ((const uint8_t *)o < (const uint8_t *)r->custom ||
        (const uint8_t *)o >= (const uint8_t *)(r->custom + RPG_MAX_CUSTOM))
        return NULL;
    return c;
}

static const RpgCustom *custom_of_spell(const Rpg *r, const DRpgSpell *s)
{
    const RpgCustom *c = (const RpgCustom *)((const uint8_t *)s - offsetof(RpgCustom, spell));
    if ((const uint8_t *)s < (const uint8_t *)r->custom ||
        (const uint8_t *)s >= (const uint8_t *)(r->custom + RPG_MAX_CUSTOM))
        return NULL;
    return c;
}

const char *rpg_name(const Rpg *r, const DRpgObject *o)
{
    const RpgCustom *c;
    if (!o)
        return "";
    c = custom_of_object(r, o);
    if (c)
        return c->name;
    return rpgdb_name(r->db, o->name);
}

const char *rpg_spell_name(const Rpg *r, uint32_t hash)
{
    const DRpgSpell *s = rpg_spell(r, hash);
    const RpgCustom *c;
    if (!s)
        return "";
    c = custom_of_spell(r, s);
    return c ? c->name : rpgdb_name(r->db, s->name);
}

const DRpgEffect *rpg_object_effects(const Rpg *r, const DRpgObject *o, int *count)
{
    const RpgCustom *c;
    *count = 0;
    if (!o)
        return NULL;
    c = custom_of_object(r, o);
    if (c) {
        *count = o->list2_count;
        return c->effects;
    }
    if (o->type != DRPG_OT_ALCH && o->type != DRPG_OT_INGR)
        return NULL;
    *count = o->list2_count;
    return &r->db->effects[o->list2_first];
}

const DRpgEffect *rpg_spell_effects(const Rpg *r, const DRpgSpell *s, int *count)
{
    const RpgCustom *c;
    *count = 0;
    if (!s)
        return NULL;
    c = custom_of_spell(r, s);
    *count = s->effect_count;
    if (c)
        return c->effects;
    return &r->db->effects[s->effect_first];
}

const char *rpg_skill_name(const Rpg *r, int skill)
{
    if (skill < 0 || skill >= SKILL_COUNT)
        return "";
    return rpgdb_name(r->db, r->db->skills[skill].name);
}

const char *rpg_attr_name(const Rpg *r, int attr)
{
    if (attr < 0 || attr >= ATTR_COUNT)
        return "";
    return rpgdb_gmst_string(r->db, attr_gmst[attr], attr_names[attr]);
}

const char *rpg_effect_name(const Rpg *r, int effect)
{
    const DRpgMgef *m = rpgdb_mgef(r->db, effect);
    return m ? rpgdb_name(r->db, m->name) : "";
}

void rpg_describe_effect(const Rpg *r, const DRpgEffect *e, char *out, uint32_t cap)
{
    const DRpgMgef *m = rpgdb_mgef(r->db, e->effect);
    char name[64], arg[32] = "", mag[32] = "", dur[32] = "", area[24] = "";
    static const char *const ranges[3] = { "Self", "Touch", "Target" };
    if (!m) {
        snprintf(out, cap, "?");
        return;
    }
    if ((m->flags & DRPG_MF_TARGET_SKILL) && e->skill >= 0)
        snprintf(arg, sizeof(arg), " %s", rpg_skill_name(r, e->skill));
    else if ((m->flags & DRPG_MF_TARGET_ATTRIBUTE) && e->attribute >= 0)
        snprintf(arg, sizeof(arg), " %s", rpg_attr_name(r, e->attribute));
    snprintf(name, sizeof(name), "%s", rpgdb_name(r->db, m->name));
    /* "Fortify Attribute Strength" reads "Fortify Strength" */
    if (arg[0]) {
        char *w = strstr(name, " Attribute");
        if (!w)
            w = strstr(name, " Skill");
        if (w)
            *w = '\0';
    }
    if (!(m->flags & DRPG_MF_NO_MAGNITUDE)) {
        if (e->mag_min == e->mag_max)
            snprintf(mag, sizeof(mag), " %u pts", (unsigned)e->mag_min);
        else
            snprintf(mag, sizeof(mag), " %u to %u pts", (unsigned)e->mag_min, (unsigned)e->mag_max);
    }
    if (!(m->flags & DRPG_MF_NO_DURATION) && e->duration > 1)
        snprintf(dur, sizeof(dur), " for %u secs", (unsigned)e->duration);
    if (e->area > 0)
        snprintf(area, sizeof(area), " in %u ft", (unsigned)e->area);
    snprintf(out, cap, "%s%s%s%s%s on %s", name, arg, mag, dur, area, ranges[e->range % 3]);
}

/* ------------------------------------------------------------------------ */
/* active effects                                                           */
/* ------------------------------------------------------------------------ */
float rpg_effect(Rpg *r, const VmRef *ref, int effect, int arg)
{
    uint32_t i;
    float sum = 0.0f;
    if (!ref || ref->kind == VM_REF_NONE)
        return 0.0f;
    for (i = 0; i < r->fx_count; ++i) {
        const RpgFx *f = &r->fx[i];
        if (f->key == ref->key && f->effect == effect && (arg < 0 || f->arg == arg))
            sum += f->magnitude;
    }
    return sum;
}

/* ------------------------------------------------------------------------ */
/* stats                                                                    */
/* ------------------------------------------------------------------------ */
float rpg_attr(Rpg *r, const VmRef *ref, int a)
{
    float v;
    if (a < 0 || a >= ATTR_COUNT)
        return 0.0f;
    v = vm_prop(r->vm, ref, MWP_ATTR_STRENGTH + a) - vm_prop(r->vm, ref, MWP_ATTRDMG_STRENGTH + a);
    v += rpg_effect(r, ref, MGEF_FORTIFY_ATTRIBUTE, a);
    v -= rpg_effect(r, ref, MGEF_DRAIN_ATTRIBUTE, a);
    v -= rpg_effect(r, ref, MGEF_ABSORB_ATTRIBUTE, a);
    return v < 0.0f ? 0.0f : v;
}

float rpg_skill(Rpg *r, const VmRef *ref, int s)
{
    float v;
    if (s < 0 || s >= SKILL_COUNT)
        return 0.0f;
    v = vm_prop(r->vm, ref, MWP_SKILL_BLOCK + s) - vm_prop(r->vm, ref, MWP_SKILLDMG_BLOCK + s);
    v += rpg_effect(r, ref, MGEF_FORTIFY_SKILL, s);
    v -= rpg_effect(r, ref, MGEF_DRAIN_SKILL, s);
    v -= rpg_effect(r, ref, MGEF_ABSORB_SKILL, s);
    return v < 0.0f ? 0.0f : v;
}

float rpg_dyn(Rpg *r, const VmRef *ref, int dyn)
{
    return vm_prop(r->vm, ref, MWP_DYN_HEALTH + dyn);
}

float rpg_dyn_max(Rpg *r, const VmRef *ref, int dyn)
{
    float base;
    int player = rpg_is_player(r, ref);
    if (dyn == 0) {
        base = vm_prop(r->vm, ref, MWP_DYNBASE_HEALTH) + rpg_effect(r, ref, MGEF_FORTIFY_HEALTH, -1);
    } else if (dyn == 1) {
        if (player)
            base = rpg_attr(r, ref, ATTR_INTELLIGENCE) *
                   (rpg_gmst_f(r, "fPCbaseMagickaMult", 1.0f) +
                    0.1f * rpg_effect(r, ref, MGEF_FORTIFY_MAXIMUM_MAGICKA, -1));
        else
            base = vm_prop(r->vm, ref, MWP_DYNBASE_MAGICKA) +
                   rpg_attr(r, ref, ATTR_INTELLIGENCE) * 0.1f * rpg_effect(r, ref, MGEF_FORTIFY_MAXIMUM_MAGICKA, -1);
        base += rpg_effect(r, ref, MGEF_FORTIFY_MAGICKA, -1);
    } else {
        if (player)
            base = rpg_attr(r, ref, ATTR_STRENGTH) + rpg_attr(r, ref, ATTR_WILLPOWER) +
                   rpg_attr(r, ref, ATTR_AGILITY) + rpg_attr(r, ref, ATTR_ENDURANCE);
        else
            base = vm_prop(r->vm, ref, MWP_DYNBASE_FATIGUE);
        base += rpg_effect(r, ref, MGEF_FORTIFY_FATIGUE, -1);
    }
    return base < 0.0f ? 0.0f : base;
}

void rpg_dyn_add(Rpg *r, const VmRef *ref, int dyn, float delta)
{
    float v = rpg_dyn(r, ref, dyn) + delta, mx = rpg_dyn_max(r, ref, dyn);
    if (v > mx)
        v = mx;
    if (dyn == 1 && v < 0.0f)
        v = 0.0f;
    if (dyn == 2 && v < -mx)
        v = -mx;
    vm_set_prop(r->vm, ref, MWP_DYN_HEALTH + dyn, v);
}

float rpg_fatigue_term(Rpg *r, const VmRef *ref)
{
    float mx = rpg_dyn_max(r, ref, 2), cur = rpg_dyn(r, ref, 2);
    float norm = floorf(mx) == 0.0f ? 1.0f : PW_MAX(0.0f, cur / mx);
    return rpg_gmst_f(r, "fFatigueBase", 1.25f) - rpg_gmst_f(r, "fFatigueMult", 0.5f) * (1.0f - norm);
}

int rpg_level(Rpg *r, const VmRef *ref)
{
    return (int)vm_prop(r->vm, ref, MWP_LEVEL);
}

int rpg_is_dead(Rpg *r, const VmRef *ref)
{
    return vm_prop(r->vm, ref, MWP_DEAD) != 0.0f;
}

int rpg_gold(Rpg *r, const VmRef *ref)
{
    vm_seed(r->vm, ref);
    return vm_item_count(r->vm, ref->key, pw_hash_name(RPG_GOLD));
}

float rpg_capacity(Rpg *r, const VmRef *ref)
{
    return rpg_attr(r, ref, ATTR_STRENGTH) * rpg_gmst_f(r, "fEncumbranceStrMult", 5.0f);
}

float rpg_encumbrance(Rpg *r, const VmRef *ref)
{
    uint32_t cursor = 0, item;
    int32_t count;
    float w = 0.0f;
    vm_seed(r->vm, ref);
    while (vm_item_next(r->vm, ref->key, &cursor, &item, &count) >= 0) {
        const DRpgObject *o = rpg_object(r, item);
        if (o)
            w += o->weight * (float)count;
    }
    w += rpg_effect(r, ref, MGEF_BURDEN, -1) - rpg_effect(r, ref, MGEF_FEATHER, -1);
    return w < 0.0f ? 0.0f : w;
}

static float normalized_encumbrance(Rpg *r, const VmRef *ref)
{
    float cap = rpg_capacity(r, ref);
    return cap > 0.0f ? rpg_encumbrance(r, ref) / cap : 1.0f;
}

float rpg_walk_speed(Rpg *r, const VmRef *ref)
{
    float mn = rpg_gmst_f(r, "fMinWalkSpeed", 100.0f), mx = rpg_gmst_f(r, "fMaxWalkSpeed", 200.0f);
    float speed = mn + 0.01f * rpg_attr(r, ref, ATTR_SPEED) * (mx - mn);
    float enc = PW_MIN(1.0f, normalized_encumbrance(r, ref));
    speed *= 1.0f - rpg_gmst_f(r, "fEncumberedMoveEffect", 0.3f) * enc;
    return speed;
}

float rpg_run_speed(Rpg *r, const VmRef *ref)
{
    return rpg_walk_speed(r, ref) * (0.01f * rpg_skill(r, ref, SKILL_ATHLETICS) *
                                     rpg_gmst_f(r, "fAthleticsRunBonus", 1.0f) +
                                     rpg_gmst_f(r, "fBaseRunMultiplier", 1.75f));
}

float rpg_jump_speed(Rpg *r, const VmRef *ref)
{
    float enc = PW_MIN(1.0f, normalized_encumbrance(r, ref));
    /* jump height in units: acrobatics plus the Jump effect */
    float x = rpg_gmst_f(r, "fJumpAcrobaticsBase", 128.0f) +
              rpg_gmst_f(r, "fJumpAcroMultiplier", 4.0f) * rpg_skill(r, ref, SKILL_ACROBATICS) +
              4.0f * rpg_effect(r, ref, MGEF_JUMP, -1);
    x *= rpg_gmst_f(r, "fJumpEncumbranceBase", 0.5f) +
         rpg_gmst_f(r, "fJumpEncumbranceMultiplier", 0.5f) * (1.0f - enc);
    /* launch speed for that apex height: v = sqrt(2 g h) */
    return sqrtf(2.0f * RPG_GRAVITY * PW_MAX(x, 16.0f));
}

int rpg_item_health_max(const Rpg *r, const DRpgObject *o)
{
    PW_UNUSED(r);
    if (!o)
        return 0;
    switch (o->type) {
    case DRPG_OT_WEAP: return o->stat[1];
    case DRPG_OT_ARMO: return o->stat[1];
    case DRPG_OT_LOCK: case DRPG_OT_PROB: case DRPG_OT_REPA: return o->stat[0];
    default: return 0;
    }
}

static float stack_health_ratio(Rpg *r, const VmItem *it)
{
    const DRpgObject *o = rpg_object(r, it->item);
    int mx = rpg_item_health_max(r, o);
    if (mx <= 0 || it->health == VM_FULL)
        return 1.0f;
    return (float)it->health / (float)mx;
}

float rpg_armor_rating(Rpg *r, const VmRef *ref)
{
    static const float weights[RPG_SLOT_COUNT] = { 0.1f, 0.3f, 0.1f, 0.1f, 0.1f, 0.05f, 0.05f, 0.1f };
    float unarmored = rpg_skill(r, ref, SKILL_UNARMORED);
    float base1 = rpg_gmst_f(r, "fUnarmoredBase1", 0.1f), base2 = rpg_gmst_f(r, "fUnarmoredBase2", 0.065f);
    float total = 0.0f;
    int armor_slots[8] = { RPG_SLOT_HELMET, RPG_SLOT_CUIRASS, RPG_SLOT_GREAVES, RPG_SLOT_LPAULDRON,
                           RPG_SLOT_RPAULDRON, RPG_SLOT_LGAUNTLET, RPG_SLOT_RGAUNTLET, RPG_SLOT_BOOTS };
    float slot_w[8] = { 0.1f, 0.3f, 0.1f, 0.1f, 0.1f, 0.05f, 0.05f, 0.1f };
    int i;
    PW_UNUSED(weights);
    if (rpg_actor_object(r, ref) && rpg_actor_object(r, ref)->type == DRPG_OT_CREA)
        return rpg_effect(r, ref, MGEF_SHIELD, -1);
    rpg_autoequip(r, ref);
    for (i = 0; i < 9; ++i) {
        int slot = i < 8 ? armor_slots[i] : RPG_SLOT_SHIELD;
        float w = i < 8 ? slot_w[i] : 0.1f;
        int st = rpg_equipped(r, ref, slot);
        const VmItem *it = st >= 0 ? vm_item_at(r->vm, st) : NULL;
        const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
        float rating;
        if (!o || o->type != DRPG_OT_ARMO) {
            if (slot == RPG_SLOT_SHIELD)
                continue; /* no shield: nothing in that slot */
            rating = (base1 * unarmored) * (base2 * unarmored);
        } else {
            int sk = armor_skill_of(r, o);
            rating = (float)o->stat[3] * rpg_skill(r, ref, sk) / (float)rpg_gmst_i(r, "iBaseArmorSkill", 30);
            rating *= stack_health_ratio(r, it);
        }
        total += rating * w;
    }
    return total + rpg_effect(r, ref, MGEF_SHIELD, -1);
}

int rpg_disposition(Rpg *r, const VmRef *npc)
{
    const DRpgObject *o = rpg_actor_object(r, npc);
    VmRef pc = vm_player_ref(r->vm);
    float x = vm_prop(r->vm, npc, MWP_DISPOSITION);
    uint32_t fac;
    if (!o || o->type != DRPG_OT_NPC)
        return (int)PW_CLAMP(x, 0.0f, 100.0f);
    if (o->link[0] == vm_pc_race(r->vm))
        x += rpg_gmst_f(r, "fDispRaceMod", 5.0f);
    x += rpg_gmst_f(r, "fDispPersonalityMult", 0.5f) *
         (rpg_attr(r, &pc, ATTR_PERSONALITY) - rpg_gmst_f(r, "fDispPersonalityBase", 50.0f));
    fac = o->link[2];
    if (fac) {
        float reaction = 0.0f;
        int rank = 0, pr = vm_pc_rank(r->vm, fac);
        if (pr >= 0 && !vm_pc_expelled(r->vm, fac)) {
            reaction = (float)vm_faction_reaction(r->vm, fac, fac);
            rank = pr;
        } else {
            uint32_t i;
            int first = 1;
            for (i = 0; i < r->db->hdr->factions.count; ++i) {
                int rr;
                if (r->vm->pc_rank[i] < 0 || r->vm->pc_expelled[i])
                    continue;
                rr = vm_faction_reaction(r->vm, fac, r->db->factions[i].hash);
                if (first || rr < reaction) {
                    reaction = (float)rr;
                    rank = r->vm->pc_rank[i];
                }
                first = 0;
            }
        }
        x += (rpg_gmst_f(r, "fDispFactionRankMult", 0.5f) * (float)rank +
              rpg_gmst_f(r, "fDispFactionRankBase", 1.0f)) * rpg_gmst_f(r, "fDispFactionMod", 3.0f) * reaction;
    }
    x -= rpg_gmst_f(r, "fDispCrimeMod", 0.001f) * vm_prop(r->vm, &pc, MWP_CRIME);
    if (rpg_has_disease(r, &pc))
        x += rpg_gmst_f(r, "fDispDiseaseMod", -10.0f);
    if (r->draw_state == 1)
        x += rpg_gmst_f(r, "fDispWeaponDrawn", -5.0f);
    x += rpg_effect(r, npc, MGEF_CHARM, -1);
    x += vm_prop(r->vm, npc, MWP_DISPTEMP);
    return (int)PW_CLAMP(x, 0.0f, 100.0f);
}

int rpg_has_disease(Rpg *r, const VmRef *ref)
{
    uint32_t cursor = 0, item;
    int32_t count;
    while (vm_item_next(r->vm, ref->key ^ VM_SPELL_SALT, &cursor, &item, &count) >= 0) {
        const DRpgSpell *s = rpg_spell(r, item);
        if (s && s->kind == 0 && (s->type == 2 || s->type == 3))
            return 1;
    }
    return 0;
}

/* script-visible stats (VM hook) */
static const int8_t effect_props[24] = { MGEF_RESIST_MAGICKA, MGEF_RESIST_FIRE, MGEF_RESIST_FROST,
    MGEF_RESIST_SHOCK, MGEF_RESIST_COMMON_DISEASE, MGEF_RESIST_BLIGHT_DISEASE,
    MGEF_RESIST_CORPRUS_DISEASE, MGEF_RESIST_POISON, MGEF_RESIST_PARALYSIS,
    MGEF_RESIST_NORMAL_WEAPONS, MGEF_WATER_BREATHING, MGEF_CHAMELEON, MGEF_WATER_WALKING,
    MGEF_SWIFT_SWIM, MGEF_JUMP, MGEF_LEVITATE, MGEF_SHIELD, MGEF_SOUND, MGEF_SILENCE, MGEF_BLIND,
    MGEF_PARALYZE, MGEF_INVISIBILITY, MGEF_FORTIFY_ATTACK, MGEF_SANCTUARY };

static float hook_stat(void *mech, const VmRef *ref, int prop, float raw)
{
    Rpg *r = (Rpg *)mech;
    if (prop >= MWP_ATTR_STRENGTH && prop < MWP_ATTR_STRENGTH + 8)
        return rpg_attr(r, ref, prop - MWP_ATTR_STRENGTH);
    if (prop >= MWP_SKILL_BLOCK && prop < MWP_SKILL_BLOCK + 27)
        return rpg_skill(r, ref, prop - MWP_SKILL_BLOCK);
    if (prop >= MWP_EFFECT_RESISTMAGICKA && prop < MWP_EFFECT_RESISTMAGICKA + 24)
        return raw + rpg_effect(r, ref, effect_props[prop - MWP_EFFECT_RESISTMAGICKA], -1);
    if (prop == MWP_DISPOSITION)
        return (float)rpg_disposition(r, ref);
    if (prop == MWP_AI_FIGHT)
        return raw + rpg_effect(r, ref, MGEF_FRENZY_HUMANOID, -1) + rpg_effect(r, ref, MGEF_FRENZY_CREATURE, -1) -
               rpg_effect(r, ref, MGEF_CALM_HUMANOID, -1) - rpg_effect(r, ref, MGEF_CALM_CREATURE, -1);
    if (prop == MWP_AI_FLEE)
        return raw + rpg_effect(r, ref, MGEF_DEMORALIZE_HUMANOID, -1) +
               rpg_effect(r, ref, MGEF_DEMORALIZE_CREATURE, -1) + rpg_effect(r, ref, MGEF_TURN_UNDEAD, -1) -
               rpg_effect(r, ref, MGEF_RALLY_HUMANOID, -1) - rpg_effect(r, ref, MGEF_RALLY_CREATURE, -1);
    return raw;
}

static float hook_max(void *mech, const VmRef *ref, int dyn_prop)
{
    return rpg_dyn_max((Rpg *)mech, ref, dyn_prop - MWP_DYN_HEALTH);
}

static void hook_change(void *mech, int what, const VmRef *ref, uint32_t id)
{
    Rpg *r = (Rpg *)mech;
    PW_UNUSED(id);
    if (what == VMN_SPELLS) {
        if (rpg_is_player(r, ref))
            r->dirty_constants = 1;
        else
            rpg_refresh_constants(r, ref);
    } else if (what == VMN_ITEMS && rpg_is_player(r, ref)) {
        r->dirty_constants = 1;
    }
}

/* ------------------------------------------------------------------------ */
/* progression                                                              */
/* ------------------------------------------------------------------------ */
static int class_skill(const Rpg *r, uint32_t cls, int k)
{
    const DRpgObject *c = rpgdb_object(r->db, cls);
    uint32_t packed;
    if (!c || c->type != DRPG_OT_CLAS || k < 0 || k > 9)
        return -1;
    packed = (uint32_t)c->stat[1 + k / 4];
    return (int)(int8_t)((packed >> (8 * (k % 4))) & 0xFF);
}

int rpg_class_spec(const Rpg *r, uint32_t cls)
{
    const DRpgObject *c = rpgdb_object(r->db, cls);
    return c && c->type == DRPG_OT_CLAS ? (int)((c->stat[0] >> 16) & 0xFF) : -1;
}

int rpg_class_attr(const Rpg *r, uint32_t cls, int i)
{
    const DRpgObject *c = rpgdb_object(r->db, cls);
    return c && c->type == DRPG_OT_CLAS ? (int)(int8_t)((c->stat[0] >> (8 * i)) & 0xFF) : -1;
}

int rpg_class_skill_type(const Rpg *r, uint32_t cls, int skill)
{
    int k;
    for (k = 0; k < 10; ++k)
        if (class_skill(r, cls, k) == skill)
            return (k & 1) ? 0 : 1; /* pairs are (minor, major) */
    return 2;
}

int rpg_skill_type(const Rpg *r, int skill)
{
    return rpg_class_skill_type(r, vm_pc_class(r->vm), skill);
}

static float progress_requirement(Rpg *r, int skill)
{
    VmRef pc = vm_player_ref(r->vm);
    int type = rpg_skill_type(r, skill);
    float req = 1.0f + vm_prop(r->vm, &pc, MWP_SKILL_BLOCK + skill);
    req *= type == 0 ? rpg_gmst_f(r, "fMajorSkillBonus", 0.75f)
           : type == 1 ? rpg_gmst_f(r, "fMinorSkillBonus", 1.0f)
                       : rpg_gmst_f(r, "fMiscSkillBonus", 1.25f);
    if (r->db->skills[skill].specialization == rpg_class_spec(r, vm_pc_class(r->vm)))
        req *= rpg_gmst_f(r, "fSpecialSkillBonus", 0.8f);
    return req;
}

float rpg_skill_progress(Rpg *r, int skill)
{
    VmRef pc = vm_player_ref(r->vm);
    float req = progress_requirement(r, skill);
    return req > 0.0f ? PW_CLAMP(vm_prop(r->vm, &pc, MWP_SKILLPROG_BLOCK + skill) / req, 0.0f, 1.0f) : 0.0f;
}

void rpg_skill_raise(Rpg *r, int skill, int by_trainer)
{
    VmRef pc = vm_player_ref(r->vm);
    int type, gov;
    float base;
    char msg[96];
    if (skill < 0 || skill >= SKILL_COUNT)
        return;
    base = vm_prop(r->vm, &pc, MWP_SKILL_BLOCK + skill);
    if (base >= 100.0f)
        return;
    vm_set_prop(r->vm, &pc, MWP_SKILL_BLOCK + skill, base + 1.0f);
    vm_set_prop(r->vm, &pc, MWP_SKILLPROG_BLOCK + skill, 0.0f);
    type = rpg_skill_type(r, skill);
    gov = r->db->skills[skill].attribute;
    if (type == 0)
        vm_set_prop(r->vm, &pc, MWP_LEVELPROG,
                    vm_prop(r->vm, &pc, MWP_LEVELPROG) + (float)rpg_gmst_i(r, "iLevelUpMajorMult", 1));
    else if (type == 1)
        vm_set_prop(r->vm, &pc, MWP_LEVELPROG,
                    vm_prop(r->vm, &pc, MWP_LEVELPROG) + (float)rpg_gmst_i(r, "iLevelUpMinorMult", 1));
    if (gov < ATTR_COUNT) {
        int add = type == 0 ? rpg_gmst_i(r, "iLevelupMajorMultAttribute", 1)
                  : type == 1 ? rpg_gmst_i(r, "iLevelupMinorMultAttribute", 1)
                              : rpg_gmst_i(r, "iLevelupMiscMultAttriubte", 1);
        vm_set_prop(r->vm, &pc, MWP_LVLATTR_STRENGTH + gov,
                    vm_prop(r->vm, &pc, MWP_LVLATTR_STRENGTH + gov) + (float)add);
    }
    snprintf(msg, sizeof(msg), rpgdb_gmst_string(r->db, "sNotifyMessage39", "Your %s skill increased to %d."),
             rpg_skill_name(r, skill), (int)base + 1);
    vm_notify(r->vm, msg);
    if (!r->level_pending && vm_prop(r->vm, &pc, MWP_LEVELPROG) >= (float)rpg_gmst_i(r, "iLevelUpTotal", 10)) {
        r->level_pending = 1;
        rpg_notify(r, "sLevelUpMsg", "You should rest and meditate on what you've learned.");
    }
    PW_UNUSED(by_trainer);
}

void rpg_skill_use(Rpg *r, int skill, int use_type, float factor)
{
    VmRef pc = vm_player_ref(r->vm);
    float gain, prog;
    if (skill < 0 || skill >= SKILL_COUNT || use_type < 0 || use_type > 3)
        return;
    gain = r->db->skills[skill].use[use_type] * factor;
    if (gain <= 0.0f)
        return;
    prog = vm_prop(r->vm, &pc, MWP_SKILLPROG_BLOCK + skill) + gain;
    vm_set_prop(r->vm, &pc, MWP_SKILLPROG_BLOCK + skill, prog);
    if (prog >= progress_requirement(r, skill))
        rpg_skill_raise(r, skill, 0);
}

int rpg_level_multiplier(Rpg *r, int attr)
{
    static const int defaults[11] = { 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5 };
    VmRef pc = vm_player_ref(r->vm);
    char id[24];
    int n;
    if (attr < 0 || attr >= ATTR_COUNT)
        return 1;
    n = (int)PW_CLAMP(vm_prop(r->vm, &pc, MWP_LVLATTR_STRENGTH + attr), 0.0f, 10.0f);
    if (n == 0)
        return 1;
    snprintf(id, sizeof(id), "iLevelUp%02dMult", n);
    return rpg_gmst_i(r, id, defaults[n]);
}

void rpg_recalc_player(Rpg *r)
{
    VmRef pc = vm_player_ref(r->vm);
    int d;
    for (d = 1; d < 3; ++d) {
        float mx = rpg_dyn_max(r, &pc, d);
        if (rpg_dyn(r, &pc, d) > mx)
            vm_set_prop(r->vm, &pc, MWP_DYN_HEALTH + d, mx);
        vm_set_prop(r->vm, &pc, MWP_DYNBASE_HEALTH + d, mx);
    }
}

int rpg_level_up(Rpg *r, const int attrs[3])
{
    VmRef pc = vm_player_ref(r->vm);
    float total = (float)rpg_gmst_i(r, "iLevelUpTotal", 10), gain, mags[3];
    int i, a;
    if (vm_prop(r->vm, &pc, MWP_LEVELPROG) < total)
        return 0;
    if (attrs[0] == attrs[1] || attrs[0] == attrs[2] || attrs[1] == attrs[2])
        return 0;
    for (i = 0; i < 3; ++i)
        if (attrs[i] < 0 || attrs[i] >= ATTR_COUNT)
            return 0;
    for (i = 0; i < 3; ++i)
        mags[i] = (float)rpg_level_multiplier(r, attrs[i]);
    for (i = 0; i < 3; ++i) {
        a = attrs[i];
        vm_set_prop(r->vm, &pc, MWP_ATTR_STRENGTH + a,
                    PW_MIN(100.0f, vm_prop(r->vm, &pc, MWP_ATTR_STRENGTH + a) + mags[i]));
    }
    gain = vm_prop(r->vm, &pc, MWP_ATTR_ENDURANCE) * rpg_gmst_f(r, "fLevelUpHealthEndMult", 0.1f);
    vm_set_prop(r->vm, &pc, MWP_DYNBASE_HEALTH, vm_prop(r->vm, &pc, MWP_DYNBASE_HEALTH) + gain);
    vm_set_prop(r->vm, &pc, MWP_DYN_HEALTH, PW_MAX(1.0f, rpg_dyn(r, &pc, 0) + gain));
    vm_set_prop(r->vm, &pc, MWP_LEVEL, vm_prop(r->vm, &pc, MWP_LEVEL) + 1.0f);
    vm_set_prop(r->vm, &pc, MWP_LEVELPROG, PW_MAX(0.0f, vm_prop(r->vm, &pc, MWP_LEVELPROG) - total));
    for (a = 0; a < ATTR_COUNT; ++a)
        vm_set_prop(r->vm, &pc, MWP_LVLATTR_STRENGTH + a, 0.0f);
    r->level_pending = vm_prop(r->vm, &pc, MWP_LEVELPROG) >= total;
    rpg_recalc_player(r);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* applying effects                                                         */
/* ------------------------------------------------------------------------ */
static RpgFx *fx_add(Rpg *r)
{
    if (r->fx_count >= RPG_MAX_FX) {
        vm_error(r->vm, "active effect pool full");
        return NULL;
    }
    memset(&r->fx[r->fx_count], 0, sizeof(RpgFx));
    return &r->fx[r->fx_count++];
}

static VmRef ref_of_key(Rpg *r, uint32_t key, uint32_t object)
{
    VmRef ref;
    if (key == r->vm->player_hash)
        return vm_player_ref(r->vm);
    memset(&ref, 0, sizeof(ref));
    ref.kind = VM_REF_OBJECT;
    ref.key = key;
    ref.object = object;
    ref.vmi = -1;
    ref.inst = -1;
    return ref;
}

/* undoes the lasting part of an effect when it ends */
static void fx_end(Rpg *r, RpgFx *f)
{
    VmRef t = ref_of_key(r, f->key, f->object);
    switch (f->effect) {
    case MGEF_FORTIFY_HEALTH:
        rpg_dyn_add(r, &t, 0, 0.0f); /* clamp to the lowered maximum */
        break;
    case MGEF_FORTIFY_MAGICKA:
        rpg_dyn_add(r, &t, 1, 0.0f);
        break;
    case MGEF_FORTIFY_FATIGUE:
        rpg_dyn_add(r, &t, 2, 0.0f);
        break;
    case MGEF_DRAIN_HEALTH:
        rpg_dyn_add(r, &t, 0, f->applied);
        break;
    case MGEF_DRAIN_MAGICKA:
        rpg_dyn_add(r, &t, 1, f->applied);
        break;
    case MGEF_DRAIN_FATIGUE:
        rpg_dyn_add(r, &t, 2, f->applied);
        break;
    default:
        if (f->effect >= MGEF_BOUND_DAGGER && f->effect <= MGEF_BOUND_GLOVES && f->effect != MGEF_EXTRA_SPELL)
            rpg_bound_end(r, f);
        break;
    }
}

static void fx_remove_at(Rpg *r, uint32_t i)
{
    RpgFx f = r->fx[i];
    r->fx[i] = r->fx[--r->fx_count];
    if (!(f.flags & RFX_CONSTANT))
        fx_end(r, &f);
}

void rpg_remove_source(Rpg *r, uint32_t key, uint32_t source)
{
    uint32_t i = 0;
    while (i < r->fx_count) {
        if (r->fx[i].key == key && r->fx[i].source == source && !(r->fx[i].flags & RFX_CONSTANT))
            fx_remove_at(r, i);
        else
            ++i;
    }
}

void rpg_dispel(Rpg *r, uint32_t key, float magnitude)
{
    uint32_t i = 0;
    while (i < r->fx_count) {
        RpgFx *f = &r->fx[i];
        if (f->key == key && !(f->flags & RFX_CONSTANT) && (f->flags & RFX_HARMFUL) &&
            rpg_roll100(r) < (int)magnitude)
            fx_remove_at(r, i);
        else
            ++i;
    }
}

static void remove_effect(Rpg *r, uint32_t key, int effect)
{
    uint32_t i = 0;
    while (i < r->fx_count) {
        if (r->fx[i].key == key && r->fx[i].effect == effect && !(r->fx[i].flags & RFX_CONSTANT))
            fx_remove_at(r, i);
        else
            ++i;
    }
}

static void cure_spells(Rpg *r, const VmRef *ref, int type)
{
    uint32_t cursor = 0, item;
    int32_t count;
    while (vm_item_next(r->vm, ref->key ^ VM_SPELL_SALT, &cursor, &item, &count) >= 0) {
        const DRpgSpell *s = rpg_spell(r, item);
        if (s && s->kind == 0 && s->type == type)
            vm_item_add(r->vm, ref->key ^ VM_SPELL_SALT, item, -count);
    }
    rpg_refresh_constants(r, ref);
}

/* resistance in percent against effect `e` (the original's roll included) */
static float resistance(Rpg *r, const VmRef *target, int e, float cast_chance)
{
    const DRpgMgef *m = rpgdb_mgef(r->db, e);
    float res = 0.0f, x, roll;
    if (!m || m->resisted_by < 0)
        return 0.0f;
    res += rpg_effect(r, target, m->resisted_by, -1);
    if (m->weakened_by >= 0)
        res -= rpg_effect(r, target, m->weakened_by, -1);
    if (e == MGEF_FIRE_DAMAGE)
        res += rpg_effect(r, target, MGEF_FIRE_SHIELD, -1);
    if (e == MGEF_SHOCK_DAMAGE)
        res += rpg_effect(r, target, MGEF_LIGHTNING_SHIELD, -1);
    if (e == MGEF_FROST_DAMAGE)
        res += rpg_effect(r, target, MGEF_FROST_SHIELD, -1);
    x = (rpg_attr(r, target, ATTR_WILLPOWER) + 0.1f * rpg_attr(r, target, ATTR_LUCK)) * rpg_fatigue_term(r, target);
    if (cast_chance > 0.0f)
        x *= 50.0f / cast_chance;
    roll = vm_random01(r->vm) * 100.0f;
    if (m->flags & DRPG_MF_NO_MAGNITUDE)
        roll -= res;
    if (x <= roll)
        x = 0.0f;
    else
        x = (m->flags & DRPG_MF_NO_MAGNITUDE) ? 100.0f : roll / PW_MIN(x, 100.0f);
    return PW_MIN(x + res, 100.0f);
}

static int is_instant(int e)
{
    switch (e) {
    case MGEF_DISPEL: case MGEF_CURE_COMMON_DISEASE: case MGEF_CURE_BLIGHT_DISEASE:
    case MGEF_CURE_CORPRUS_DISEASE: case MGEF_CURE_POISON: case MGEF_CURE_PARALYZATION:
    case MGEF_REMOVE_CURSE: case MGEF_MARK: case MGEF_RECALL: case MGEF_DIVINE_INTERVENTION:
    case MGEF_ALMSIVI_INTERVENTION: case MGEF_OPEN: case MGEF_LOCK:
        return 1;
    default:
        return 0;
    }
}

static void instant_effect(Rpg *r, const VmRef *caster, const VmRef *target, int e, float mag)
{
    switch (e) {
    case MGEF_DISPEL:
        rpg_dispel(r, target->key, mag);
        break;
    case MGEF_CURE_COMMON_DISEASE:
        cure_spells(r, target, 3);
        break;
    case MGEF_CURE_BLIGHT_DISEASE:
        cure_spells(r, target, 2);
        break;
    case MGEF_REMOVE_CURSE:
        cure_spells(r, target, 4);
        break;
    case MGEF_CURE_CORPRUS_DISEASE:
        remove_effect(r, target->key, MGEF_CORPRUS);
        break;
    case MGEF_CURE_POISON:
        remove_effect(r, target->key, MGEF_POISON);
        break;
    case MGEF_CURE_PARALYZATION:
        remove_effect(r, target->key, MGEF_PARALYZE);
        break;
    case MGEF_MARK:
        if (rpg_is_player(r, target) && r->host.locate) {
            r->host.locate(r->host.user, &r->mark_cell, r->mark_pos, &r->mark_rot);
            r->has_mark = 1;
        }
        break;
    case MGEF_RECALL:
        if (rpg_is_player(r, target) && r->has_mark && r->host.teleport)
            r->host.teleport(r->host.user, r->mark_cell, r->mark_pos, r->mark_rot);
        break;
    case MGEF_DIVINE_INTERVENTION:
    case MGEF_ALMSIVI_INTERVENTION:
        if (rpg_is_player(r, target))
            rpg_intervention(r, e == MGEF_DIVINE_INTERVENTION ? 0 : 1);
        break;
    case MGEF_OPEN:
    case MGEF_LOCK:
        if (target->kind == VM_REF_OBJECT) {
            float lock = vm_prop(r->vm, target, MWP_LOCK);
            if (e == MGEF_OPEN) {
                if (lock > 0.0f && mag >= lock) {
                    vm_set_prop(r->vm, target, MWP_LOCK, -1.0f);
                    rpg_notify(r, "sMagicOpenSuccess", "The lock has been opened.");
                }
            } else if (lock <= 0.0f || mag > lock) {
                vm_set_prop(r->vm, target, MWP_LOCK, mag);
            }
        }
        break;
    default:
        break;
    }
    PW_UNUSED(caster);
}

int rpg_apply(Rpg *r, const VmRef *caster, const VmRef *target, const DRpgEffect *effects, int count,
              int range, uint32_t source, float cast_chance, int harmful_allowed)
{
    int i, landed = 0;
    VmRef tgt = *target;
    if (!target || target->kind == VM_REF_NONE)
        return 0;
    if (target->key != caster->key && !(range == RPG_RANGE_SELF))
        rpg_remove_source(r, target->key, source);
    else if (range == RPG_RANGE_SELF)
        rpg_remove_source(r, target->key, source);
    for (i = 0; i < count; ++i) {
        const DRpgEffect *e = &effects[i];
        const DRpgMgef *m = rpgdb_mgef(r->db, e->effect);
        const VmRef *dst = &tgt;
        float mag, res, dur;
        RpgFx *f;
        if (!m || e->range != range)
            continue;
        if ((m->flags & DRPG_MF_HARMFUL) && !harmful_allowed)
            continue;
        mag = (m->flags & DRPG_MF_NO_MAGNITUDE) ? 1.0f
              : (float)e->mag_min + vm_random01(r->vm) * (float)(e->mag_max - e->mag_min + 1);
        if (mag > (float)e->mag_max && !(m->flags & DRPG_MF_NO_MAGNITUDE))
            mag = (float)e->mag_max;
        if ((m->flags & DRPG_MF_HARMFUL) && target->key != caster->key) {
            float reflect = rpg_effect(r, target, MGEF_REFLECT, -1);
            float absorb = rpg_effect(r, target, MGEF_SPELL_ABSORPTION, -1);
            if (!(m->flags & DRPG_MF_UNREFLECTABLE) && reflect > 0.0f && rpg_roll100(r) < (int)reflect) {
                dst = caster;
                rpg_notify(r, "sMagicReflect", "Spell reflected.");
            } else if (absorb > 0.0f && rpg_roll100(r) < (int)absorb) {
                rpg_dyn_add(r, target, 1, mag);
                continue;
            }
            vm_set_prop(r->vm, target, MWP_ATTACKED, 1.0f);
            if (rpg_is_player(r, caster))
                vm_set_prop(r->vm, target, MWP_HOSTILE, 1.0f);
        }
        res = resistance(r, dst, e->effect, cast_chance);
        if (res >= 100.0f) {
            if (rpg_is_player(r, dst) || rpg_is_player(r, caster))
                rpg_notify(r, "sMagicTargetResisted", "The target resisted.");
            continue;
        }
        mag *= 1.0f - res / 100.0f;
        if (is_instant(e->effect)) {
            instant_effect(r, caster, dst, e->effect, mag);
            landed++;
            continue;
        }
        if (e->effect >= MGEF_SUMMON_SCAMP && e->effect <= MGEF_SUMMON_STORM_ATRONACH) {
            rpg_notify(r, "", "Summoned creatures arrive with the AI milestone (M6).");
            continue;
        }
        if (e->effect == MGEF_SUMMON_CENTURION_SPHERE || e->effect >= MGEF_SUMMON_FABRICANT) {
            rpg_notify(r, "", "Summoned creatures arrive with the AI milestone (M6).");
            continue;
        }
        dur = (m->flags & DRPG_MF_NO_DURATION) ? 1.0f : PW_MAX(1.0f, (float)e->duration);
        f = fx_add(r);
        if (!f)
            break;
        f->key = dst->key;
        f->object = dst->object;
        f->source = source;
        f->caster = caster->key;
        f->effect = (uint8_t)e->effect;
        f->arg = (m->flags & DRPG_MF_TARGET_SKILL) ? e->skill
                 : (m->flags & DRPG_MF_TARGET_ATTRIBUTE) ? e->attribute : -1;
        f->magnitude = mag;
        f->duration = f->remaining = dur;
        f->flags = (m->flags & DRPG_MF_HARMFUL) ? RFX_HARMFUL : 0;
        rpg_fx_start(r, f, caster);
        landed++;
    }
    return landed;
}

/* the immediate part of a lasting effect */
void rpg_fx_start(Rpg *r, RpgFx *f, const VmRef *caster)
{
    VmRef t = ref_of_key(r, f->key, f->object);
    switch (f->effect) {
    case MGEF_FORTIFY_HEALTH:
        rpg_dyn_add(r, &t, 0, f->magnitude);
        break;
    case MGEF_FORTIFY_MAGICKA:
        rpg_dyn_add(r, &t, 1, f->magnitude);
        break;
    case MGEF_FORTIFY_FATIGUE:
        rpg_dyn_add(r, &t, 2, f->magnitude);
        break;
    case MGEF_DRAIN_HEALTH:
    case MGEF_DRAIN_MAGICKA:
    case MGEF_DRAIN_FATIGUE: {
        int d = f->effect - MGEF_DRAIN_HEALTH;
        float before = rpg_dyn(r, &t, d);
        rpg_dyn_add(r, &t, d, -f->magnitude);
        f->applied = before - rpg_dyn(r, &t, d);
        break;
    }
    case MGEF_ABSORB_ATTRIBUTE:
    case MGEF_ABSORB_SKILL: {
        /* the caster gains what the target loses, for the same time */
        RpgFx *g = fx_add(r);
        if (g && caster) {
            *g = *f;
            g->key = caster->key;
            g->object = caster->object;
            g->effect = f->effect == MGEF_ABSORB_ATTRIBUTE ? MGEF_FORTIFY_ATTRIBUTE : MGEF_FORTIFY_SKILL;
            g->flags = 0;
        }
        break;
    }
    default:
        if (f->effect >= MGEF_BOUND_DAGGER && f->effect <= MGEF_BOUND_GLOVES && f->effect != MGEF_EXTRA_SPELL)
            rpg_bound_start(r, f);
        break;
    }
}

/* per-second part of lasting effects */
static void fx_tick(Rpg *r, RpgFx *f, float dt)
{
    VmRef t = ref_of_key(r, f->key, f->object);
    float amount = f->magnitude * (f->duration <= 1.0f ? (dt >= f->remaining ? 1.0f : dt) : dt);
    if (f->flags & RFX_CONSTANT)
        amount = f->magnitude * dt;
    switch (f->effect) {
    case MGEF_DAMAGE_HEALTH: case MGEF_FIRE_DAMAGE: case MGEF_SHOCK_DAMAGE: case MGEF_FROST_DAMAGE:
    case MGEF_POISON:
        rpg_damage(r, &t, 0, amount, NULL);
        break;
    case MGEF_DAMAGE_MAGICKA:
        rpg_dyn_add(r, &t, 1, -amount);
        break;
    case MGEF_DAMAGE_FATIGUE:
        rpg_dyn_add(r, &t, 2, -amount);
        break;
    case MGEF_RESTORE_HEALTH:
        rpg_dyn_add(r, &t, 0, amount);
        break;
    case MGEF_RESTORE_MAGICKA:
        rpg_dyn_add(r, &t, 1, amount);
        break;
    case MGEF_RESTORE_FATIGUE:
        rpg_dyn_add(r, &t, 2, amount);
        break;
    case MGEF_ABSORB_HEALTH: case MGEF_ABSORB_MAGICKA: case MGEF_ABSORB_FATIGUE: {
        int d = f->effect - MGEF_ABSORB_HEALTH;
        VmRef c = ref_of_key(r, f->caster, 0);
        float before = rpg_dyn(r, &t, d);
        if (d == 0)
            rpg_damage(r, &t, 0, amount, &c);
        else
            rpg_dyn_add(r, &t, d, -amount);
        rpg_dyn_add(r, &c, d, before - rpg_dyn(r, &t, d));
        break;
    }
    case MGEF_DAMAGE_ATTRIBUTE:
        if (f->arg >= 0)
            vm_set_prop(r->vm, &t, MWP_ATTRDMG_STRENGTH + f->arg,
                        vm_prop(r->vm, &t, MWP_ATTRDMG_STRENGTH + f->arg) + amount);
        break;
    case MGEF_DAMAGE_SKILL:
        if (f->arg >= 0)
            vm_set_prop(r->vm, &t, MWP_SKILLDMG_BLOCK + f->arg,
                        vm_prop(r->vm, &t, MWP_SKILLDMG_BLOCK + f->arg) + amount);
        break;
    case MGEF_RESTORE_ATTRIBUTE:
        if (f->arg >= 0)
            vm_set_prop(r->vm, &t, MWP_ATTRDMG_STRENGTH + f->arg,
                        PW_MAX(0.0f, vm_prop(r->vm, &t, MWP_ATTRDMG_STRENGTH + f->arg) - amount));
        break;
    case MGEF_RESTORE_SKILL:
        if (f->arg >= 0)
            vm_set_prop(r->vm, &t, MWP_SKILLDMG_BLOCK + f->arg,
                        PW_MAX(0.0f, vm_prop(r->vm, &t, MWP_SKILLDMG_BLOCK + f->arg) - amount));
        break;
    case MGEF_DISINTEGRATE_WEAPON:
    case MGEF_DISINTEGRATE_ARMOR:
        rpg_disintegrate(r, &t, f->effect == MGEF_DISINTEGRATE_WEAPON, amount);
        break;
    default:
        break;
    }
}

void rpg_damage(Rpg *r, const VmRef *ref, int dyn, float amount, const VmRef *source)
{
    if (rpg_is_dead(r, ref) && dyn == 0)
        return;
    vm_set_prop(r->vm, ref, MWP_DYN_HEALTH + dyn, rpg_dyn(r, ref, dyn) - amount);
    if (dyn == 0 && rpg_dyn(r, ref, 0) <= 0.0f)
        rpg_kill(r, ref, source);
}

/* ------------------------------------------------------------------------ */
/* constant effects (abilities, diseases, curses, constant enchantments)    */
/* ------------------------------------------------------------------------ */
static void add_constants(Rpg *r, const VmRef *ref, const DRpgEffect *e, int n, uint32_t source)
{
    int i;
    for (i = 0; i < n; ++i) {
        const DRpgMgef *m = rpgdb_mgef(r->db, e[i].effect);
        RpgFx *f;
        if (!m || is_instant(e[i].effect))
            continue;
        f = fx_add(r);
        if (!f)
            return;
        f->key = ref->key;
        f->object = ref->object;
        f->source = source;
        f->caster = ref->key;
        f->effect = (uint8_t)e[i].effect;
        f->arg = (m->flags & DRPG_MF_TARGET_SKILL) ? e[i].skill
                 : (m->flags & DRPG_MF_TARGET_ATTRIBUTE) ? e[i].attribute : -1;
        f->magnitude = (float)(e[i].mag_min + e[i].mag_max) * 0.5f;
        f->flags = RFX_CONSTANT | ((m->flags & DRPG_MF_HARMFUL) ? RFX_HARMFUL : 0);
        f->remaining = f->duration = 0.0f;
    }
}

void rpg_refresh_constants(Rpg *r, const VmRef *ref)
{
    uint32_t i = 0, cursor = 0, item;
    int32_t count;
    int slot;
    while (i < r->fx_count) {
        if (r->fx[i].key == ref->key && (r->fx[i].flags & RFX_CONSTANT))
            r->fx[i] = r->fx[--r->fx_count];
        else
            ++i;
    }
    while (vm_item_next(r->vm, ref->key ^ VM_SPELL_SALT, &cursor, &item, &count) >= 0) {
        const DRpgSpell *s = rpg_spell(r, item);
        int n;
        const DRpgEffect *e;
        if (!s || s->kind != 0 || s->type == 0 || s->type == 5)
            continue;
        e = rpg_spell_effects(r, s, &n);
        add_constants(r, ref, e, n, s->hash);
    }
    for (slot = 0; slot < RPG_SLOT_COUNT; ++slot) {
        int st = rpg_equipped(r, ref, slot);
        const VmItem *it = st >= 0 ? vm_item_at(r->vm, st) : NULL;
        const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
        const DRpgSpell *en;
        int n;
        const DRpgEffect *e;
        if (!o || !o->link[0] || o->type == DRPG_OT_NPC || o->type == DRPG_OT_RACE)
            continue;
        en = rpg_spell(r, o->link[0]);
        if (!en || en->kind != 1 || en->type != 3)
            continue;
        e = rpg_spell_effects(r, en, &n);
        add_constants(r, ref, e, n, en->hash);
    }
}

/* ------------------------------------------------------------------------ */
/* death                                                                    */
/* ------------------------------------------------------------------------ */
static void soul_trap(Rpg *r, const VmRef *victim)
{
    const DRpgObject *o = rpg_actor_object(r, victim);
    const DRpgActor *a = o ? rpgdb_actor(r->db, o) : NULL;
    uint32_t i, cursor = 0, item;
    int32_t count;
    int best = -1;
    float best_cap = 0.0f, mult = rpg_gmst_f(r, "fSoulGemMult", 3.0f);
    uint32_t caster = 0;
    for (i = 0; i < r->fx_count; ++i)
        if (r->fx[i].key == victim->key && r->fx[i].effect == MGEF_SOULTRAP)
            caster = r->fx[i].caster;
    if (!caster || !a || o->type != DRPG_OT_CREA || a->soul <= 0)
        return;
    while (vm_item_next(r->vm, caster, &cursor, &item, &count) >= 0) {
        const VmItem *it = vm_item_at(r->vm, (int)cursor - 1);
        const DRpgObject *gem = rpg_object(r, item);
        float cap;
        if (!gem || !rpg_is_soulgem(r, gem) || it->soul)
            continue;
        cap = (float)gem->value * mult;
        if (cap >= (float)a->soul && (best < 0 || cap < best_cap)) {
            best = (int)cursor - 1;
            best_cap = cap;
        }
    }
    if (best < 0)
        return;
    {
        VmItem *g = vm_item_at(r->vm, best);
        uint32_t gem = g->item;
        g->count--;
        vm_item_add_state(r->vm, caster, gem, 1, VM_FULL, -1.0f, victim->object);
    }
    if (caster == r->vm->player_hash)
        rpg_notify(r, "sSoultrapSuccess", "You have trapped a soul.");
}

int rpg_is_soulgem(const Rpg *r, const DRpgObject *o)
{
    const char *n;
    PW_UNUSED(r);
    if (!o || o->type != DRPG_OT_MISC)
        return 0;
    n = rpgdb_name(r->db, o->name);
    return strstr(n, "Soul Gem") != NULL || (o->flags & RPG_OF_SOULGEM) != 0;
}

int rpg_kill(Rpg *r, const VmRef *ref, const VmRef *killer)
{
    if (rpg_is_dead(r, ref))
        return 0;
    vm_set_prop(r->vm, ref, MWP_DEAD, 1.0f);
    if (vm_prop(r->vm, ref, MWP_DYN_HEALTH) > 0.0f)
        vm_set_prop(r->vm, ref, MWP_DYN_HEALTH, 0.0f);
    if (!rpg_is_player(r, ref)) {
        vm_set_prop_key(r->vm, ref->object, MWP_DEATHS, vm_prop_key(r->vm, ref->object, ref->object, MWP_DEATHS) + 1.0f);
        vm_post_event(r->vm, ref, VMEV_DEATH);
        if (killer && rpg_is_player(r, killer))
            vm_post_event(r->vm, ref, VMEV_MURDER);
        soul_trap(r, ref);
    }
    if (r->host.killed)
        r->host.killed(r->host.user, ref);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* tick                                                                     */
/* ------------------------------------------------------------------------ */
void rpg_tick(Rpg *r, float dt)
{
    VmRef pc = vm_player_ref(r->vm);
    uint32_t i = 0;
    if (r->dirty_constants) {
        r->dirty_constants = 0;
        rpg_refresh_constants(r, &pc);
    }
    while (i < r->fx_count) {
        RpgFx *f = &r->fx[i];
        float step = (f->flags & RFX_CONSTANT) ? dt : PW_MIN(dt, f->remaining);
        fx_tick(r, f, step);
        if (i >= r->fx_count || &r->fx[i] != f) {
            continue; /* removed by a death/dispel side effect */
        }
        if (!(f->flags & RFX_CONSTANT)) {
            f->remaining -= dt;
            if (f->remaining <= 0.0f) {
                fx_remove_at(r, i);
                continue;
            }
        }
        ++i;
    }
    if (rpg_is_dead(r, &pc))
        return;
    /* fatigue: regenerates, running and swimming cost */
    {
        float regen = rpg_gmst_f(r, "fFatigueReturnBase", 2.5f) +
                      rpg_gmst_f(r, "fFatigueReturnMult", 0.02f) * rpg_attr(r, &pc, ATTR_ENDURANCE);
        float enc = PW_MIN(1.0f, normalized_encumbrance(r, &pc));
        float cost = 0.0f;
        if (r->running && r->moving)
            cost = rpg_gmst_f(r, "fFatigueRunBase", 5.0f) + rpg_gmst_f(r, "fFatigueRunMult", 2.0f) * enc;
        if (r->swimming && r->moving)
            cost += rpg_gmst_f(r, "fFatigueSwimWalkBase", 2.0f);
        rpg_dyn_add(r, &pc, 2, (regen - cost) * dt);
        if (r->moving && r->running && !r->swimming)
            rpg_skill_use(r, SKILL_ATHLETICS, 0, dt);
        if (r->moving && r->swimming)
            rpg_skill_use(r, SKILL_ATHLETICS, 1, dt);
    }
    if (rpg_dyn(r, &pc, 0) <= 0.0f)
        rpg_kill(r, &pc, NULL);
}

void rpg_player_jumped(Rpg *r)
{
    VmRef pc = vm_player_ref(r->vm);
    float enc = PW_MIN(1.0f, normalized_encumbrance(r, &pc));
    rpg_dyn_add(r, &pc, 2, -(rpg_gmst_f(r, "fFatigueJumpBase", 5.0f) + rpg_gmst_f(r, "fFatigueJumpMult", 0.0f) * enc));
    rpg_skill_use(r, SKILL_ACROBATICS, 0, 1.0f);
}

void rpg_player_landed(Rpg *r, float fall)
{
    VmRef pc = vm_player_ref(r->vm);
    float min_fall = rpg_gmst_f(r, "fFallDamageDistanceMin", 400.0f);
    float x;
    if (fall <= min_fall || rpg_effect(r, &pc, MGEF_SLOW_FALL, -1) > 0.0f)
        return;
    x = fall - min_fall;
    x -= 1.5f * rpg_skill(r, &pc, SKILL_ACROBATICS) + rpg_effect(r, &pc, MGEF_JUMP, -1);
    x = PW_MAX(0.0f, x);
    x *= (rpg_gmst_f(r, "fFallDistanceBase", 0.0f) + rpg_gmst_f(r, "fFallDistanceMult", 0.07f) * x);
    x *= 1.0f - 0.25f * rpg_fatigue_term(r, &pc);
    if (x > 0.0f) {
        rpg_damage(r, &pc, 0, x, NULL);
        rpg_skill_use(r, SKILL_ACROBATICS, 1, 1.0f);
    }
}

/* ------------------------------------------------------------------------ */
/* setup                                                                    */
/* ------------------------------------------------------------------------ */
int rpg_init(Rpg *r, Vm *vm, RpgDb *db, const RpgHost *host)
{
    memset(r, 0, sizeof(*r));
    r->vm = vm;
    r->db = db;
    if (host)
        r->host = *host;
    if (!db->skills || !db->mgefs)
        return -1;
    vm->mech = r;
    vm->stat_get = hook_stat;
    vm->stat_max = hook_max;
    vm->notify_change = hook_change;
    rpg_new_game(r);
    return 0;
}

void rpg_new_game(Rpg *r)
{
    VmRef pc = vm_player_ref(r->vm);
    int d;
    r->fx_count = 0;
    r->eq_count = 0;
    r->custom_count = 0;
    r->race = r->cls = r->sign = 0;
    r->female = 0;
    r->chargen_done = 0;
    r->sel_spell = 0;
    r->sel_item = -1;
    r->has_mark = 0;
    r->level_pending = 0;
    r->visited_count = 0;
    r->draw_state = 0;
    r->dirty_constants = 1;
    /* the player record's dynamics become the starting values */
    for (d = 0; d < 3; ++d) {
        float mx = rpg_dyn_max(r, &pc, d);
        vm_set_prop(r->vm, &pc, MWP_DYN_HEALTH + d, mx);
        if (d > 0)
            vm_set_prop(r->vm, &pc, MWP_DYNBASE_HEALTH + d, mx);
    }
}

/* ------------------------------------------------------------------------ */
/* rest, map                                                                */
/* ------------------------------------------------------------------------ */
int rpg_can_rest(Rpg *r, char *msg, uint32_t cap)
{
    if (r->host.hostiles_near && r->host.hostiles_near(r->host.user, 2048.0f)) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sNotifyMessage2",
                                                       "You cannot rest with enemies nearby."));
        return 0;
    }
    msg[0] = '\0';
    return 1;
}

int rpg_rest(Rpg *r, int hours, int sleep)
{
    VmRef pc = vm_player_ref(r->vm);
    int h;
    for (h = 0; h < hours; ++h) {
        if (sleep) {
            rpg_dyn_add(r, &pc, 0, 0.1f * rpg_attr(r, &pc, ATTR_ENDURANCE));
            if (rpg_effect(r, &pc, MGEF_STUNTED_MAGICKA, -1) <= 0.0f)
                rpg_dyn_add(r, &pc, 1, rpg_gmst_f(r, "fRestMagicMult", 0.15f) * rpg_attr(r, &pc, ATTR_INTELLIGENCE));
        }
        rpg_dyn_add(r, &pc, 2, rpg_dyn_max(r, &pc, 2));
        /* lasting effects run their course while time passes */
        rpg_tick(r, 3600.0f / PW_MAX(1.0f, r->vm->timescale));
        vm_advance_hours(r->vm, 1.0f);
        if (rpg_is_dead(r, &pc))
            return h + 1;
    }
    return hours;
}

void rpg_visit_cell(Rpg *r, int x, int y)
{
    uint32_t key = ((uint32_t)(uint16_t)(x + 32768) << 16) | (uint16_t)(y + 32768), i;
    for (i = 0; i < r->visited_count; ++i)
        if (r->visited[i] == key)
            return;
    if (r->visited_count < RPG_MAX_VISITED)
        r->visited[r->visited_count++] = key;
}

int rpg_visited(const Rpg *r, int x, int y)
{
    uint32_t key = ((uint32_t)(uint16_t)(x + 32768) << 16) | (uint16_t)(y + 32768), i;
    for (i = 0; i < r->visited_count; ++i)
        if (r->visited[i] == key)
            return 1;
    return 0;
}

void rpg_intervention(Rpg *r, int kind)
{
    uint32_t i, best = DRPG_NONE, cell = 0;
    float pos[3], rot, best_d = 1e30f;
    if (!r->host.locate || !r->host.teleport)
        return;
    r->host.locate(r->host.user, &cell, pos, &rot);
    for (i = 0; i < r->db->hdr->markers.count; ++i) {
        const DRpgMarker *m = &r->db->markers[i];
        float dx, dy, d;
        if (m->kind != kind)
            continue;
        dx = m->pos[0] - pos[0];
        dy = m->pos[1] - pos[1];
        /* interior markers count by their cell's position only when we are inside it */
        d = m->cell ? (m->cell == cell ? 0.0f : 1e20f) : dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    if (best == DRPG_NONE) {
        rpg_notify(r, "", "There is nowhere to be taken.");
        return;
    }
    r->host.teleport(r->host.user, r->db->markers[best].cell, r->db->markers[best].pos,
                     r->db->markers[best].rot_z);
}

/* ------------------------------------------------------------------------ */
/* character creation                                                       */
/* ------------------------------------------------------------------------ */
static void clear_spells(Rpg *r, uint32_t key)
{
    uint32_t cursor = 0, item;
    int32_t count;
    while (vm_item_next(r->vm, key ^ VM_SPELL_SALT, &cursor, &item, &count) >= 0)
        vm_item_add(r->vm, key ^ VM_SPELL_SALT, item, -count);
}

static void add_object_spells(Rpg *r, uint32_t key, const DRpgObject *o)
{
    uint32_t i;
    if (!o)
        return;
    for (i = 0; i < o->list2_count; ++i) {
        uint32_t sp = r->db->lists[o->list2_first + i].hash;
        if (!vm_item_count(r->vm, key ^ VM_SPELL_SALT, sp))
            vm_item_add(r->vm, key ^ VM_SPELL_SALT, sp, 1);
    }
}

void rpg_chargen(Rpg *r, const char *name, uint32_t race, int female, uint32_t cls, uint32_t sign)
{
    VmRef pc = vm_player_ref(r->vm);
    const DRpgRace *rc = rpgdb_race(r->db, race);
    int a, s, k, spec = rpg_class_spec(r, cls);
    uint32_t i;
    if (name && *name) {
        strncpy(r->vm->pc_name, name, sizeof(r->vm->pc_name) - 1);
        r->vm->pc_name[sizeof(r->vm->pc_name) - 1] = '\0';
    }
    r->race = race;
    r->cls = cls;
    r->sign = sign;
    r->female = female;
    r->vm->pc_race = race;
    r->vm->pc_class = cls;
    r->vm->pc_female = female;
    vm_seed(r->vm, &pc);
    for (a = 0; a < ATTR_COUNT; ++a) {
        float v = rc ? (float)rc->attribute[a][female ? 1 : 0] : 40.0f;
        if (rpg_class_attr(r, cls, 0) == a || rpg_class_attr(r, cls, 1) == a)
            v += 10.0f;
        vm_set_prop(r->vm, &pc, MWP_ATTR_STRENGTH + a, v);
        vm_set_prop(r->vm, &pc, MWP_ATTRDMG_STRENGTH + a, 0.0f);
        vm_set_prop(r->vm, &pc, MWP_LVLATTR_STRENGTH + a, 0.0f);
    }
    for (s = 0; s < SKILL_COUNT; ++s) {
        float v = 5.0f;
        int type = rpg_class_skill_type(r, cls, s);
        if (rc)
            for (k = 0; k < 7; ++k)
                if (rc->bonus_skill[k] == s)
                    v += (float)rc->bonus[k];
        if (type == 0)
            v += 25.0f;
        else if (type == 1)
            v += 10.0f;
        if (spec >= 0 && r->db->skills[s].specialization == spec)
            v += 5.0f;
        vm_set_prop(r->vm, &pc, MWP_SKILL_BLOCK + s, v);
        vm_set_prop(r->vm, &pc, MWP_SKILLDMG_BLOCK + s, 0.0f);
        vm_set_prop(r->vm, &pc, MWP_SKILLPROG_BLOCK + s, 0.0f);
    }
    vm_set_prop(r->vm, &pc, MWP_LEVEL, 1.0f);
    vm_set_prop(r->vm, &pc, MWP_LEVELPROG, 0.0f);
    r->level_pending = 0;
    vm_set_prop(r->vm, &pc, MWP_DYNBASE_HEALTH,
                floorf(0.5f * (vm_prop(r->vm, &pc, MWP_ATTR_STRENGTH) + vm_prop(r->vm, &pc, MWP_ATTR_ENDURANCE))));
    /* spells: race powers, birthsign, starting spells the character can cast */
    clear_spells(r, pc.key);
    add_object_spells(r, pc.key, rpgdb_object(r->db, race));
    add_object_spells(r, pc.key, rpgdb_object(r->db, sign));
    {
        uint32_t picked[64];
        int n = 0, max = rpg_gmst_i(r, "iAutoPCSpellMax", 100);
        float base_magicka = rpg_gmst_f(r, "fPCbaseMagickaMult", 1.0f) * vm_prop(r->vm, &pc, MWP_ATTR_INTELLIGENCE);
        float min_chance = rpg_gmst_f(r, "fAutoPCSpellChance", 80.0f);
        for (i = 0; i < r->db->hdr->spells.count && n < 64; ++i) {
            const DRpgSpell *sp = &r->db->spells[i];
            if (sp->kind != 0 || sp->type != 0 || !(sp->flags & 2))
                continue;
            if ((float)rpg_spell_cost(r, sp) > base_magicka)
                continue;
            if (rpg_spell_base_chance(r, &pc, sp) < min_chance)
                continue;
            picked[n++] = sp->hash;
        }
        /* keep the most expensive ones when over the limit */
        while (n > max) {
            int j, weakest = 0;
            for (j = 1; j < n; ++j)
                if (rpg_spell_cost(r, rpg_spell(r, picked[j])) < rpg_spell_cost(r, rpg_spell(r, picked[weakest])))
                    weakest = j;
            picked[weakest] = picked[--n];
        }
        for (k = 0; k < n; ++k)
            if (!vm_item_count(r->vm, pc.key ^ VM_SPELL_SALT, picked[k]))
                vm_item_add(r->vm, pc.key ^ VM_SPELL_SALT, picked[k], 1);
    }
    rpg_refresh_constants(r, &pc);
    vm_set_prop(r->vm, &pc, MWP_DYN_HEALTH, rpg_dyn_max(r, &pc, 0));
    rpg_recalc_player(r);
    vm_set_prop(r->vm, &pc, MWP_DYN_MAGICKA, rpg_dyn_max(r, &pc, 1));
    vm_set_prop(r->vm, &pc, MWP_DYN_FATIGUE, rpg_dyn_max(r, &pc, 2));
    r->chargen_done = 1;
}
