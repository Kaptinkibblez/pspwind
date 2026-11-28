/*
 * rpg_trade.c - barter, services (training, spells, repair, travel,
 * persuasion) and crafting (alchemy, spellmaking, enchanting).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rpg_internal.h"

/* ------------------------------------------------------------------------ */
/* barter                                                                   */
/* ------------------------------------------------------------------------ */
uint32_t rpg_services(Rpg *r, const VmRef *npc)
{
    const DRpgObject *o = rpg_actor_object(r, npc);
    const DRpgActor *a = rpgdb_actor(r->db, o);
    return a ? a->services : 0;
}

int rpg_merchant_buys(Rpg *r, const VmRef *npc, const DRpgObject *o)
{
    uint32_t s = rpg_services(r, npc);
    if (!o || o->hash == pw_hash_name(RPG_GOLD))
        return 0;
    if (o->link[0] && (o->type == DRPG_OT_WEAP || o->type == DRPG_OT_ARMO || o->type == DRPG_OT_CLOT) &&
        (s & RPG_SVC_MAGICITEMS))
        return 1;
    switch (o->type) {
    case DRPG_OT_WEAP: return (s & RPG_SVC_WEAPON) != 0;
    case DRPG_OT_ARMO: return (s & RPG_SVC_ARMOR) != 0;
    case DRPG_OT_CLOT: return (s & RPG_SVC_CLOTHING) != 0;
    case DRPG_OT_BOOK: return (s & RPG_SVC_BOOKS) != 0;
    case DRPG_OT_INGR: return (s & RPG_SVC_INGREDIENTS) != 0;
    case DRPG_OT_LOCK: return (s & RPG_SVC_PICKS) != 0;
    case DRPG_OT_PROB: return (s & RPG_SVC_PROBES) != 0;
    case DRPG_OT_LIGH: return (s & RPG_SVC_LIGHTS) != 0;
    case DRPG_OT_APPA: return (s & RPG_SVC_APPARATUS) != 0;
    case DRPG_OT_REPA: return (s & RPG_SVC_REPAIRITEMS) != 0;
    case DRPG_OT_MISC: return (s & RPG_SVC_MISC) != 0;
    case DRPG_OT_ALCH: return (s & RPG_SVC_POTIONS) != 0;
    default: return 0;
    }
}

int rpg_price(Rpg *r, const VmRef *npc, int base, int buying)
{
    const DRpgObject *o = rpg_actor_object(r, npc);
    VmRef pc = vm_player_ref(r->vm);
    float a, b, c, d, e, f, pc_term, npc_term, term;
    int offer;
    if (base == 0 || !o || o->type != DRPG_OT_NPC)
        return base;
    a = PW_MIN(rpg_skill(r, &pc, SKILL_MERCANTILE), 100.0f);
    b = PW_MIN(0.1f * rpg_attr(r, &pc, ATTR_LUCK), 10.0f);
    c = PW_MIN(0.2f * rpg_attr(r, &pc, ATTR_PERSONALITY), 10.0f);
    d = PW_MIN(rpg_skill(r, npc, SKILL_MERCANTILE), 100.0f);
    e = PW_MIN(0.1f * rpg_attr(r, npc, ATTR_LUCK), 10.0f);
    f = PW_MIN(0.2f * rpg_attr(r, npc, ATTR_PERSONALITY), 10.0f);
    pc_term = ((float)rpg_disposition(r, npc) - 50.0f + a + b + c) * rpg_fatigue_term(r, &pc);
    npc_term = (d + e + f) * rpg_fatigue_term(r, npc);
    term = buying ? 0.01f * (100.0f - 0.5f * (pc_term - npc_term)) : 0.01f * (50.0f - 0.5f * (npc_term - pc_term));
    offer = (int)((float)base * term);
    return PW_MAX(1, offer);
}

int rpg_merchant_gold(Rpg *r, const VmRef *npc)
{
    const DRpgActor *a = rpgdb_actor(r->db, rpg_actor_object(r, npc));
    float day = (float)r->vm->days_passed;
    if (vm_prop(r->vm, npc, MWP_RESTOCK_DAY) != day + 1.0f) {
        /* merchants get their gold back once a day */
        vm_set_prop(r->vm, npc, MWP_RESTOCK_DAY, day + 1.0f);
        vm_set_prop(r->vm, npc, MWP_GOLDPOOL, a ? (float)a->gold : 0.0f);
    }
    return (int)vm_prop(r->vm, npc, MWP_GOLDPOOL);
}

static void pay(Rpg *r, uint32_t from, uint32_t to_npc_key, const VmRef *npc, int amount)
{
    vm_item_add(r->vm, from, pw_hash_name(RPG_GOLD), -amount);
    if (npc)
        vm_set_prop(r->vm, npc, MWP_GOLDPOOL, vm_prop(r->vm, npc, MWP_GOLDPOOL) + (float)amount);
    PW_UNUSED(to_npc_key);
}

int rpg_buy(Rpg *r, const VmRef *npc, int stack, int count, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    VmItem *it = vm_item_at(r->vm, stack);
    int price;
    if (!it || it->owner != npc->key || it->count < count || count <= 0)
        return 0;
    rpg_merchant_gold(r, npc);
    price = rpg_price(r, npc, rpg_item_value(r, it), 1) * count;
    if (rpg_gold(r, &pc) < price) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sBarterDialog1", "You do not have enough gold."));
        return 0;
    }
    pay(r, pc.key, npc->key, npc, price);
    rpg_move_stack(r, stack, pc.key, count);
    rpg_skill_use(r, SKILL_MERCANTILE, 0, 1.0f);
    snprintf(msg, cap, "Bought for %d gold.", price);
    return price;
}

int rpg_sell(Rpg *r, const VmRef *npc, int stack, int count, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    VmItem *it = vm_item_at(r->vm, stack);
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    int price, gold;
    if (!it || it->owner != pc.key || it->count < count || count <= 0)
        return 0;
    if (!rpg_merchant_buys(r, npc, o)) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sBarterDialog4", "I don't trade in that."));
        return 0;
    }
    if (r->host.refuses && r->host.refuses(r->host.user, npc, it->item)) {
        snprintf(msg, cap, "%s", "I won't buy back what was stolen from me.");
        return 0;
    }
    gold = rpg_merchant_gold(r, npc);
    price = rpg_price(r, npc, rpg_item_value(r, it), 0) * count;
    if (gold < price) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sBarterDialog2", "The merchant does not have enough gold."));
        return 0;
    }
    rpg_move_stack(r, stack, npc->key, count);
    vm_item_add(r->vm, pc.key, pw_hash_name(RPG_GOLD), price);
    vm_set_prop(r->vm, npc, MWP_GOLDPOOL, (float)(gold - price));
    rpg_skill_use(r, SKILL_MERCANTILE, 0, 1.0f);
    snprintf(msg, cap, "Sold for %d gold.", price);
    return price;
}

/* ------------------------------------------------------------------------ */
/* training                                                                 */
/* ------------------------------------------------------------------------ */
int rpg_trainer_skills(Rpg *r, const VmRef *npc, int out[3])
{
    int best[3] = { -1, -1, -1 }, s, k;
    float val[3] = { -1.0f, -1.0f, -1.0f };
    for (s = 0; s < SKILL_COUNT; ++s) {
        float v = vm_prop(r->vm, npc, MWP_SKILL_BLOCK + s);
        for (k = 0; k < 3; ++k) {
            if (v > val[k]) {
                int j;
                for (j = 2; j > k; --j) {
                    val[j] = val[j - 1];
                    best[j] = best[j - 1];
                }
                val[k] = v;
                best[k] = s;
                break;
            }
        }
    }
    for (k = 0; k < 3; ++k)
        out[k] = best[k];
    return 3;
}

int rpg_training_price(Rpg *r, const VmRef *npc, int skill)
{
    VmRef pc = vm_player_ref(r->vm);
    int base = (int)(vm_prop(r->vm, &pc, MWP_SKILL_BLOCK + skill) * (float)rpg_gmst_i(r, "iTrainingMod", 10));
    return rpg_price(r, npc, base, 1);
}

int rpg_train(Rpg *r, const VmRef *npc, int skill, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    int price = rpg_training_price(r, npc, skill), gov;
    float pcs = vm_prop(r->vm, &pc, MWP_SKILL_BLOCK + skill);
    if (rpg_gold(r, &pc) < price) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sBarterDialog1", "You do not have enough gold."));
        return 0;
    }
    if (vm_prop(r->vm, npc, MWP_SKILL_BLOCK + skill) <= pcs) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sServiceTrainingWords",
                                                       "You know more about that than I do."));
        return 0;
    }
    gov = r->db->skills[skill].attribute;
    if (pcs >= rpg_attr(r, &pc, gov)) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sNotifyMessage17",
                                                       "Your attribute is too low to advance this skill."));
        return 0;
    }
    pay(r, pc.key, npc->key, npc, price);
    rpg_skill_raise(r, skill, 1);
    rpg_rest(r, 2, 0);
    snprintf(msg, cap, "Trained %s.", rpg_skill_name(r, skill));
    return 1;
}

/* ------------------------------------------------------------------------ */
/* spells for sale                                                          */
/* ------------------------------------------------------------------------ */
int rpg_spells_for_sale(Rpg *r, const VmRef *npc, uint32_t *out, int max)
{
    VmRef pc = vm_player_ref(r->vm);
    uint32_t cursor = 0, item;
    int32_t count;
    int n = 0;
    vm_seed(r->vm, npc);
    while (n < max && vm_item_next(r->vm, npc->key ^ VM_SPELL_SALT, &cursor, &item, &count) >= 0) {
        const DRpgSpell *s = rpg_spell(r, item);
        if (!s || s->kind != 0 || s->type != 0)
            continue;
        if (vm_item_count(r->vm, pc.key ^ VM_SPELL_SALT, item))
            continue;
        out[n++] = item;
    }
    return n;
}

int rpg_spell_price(Rpg *r, const VmRef *npc, uint32_t spell)
{
    const DRpgSpell *s = rpg_spell(r, spell);
    int base = (int)((float)rpg_spell_cost(r, s) * rpg_gmst_f(r, "fSpellValueMult", 10.0f));
    return rpg_price(r, npc, base, 1);
}

int rpg_buy_spell(Rpg *r, const VmRef *npc, uint32_t spell, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    int price = rpg_spell_price(r, npc, spell);
    if (vm_item_count(r->vm, pc.key ^ VM_SPELL_SALT, spell))
        return 0;
    if (rpg_gold(r, &pc) < price) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sBarterDialog1", "You do not have enough gold."));
        return 0;
    }
    pay(r, pc.key, npc->key, npc, price);
    vm_item_add(r->vm, pc.key ^ VM_SPELL_SALT, spell, 1);
    r->dirty_constants = 1;
    snprintf(msg, cap, "You learned %s.", rpg_spell_name(r, spell));
    return 1;
}

/* ------------------------------------------------------------------------ */
/* repair                                                                   */
/* ------------------------------------------------------------------------ */
int rpg_repair_price(Rpg *r, const VmRef *npc, int stack)
{
    const VmItem *it = vm_item_at(r->vm, stack);
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    int mx = rpg_item_health_max(r, o), cur, x;
    float p, q;
    if (!o || mx <= 0 || it->health == VM_FULL || it->health >= mx)
        return 0;
    if (o->type != DRPG_OT_WEAP && o->type != DRPG_OT_ARMO)
        return 0;
    cur = it->health;
    p = (float)PW_MAX(1, o->value);
    q = (float)PW_MAX(1, (int)((float)mx / p));
    x = (int)((float)(mx - cur) / q);
    x = (int)(rpg_gmst_f(r, "fRepairMult", 1.0f) * (float)x);
    return rpg_price(r, npc, PW_MAX(1, x), 1);
}

int rpg_repair_service(Rpg *r, const VmRef *npc, int stack, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    VmItem *it = vm_item_at(r->vm, stack);
    int price = rpg_repair_price(r, npc, stack);
    if (!price || !it || it->owner != pc.key)
        return 0;
    if (rpg_gold(r, &pc) < price) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sBarterDialog1", "You do not have enough gold."));
        return 0;
    }
    pay(r, pc.key, npc->key, npc, price);
    rpg_set_condition(r, stack, rpg_item_health_max(r, rpg_object(r, it->item)));
    snprintf(msg, cap, "Repaired.");
    return 1;
}

int rpg_repair_self(Rpg *r, int tool_stack, int stack, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    VmItem *tool = vm_item_at(r->vm, tool_stack), *it = vm_item_at(r->vm, stack);
    const DRpgObject *to = tool ? rpg_object(r, tool->item) : NULL;
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    float quality, x;
    int roll, mx, uses, health;
    if (!to || to->type != DRPG_OT_REPA || !o || tool->owner != pc.key || it->owner != pc.key)
        return 0;
    mx = rpg_item_health_max(r, o);
    if ((o->type != DRPG_OT_WEAP && o->type != DRPG_OT_ARMO) || mx <= 0 || it->health == VM_FULL || it->health >= mx) {
        snprintf(msg, cap, "That does not need repairing.");
        return 0;
    }
    memcpy(&quality, &to->stat[1], 4);
    x = (0.1f * rpg_attr(r, &pc, ATTR_STRENGTH) + 0.1f * rpg_attr(r, &pc, ATTR_LUCK) +
         rpg_skill(r, &pc, SKILL_ARMORER)) * rpg_fatigue_term(r, &pc);
    roll = rpg_roll100(r);
    health = it->health;
    if ((float)roll <= x) {
        int y = PW_MAX(1, (int)(rpg_gmst_f(r, "fRepairAmountMult", 3.0f) * quality * (float)roll));
        rpg_set_condition(r, stack, PW_MIN(mx, health + y));
        rpg_skill_use(r, SKILL_ARMORER, 0, 1.0f);
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sRepairSuccess", "You have repaired the item."));
    } else {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sRepairFailed", "You failed to repair the item."));
    }
    tool = vm_item_at(r->vm, tool_stack);
    uses = (tool->health == VM_FULL ? to->stat[0] : tool->health) - 1;
    if (uses <= 0)
        vm_item_add_state(r->vm, pc.key, to->hash, -1, tool->health, tool->charge, tool->soul);
    else
        rpg_set_condition(r, tool_stack, uses);
    return (float)roll <= x;
}

/* ------------------------------------------------------------------------ */
/* travel                                                                   */
/* ------------------------------------------------------------------------ */
int rpg_travel_count(Rpg *r, const VmRef *npc)
{
    const DRpgObject *o = rpg_actor_object(r, npc);
    return (o && o->type == DRPG_OT_NPC) ? o->stat[2] : 0;
}

static const DRpgTravel *travel_at(Rpg *r, const VmRef *npc, int index)
{
    const DRpgObject *o = rpg_actor_object(r, npc);
    if (!o || o->type != DRPG_OT_NPC || index < 0 || index >= o->stat[2])
        return NULL;
    return &r->db->travel[o->stat[1] + index];
}

int rpg_travel_price(Rpg *r, const VmRef *npc, int index, int npc_exterior, const float npc_pos[3])
{
    const DRpgTravel *t = travel_at(r, npc, index);
    float price;
    if (!t)
        return 0;
    if (!npc_exterior) {
        price = rpg_gmst_f(r, "fMagesGuildTravel", 10.0f);
    } else {
        float dx = t->pos[0] - npc_pos[0], dy = t->pos[1] - npc_pos[1], dz = t->pos[2] - npc_pos[2];
        float mult = rpg_gmst_f(r, "fTravelMult", 4000.0f);
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        price = mult != 0.0f ? d / mult : d;
    }
    return rpg_price(r, npc, PW_MAX(1, (int)price), 1);
}

int rpg_travel(Rpg *r, const VmRef *npc, int index, int price, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    const DRpgTravel *t = travel_at(r, npc, index);
    uint32_t cell = 0;
    float pos[3], rot;
    if (!t || !r->host.teleport)
        return 0;
    if (rpg_gold(r, &pc) < price) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sBarterDialog1", "You do not have enough gold."));
        return 0;
    }
    pay(r, pc.key, npc->key, npc, price);
    if (r->host.locate) {
        r->host.locate(r->host.user, &cell, pos, &rot);
        if (!cell) {
            float dx = t->pos[0] - pos[0], dy = t->pos[1] - pos[1];
            int hours = (int)(sqrtf(dx * dx + dy * dy) / rpg_gmst_f(r, "fTravelTimeMult", 16000.0f));
            if (hours > 0)
                rpg_rest(r, hours, 0);
        }
    }
    r->host.teleport(r->host.user, t->cell, t->pos, t->rot_z);
    snprintf(msg, cap, "You travel.");
    return 1;
}

/* ------------------------------------------------------------------------ */
/* persuasion                                                               */
/* ------------------------------------------------------------------------ */
static void persuasion_ratings(Rpg *r, const VmRef *ref, int player, float out[3])
{
    float pers = rpg_attr(r, ref, ATTR_PERSONALITY) / rpg_gmst_f(r, "fPersonalityMod", 5.0f);
    float luck = rpg_attr(r, ref, ATTR_LUCK) / rpg_gmst_f(r, "fLuckMod", 10.0f);
    float rep = vm_prop(r->vm, ref, MWP_REPUTATION) * rpg_gmst_f(r, "fReputationMod", 1.0f);
    float fat = rpg_fatigue_term(r, ref);
    float level = vm_prop(r->vm, ref, MWP_LEVEL) * rpg_gmst_f(r, "fLevelMod", 5.0f);
    float speech = rpg_skill(r, ref, SKILL_SPEECHCRAFT), merc = rpg_skill(r, ref, SKILL_MERCANTILE);
    out[0] = (rep + luck + pers + speech) * fat;
    if (player) {
        out[1] = out[0] + level;
        out[2] = (merc + luck + pers) * fat;
    } else {
        out[1] = (level + rep + luck + pers + speech) * fat;
        out[2] = (merc + rep + luck + pers) * fat;
    }
}

int rpg_persuade(Rpg *r, const VmRef *npc, int type, int *success)
{
    VmRef pc = vm_player_ref(r->vm);
    float npcr[3], pcr[3], d, t1, t2, t3, x = 0.0f, y = 0.0f, bribe = 0.0f;
    float min_chance = (float)rpg_gmst_i(r, "iPerMinChance", 5), min_change = (float)rpg_gmst_i(r, "iPerMinChange", 5);
    float die = rpg_gmst_f(r, "fPerDieRollMult", 0.15f), temp_mult = rpg_gmst_f(r, "fPerTempMult", 5.0f);
    int cur = rpg_disposition(r, npc), roll, temp, perm;
    static const int bribes[3] = { 10, 100, 1000 };
    *success = 0;
    if (type >= RPG_PERSUADE_BRIBE10) {
        int amount = bribes[type - RPG_PERSUADE_BRIBE10];
        if (rpg_gold(r, &pc) < amount)
            return 0;
        pay(r, pc.key, npc->key, npc, amount);
        bribe = rpg_gmst_f(r, type == RPG_PERSUADE_BRIBE10 ? "fBribe10Mod"
                              : type == RPG_PERSUADE_BRIBE100 ? "fBribe100Mod" : "fBribe1000Mod",
                           type == RPG_PERSUADE_BRIBE10 ? 35.0f : type == RPG_PERSUADE_BRIBE100 ? 75.0f : 150.0f);
    }
    persuasion_ratings(r, npc, 0, npcr);
    persuasion_ratings(r, &pc, 1, pcr);
    d = 1.0f - 0.02f * fabsf((float)cur - 50.0f);
    t1 = d * (pcr[0] - npcr[0] + 50.0f);
    t2 = d * (pcr[1] - npcr[1] + 50.0f);
    t3 = d * (pcr[2] - npcr[2] + 50.0f) + bribe;
    roll = rpg_roll100(r);
    if (type == RPG_PERSUADE_ADMIRE) {
        float c;
        t1 = PW_MAX(min_chance, t1);
        *success = (float)roll <= t1;
        c = floorf(die * (t1 - (float)roll));
        x = *success ? PW_MAX(min_change, c) : c;
    } else if (type == RPG_PERSUADE_INTIMIDATE) {
        float rr, c;
        t2 = PW_MAX(min_chance, t2);
        *success = (float)roll <= t2;
        rr = (float)roll != t2 ? floorf(t2 - (float)roll) : 1.0f;
        if (*success) {
            float s = floorf(rr * die * temp_mult);
            vm_set_prop(r->vm, npc, MWP_AI_FLEE,
                        PW_CLAMP(vm_prop(r->vm, npc, MWP_AI_FLEE) + PW_MAX(min_change, s), 0.0f, 100.0f));
            vm_set_prop(r->vm, npc, MWP_AI_FIGHT,
                        PW_CLAMP(vm_prop(r->vm, npc, MWP_AI_FIGHT) + PW_MIN(-min_change, -s), 0.0f, 100.0f));
        }
        c = -fabsf(floorf(rr * die));
        if (*success) {
            if (fabsf(c) < min_change) {
                x = min_change;
                y = x;
            } else {
                x = -floorf(c * temp_mult);
                y = c;
            }
        } else {
            x = floorf(c * temp_mult);
            y = c;
        }
    } else if (type == RPG_PERSUADE_TAUNT) {
        float c;
        t1 = PW_MAX(min_chance, t1);
        *success = (float)roll <= t1;
        c = fabsf(floorf(t1 - (float)roll));
        if (*success) {
            float s = c * die * temp_mult;
            vm_set_prop(r->vm, npc, MWP_AI_FLEE,
                        PW_CLAMP(vm_prop(r->vm, npc, MWP_AI_FLEE) + PW_MIN(-min_change, -s), 0.0f, 100.0f));
            vm_set_prop(r->vm, npc, MWP_AI_FIGHT,
                        PW_CLAMP(vm_prop(r->vm, npc, MWP_AI_FIGHT) + PW_MAX(min_change, s), 0.0f, 100.0f));
        }
        x = floorf(-c * die);
        if (*success && fabsf(x) < min_change)
            x = -min_change;
    } else {
        float c;
        t3 = PW_MAX(min_chance, t3);
        *success = (float)roll <= t3;
        c = floorf((t3 - (float)roll) * die);
        x = *success ? PW_MAX(min_change, c) : c;
    }
    if (type == RPG_PERSUADE_INTIMIDATE) {
        temp = (int)x;
        if (cur + temp > 100)
            temp = 100 - cur;
        else if (cur + temp < 0)
            temp = -cur;
        perm = *success ? -(int)((float)temp / temp_mult) : (int)y;
    } else {
        temp = (int)(x * temp_mult);
        if (cur + temp > 100)
            temp = 100 - cur;
        else if (cur + temp < 0)
            temp = -cur;
        perm = (int)((float)temp / temp_mult);
    }
    vm_set_prop(r->vm, npc, MWP_DISPTEMP, vm_prop(r->vm, npc, MWP_DISPTEMP) + (float)temp);
    r->persuade_key = npc->key;
    r->persuade_perm += perm;
    if (*success)
        rpg_skill_use(r, type >= RPG_PERSUADE_BRIBE10 ? SKILL_MERCANTILE : SKILL_SPEECHCRAFT,
                      type >= RPG_PERSUADE_BRIBE10 ? 1 : 0, 1.0f);
    return temp;
}

void rpg_end_conversation(Rpg *r, const VmRef *npc)
{
    if (r->persuade_key == npc->key && r->persuade_perm)
        vm_set_prop(r->vm, npc, MWP_DISPOSITION,
                    PW_CLAMP(vm_prop(r->vm, npc, MWP_DISPOSITION) + (float)r->persuade_perm, 0.0f, 100.0f));
    vm_set_prop(r->vm, npc, MWP_DISPTEMP, 0.0f);
    r->persuade_perm = 0;
    r->persuade_key = 0;
}

/* ------------------------------------------------------------------------ */
/* player-made objects                                                      */
/* ------------------------------------------------------------------------ */
static RpgCustom *new_custom(Rpg *r, int kind, const char *name)
{
    RpgCustom *c;
    char id[32];
    if (r->custom_count >= RPG_MAX_CUSTOM) {
        rpg_notify(r, "", "No room for more custom items.");
        return NULL;
    }
    c = &r->custom[r->custom_count];
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->kind = (uint8_t)kind;
    snprintf(id, sizeof(id), "pw_custom_%u", (unsigned)r->custom_count);
    c->obj.hash = pw_hash_name(id);
    c->spell.hash = c->obj.hash;
    c->obj.name = RPG_CUSTOM_NAME | r->custom_count;
    c->obj.script = DRPG_NONE16;
    strncpy(c->name, name && *name ? name : "Custom", sizeof(c->name) - 1);
    r->custom_count++;
    return c;
}

/* ------------------------------------------------------------------------ */
/* alchemy                                                                  */
/* ------------------------------------------------------------------------ */
static float alchemy_factor(Rpg *r)
{
    VmRef pc = vm_player_ref(r->vm);
    return rpg_skill(r, &pc, SKILL_ALCHEMY) + 0.1f * rpg_attr(r, &pc, ATTR_INTELLIGENCE) +
           0.1f * rpg_attr(r, &pc, ATTR_LUCK);
}

static float tool_quality(Rpg *r, int stack)
{
    const VmItem *it = stack >= 0 ? vm_item_at(r->vm, stack) : NULL;
    const DRpgObject *o = it && it->count > 0 ? rpg_object(r, it->item) : NULL;
    float q;
    if (!o || o->type != DRPG_OT_APPA)
        return 0.0f;
    memcpy(&q, &o->stat[1], 4);
    return q;
}

static void apply_tools(Rpg *r, const RpgAlchemy *a, uint32_t flags, float *value)
{
    int magnitude = !(flags & DRPG_MF_NO_MAGNITUDE), duration = !(flags & DRPG_MF_NO_DURATION);
    int negative = (flags & DRPG_MF_HARMFUL) != 0;
    float tq = tool_quality(r, negative ? a->tools[1] : a->tools[3]), cq = tool_quality(r, a->tools[2]);
    int setup = (tq > 0.0f && cq > 0.0f) ? 1 : tq > 0.0f ? 2 : cq > 0.0f ? 3 : 0;
    float quality = 1.0f;
    switch (setup) {
    case 1:
        quality = negative ? 2.0f * tq + 3.0f * cq
                  : (magnitude && duration ? 2.0f * tq + cq : 2.0f / 3.0f * (tq + cq) + 0.5f);
        break;
    case 2:
        quality = negative ? 1.0f + tq : (magnitude && duration ? tq : tq + 0.5f);
        break;
    case 3:
        quality = magnitude && duration ? cq : cq + 0.5f;
        break;
    default:
        return;
    }
    if (setup == 3 || !negative)
        *value += quality;
    else if (quality != 0.0f)
        *value /= quality;
}

int rpg_alchemy_preview(Rpg *r, const RpgAlchemy *a, DRpgEffect *out, int max, int *value)
{
    DRpgEffect cand[16];
    int counts[16], nc = 0, i, k, n = 0, ingredients = 0;
    float x;
    *value = 0;
    if (tool_quality(r, a->tools[0]) <= 0.0f)
        return 0;
    for (i = 0; i < 4; ++i) {
        const VmItem *it = a->stacks[i] >= 0 ? vm_item_at(r->vm, a->stacks[i]) : NULL;
        const DRpgObject *o = it && it->count > 0 ? rpg_object(r, it->item) : NULL;
        int ne, j;
        const DRpgEffect *e;
        if (!o || o->type != DRPG_OT_INGR)
            continue;
        ingredients++;
        e = rpg_object_effects(r, o, &ne);
        for (j = 0; j < ne; ++j) {
            for (k = 0; k < nc; ++k)
                if (cand[k].effect == e[j].effect && cand[k].skill == e[j].skill && cand[k].attribute == e[j].attribute)
                    break;
            if (k == nc && nc < 16) {
                cand[nc] = e[j];
                counts[nc++] = 0;
            }
            if (k < 16)
                counts[k]++;
        }
    }
    if (ingredients < 2)
        return 0;
    x = alchemy_factor(r) * tool_quality(r, a->tools[0]) * rpg_gmst_f(r, "fPotionStrengthMult", 0.5f);
    *value = (int)(x * rpg_gmst_f(r, "iAlchemyMod", 2.0f));
    for (k = 0; k < nc && n < max; ++k) {
        const DRpgMgef *m = rpgdb_mgef(r->db, cand[k].effect);
        float mag, dur;
        if (counts[k] < 2 || !m || m->base_cost <= 0.0f)
            continue;
        mag = (m->flags & DRPG_MF_NO_MAGNITUDE) ? 1.0f
              : (x / rpg_gmst_f(r, "fPotionT1MagMult", 1.5f)) / m->base_cost;
        dur = (m->flags & DRPG_MF_NO_DURATION) ? 1.0f
              : (x / rpg_gmst_f(r, "fPotionT1DurMult", 0.5f)) / m->base_cost;
        if (!(m->flags & DRPG_MF_NO_MAGNITUDE))
            apply_tools(r, a, m->flags, &mag);
        if (!(m->flags & DRPG_MF_NO_DURATION))
            apply_tools(r, a, m->flags, &dur);
        mag = roundf(mag);
        dur = roundf(dur);
        if (mag > 0.0f && dur > 0.0f) {
            out[n] = cand[k];
            out[n].range = RPG_RANGE_SELF;
            out[n].area = 0;
            out[n].duration = (uint16_t)PW_MIN(dur, 65535.0f);
            out[n].mag_min = out[n].mag_max = (uint16_t)PW_MIN(mag, 65535.0f);
            n++;
        }
    }
    return n;
}

int rpg_alchemy_brew(Rpg *r, const RpgAlchemy *a, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    DRpgEffect e[RPG_CUSTOM_EFFECTS];
    int value, n = rpg_alchemy_preview(r, a, e, RPG_CUSTOM_EFFECTS, &value), i, ok;
    RpgCustom *c = NULL;
    uint32_t k;
    /* the ingredients are used up whatever happens */
    for (i = 0; i < 4; ++i) {
        const VmItem *it = a->stacks[i] >= 0 ? vm_item_at(r->vm, a->stacks[i]) : NULL;
        if (it && it->count > 0)
            vm_item_add(r->vm, pc.key, it->item, -1);
    }
    if (n == 0) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sNotifyMessage8", "Your potion failed."));
        return 0;
    }
    ok = alchemy_factor(r) >= (float)rpg_roll100(r);
    if (!ok) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sNotifyMessage8", "Your potion failed."));
        return 0;
    }
    /* identical brews stack */
    for (k = 0; k < r->custom_count; ++k) {
        RpgCustom *x = &r->custom[k];
        if (x->kind == 0 && x->obj.type == DRPG_OT_ALCH && x->obj.list2_count == (uint16_t)n &&
            x->obj.value == value && memcmp(x->effects, e, (size_t)n * sizeof(DRpgEffect)) == 0) {
            c = x;
            break;
        }
    }
    if (!c) {
        char name[32];
        if (a->name[0])
            snprintf(name, sizeof(name), "%s", a->name);
        else
            snprintf(name, sizeof(name), "Potion of %s", rpg_effect_name(r, e[0].effect));
        c = new_custom(r, 0, name);
        if (!c)
            return 0;
        c->obj.type = DRPG_OT_ALCH;
        c->obj.weight = 1.0f;
        c->obj.value = value;
        c->obj.list2_count = (uint16_t)n;
        memcpy(c->effects, e, (size_t)n * sizeof(DRpgEffect));
    }
    vm_item_add(r->vm, pc.key, c->obj.hash, 1);
    rpg_skill_use(r, SKILL_ALCHEMY, 0, 1.0f);
    snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sPotionSuccess", "You created a potion."));
    return 1;
}

/* ------------------------------------------------------------------------ */
/* spellmaking                                                              */
/* ------------------------------------------------------------------------ */
int rpg_known_effects(Rpg *r, uint8_t *mask)
{
    VmRef pc = vm_player_ref(r->vm);
    uint32_t cursor = 0, item;
    int32_t count;
    int n = 0, i;
    memset(mask, 0, RPGDB_EFFECTS);
    while (vm_item_next(r->vm, pc.key ^ VM_SPELL_SALT, &cursor, &item, &count) >= 0) {
        const DRpgSpell *s = rpg_spell(r, item);
        int ne;
        const DRpgEffect *e;
        if (!s || s->kind != 0 || s->type != 0)
            continue;
        e = rpg_spell_effects(r, s, &ne);
        for (i = 0; i < ne; ++i) {
            const DRpgMgef *m = rpgdb_mgef(r->db, e[i].effect);
            if (m && (m->flags & DRPG_MF_SPELLMAKING) && !mask[e[i].effect]) {
                mask[e[i].effect] = 1;
                n++;
            }
        }
    }
    return n;
}

int rpg_spellmaking_price(Rpg *r, const VmRef *npc, const DRpgEffect *e, int n, int *cost)
{
    float c = rpg_effects_cost(r, e, n, 1);
    *cost = (int)roundf(c);
    return rpg_price(r, npc, PW_MAX(1, (int)(c * rpg_gmst_f(r, "fSpellMakingValueMult", 7.0f))), 1);
}

int rpg_make_spell(Rpg *r, const VmRef *npc, const DRpgEffect *e, int n, const char *name, char *msg,
                   uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    uint8_t known[RPGDB_EFFECTS];
    int cost, price, i;
    RpgCustom *c;
    if (n <= 0 || n > RPG_CUSTOM_EFFECTS)
        return 0;
    rpg_known_effects(r, known);
    for (i = 0; i < n; ++i)
        if (e[i].effect >= RPGDB_EFFECTS || !known[e[i].effect]) {
            snprintf(msg, cap, "You do not know that effect.");
            return 0;
        }
    price = rpg_spellmaking_price(r, npc, e, n, &cost);
    if (rpg_gold(r, &pc) < price) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sBarterDialog1", "You do not have enough gold."));
        return 0;
    }
    c = new_custom(r, 1, name);
    if (!c)
        return 0;
    pay(r, pc.key, npc->key, npc, price);
    c->spell.kind = 0;
    c->spell.type = 0;
    c->spell.cost = cost;
    c->spell.effect_count = (uint16_t)n;
    c->spell.name = RPG_CUSTOM_NAME | (uint32_t)(c - r->custom);
    memcpy(c->effects, e, (size_t)n * sizeof(DRpgEffect));
    vm_item_add(r->vm, pc.key ^ VM_SPELL_SALT, c->spell.hash, 1);
    snprintf(msg, cap, "You created %s.", c->name);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* enchanting                                                               */
/* ------------------------------------------------------------------------ */
static int item_enchant_capacity(const DRpgObject *o)
{
    if (!o || o->link[0])
        return 0;
    switch (o->type) {
    case DRPG_OT_WEAP: return o->stat[4];
    case DRPG_OT_ARMO: return o->stat[2];
    case DRPG_OT_CLOT: return o->stat[2];
    case DRPG_OT_BOOK: return o->stat[2];
    default: return 0;
    }
}

int rpg_enchant_points(Rpg *r, const RpgEnchant *e, int *capacity, int *charge, float *chance)
{
    VmRef pc = vm_player_ref(r->vm);
    const VmItem *it = vm_item_at(r->vm, e->item_stack), *gem = vm_item_at(r->vm, e->gem_stack);
    const DRpgObject *o = it ? rpg_object(r, it->item) : NULL;
    DRpgEffect tmp[RPG_CUSTOM_EFFECTS];
    float points, mult;
    int i;
    for (i = 0; i < e->count && i < RPG_CUSTOM_EFFECTS; ++i) {
        tmp[i] = e->effects[i];
        if (e->type == 3)
            tmp[i].duration = (uint16_t)rpg_gmst_f(r, "fEnchantmentConstantDurationMult", 100.0f);
    }
    points = rpg_effects_cost(r, tmp, PW_MIN(e->count, RPG_CUSTOM_EFFECTS), 0);
    *capacity = (int)((float)item_enchant_capacity(o) * rpg_gmst_f(r, "fEnchantmentMult", 0.1f));
    *charge = 0;
    if (gem && gem->soul) {
        const DRpgActor *a = rpgdb_actor(r->db, rpgdb_object(r->db, gem->soul));
        *charge = a ? a->soul : 0;
    }
    mult = rpg_gmst_f(r, "fEnchantmentChanceMult", 3.0f) *
           (e->type == 3 ? rpg_gmst_f(r, "fEnchantmentConstantChanceMult", 0.5f) : 1.0f);
    *chance = rpg_skill(r, &pc, SKILL_ENCHANT) + 0.25f * rpg_attr(r, &pc, ATTR_INTELLIGENCE) +
              0.125f * rpg_attr(r, &pc, ATTR_LUCK) - (mult != 0.0f ? 7.5f / mult : 0.0f) * points;
    return (int)ceilf(points);
}

int rpg_enchant(Rpg *r, const VmRef *npc, const RpgEnchant *e, char *msg, uint32_t cap)
{
    VmRef pc = vm_player_ref(r->vm);
    VmItem *it = vm_item_at(r->vm, e->item_stack), *gem = vm_item_at(r->vm, e->gem_stack);
    const DRpgObject *base = it ? rpg_object(r, it->item) : NULL;
    int capacity, charge, points, price = 0;
    float chance;
    RpgCustom *en, *obj;
    uint32_t gem_item;
    if (!base || !gem || it->owner != pc.key || gem->owner != pc.key || e->count <= 0 || !gem->soul) {
        snprintf(msg, cap, "Choose an item, a filled soul gem and effects.");
        return 0;
    }
    points = rpg_enchant_points(r, e, &capacity, &charge, &chance);
    if (points > capacity || capacity <= 0) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sNotifyMessage29", "The item cannot hold that much."));
        return 0;
    }
    if (e->type != 3 && points > charge) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sNotifyMessage32", "The soul is too weak."));
        return 0;
    }
    if (npc) {
        price = rpg_price(r, npc, (int)((float)points * rpg_gmst_f(r, "fEnchantmentValueMult", 1000.0f) / 100.0f), 1);
        if (rpg_gold(r, &pc) < price) {
            snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sBarterDialog1", "You do not have enough gold."));
            return 0;
        }
    }
    gem_item = gem->item;
    vm_item_add_state(r->vm, pc.key, gem_item, -1, gem->health, gem->charge, gem->soul);
    if (!npc && (float)rpg_roll100(r) >= chance) {
        snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sNotifyMessage34", "You failed to enchant the item."));
        return 0;
    }
    en = new_custom(r, 2, e->name);
    obj = en ? new_custom(r, 0, e->name) : NULL;
    if (!obj)
        return 0;
    if (npc)
        pay(r, pc.key, npc->key, npc, price);
    en->spell.kind = 1;
    en->spell.type = (uint8_t)e->type;
    en->spell.cost = points;
    en->spell.charge = e->type == 3 ? 0 : charge;
    en->spell.effect_count = (uint16_t)PW_MIN(e->count, RPG_CUSTOM_EFFECTS);
    memcpy(en->effects, e->effects, en->spell.effect_count * sizeof(DRpgEffect));
    obj->obj = *base;
    obj->obj.hash = pw_hash_name("pw_custom_item") ^ (uint32_t)(obj - r->custom) * 2654435761u;
    obj->obj.name = RPG_CUSTOM_NAME | (uint32_t)(obj - r->custom);
    obj->obj.link[0] = en->spell.hash;
    obj->obj.value = base->value + points * 10;
    vm_item_add_state(r->vm, pc.key, it->item, -1, it->health, it->charge, it->soul);
    vm_item_add(r->vm, pc.key, obj->obj.hash, 1);
    if (!npc)
        rpg_skill_use(r, SKILL_ENCHANT, 2, 1.0f);
    snprintf(msg, cap, "%s", rpgdb_gmst_string(r->db, "sEnchantmentMenu12", "You created an enchanted item."));
    return 1;
}
