/*
 * dialogue.c - info filtering, greetings, topics, choices and hyperlinks.
 */
#include "dialogue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

/* SCVR function codes (original order) used below */
enum {
    F_FACREACTION_LOWEST = 0, F_FACREACTION_HIGHEST = 1, F_RANK_REQUIREMENT = 2,
    F_REPUTATION = 3, F_HEALTH_PERCENT = 4, F_PC_REPUTATION = 5, F_PC_LEVEL = 6,
    F_PC_HEALTH_PERCENT = 7, F_PC_MAGICKA = 8, F_PC_FATIGUE = 9, F_PC_STRENGTH = 10,
    F_PC_BLOCK = 11, F_PC_HANDTOHAND = 37, F_PC_GENDER = 38, F_PC_EXPELLED = 39,
    F_PC_COMMON_DISEASE = 40, F_PC_BLIGHT_DISEASE = 41, F_PC_CLOTHING_MODIFIER = 42,
    F_PC_CRIME_LEVEL = 43, F_SAME_SEX = 44, F_SAME_RACE = 45, F_SAME_FACTION = 46,
    F_FACTION_RANK_DIFFERENCE = 47, F_DETECTED = 48, F_ALARMED = 49, F_CHOICE = 50,
    F_PC_INTELLIGENCE = 51, F_PC_LUCK = 57, F_PC_CORPRUS = 58, F_WEATHER = 59,
    F_PC_VAMPIRE = 60, F_LEVEL = 61, F_ATTACKED = 62, F_TALKED_TO_PC = 63, F_PC_HEALTH = 64,
    F_CREATURE_TARGET = 65, F_FRIEND_HIT = 66, F_FIGHT = 67, F_HELLO = 68, F_ALARM = 69,
    F_FLEE = 70, F_SHOULD_ATTACK = 71, F_WEREWOLF = 72, F_PC_WEREWOLF_KILLS = 73
};

#define SPELL_SALT VM_SPELL_SALT

static int is_word_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '\'' || c >= 128;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int ci_starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (lower((unsigned char)*s) != lower((unsigned char)*prefix))
            return 0;
        ++s;
        ++prefix;
    }
    return 1;
}

static int ci_compare(const char *a, const char *b)
{
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        ++a;
        ++b;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static const char *player_cell(const Dialogue *d)
{
    const Vm *vm = d->vm;
    return vm->host.cell_name ? vm->host.cell_name(vm->host.user) : "";
}


/* ------------------------------------------------------------------------ */
/* condition evaluation                                                     */
/* ------------------------------------------------------------------------ */
static int compare_int(int comp, int32_t a, int32_t b)
{
    switch (comp) {
    case DRPG_CMP_EQ: return a == b;
    case DRPG_CMP_NE: return a != b;
    case DRPG_CMP_GT: return a > b;
    case DRPG_CMP_GE: return a >= b;
    case DRPG_CMP_LT: return a < b;
    default: return a <= b;
    }
}

static int compare_float(int comp, float a, float b)
{
    switch (comp) {
    case DRPG_CMP_EQ: return a == b;
    case DRPG_CMP_NE: return a != b;
    case DRPG_CMP_GT: return a > b;
    case DRPG_CMP_GE: return a >= b;
    case DRPG_CMP_LT: return a < b;
    default: return a <= b;
    }
}

static int compare_value_int(const DRpgCond *c, int32_t v)
{
    if (c->is_float) {
        float f;
        memcpy(&f, &c->value, 4);
        return compare_float(c->comp, (float)v, f);
    }
    return compare_int(c->comp, v, c->value);
}

static int compare_value_float(const DRpgCond *c, float v)
{
    float f;
    if (c->is_float)
        memcpy(&f, &c->value, 4);
    else
        f = (float)c->value;
    return compare_float(c->comp, v, f);
}

static int actor_rank(Dialogue *d)
{
    return vm_ref_rank(d->vm, &d->actor);
}

static uint32_t actor_faction(Dialogue *d)
{
    return vm_ref_faction(d->vm, &d->actor);
}

static const DRpgObject *actor_object(const Dialogue *d)
{
    return vm_ref_object(d->vm, &d->actor);
}

static int has_spell_type(Dialogue *d, int type)
{
    Vm *vm = d->vm;
    uint32_t cursor = 0, item;
    int32_t count;
    while (vm_item_next(vm, vm->player_hash ^ SPELL_SALT, &cursor, &item, &count) >= 0) {
        const DRpgSpell *s = rpgdb_spell(d->db, item);
        if (s && s->kind == 0 && s->type == type)
            return 1;
    }
    return 0;
}

static int faction_rank_ok(Dialogue *d, uint32_t faction, int rank, int *skills_ok, int *rep_ok)
{
    const DRpgFaction *f = rpgdb_faction(d->db, faction);
    VmRef pc = vm_player_ref(d->vm);
    int best = -1, second = -1, i, n = 0;
    if (!f || rank < 0 || rank > 9)
        return 0;
    for (i = 0; i < 7; ++i) {
        int v;
        if (f->skill[i] < 0 || f->skill[i] > 26)
            continue;
        v = (int)vm_stat(d->vm, &pc, MWP_SKILL_BLOCK + f->skill[i]);
        n++;
        if (v > best) {
            second = best;
            best = v;
        } else if (v > second) {
            second = v;
        }
    }
    *skills_ok = 1;
    if (n > 0 && best < f->rank_req[rank][2])
        *skills_ok = 0;
    if (n > 1 && second < f->rank_req[rank][3])
        *skills_ok = 0;
    for (i = 0; i < 2 && *skills_ok; ++i) {
        int attr = f->attribute[i];
        if (attr >= 0 && attr < 8 &&
            vm_stat(d->vm, &pc, MWP_ATTR_STRENGTH + attr) < (float)f->rank_req[rank][i])
            *skills_ok = 0;
    }
    *rep_ok = vm_pc_facrep(d->vm, faction) >= f->rank_req[rank][4];
    return 1;
}

static int test_local(Dialogue *d, const DRpgCond *c)
{
    const DRpgObject *o = actor_object(d);
    const DRpgVar *var;
    const DRpgScript *s;
    int vmi;
    VmValue v;
    if (!o || o->script == DRPG_NONE16)
        return 0;
    var = rpgdb_script_var(d->db, o->script, c->key);
    if (!var)
        return 0;
    s = &d->db->scripts[o->script];
    vmi = d->actor.vmi;
    if (vmi < 0 || d->vm->inst[vmi].script != o->script) {
        /* no locals yet: the original compares against zero */
        return var->type == DRPG_VT_FLOAT ? compare_value_float(c, 0.0f) : compare_value_int(c, 0);
    }
    if ((uint32_t)var->slot >= (uint32_t)s->num_shorts + s->num_longs + s->num_floats)
        return 0;
    v = d->vm->locals[d->vm->inst[vmi].locals + var->slot];
    return var->type == DRPG_VT_FLOAT ? compare_value_float(c, v.f) : compare_value_int(c, v.i);
}

static int eval_cond(Dialogue *d, const DRpgCond *c)
{
    Vm *vm = d->vm;
    VmRef pc = vm_player_ref(vm);
    const DRpgObject *ao = actor_object(d);
    int npc = ao && ao->type == DRPG_OT_NPC;
    int f = c->func;
    switch (f) {
    /* ---- header filters ---- */
    case DRPG_CF_ACTOR:
        return d->actor.object == c->key;
    case DRPG_CF_RACE:
        return d->creature || (npc && ao->link[0] == c->key);
    case DRPG_CF_CLASS:
        return d->creature || (npc && ao->link[1] == c->key);
    case DRPG_CF_FACTION:
        return d->creature || (actor_faction(d) == c->key && actor_rank(d) >= c->value);
    case DRPG_CF_FACTIONLESS:
        return d->creature || actor_faction(d) == 0;
    case DRPG_CF_RANK:
        return d->creature || actor_rank(d) >= c->value;
    case DRPG_CF_GENDER:
        return d->creature || !npc || (int32_t)ao->sex == c->value;
    case DRPG_CF_PCFACTION:
    case DRPG_CF_PCRANK: {
        uint32_t fac = f == DRPG_CF_PCFACTION ? c->key : actor_faction(d);
        int r = vm_pc_rank(vm, fac);
        return r >= 0 && r >= c->value;
    }
    case DRPG_CF_CELL:
        return ci_starts_with(player_cell(d), rpgdb_name(d->db, c->key));
    /* ---- variable kinds ---- */
    case DRPG_CF_GLOBAL: {
        int g = rpgdb_global(d->db, c->key);
        if (g < 0)
            return 1; /* missing globals are ignored, as in the original */
        return compare_value_float(c, vm_global_float(vm, g));
    }
    case DRPG_CF_LOCAL:
        return test_local(d, c);
    case DRPG_CF_NOT_LOCAL:
        return !test_local(d, c);
    case DRPG_CF_JOURNAL:
        return compare_value_int(c, vm_journal_index(vm, c->key));
    case DRPG_CF_ITEM:
        return compare_value_int(c, vm_item_count(vm, vm->player_hash, c->key));
    case DRPG_CF_DEAD:
        return compare_value_int(c, (int32_t)vm_prop_key(vm, c->key, c->key, MWP_DEATHS));
    case DRPG_CF_NOT_ID:
        return d->actor.object != c->key;
    case DRPG_CF_NOT_FACTION:
        return actor_faction(d) != c->key;
    case DRPG_CF_NOT_CLASS:
        return !npc || ao->link[1] != c->key;
    case DRPG_CF_NOT_RACE:
        return !npc || ao->link[0] != c->key;
    case DRPG_CF_NOT_CELL:
        return !ci_starts_with(player_cell(d), rpgdb_name(d->db, c->key));
    default:
        break;
    }
    if (f == F_CHOICE && d->choice < 0)
        return 0;
    if (f == F_WEATHER && vm->host.interior && vm->host.interior(vm->host.user))
        return 0;
    /* ---- numeric functions ---- */
    if (f == F_PC_HEALTH_PERCENT || f == F_HEALTH_PERCENT) {
        const VmRef *r = f == F_PC_HEALTH_PERCENT ? &pc : &d->actor;
        float base = vm->stat_max ? vm->stat_max(vm->mech, r, MWP_DYN_HEALTH) : vm_prop(vm, r, MWP_DYNBASE_HEALTH);
        int pct = base > 0.0f ? (int)(vm_stat(vm, r, MWP_DYN_HEALTH) / base * 100.0f) : 0;
        return compare_value_int(c, pct);
    }
    if (f == F_PC_MAGICKA)
        return compare_value_float(c, vm_stat(vm, &pc, MWP_DYN_MAGICKA));
    if (f == F_PC_FATIGUE)
        return compare_value_float(c, vm_stat(vm, &pc, MWP_DYN_FATIGUE));
    if (f == F_PC_HEALTH)
        return compare_value_float(c, vm_stat(vm, &pc, MWP_DYN_HEALTH));
    /* ---- boolean functions ---- */
    {
        int b = -1;
        switch (f) {
        case F_PC_EXPELLED: {
            uint32_t fac = actor_faction(d);
            b = fac ? vm_pc_expelled(vm, fac) : 0;
            break;
        }
        case F_PC_COMMON_DISEASE: b = has_spell_type(d, 3); break;
        case F_PC_BLIGHT_DISEASE: b = has_spell_type(d, 2); break;
        case F_PC_CORPRUS: /* active magic effects (corprus, vampirism) arrive with M5 */
        case F_PC_VAMPIRE: b = 0; break;
        case F_SAME_SEX: {
            b = npc && vm_pc_female(vm) == ao->sex;
            break;
        }
        case F_SAME_RACE: {
            b = npc && vm_pc_race(vm) == ao->link[0];
            break;
        }
        case F_SAME_FACTION: {
            uint32_t fac = actor_faction(d);
            b = fac && vm_pc_rank(vm, fac) >= 0;
            break;
        }
        case F_DETECTED: b = 1; break;
        case F_ALARMED: b = 0; break;
        case F_ATTACKED: b = vm_stat(vm, &d->actor, MWP_ATTACKED) != 0.0f; break;
        case F_TALKED_TO_PC: b = d->talked_to; break;
        case F_SHOULD_ATTACK: b = vm_stat(vm, &d->actor, MWP_AI_FIGHT) >= 100.0f; break;
        case F_WEREWOLF: b = 0; break;
        default: break;
        }
        if (b >= 0)
            return compare_value_int(c, b ? 1 : 0);
    }
    /* ---- integer functions ---- */
    {
        int32_t v = 0;
        if (f >= F_PC_BLOCK && f <= F_PC_HANDTOHAND) {
            v = (int32_t)vm_stat(vm, &pc, MWP_SKILL_BLOCK + (f - F_PC_BLOCK));
        } else if (f == F_PC_STRENGTH) {
            v = (int32_t)vm_stat(vm, &pc, MWP_ATTR_STRENGTH);
        } else if (f >= F_PC_INTELLIGENCE && f <= F_PC_LUCK) {
            v = (int32_t)vm_stat(vm, &pc, MWP_ATTR_STRENGTH + (f - F_PC_INTELLIGENCE + 1));
        } else {
            switch (f) {
            case F_FACREACTION_LOWEST:
            case F_FACREACTION_HIGHEST: {
                uint32_t fac = actor_faction(d);
                uint32_t i;
                v = 0;
                if (fac) {
                    for (i = 0; i < d->db->hdr->factions.count; ++i) {
                        int r;
                        if (vm->pc_rank[i] < 0)
                            continue;
                        r = vm_faction_reaction(vm, fac, d->db->factions[i].hash);
                        if (f == F_FACREACTION_LOWEST ? r < v : r > v)
                            v = r;
                    }
                }
                break;
            }
            case F_RANK_REQUIREMENT: {
                uint32_t fac = actor_faction(d);
                int rank, sk = 0, rep = 0;
                if (!fac)
                    break;
                rank = vm_pc_rank(vm, fac);
                if (rank >= 9)
                    break;
                if (faction_rank_ok(d, fac, rank + 1, &sk, &rep))
                    v = (sk ? 1 : 0) + (rep ? 2 : 0);
                break;
            }
            case F_REPUTATION: v = npc ? (int32_t)vm_stat(vm, &d->actor, MWP_REPUTATION) : 0; break;
            case F_PC_REPUTATION: v = (int32_t)vm_stat(vm, &pc, MWP_REPUTATION); break;
            case F_PC_LEVEL: v = (int32_t)vm_stat(vm, &pc, MWP_LEVEL); break;
            case F_PC_GENDER: {
                v = vm_pc_female(vm);
                break;
            }
            case F_PC_CLOTHING_MODIFIER: v = 0; break; /* nothing is equipped before M5 */
            case F_PC_CRIME_LEVEL: v = (int32_t)vm_stat(vm, &pc, MWP_CRIME); break;
            case F_FACTION_RANK_DIFFERENCE: {
                uint32_t fac = actor_faction(d);
                v = fac ? vm_pc_rank(vm, fac) - actor_rank(d) : 0;
                break;
            }
            case F_CHOICE: v = d->choice; break;
            case F_WEATHER: v = 0; break;
            case F_LEVEL: v = (int32_t)vm_stat(vm, &d->actor, MWP_LEVEL); break;
            case F_CREATURE_TARGET: v = 0; break;
            case F_FRIEND_HIT: v = 0; break;
            case F_FIGHT: v = (int32_t)vm_stat(vm, &d->actor, MWP_AI_FIGHT); break;
            case F_HELLO: v = (int32_t)vm_stat(vm, &d->actor, MWP_AI_HELLO); break;
            case F_ALARM: v = (int32_t)vm_stat(vm, &d->actor, MWP_AI_ALARM); break;
            case F_FLEE: v = (int32_t)vm_stat(vm, &d->actor, MWP_AI_FLEE); break;
            case F_PC_WEREWOLF_KILLS: v = 0; break;
            default: return 1;
            }
        }
        return compare_value_int(c, v);
    }
}

static int test_info_mode(Dialogue *d, const DRpgTopic *t, uint32_t info, int *disp_fail, int invert);

static int test_info(Dialogue *d, const DRpgTopic *t, uint32_t info, int *disp_fail)
{
    return test_info_mode(d, t, info, disp_fail, 0);
}

static int test_info_mode(Dialogue *d, const DRpgTopic *t, uint32_t info, int *disp_fail, int invert)
{
    const DRpgInfo *in = &d->db->infos[info];
    uint32_t i;
    d->infos_tested++;
    if (d->creature && !(in->flags & DRPG_IF_HAS_ACTOR))
        return 0;
    for (i = 0; i < in->cond_count; ++i)
        if (!eval_cond(d, &d->db->conds[in->cond_first + i]))
            return 0;
    if (invert)
        return d->creature || in->data == 0 || vm_disposition(d->vm, &d->actor) < in->data;
    if (t->type != DRPG_TT_JOURNAL && !d->creature && vm_disposition(d->vm, &d->actor) < in->data) {
        *disp_fail = 1;
        return 0;
    }
    return 1;
}

int dlg_search(Dialogue *d, uint32_t topic, int fallback, int *refusal)
{
    const DRpgTopic *t;
    uint32_t i;
    int disp_fail = 0;
    if (refusal)
        *refusal = 0;
    if (topic >= d->db->hdr->topics.count)
        return -1;
    t = &d->db->topics[topic];
    for (i = 0; i < t->info_count; ++i)
        if (test_info(d, t, t->info_first + i, &disp_fail))
            return (int)(t->info_first + i);
    if (disp_fail && fallback) {
        int ri = rpgdb_topic(d->db, pw_hash_name("info refusal"));
        if (ri >= 0) {
            const DRpgTopic *rt = &d->db->topics[ri];
            for (i = 0; i < rt->info_count; ++i) {
                int dummy = 0;
                if (test_info(d, rt, rt->info_first + i, &dummy)) {
                    if (refusal)
                        *refusal = 1;
                    return (int)(rt->info_first + i);
                }
            }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/* keywords                                                                 */
/* ------------------------------------------------------------------------ */
static const RpgDb *sort_db;

static int keyword_cmp(const void *a, const void *b)
{
    const char *na = rpgdb_topic_name(sort_db, *(const uint16_t *)a);
    const char *nb = rpgdb_topic_name(sort_db, *(const uint16_t *)b);
    return ci_compare(na, nb);
}

static int find_keyword_at(const Dialogue *d, const char *p, uint32_t *len_out)
{
    uint32_t lo = 0, hi = d->keyword_count, i;
    int best = -1;
    uint32_t best_len = 0;
    int c = lower((unsigned char)*p);
    /* first keyword whose first character is >= c */
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        int k = lower((unsigned char)rpgdb_topic_name(d->db, d->keywords[mid])[0]);
        if (k < c)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (i = lo; i < d->keyword_count; ++i) {
        const char *name = rpgdb_topic_name(d->db, d->keywords[i]);
        uint32_t n;
        if (lower((unsigned char)name[0]) != c)
            break;
        if (!ci_starts_with(p, name))
            continue;
        n = (uint32_t)strlen(name);
        if (n > best_len) {
            best_len = n;
            best = d->keywords[i];
        }
    }
    *len_out = best_len;
    return best;
}

int dlg_keywords(const Dialogue *d, const char *text, DlgLink *out, int max)
{
    const char *p = text;
    int n = 0;
    while (*p && n < max) {
        uint32_t len = 0;
        int topic;
        if (p != text && is_word_char((unsigned char)p[-1])) {
            ++p;
            continue;
        }
        topic = is_word_char((unsigned char)*p) ? find_keyword_at(d, p, &len) : -1;
        if (topic >= 0 && len) {
            out[n].start = (uint16_t)(p - text);
            out[n].len = (uint16_t)len;
            out[n].topic = (uint16_t)topic;
            n++;
            p += len;
        } else {
            ++p;
        }
    }
    return n;
}

static int listed(const Dialogue *d, uint32_t topic)
{
    uint32_t i;
    for (i = 0; i < d->list_count; ++i)
        if (d->list[i] == topic)
            return 1;
    return 0;
}

int dlg_links(const Dialogue *d, const char *text, DlgLink *out, int max)
{
    DlgLink all[128];
    int n = dlg_keywords(d, text, all, (int)PW_COUNTOF(all)), i, k = 0;
    for (i = 0; i < n && k < max; ++i)
        if (listed(d, all[i].topic))
            out[k++] = all[i];
    return k;
}

/* ------------------------------------------------------------------------ */
/* state updates                                                            */
/* ------------------------------------------------------------------------ */
static void update_list(Dialogue *d)
{
    uint32_t t, count = d->db->hdr->topics.count;
    d->list_count = 0;
    for (t = 0; t < count; ++t) {
        int info;
        if (d->db->topics[t].type != DRPG_TT_TOPIC || !vm_topic_known(d->vm, t))
            continue;
        info = dlg_search(d, t, 1, NULL);
        if (info < 0)
            continue;
        d->list_exhausted[d->list_count] = (uint8_t)vm_heard(d->vm, (uint32_t)info);
        d->list[d->list_count++] = (uint16_t)t;
    }
    d->revision++;
}

static void add_topics_from_text(Dialogue *d, const char *text)
{
    DlgLink links[128];
    int n = dlg_keywords(d, text, links, (int)PW_COUNTOF(links)), i;
    for (i = 0; i < n; ++i) {
        uint32_t t = links[i].topic;
        if (!vm_topic_known(d->vm, t) && dlg_search(d, t, 1, NULL) >= 0)
            vm_add_topic(d->vm, d->db->topics[t].hash);
    }
}

static void history_add(Dialogue *d, int kind, const char *text)
{
    uint32_t n = (uint32_t)strlen(text);
    if (n + 2 > DLG_HISTORY)
        n = DLG_HISTORY - 2;
    while (d->history_len + n + 2 > DLG_HISTORY) {
        /* drop the oldest entry */
        uint32_t first = (uint32_t)strlen(d->history + 1) + 2;
        memmove(d->history, d->history + first, d->history_len - first);
        d->history_len -= first;
    }
    d->history[d->history_len] = (char)kind;
    memcpy(d->history + d->history_len + 1, text, n);
    d->history[d->history_len + 1 + n] = '\0';
    d->history_len += n + 2;
    d->revision++;
}

int dlg_history_next(const Dialogue *d, uint32_t *cursor, int *kind, const char **text)
{
    if (*cursor >= d->history_len)
        return 0;
    *kind = (unsigned char)d->history[*cursor];
    *text = d->history + *cursor + 1;
    *cursor += (uint32_t)strlen(*text) + 2;
    return 1;
}

static void collect_script_output(Dialogue *d)
{
    Vm *vm = d->vm;
    if (vm->choice_count) {
        memcpy(d->choices, vm->choices, vm->choice_count * sizeof(VmChoice));
        d->choice_count = vm->choice_count;
        d->in_choice = 1;
        vm->choice_count = 0;
    }
    if (vm->goodbye) {
        d->goodbye = 1;
        vm->goodbye = 0;
    }
}

/* shows an info: response text, then its result script, then learning */
static void respond(Dialogue *d, uint32_t info, const char *title, int learn_first)
{
    const DRpgInfo *in = &d->db->infos[info];
    rpgdb_text(d->db, in->text, d->text, sizeof(d->text));
    if (learn_first)
        add_topics_from_text(d, d->text);
    vm_fix_defines(d->vm, '%', d->text, d->fixed, sizeof(d->fixed), &d->actor);
    if (title && *title)
        history_add(d, DLG_ENTRY_TITLE, title);
    history_add(d, DLG_ENTRY_TEXT, d->fixed);
    if (in->script != DRPG_NONE) {
        uint32_t avail = 0;
        const uint8_t *blob = rpgdb_blob(d->db, in->script, &avail);
        d->vm->choice_count = 0;
        d->vm->goodbye = 0;
        if (blob)
            vm_run_snippet(d->vm, blob, avail, &d->actor);
        collect_script_output(d);
    }
    if (!learn_first)
        add_topics_from_text(d, d->text);
}

int dlg_init(Dialogue *d, Vm *vm, RpgDb *db)
{
    uint32_t t, n = db->hdr->topics.count, k = 0;
    memset(d, 0, sizeof(*d));
    d->vm = vm;
    d->db = db;
    d->last_topic = -1;
    d->choice = -1;
    d->list = mem_try_alloc(MEM_REGION_SCRIPT, (n ? n : 1) * (uint32_t)sizeof(uint16_t), 16);
    d->list_exhausted = mem_try_alloc(MEM_REGION_SCRIPT, n ? n : 1, 16);
    d->keywords = mem_try_alloc(MEM_REGION_SCRIPT, (n ? n : 1) * (uint32_t)sizeof(uint16_t), 16);
    if (!d->list || !d->list_exhausted || !d->keywords)
        return -1;
    for (t = 0; t < n; ++t)
        if (db->topics[t].type == DRPG_TT_TOPIC && rpgdb_topic_name(db, t)[0])
            d->keywords[k++] = (uint16_t)t;
    d->keyword_count = k;
    sort_db = db;
    qsort(d->keywords, k, sizeof(uint16_t), keyword_cmp);
    return 0;
}

int dlg_start(Dialogue *d, const VmRef *actor, const char *actor_name)
{
    const DRpgObject *o;
    uint32_t t, count = d->db->hdr->topics.count;
    d->active = 0;
    d->actor = *actor;
    o = actor_object(d);
    d->creature = !o || o->type != DRPG_OT_NPC;
    strncpy(d->actor_name, actor_name ? actor_name : "", sizeof(d->actor_name) - 1);
    d->actor_name[sizeof(d->actor_name) - 1] = '\0';
    d->last_topic = -1;
    d->choice = -1;
    d->in_choice = 0;
    d->choice_count = 0;
    d->goodbye = 0;
    d->history_len = 0;
    d->list_count = 0;
    d->talked_to = vm_prop(d->vm, actor, MWP_TALKED) != 0.0f;
    if (vm_prop(d->vm, actor, MWP_DEAD) != 0.0f ||
        (vm_prop(d->vm, actor, MWP_DYN_HEALTH) <= 0.0f && vm_prop(d->vm, actor, MWP_DYNBASE_HEALTH) > 0.0f))
        return 0; /* no conversations with the dead */
    for (t = 0; t < count; ++t) {
        int info;
        if (d->db->topics[t].type != DRPG_TT_GREETING)
            continue;
        info = dlg_search(d, t, 0, NULL);
        if (info < 0)
            continue;
        vm_set_prop(d->vm, actor, MWP_TALKED, 1.0f);
        d->active = 1;
        d->last_topic = (int32_t)t;
        respond(d, (uint32_t)info, NULL, 0);
        update_list(d);
        return 1;
    }
    return 0;
}

void dlg_ask(Dialogue *d, uint32_t topic)
{
    const DRpgTopic *t;
    int refusal = 0, info;
    const char *title;
    if (!d->active || d->in_choice || d->goodbye || topic >= d->db->hdr->topics.count)
        return;
    t = &d->db->topics[topic];
    if (t->type != DRPG_TT_TOPIC && t->type != DRPG_TT_PERSUASION)
        return;
    info = dlg_search(d, topic, 1, &refusal);
    if (info < 0)
        return;
    title = rpgdb_topic_name(d->db, topic);
    if (t->type == DRPG_TT_TOPIC && !refusal)
        vm_mark_heard(d->vm, (uint32_t)info);
    d->last_topic = (int32_t)topic;
    respond(d, (uint32_t)info, title, 0);
    update_list(d);
}

void dlg_answer(Dialogue *d, uint32_t choice_index)
{
    const DRpgTopic *t;
    int refusal = 0, info;
    if (!d->active || !d->in_choice || choice_index >= d->choice_count || d->last_topic < 0)
        return;
    history_add(d, DLG_ENTRY_PLAYER, d->choices[choice_index].text);
    d->choice = d->choices[choice_index].value;
    t = &d->db->topics[d->last_topic];
    info = -1;
    if (t->type == DRPG_TT_TOPIC || t->type == DRPG_TT_GREETING)
        info = dlg_search(d, (uint32_t)d->last_topic, 1, &refusal);
    d->choice = -1;
    d->in_choice = 0;
    d->choice_count = 0;
    if (info >= 0) {
        if (t->type == DRPG_TT_TOPIC && !refusal)
            vm_mark_heard(d->vm, (uint32_t)info);
        respond(d, (uint32_t)info, NULL, 1);
    }
    update_list(d);
}

static int say_mode(Dialogue *d, const char *topic, int invert)
{
    int t = rpgdb_topic(d->db, pw_hash_name(topic));
    uint32_t i;
    const DRpgTopic *tp;
    if (!d->active || t < 0)
        return 0;
    tp = &d->db->topics[t];
    for (i = 0; i < tp->info_count; ++i) {
        int dummy = 0;
        if (test_info_mode(d, tp, tp->info_first + i, &dummy, invert)) {
            respond(d, tp->info_first + i, NULL, 0);
            update_list(d);
            return 1;
        }
    }
    return 0;
}

int dlg_service_refused(Dialogue *d)
{
    return say_mode(d, "service refusal", 1);
}

int dlg_say(Dialogue *d, const char *topic)
{
    return say_mode(d, topic, 0);
}

void dlg_end(Dialogue *d)
{
    d->active = 0;
    d->in_choice = 0;
    d->choice_count = 0;
    d->goodbye = 0;
    d->revision++;
}
