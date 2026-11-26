/*
 * mech_test: rpg.c / rpg_items.c / rpg_trade.c on the cooked test world:
 * character creation, skill progress and level-up, effects (constant,
 * timed, instant), equipment and armor, combat to the death, casting,
 * potions, ingredients, alchemy, barter, training, spell merchants,
 * repair, persuasion, soul trapping and enchanting, bound weapons,
 * resting, Mark/Recall/Divine Intervention, travel and lock picking.
 *
 *   mech_test <test world data dir>
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mem.h"
#include "rpg.h"
#include "rpgdb.h"
#include "vm.h"

static int g_fail, g_checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fail++;                                                         \
            printf("  FAIL: ");                                               \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

static RpgDb g_db;
static Vm g_vm;
static Rpg g_rpg;
static int g_teleports, g_killed;
static uint32_t g_tp_cell;
static float g_tp_pos[3];

static void mock_teleport(void *user, uint32_t cell, const float pos[3], float rot)
{
    (void)user;
    (void)rot;
    g_teleports++;
    g_tp_cell = cell;
    memcpy(g_tp_pos, pos, sizeof(g_tp_pos));
}

static void mock_locate(void *user, uint32_t *cell, float pos[3], float *rot)
{
    (void)user;
    *cell = 0;
    pos[0] = 1000.0f;
    pos[1] = 500.0f;
    pos[2] = 40.0f;
    *rot = 0.5f;
}

static void mock_killed(void *user, const VmRef *ref)
{
    (void)user;
    (void)ref;
    g_killed++;
}

static int mock_hostiles(void *user, float radius)
{
    (void)user;
    (void)radius;
    return 0;
}

static VmRef npc(const char *id)
{
    VmRef r;
    memset(&r, 0, sizeof(r));
    r.kind = VM_REF_OBJECT;
    r.object = pw_hash_name(id);
    r.key = r.object;
    r.vmi = -1;
    r.inst = -1;
    return r;
}

static uint32_t H(const char *s)
{
    return pw_hash_name(s);
}

static int knows(uint32_t spell)
{
    return vm_item_count(&g_vm, g_vm.player_hash ^ VM_SPELL_SALT, spell) > 0;
}

static int stack_of(const char *item)
{
    return rpg_find_stack(&g_rpg, g_vm.player_hash, H(item));
}

static void set_skill(int skill, float v)
{
    VmRef pc = vm_player_ref(&g_vm);
    vm_set_prop(&g_vm, &pc, MWP_SKILL_BLOCK + skill, v);
}

static void test_chargen(void)
{
    VmRef pc = vm_player_ref(&g_vm);
    rpg_chargen(&g_rpg, "Tester", H("tw_folk"), 0, H("tw_warrior"), H("tw_sign_blade"));
    CHECK(strcmp(g_vm.pc_name, "Tester") == 0, "name");
    CHECK(rpg_attr(&g_rpg, &pc, ATTR_STRENGTH) == 50.0f && rpg_attr(&g_rpg, &pc, ATTR_ENDURANCE) == 50.0f &&
          rpg_attr(&g_rpg, &pc, ATTR_SPEED) == 40.0f, "race + favoured attributes");
    CHECK(rpg_skill(&g_rpg, &pc, SKILL_LONGBLADE) == 45.0f, "major + race bonus + specialisation (%g)",
          rpg_skill(&g_rpg, &pc, SKILL_LONGBLADE));
    CHECK(rpg_skill(&g_rpg, &pc, SKILL_SNEAK) == 20.0f, "minor + race bonus (%g)", rpg_skill(&g_rpg, &pc, SKILL_SNEAK));
    CHECK(rpg_skill(&g_rpg, &pc, SKILL_ATHLETICS) == 20.0f, "minor + specialisation");
    CHECK(rpg_skill(&g_rpg, &pc, SKILL_ALCHEMY) == 5.0f, "misc skill");
    CHECK(rpg_dyn_max(&g_rpg, &pc, 0) == 50.0f && rpg_dyn(&g_rpg, &pc, 0) == 50.0f, "health (STR+END)/2");
    CHECK(rpg_dyn_max(&g_rpg, &pc, 1) == 40.0f, "magicka = INT x fPCbaseMagickaMult");
    CHECK(rpg_dyn_max(&g_rpg, &pc, 2) == 50.0f + 40.0f + 40.0f + 50.0f, "fatigue = STR+WIL+AGI+END");
    CHECK(knows(H("tw_power")) && knows(H("tw_ability_attack")), "race power + birthsign ability");
    CHECK(!knows(H("tw_spark")), "a warrior cannot reliably cast the starting spell");
    CHECK(rpg_effect(&g_rpg, &pc, MGEF_FORTIFY_ATTACK, -1) == 10.0f, "ability is a constant effect");
    CHECK(vm_pc_race(&g_vm) == H("tw_folk") && vm_pc_class(&g_vm) == H("tw_warrior"), "player overrides");
    CHECK(rpg_skill_type(&g_rpg, SKILL_LONGBLADE) == 0 && rpg_skill_type(&g_rpg, SKILL_SNEAK) == 1 &&
          rpg_skill_type(&g_rpg, SKILL_ALCHEMY) == 2, "skill types");
}

static void test_levelling(void)
{
    VmRef pc = vm_player_ref(&g_vm);
    int i, attrs[3] = { ATTR_STRENGTH, ATTR_ENDURANCE, ATTR_LUCK };
    float hp = rpg_dyn_max(&g_rpg, &pc, 0);
    /* (45 + 1) x 0.75 major x 0.8 combat specialisation = 27.6 uses */
    for (i = 0; i < 27; ++i)
        rpg_skill_use(&g_rpg, SKILL_LONGBLADE, 0, 1.0f);
    CHECK(rpg_skill(&g_rpg, &pc, SKILL_LONGBLADE) == 45.0f, "27 uses are not enough");
    rpg_skill_use(&g_rpg, SKILL_LONGBLADE, 0, 1.0f);
    CHECK(rpg_skill(&g_rpg, &pc, SKILL_LONGBLADE) == 46.0f, "28th use raises the skill");
    CHECK(vm_prop(&g_vm, &pc, MWP_LEVELPROG) == 1.0f && vm_prop(&g_vm, &pc, MWP_LVLATTR_STRENGTH) == 1.0f,
          "level progress + governing attribute counter");
    for (i = 0; i < 9; ++i)
        rpg_skill_raise(&g_rpg, SKILL_LONGBLADE, 0);
    CHECK(g_rpg.level_pending, "ten major/minor increases call for rest");
    CHECK(rpg_level_multiplier(&g_rpg, ATTR_STRENGTH) == 5 && rpg_level_multiplier(&g_rpg, ATTR_LUCK) == 1,
          "attribute multipliers x%d", rpg_level_multiplier(&g_rpg, ATTR_STRENGTH));
    CHECK(rpg_level_up(&g_rpg, attrs) == 1, "level up");
    CHECK(rpg_level(&g_rpg, &pc) == 2 && rpg_attr(&g_rpg, &pc, ATTR_STRENGTH) == 55.0f &&
          rpg_attr(&g_rpg, &pc, ATTR_ENDURANCE) == 51.0f && rpg_attr(&g_rpg, &pc, ATTR_LUCK) == 41.0f,
          "attributes raised by multiplier");
    CHECK(fabsf(rpg_dyn_max(&g_rpg, &pc, 0) - (hp + 5.1f)) < 0.01f, "health +10%% of endurance (%g)",
          rpg_dyn_max(&g_rpg, &pc, 0));
    CHECK(!g_rpg.level_pending && vm_prop(&g_vm, &pc, MWP_LEVELPROG) == 0.0f, "progress consumed");
    CHECK(g_vm.notify_count > 0, "skill-up and rest notices");
    g_vm.notify_count = 0;
}

static void test_equipment(void)
{
    VmRef pc = vm_player_ref(&g_vm);
    float unarmored = rpg_armor_rating(&g_rpg, &pc), with;
    int cuirass = stack_of("tw_cuirass"), boots, shield, sword;
    CHECK(cuirass >= 0 && rpg_equip(&g_rpg, &pc, cuirass) == 1, "equip cuirass");
    with = rpg_armor_rating(&g_rpg, &pc);
    CHECK(with > unarmored, "armor rating rises (%g -> %g)", unarmored, with);
    CHECK(rpg_equipped(&g_rpg, &pc, RPG_SLOT_CUIRASS) == cuirass, "slot");
    vm_item_add(&g_vm, pc.key, H("tw_sword"), 1);
    sword = stack_of("tw_sword");
    shield = stack_of("tw_shield");
    CHECK(rpg_equip(&g_rpg, &pc, shield) == 1 && rpg_equip(&g_rpg, &pc, sword) == 1, "shield + one-handed sword");
    CHECK(rpg_equipped(&g_rpg, &pc, RPG_SLOT_SHIELD) == shield, "shield stays with a one-handed weapon");
    CHECK(rpg_weapon_skill(&g_rpg, &pc) == SKILL_LONGBLADE, "weapon skill");
    CHECK(rpg_weapon_reach(&g_rpg, &pc) == 128.0f, "reach x fCombatDistance");
    boots = stack_of("tw_boots");
    g_vm.pc_race = H("tw_beast");
    CHECK(rpg_equip(&g_rpg, &pc, boots) == -1, "beast races cannot wear boots");
    g_vm.pc_race = H("tw_folk");
    CHECK(rpg_equip(&g_rpg, &pc, boots) == 1, "boots fit the folk");
    CHECK(rpg_encumbrance(&g_rpg, &pc) > 40.0f && rpg_capacity(&g_rpg, &pc) == 275.0f,
          "encumbrance %g / capacity %g", rpg_encumbrance(&g_rpg, &pc), rpg_capacity(&g_rpg, &pc));
}

static void test_combat(void)
{
    VmRef pc = vm_player_ref(&g_vm), critter = npc("tw_critter");
    int swings = 0, hits = 0, sword = stack_of("tw_sword");
    const VmItem *it;
    float before = rpg_dyn(&g_rpg, &critter, 0);
    CHECK(before == 30.0f, "critter health from its record (%g)", before);
    while (!rpg_is_dead(&g_rpg, &critter) && swings < 200) {
        RpgHit h = rpg_attack(&g_rpg, &pc, &critter, 1.0f);
        swings++;
        hits += h.hit;
        rpg_dyn_add(&g_rpg, &pc, 2, 1000.0f);
    }
    CHECK(rpg_is_dead(&g_rpg, &critter), "critter killed in %d swings (%d hits)", swings, hits);
    CHECK(g_killed == 1, "death reported to the host");
    CHECK(vm_prop_key(&g_vm, critter.object, critter.object, MWP_DEATHS) == 1.0f, "death count");
    CHECK(vm_prop(&g_vm, &critter, MWP_HOSTILE) != 0.0f, "attacked creature turns hostile");
    it = vm_item_at(&g_vm, rpg_equipped(&g_rpg, &pc, RPG_SLOT_WEAPON));
    CHECK(it && it->health != VM_FULL && it->health < 400 && it->count == 1, "sword wears down (%u)",
          it ? (unsigned)it->health : 0u);
    CHECK(rpg_equipped(&g_rpg, &pc, RPG_SLOT_WEAPON) != sword || it->variant != 0, "damaged unit split off");
    {
        RpgHit h = rpg_attack(&g_rpg, &pc, &critter, 1.0f);
        CHECK(!h.hit, "the dead are not hit again");
    }
    /* creatures fight back with their attack table */
    {
        VmRef c2 = npc("tw_critter");
        RpgHit h;
        float hp = rpg_dyn(&g_rpg, &pc, 0);
        int i;
        c2.key = 0xC0FFEEu;
        for (i = 0; i < 50 && rpg_dyn(&g_rpg, &pc, 0) == hp; ++i)
            h = rpg_attack(&g_rpg, &c2, &pc, 1.0f);
        PW_UNUSED(h);
        CHECK(rpg_dyn(&g_rpg, &pc, 0) < hp, "critter bites the player (%g -> %g)", hp, rpg_dyn(&g_rpg, &pc, 0));
        rpg_dyn_add(&g_rpg, &pc, 0, 1000.0f);
    }
}

static void test_magic(void)
{
    VmRef pc = vm_player_ref(&g_vm), elder = npc("tw_villager");
    RpgCast cast;
    float hp;
    int i;
    vm_item_add(&g_vm, pc.key ^ VM_SPELL_SALT, H("tw_bolt"), 1);
    set_skill(SKILL_DESTRUCTION, 100.0f);
    CHECK(rpg_select_spell(&g_rpg, H("tw_bolt")), "select spell");
    CHECK(rpg_spell_chance(&g_rpg, &pc, rpg_spell(&g_rpg, H("tw_bolt"))) >= 100.0f, "high skill = sure cast");
    hp = rpg_dyn(&g_rpg, &pc, 1);
    i = rpg_begin_cast(&g_rpg, &pc, &cast);
    CHECK(i && cast.success && cast.has[RPG_RANGE_TARGET] && cast.area == 5.0f,
          "cast rolled (%s)", cast.message);
    CHECK(rpg_dyn(&g_rpg, &pc, 1) == hp - (float)rpg_spell_cost(&g_rpg, rpg_spell(&g_rpg, H("tw_bolt"))), "magicka paid (%g -> %g)", hp, rpg_dyn(&g_rpg, &pc, 1));
    hp = rpg_dyn(&g_rpg, &elder, 0);
    CHECK(rpg_apply(&g_rpg, &pc, &elder, cast.effects, cast.count, RPG_RANGE_TARGET, cast.source, cast.chance, 1) == 1,
          "frost lands");
    for (i = 0; i < 40; ++i)
        rpg_tick(&g_rpg, 0.05f);
    CHECK(rpg_dyn(&g_rpg, &elder, 0) < hp, "frost damage over its second (%g -> %g)", hp, rpg_dyn(&g_rpg, &elder, 0));
    CHECK(rpg_effect(&g_rpg, &elder, MGEF_FROST_DAMAGE, -1) == 0.0f, "effect expired");
    CHECK(vm_prop(&g_vm, &elder, MWP_HOSTILE) != 0.0f, "harmful spell angers the target");
    /* power: once a day */
    CHECK(rpg_select_spell(&g_rpg, H("tw_power")) && rpg_begin_cast(&g_rpg, &pc, &cast) && cast.success,
          "power cast");
    rpg_apply(&g_rpg, &pc, &pc, cast.effects, cast.count, RPG_RANGE_SELF, cast.source, 100.0f, 1);
    CHECK(rpg_effect(&g_rpg, &pc, MGEF_FORTIFY_ATTRIBUTE, ATTR_STRENGTH) == 20.0f, "fortify strength");
    CHECK(!rpg_begin_cast(&g_rpg, &pc, &cast), "power used today");
    /* potions and timed restoration */
    rpg_damage(&g_rpg, &pc, 0, 35.0f, NULL);
    hp = rpg_dyn(&g_rpg, &pc, 0);
    {
        char msg[96];
        int st = stack_of("tw_potion_heal");
        CHECK(rpg_use(&g_rpg, st, msg, sizeof(msg)) == RPG_USE_DRINK, "drink");
    }
    for (i = 0; i < 80; ++i)
        rpg_tick(&g_rpg, 0.05f);
    CHECK(rpg_dyn(&g_rpg, &pc, 0) >= hp + 29.0f && rpg_dyn(&g_rpg, &pc, 0) <= rpg_dyn_max(&g_rpg, &pc, 0),
          "restore health 10/s for 3 s (%g -> %g)", hp, rpg_dyn(&g_rpg, &pc, 0));
    CHECK(vm_item_count(&g_vm, pc.key, H("tw_potion_heal")) == 0, "potion consumed");
    /* scroll: cast once */
    {
        int st = stack_of("tw_scroll");
        CHECK(rpg_select_item(&g_rpg, st) && rpg_begin_cast(&g_rpg, &pc, &cast) && cast.success, "read scroll");
        CHECK(vm_item_count(&g_vm, pc.key, H("tw_scroll")) == 0, "scroll consumed");
    }
    /* bound dagger appears and vanishes */
    {
        DRpgEffect e;
        memset(&e, 0, sizeof(e));
        e.effect = MGEF_BOUND_DAGGER;
        e.duration = 2;
        e.skill = e.attribute = -1;
        rpg_apply(&g_rpg, &pc, &pc, &e, 1, RPG_RANGE_SELF, H("test_bound"), 100.0f, 1);
        CHECK(vm_item_count(&g_vm, pc.key, H("bound_dagger")) == 1 && rpg_weapon_skill(&g_rpg, &pc) == SKILL_SHORTBLADE,
              "bound dagger summoned and equipped");
        for (i = 0; i < 50; ++i)
            rpg_tick(&g_rpg, 0.05f);
        CHECK(vm_item_count(&g_vm, pc.key, H("bound_dagger")) == 0, "bound dagger gone");
    }
    /* disease lowers dispositions, cures remove it */
    {
        VmRef weaver = npc("tw_villager_f");
        int d0 = rpg_disposition(&g_rpg, &weaver);
        DRpgEffect cure;
        vm_item_add(&g_vm, pc.key ^ VM_SPELL_SALT, H("tw_disease"), 1);
        CHECK(rpg_has_disease(&g_rpg, &pc) && rpg_disposition(&g_rpg, &weaver) == d0 - 10, "diseased (%d -> %d)",
              d0, rpg_disposition(&g_rpg, &weaver));
        memset(&cure, 0, sizeof(cure));
        cure.effect = MGEF_CURE_COMMON_DISEASE;
        cure.skill = cure.attribute = -1;
        rpg_apply(&g_rpg, &pc, &pc, &cure, 1, RPG_RANGE_SELF, H("test_cure"), 100.0f, 1);
        CHECK(!rpg_has_disease(&g_rpg, &pc), "cured");
    }
}

static void test_alchemy(void)
{
    VmRef pc = vm_player_ref(&g_vm);
    RpgAlchemy a;
    DRpgEffect e[8];
    int value, n;
    char msg[96];
    memset(&a, 0, sizeof(a));
    a.stacks[0] = stack_of("tw_reed");
    a.stacks[1] = stack_of("tw_root");
    a.stacks[2] = a.stacks[3] = -1;
    a.tools[0] = stack_of("tw_mortar");
    a.tools[1] = a.tools[2] = a.tools[3] = -1;
    set_skill(SKILL_ALCHEMY, 100.0f);
    n = rpg_alchemy_preview(&g_rpg, &a, e, 8, &value);
    CHECK(n == 1 && e[0].effect == MGEF_RESTORE_FATIGUE && value > 0, "shared effect found (%d)", n);
    CHECK(rpg_alchemy_brew(&g_rpg, &a, msg, sizeof(msg)) == 1, "brewed (%s)", msg);
    CHECK(vm_item_count(&g_vm, pc.key, H("tw_reed")) == 1 && vm_item_count(&g_vm, pc.key, H("tw_root")) == 1,
          "ingredients used");
    CHECK(g_rpg.custom_count == 1 && vm_item_count(&g_vm, pc.key, g_rpg.custom[0].obj.hash) == 1,
          "custom potion in inventory");
    CHECK(strcmp(rpg_name(&g_rpg, &g_rpg.custom[0].obj), "Potion of Restore Fatigue") == 0, "potion name '%s'",
          rpg_name(&g_rpg, &g_rpg.custom[0].obj));
    CHECK(rpg_use(&g_rpg, rpg_find_stack(&g_rpg, pc.key, g_rpg.custom[0].obj.hash), msg, sizeof(msg)) == RPG_USE_DRINK,
          "custom potion drinkable");
    CHECK(rpg_use(&g_rpg, stack_of("tw_reed"), msg, sizeof(msg)) == RPG_USE_EAT, "eat ingredient");
}

static void test_trade(void)
{
    VmRef pc = vm_player_ref(&g_vm), weaver = npc("tw_villager_f"), elder = npc("tw_villager");
    char msg[128];
    int gold = rpg_gold(&g_rpg, &pc), buy = rpg_price(&g_rpg, &weaver, 100, 1), sell = rpg_price(&g_rpg, &weaver, 100, 0);
    int skills[3], k, before;
    uint32_t spells[8];
    CHECK(buy > sell && buy > 0 && sell > 0, "buy %d > sell %d", buy, sell);
    CHECK(rpg_merchant_gold(&g_rpg, &weaver) == 200, "merchant gold pool");
    CHECK(rpg_sell(&g_rpg, &weaver, stack_of("tw_root"), 1, msg, sizeof(msg)) > 0, "sell an ingredient (%s)", msg);
    CHECK(rpg_gold(&g_rpg, &pc) > gold && vm_item_count(&g_vm, weaver.key, H("tw_root")) == 1, "gold and item moved");
    CHECK(rpg_sell(&g_rpg, &weaver, stack_of("tw_pick"), 1, msg, sizeof(msg)) == 0, "weaver refuses lockpicks");
    CHECK(rpg_buy(&g_rpg, &weaver, rpg_find_stack(&g_rpg, weaver.key, H("tw_root")), 1, msg, sizeof(msg)) > 0,
          "buy it back (%s)", msg);
    /* training */
    vm_item_add(&g_vm, pc.key, H("gold_001"), 1000);
    rpg_trainer_skills(&g_rpg, &elder, skills);
    CHECK(skills[0] >= 0 && skills[1] >= 0 && skills[2] >= 0, "three trainer skills");
    for (k = 0; k < 3; ++k)
        if (rpg_skill_type(&g_rpg, skills[k]) >= 0 &&
            vm_prop(&g_vm, &pc, MWP_SKILL_BLOCK + skills[k]) < vm_prop(&g_vm, &elder, MWP_SKILL_BLOCK + skills[k]))
            break;
    if (k < 3) {
        float s0 = vm_prop(&g_vm, &pc, MWP_SKILL_BLOCK + skills[k]);
        float hour = g_vm.hour;
        before = rpg_gold(&g_rpg, &pc);
        CHECK(rpg_train(&g_rpg, &elder, skills[k], msg, sizeof(msg)) == 1, "trained (%s)", msg);
        CHECK(vm_prop(&g_vm, &pc, MWP_SKILL_BLOCK + skills[k]) == s0 + 1.0f && rpg_gold(&g_rpg, &pc) < before,
              "skill +1 for gold");
        CHECK(fabsf(fmodf(g_vm.hour - hour + 24.0f, 24.0f) - 2.0f) < 0.01f, "training takes two hours");
    } else {
        CHECK(0, "no trainable skill");
    }
    /* spells */
    {
        int n = rpg_spells_for_sale(&g_rpg, &elder, spells, 8);
        CHECK(n == 2, "spark + mend for sale (%d)", n);
        CHECK(rpg_buy_spell(&g_rpg, &elder, H("tw_heal"), msg, sizeof(msg)) && knows(H("tw_heal")), "bought Mend");
    }
    /* repair */
    {
        int sword = rpg_equipped(&g_rpg, &pc, RPG_SLOT_WEAPON);
        const VmItem *it;
        int price = rpg_repair_price(&g_rpg, &elder, sword), h0;
        set_skill(SKILL_ARMORER, 100.0f);
        it = vm_item_at(&g_vm, sword);
        h0 = it->health;
        CHECK(price > 0, "repair price %d", price);
        rpg_repair_self(&g_rpg, stack_of("tw_hammer"), sword, msg, sizeof(msg));
        sword = rpg_equipped(&g_rpg, &pc, RPG_SLOT_WEAPON);
        it = vm_item_at(&g_vm, sword);
        CHECK(it->health > h0, "hammer repairs (%d -> %u)", h0, (unsigned)it->health);
        if (it->health < 400)
            CHECK(rpg_repair_service(&g_rpg, &elder, sword, msg, sizeof(msg)) == 1, "smith repairs the rest");
    }
    /* persuasion */
    {
        int ok, d0 = rpg_disposition(&g_rpg, &weaver), change = 0, tries;
        for (tries = 0; tries < 10 && !change; ++tries)
            change = rpg_persuade(&g_rpg, &weaver, RPG_PERSUADE_ADMIRE, &ok);
        CHECK(change != 0 && rpg_disposition(&g_rpg, &weaver) == PW_CLAMP(d0 + change, 0, 100),
              "admire changes disposition (%d)", change);
        rpg_end_conversation(&g_rpg, &weaver);
        CHECK(vm_prop(&g_vm, &weaver, MWP_DISPTEMP) == 0.0f, "temporary part cleared at goodbye");
    }
    /* travel */
    {
        float npos[3] = { 2000.0f, 0.0f, 0.0f };
        int price = rpg_travel_price(&g_rpg, &elder, 0, 1, npos);
        CHECK(rpg_travel_count(&g_rpg, &elder) == 1 && price >= 1, "travel destination priced %d", price);
        g_teleports = 0;
        CHECK(rpg_travel(&g_rpg, &elder, 0, price, msg, sizeof(msg)) && g_teleports == 1 &&
              g_tp_cell == H("test hamlet, cellar") && g_tp_pos[1] == -300.0f, "travel teleports");
    }
}

static void test_soul_and_enchant(void)
{
    VmRef pc = vm_player_ref(&g_vm), c = npc("tw_critter"), elder = npc("tw_villager");
    DRpgEffect trap;
    RpgEnchant en;
    char msg[128];
    int gem, cap, charge, points;
    float chance;
    const VmItem *gi;
    c.key = 0x5011u;
    memset(&trap, 0, sizeof(trap));
    trap.effect = MGEF_SOULTRAP;
    trap.range = RPG_RANGE_TOUCH;
    trap.duration = 30;
    trap.skill = trap.attribute = -1;
    rpg_apply(&g_rpg, &pc, &c, &trap, 1, RPG_RANGE_TOUCH, H("tw_trap"), 100.0f, 1);
    rpg_kill(&g_rpg, &c, &pc);
    gem = stack_of("tw_soulgem");
    gi = vm_item_at(&g_vm, gem);
    CHECK(gi && gi->soul == H("tw_critter"), "soul trapped in the petty gem");
    CHECK(rpg_item_value(&g_rpg, gi) == 10 + 75, "filled gem value (%d)", rpg_item_value(&g_rpg, gi));
    memset(&en, 0, sizeof(en));
    en.item_stack = stack_of("tw_boots");
    en.gem_stack = gem;
    en.type = 2;
    en.count = 1;
    en.effects[0].effect = MGEF_FEATHER;
    en.effects[0].mag_min = en.effects[0].mag_max = 1;
    en.effects[0].duration = 1;
    en.effects[0].skill = en.effects[0].attribute = -1;
    snprintf(en.name, sizeof(en.name), "Light Boots");
    points = rpg_enchant_points(&g_rpg, &en, &cap, &charge, &chance);
    CHECK(points == 1 && cap == 2 && charge == 25, "points %d capacity %d charge %d", points, cap, charge);
    CHECK(rpg_enchant(&g_rpg, &elder, &en, msg, sizeof(msg)) == 1, "enchanter succeeds (%s)", msg);
    CHECK(vm_item_count(&g_vm, pc.key, H("tw_soulgem")) == 0, "gem used");
    {
        uint32_t i;
        int found = 0;
        for (i = 0; i < g_rpg.custom_count; ++i)
            if (strcmp(g_rpg.custom[i].name, "Light Boots") == 0 && g_rpg.custom[i].kind == 0 &&
                vm_item_count(&g_vm, pc.key, g_rpg.custom[i].obj.hash) == 1)
                found = 1;
        CHECK(found, "enchanted boots in inventory");
    }
    /* spellmaking from known effects */
    {
        DRpgEffect e;
        uint8_t mask[RPGDB_EFFECTS];
        memset(&e, 0, sizeof(e));
        e.effect = MGEF_FROST_DAMAGE;
        e.range = RPG_RANGE_TOUCH;
        e.mag_min = e.mag_max = 5;
        e.duration = 1;
        e.skill = e.attribute = -1;
        CHECK(rpg_known_effects(&g_rpg, mask) >= 1 && mask[MGEF_FROST_DAMAGE], "frost known from Frost Bolt");
        vm_item_add(&g_vm, pc.key, H("gold_001"), 500);
        CHECK(rpg_make_spell(&g_rpg, &elder, &e, 1, "Chill Touch", msg, sizeof(msg)) == 1, "spell made (%s)", msg);
        CHECK(strcmp(rpg_spell_name(&g_rpg, g_rpg.custom[g_rpg.custom_count - 1].spell.hash), "Chill Touch") == 0 &&
              knows(g_rpg.custom[g_rpg.custom_count - 1].spell.hash), "custom spell known");
    }
}

static void test_world_effects(void)
{
    VmRef pc = vm_player_ref(&g_vm), door = npc("tw_door");
    DRpgEffect e;
    char msg[96];
    int lvl;
    memset(&e, 0, sizeof(e));
    e.skill = e.attribute = -1;
    e.effect = MGEF_MARK;
    rpg_apply(&g_rpg, &pc, &pc, &e, 1, RPG_RANGE_SELF, H("t_mark"), 100.0f, 1);
    CHECK(g_rpg.has_mark && g_rpg.mark_pos[0] == 1000.0f, "mark stored");
    e.effect = MGEF_RECALL;
    g_teleports = 0;
    rpg_apply(&g_rpg, &pc, &pc, &e, 1, RPG_RANGE_SELF, H("t_recall"), 100.0f, 1);
    CHECK(g_teleports == 1 && g_tp_pos[1] == 500.0f, "recall teleports to the mark");
    e.effect = MGEF_DIVINE_INTERVENTION;
    rpg_apply(&g_rpg, &pc, &pc, &e, 1, RPG_RANGE_SELF, H("t_div"), 100.0f, 1);
    CHECK(g_teleports == 2 && g_tp_cell == 0 && g_tp_pos[0] == 600.0f, "divine intervention to the marker (%g)",
          g_tp_pos[0]);
    /* locks */
    vm_set_prop(&g_vm, &door, MWP_LOCK, 20.0f);
    e.effect = MGEF_OPEN;
    e.range = RPG_RANGE_TOUCH;
    e.mag_min = e.mag_max = 25;
    rpg_apply(&g_rpg, &pc, &door, &e, 1, RPG_RANGE_TOUCH, H("t_open"), 100.0f, 1);
    CHECK(vm_prop(&g_vm, &door, MWP_LOCK) <= 0.0f, "open spell unlocks");
    set_skill(SKILL_SECURITY, 100.0f);
    rpg_equip(&g_rpg, &pc, stack_of("tw_pick"));
    CHECK(rpg_pick_lock(&g_rpg, &door, 10, &lvl, msg, sizeof(msg)) == 1 && lvl == 0, "lock picked (%s)", msg);
    {
        int st = rpg_equipped(&g_rpg, &pc, RPG_SLOT_WEAPON);
        CHECK(st >= 0 && vm_item_at(&g_vm, st)->health == 4, "a use of the lockpick spent");
    }
}

static void test_rest(void)
{
    VmRef pc = vm_player_ref(&g_vm);
    float hp;
    char msg[96];
    rpg_damage(&g_rpg, &pc, 0, 30.0f, NULL);
    hp = rpg_dyn(&g_rpg, &pc, 0);
    CHECK(rpg_can_rest(&g_rpg, msg, sizeof(msg)), "can rest");
    CHECK(rpg_rest(&g_rpg, 4, 1) == 4, "rested four hours");
    CHECK(fabsf(rpg_dyn(&g_rpg, &pc, 0) - PW_MIN(rpg_dyn_max(&g_rpg, &pc, 0), hp + 4.0f * 5.1f)) < 0.5f,
          "health back by 10%% of END per hour (%g -> %g)", hp, rpg_dyn(&g_rpg, &pc, 0));
    CHECK(rpg_dyn(&g_rpg, &pc, 2) == rpg_dyn_max(&g_rpg, &pc, 2), "fatigue full");
    CHECK(rpg_read_book(&g_rpg, H("tw_skillbook")) == SKILL_ALCHEMY && rpg_read_book(&g_rpg, H("tw_skillbook")) == -1,
          "skill book teaches once");
}

int main(int argc, char **argv)
{
    char err[200], path[512];
    RpgHost host;
    if (argc < 2) {
        fprintf(stderr, "usage: mech_test <data dir>\n");
        return 2;
    }
    mem_init();
    snprintf(path, sizeof(path), "%s/rpg.dpak", argv[1]);
    if (rpgdb_open(&g_db, path, err, sizeof(err)) != 0) {
        printf("rpgdb_open failed: %s\n", err);
        return 1;
    }
    CHECK(vm_init(&g_vm, &g_db, NULL) == 0, "vm");
    memset(&host, 0, sizeof(host));
    host.teleport = mock_teleport;
    host.locate = mock_locate;
    host.killed = mock_killed;
    host.hostiles_near = mock_hostiles;
    CHECK(rpg_init(&g_rpg, &g_vm, &g_db, &host) == 0, "rpg");
    test_chargen();
    test_levelling();
    test_equipment();
    test_combat();
    test_magic();
    test_alchemy();
    test_trade();
    test_soul_and_enchant();
    test_world_effects();
    test_rest();
    CHECK(g_vm.errors == 0, "no VM errors (%s)", g_vm.last_error);
    printf("mech_test: %d/%d checks passed\n", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
