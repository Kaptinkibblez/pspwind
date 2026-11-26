"""Engine constants of the Morrowind ruleset used by the cooker and (via
gen_mwfuncs.py) the runtime: the 143 magic effects (identifier, GMST name,
the flags the original engine hard-codes per effect), the resistance /
weakness pairing, skills and attributes with their default data."""

# (identifier, GMST holding the display name, hard-coded MGEF flags)
EFFECTS = [
    ("WaterBreathing", "sEffectWaterBreathing", 0x11c8),  # 0
    ("SwiftSwim", "sEffectSwiftSwim", 0x11c0),  # 1
    ("WaterWalking", "sEffectWaterWalking", 0x11c8),  # 2
    ("Shield", "sEffectShield", 0x11e0),  # 3
    ("FireShield", "sEffectFireShield", 0x11e0),  # 4
    ("LightningShield", "sEffectLightningShield", 0x11e0),  # 5
    ("FrostShield", "sEffectFrostShield", 0x11e0),  # 6
    ("Burden", "sEffectBurden", 0x11d0),  # 7
    ("Feather", "sEffectFeather", 0x11c0),  # 8
    ("Jump", "sEffectJump", 0x11c0),  # 9
    ("Levitate", "sEffectLevitate", 0x11e0),  # 10
    ("SlowFall", "sEffectSlowFall", 0x11c0),  # 11
    ("Lock", "sEffectLock", 0x11184),  # 12
    ("Open", "sEffectOpen", 0x11184),  # 13
    ("FireDamage", "sEffectFireDamage", 0x1f0),  # 14
    ("ShockDamage", "sEffectShockDamage", 0x1f0),  # 15
    ("FrostDamage", "sEffectFrostDamage", 0x1f0),  # 16
    ("DrainAttribute", "sEffectDrainAttribute", 0x11d2),  # 17
    ("DrainHealth", "sEffectDrainHealth", 0x11f0),  # 18
    ("DrainMagicka", "sEffectDrainSpellpoints", 0x11d0),  # 19
    ("DrainFatigue", "sEffectDrainFatigue", 0x11d0),  # 20
    ("DrainSkill", "sEffectDrainSkill", 0x11d1),  # 21
    ("DamageAttribute", "sEffectDamageAttribute", 0x1d2),  # 22
    ("DamageHealth", "sEffectDamageHealth", 0x1f0),  # 23
    ("DamageMagicka", "sEffectDamageMagicka", 0x1d0),  # 24
    ("DamageFatigue", "sEffectDamageFatigue", 0x1d0),  # 25
    ("DamageSkill", "sEffectDamageSkill", 0x1d1),  # 26
    ("Poison", "sEffectPoison", 0x1f0),  # 27
    ("WeaknessToFire", "sEffectWeaknesstoFire", 0x11d0),  # 28
    ("WeaknessToFrost", "sEffectWeaknesstoFrost", 0x11d0),  # 29
    ("WeaknessToShock", "sEffectWeaknesstoShock", 0x11d0),  # 30
    ("WeaknessToMagicka", "sEffectWeaknesstoMagicka", 0x11d0),  # 31
    ("WeaknessToCommonDisease", "sEffectWeaknesstoCommonDisease", 0x11d0),  # 32
    ("WeaknessToBlightDisease", "sEffectWeaknesstoBlightDisease", 0x11d0),  # 33
    ("WeaknessToCorprusDisease", "sEffectWeaknesstoCorprusDisease", 0x11d0),  # 34
    ("WeaknessToPoison", "sEffectWeaknesstoPoison", 0x11d0),  # 35
    ("WeaknessToNormalWeapons", "sEffectWeaknesstoNormalWeapons", 0x11d0),  # 36
    ("DisintegrateWeapon", "sEffectDisintegrateWeapon", 0x1d0),  # 37
    ("DisintegrateArmor", "sEffectDisintegrateArmor", 0x1d0),  # 38
    ("Invisibility", "sEffectInvisibility", 0x11c8),  # 39
    ("Chameleon", "sEffectChameleon", 0x31c0),  # 40
    ("Light", "sEffectLight", 0x11c0),  # 41
    ("Sanctuary", "sEffectSanctuary", 0x11c0),  # 42
    ("NightEye", "sEffectNightEye", 0x11c0),  # 43
    ("Charm", "sEffectCharm", 0x1180),  # 44
    ("Paralyze", "sEffectParalyze", 0x11d8),  # 45
    ("Silence", "sEffectSilence", 0x11d8),  # 46
    ("Blind", "sEffectBlind", 0x11d0),  # 47
    ("Sound", "sEffectSound", 0x11d0),  # 48
    ("CalmHumanoid", "sEffectCalmHumanoid", 0x11180),  # 49
    ("CalmCreature", "sEffectCalmCreature", 0x11180),  # 50
    ("FrenzyHumanoid", "sEffectFrenzyHumanoid", 0x11180),  # 51
    ("FrenzyCreature", "sEffectFrenzyCreature", 0x11180),  # 52
    ("DemoralizeHumanoid", "sEffectDemoralizeHumanoid", 0x11180),  # 53
    ("DemoralizeCreature", "sEffectDemoralizeCreature", 0x11180),  # 54
    ("RallyHumanoid", "sEffectRallyHumanoid", 0x11180),  # 55
    ("RallyCreature", "sEffectRallyCreature", 0x11180),  # 56
    ("Dispel", "sEffectDispel", 0x11c4),  # 57
    ("Soultrap", "sEffectSoultrap", 0x111b8),  # 58
    ("Telekinesis", "sEffectTelekinesis", 0x1040),  # 59
    ("Mark", "sEffectMark", 0x104c),  # 60
    ("Recall", "sEffectRecall", 0x104c),  # 61
    ("DivineIntervention", "sEffectDivineIntervention", 0x104c),  # 62
    ("AlmsiviIntervention", "sEffectAlmsiviIntervention", 0x104c),  # 63
    ("DetectAnimal", "sEffectDetectAnimal", 0x1040),  # 64
    ("DetectEnchantment", "sEffectDetectEnchantment", 0x1040),  # 65
    ("DetectKey", "sEffectDetectKey", 0x1040),  # 66
    ("SpellAbsorption", "sEffectSpellAbsorption", 0x11c0),  # 67
    ("Reflect", "sEffectReflect", 0x11c0),  # 68
    ("CureCommonDisease", "sEffectCureCommonDisease", 0x1cc),  # 69
    ("CureBlightDisease", "sEffectCureBlightDisease", 0x1cc),  # 70
    ("CureCorprusDisease", "sEffectCureCorprusDisease", 0x1cc),  # 71
    ("CurePoison", "sEffectCurePoison", 0x1cc),  # 72
    ("CureParalyzation", "sEffectCureParalyzation", 0x1cc),  # 73
    ("RestoreAttribute", "sEffectRestoreAttribute", 0x1c2),  # 74
    ("RestoreHealth", "sEffectRestoreHealth", 0x1c0),  # 75
    ("RestoreMagicka", "sEffectRestoreSpellPoints", 0x1c0),  # 76
    ("RestoreFatigue", "sEffectRestoreFatigue", 0x1c0),  # 77
    ("RestoreSkill", "sEffectRestoreSkill", 0x1c1),  # 78
    ("FortifyAttribute", "sEffectFortifyAttribute", 0x11c2),  # 79
    ("FortifyHealth", "sEffectFortifyHealth", 0x11c0),  # 80
    ("FortifyMagicka", "sEffectFortifySpellpoints", 0x11c0),  # 81
    ("FortifyFatigue", "sEffectFortifyFatigue", 0x11c0),  # 82
    ("FortifySkill", "sEffectFortifySkill", 0x11c1),  # 83
    ("FortifyMaximumMagicka", "sEffectFortifyMagickaMultiplier", 0x11c0),  # 84
    ("AbsorbAttribute", "sEffectAbsorbAttribute", 0x21192),  # 85
    ("AbsorbHealth", "sEffectAbsorbHealth", 0x20190),  # 86
    ("AbsorbMagicka", "sEffectAbsorbSpellPoints", 0x20190),  # 87
    ("AbsorbFatigue", "sEffectAbsorbFatigue", 0x20190),  # 88
    ("AbsorbSkill", "sEffectAbsorbSkill", 0x21191),  # 89
    ("ResistFire", "sEffectResistFire", 0x11c0),  # 90
    ("ResistFrost", "sEffectResistFrost", 0x11c0),  # 91
    ("ResistShock", "sEffectResistShock", 0x11c0),  # 92
    ("ResistMagicka", "sEffectResistMagicka", 0x11c0),  # 93
    ("ResistCommonDisease", "sEffectResistCommonDisease", 0x11c0),  # 94
    ("ResistBlightDisease", "sEffectResistBlightDisease", 0x11c0),  # 95
    ("ResistCorprusDisease", "sEffectResistCorprusDisease", 0x11c0),  # 96
    ("ResistPoison", "sEffectResistPoison", 0x11c0),  # 97
    ("ResistNormalWeapons", "sEffectResistNormalWeapons", 0x11c0),  # 98
    ("ResistParalysis", "sEffectResistParalysis", 0x11c0),  # 99
    ("RemoveCurse", "sEffectRemoveCurse", 0x1c0),  # 100
    ("TurnUndead", "sEffectTurnUndead", 0x11190),  # 101
    ("SummonScamp", "sEffectSummonScamp", 0x9048),  # 102
    ("SummonClannfear", "sEffectSummonClannfear", 0x9048),  # 103
    ("SummonDaedroth", "sEffectSummonDaedroth", 0x9048),  # 104
    ("SummonDremora", "sEffectSummonDremora", 0x9048),  # 105
    ("SummonAncestralGhost", "sEffectSummonAncestralGhost", 0x9048),  # 106
    ("SummonSkeletalMinion", "sEffectSummonSkeletalMinion", 0x9048),  # 107
    ("SummonBonewalker", "sEffectSummonLeastBonewalker", 0x9048),  # 108
    ("SummonGreaterBonewalker", "sEffectSummonGreaterBonewalker", 0x9048),  # 109
    ("SummonBonelord", "sEffectSummonBonelord", 0x9048),  # 110
    ("SummonWingedTwilight", "sEffectSummonWingedTwilight", 0x9048),  # 111
    ("SummonHunger", "sEffectSummonHunger", 0x9048),  # 112
    ("SummonGoldenSaint", "sEffectSummonGoldenSaint", 0x9048),  # 113
    ("SummonFlameAtronach", "sEffectSummonFlameAtronach", 0x9048),  # 114
    ("SummonFrostAtronach", "sEffectSummonFrostAtronach", 0x9048),  # 115
    ("SummonStormAtronach", "sEffectSummonStormAtronach", 0x9048),  # 116
    ("FortifyAttack", "sEffectFortifyAttackBonus", 0x11c0),  # 117
    ("CommandCreature", "sEffectCommandCreatures", 0x1180),  # 118
    ("CommandHumanoid", "sEffectCommandHumanoids", 0x1180),  # 119
    ("BoundDagger", "sEffectBoundDagger", 0x5048),  # 120
    ("BoundLongsword", "sEffectBoundLongsword", 0x5048),  # 121
    ("BoundMace", "sEffectBoundMace", 0x5048),  # 122
    ("BoundBattleAxe", "sEffectBoundBattleAxe", 0x5048),  # 123
    ("BoundSpear", "sEffectBoundSpear", 0x5048),  # 124
    ("BoundLongbow", "sEffectBoundLongbow", 0x5048),  # 125
    ("ExtraSpell", "sEffectExtraSpell", 0x1188),  # 126
    ("BoundCuirass", "sEffectBoundCuirass", 0x5048),  # 127
    ("BoundHelm", "sEffectBoundHelm", 0x5048),  # 128
    ("BoundBoots", "sEffectBoundBoots", 0x5048),  # 129
    ("BoundShield", "sEffectBoundShield", 0x5048),  # 130
    ("BoundGloves", "sEffectBoundGloves", 0x5048),  # 131
    ("Corprus", "sEffectCorpus", 0x1048),  # 132
    ("Vampirism", "sEffectVampirism", 0x104c),  # 133
    ("SummonCenturionSphere", "sEffectSummonCenturionSphere", 0x1048),  # 134
    ("SunDamage", "sEffectSunDamage", 0x40),  # 135
    ("StuntedMagicka", "sEffectStuntedMagicka", 0x11c8),  # 136
    ("SummonFabricant", "sEffectSummonFabricant", 0x1048),  # 137
    ("SummonWolf", "sEffectSummonCreature01", 0x1048),  # 138
    ("SummonBear", "sEffectSummonCreature02", 0x1048),  # 139
    ("SummonBonewolf", "sEffectSummonCreature03", 0x1048),  # 140
    ("SummonCreature04", "sEffectSummonCreature04", 0x1048),  # 141
    ("SummonCreature05", "sEffectSummonCreature05", 0x1048),  # 142
]

# effect index -> effect whose magnitude resists it
RESISTED_BY = {
    38: 42,
    37: 42,
    17: 93,
    18: 93,
    19: 93,
    20: 93,
    21: 93,
    22: 93,
    23: 93,
    24: 93,
    25: 93,
    26: 93,
    85: 93,
    86: 93,
    87: 93,
    88: 93,
    89: 93,
    28: 93,
    29: 93,
    30: 93,
    31: 93,
    32: 93,
    33: 93,
    34: 93,
    35: 93,
    36: 93,
    7: 93,
    44: 93,
    46: 93,
    47: 93,
    48: 93,
    49: 93,
    50: 93,
    51: 93,
    52: 93,
    53: 93,
    54: 93,
    55: 93,
    56: 93,
    101: 93,
    14: 90,
    16: 91,
    15: 92,
    133: 94,
    132: 96,
    27: 97,
    45: 99,
}

# effect index -> effect whose magnitude makes the target weak to it
WEAKENED_BY = {
    17: 31,
    18: 31,
    19: 31,
    20: 31,
    21: 31,
    22: 31,
    23: 31,
    24: 31,
    25: 31,
    26: 31,
    85: 31,
    86: 31,
    87: 31,
    88: 31,
    89: 31,
    28: 31,
    29: 31,
    30: 31,
    31: 31,
    32: 31,
    33: 31,
    34: 31,
    35: 31,
    36: 31,
    7: 31,
    44: 31,
    46: 31,
    47: 31,
    48: 31,
    49: 31,
    50: 31,
    51: 31,
    52: 31,
    53: 31,
    54: 31,
    55: 31,
    56: 31,
    101: 31,
    14: 28,
    16: 29,
    15: 30,
    133: 32,
    132: 34,
    27: 35,
}
# MGEF flag bits
MF_TARGET_SKILL = 0x1
MF_TARGET_ATTRIBUTE = 0x2
MF_NO_DURATION = 0x4
MF_NO_MAGNITUDE = 0x8
MF_HARMFUL = 0x10
MF_CAST_SELF = 0x40
MF_CAST_TOUCH = 0x80
MF_CAST_TARGET = 0x100
MF_SPELLMAKING = 0x200
MF_ENCHANTING = 0x400
MF_APPLIED_ONCE = 0x1000
MF_NON_RECASTABLE = 0x4000
MF_UNREFLECTABLE = 0x10000
MF_CASTER_LINKED = 0x20000
MF_MODIFIABLE = MF_SPELLMAKING | MF_ENCHANTING | 0x800

ATTRIBUTES = ["Strength", "Intelligence", "Willpower", "Agility", "Speed", "Endurance",
              "Personality", "Luck"]

# (name, GMST, governing attribute, specialization 0 combat/1 magic/2 stealth,
#  default use values) - defaults only apply when a load order lacks SKIL records
SKILLS = [
    ("Block", "sSkillBlock", 3, 0, (2.5, 0, 0, 0)),
    ("Armorer", "sSkillArmorer", 0, 0, (0.2, 0, 0, 0)),
    ("Medium Armor", "sSkillMediumarmor", 5, 0, (1, 0, 0, 0)),
    ("Heavy Armor", "sSkillHeavyarmor", 5, 0, (1, 0, 0, 0)),
    ("Blunt Weapon", "sSkillBluntweapon", 0, 0, (1, 0, 0, 0)),
    ("Long Blade", "sSkillLongblade", 0, 0, (1, 0, 0, 0)),
    ("Axe", "sSkillAxe", 0, 0, (1, 0, 0, 0)),
    ("Spear", "sSkillSpear", 5, 0, (1, 0, 0, 0)),
    ("Athletics", "sSkillAthletics", 4, 0, (0.02, 0.03, 0, 0)),
    ("Enchant", "sSkillEnchant", 1, 1, (5, 0.1, 0, 0)),
    ("Destruction", "sSkillDestruction", 2, 1, (1, 0, 0, 0)),
    ("Alteration", "sSkillAlteration", 2, 1, (1, 0, 0, 0)),
    ("Illusion", "sSkillIllusion", 6, 1, (1, 0, 0, 0)),
    ("Conjuration", "sSkillConjuration", 1, 1, (1, 0, 0, 0)),
    ("Mysticism", "sSkillMysticism", 2, 1, (1, 0, 0, 0)),
    ("Restoration", "sSkillRestoration", 2, 1, (1, 0, 0, 0)),
    ("Alchemy", "sSkillAlchemy", 1, 1, (5, 0.5, 0, 0)),
    ("Unarmored", "sSkillUnarmored", 4, 1, (1, 0, 0, 0)),
    ("Security", "sSkillSecurity", 1, 2, (2, 2, 0, 0)),
    ("Sneak", "sSkillSneak", 3, 2, (0.5, 2, 0, 0)),
    ("Acrobatics", "sSkillAcrobatics", 0, 2, (0.15, 0.5, 0, 0)),
    ("Light Armor", "sSkillLightarmor", 3, 2, (1, 0, 0, 0)),
    ("Short Blade", "sSkillShortblade", 4, 2, (1, 0, 0, 0)),
    ("Marksman", "sSkillMarksman", 3, 2, (1, 0, 0, 0)),
    ("Mercantile", "sSkillMercantile", 6, 2, (0.3, 0.1, 0, 0)),
    ("Speechcraft", "sSkillSpeechcraft", 6, 2, (1, 0.5, 0, 0)),
    ("Hand-to-hand", "sSkillHandtohand", 4, 2, (1, 0, 0, 0)),
]

# magic school (MGEF) -> skill index
SCHOOL_SKILLS = [11, 13, 10, 12, 14, 15]  # alteration conjuration destruction illusion mysticism restoration

# ids of the intervention markers placed in the world
DIVINE_MARKER = "divinemarker"
TEMPLE_MARKER = "templemarker"
