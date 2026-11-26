/*
 * rpg_internal.h - helpers shared by rpg.c, rpg_items.c and rpg_trade.c.
 */
#ifndef PSPWIND_RPG_INTERNAL_H
#define PSPWIND_RPG_INTERNAL_H

#include "rpg.h"

#define RPG_GRAVITY 627.2f       /* units/s^2, the same as phys.c */
#define RPG_OF_SOULGEM (1u << 14) /* DRpgObject flag set on player-made objects never; ids decide */
#define RPG_POWER_SALT 0x2545F491u /* item store owner for "power used on day" records */

void rpg_notify(Rpg *r, const char *gmst, const char *fallback);
int rpg_is_player(const Rpg *r, const VmRef *ref);
int rpg_roll100(Rpg *r);
const DRpgObject *rpg_actor_object(const Rpg *r, const VmRef *ref);
void rpg_fx_start(Rpg *r, RpgFx *f, const VmRef *caster);
void rpg_bound_start(Rpg *r, RpgFx *f);
void rpg_bound_end(Rpg *r, RpgFx *f);
void rpg_disintegrate(Rpg *r, const VmRef *ref, int weapon, float amount);
void rpg_intervention(Rpg *r, int kind);
float rpg_spell_base_chance(Rpg *r, const VmRef *caster, const DRpgSpell *s);
/* sets the condition of one unit of a stack (splits it off); returns its stack */
int rpg_set_condition(Rpg *r, int stack, int health);
void rpg_set_charge(Rpg *r, int stack, float charge);
int rpg_is_equipped_any(Rpg *r, int stack);
int school_skill(int school);
int armor_skill_of(Rpg *r, const DRpgObject *o);
float rpg_effects_cost(Rpg *r, const DRpgEffect *e, int n, int player_spell);

#endif /* PSPWIND_RPG_INTERNAL_H */
