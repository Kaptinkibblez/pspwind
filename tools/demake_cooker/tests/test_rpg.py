"""rpg_compiler.py: record extraction, INFO ordering, conditions, rpg.dpak."""

import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.dirname(HERE))

import dmcommon  # noqa: E402
import esm  # noqa: E402
import make_testworld  # noqa: E402
import mwscript  # noqa: E402
import rpg_compiler as R  # noqa: E402


def info(iid, prev="", nxt="", text=""):
    return {"id": iid, "prev": prev, "next": nxt, "text": text}


class InfoOrderTests(unittest.TestCase):
    def test_master_order_then_plugin_inserts(self):
        o = R.InfoOrder()
        o.insert(info("a"), False)
        o.insert(info("b", "a"), False)
        o.insert(info("c", "b"), False)
        o.insert(info("x", "a", "b"), False)   # plugin: between a and b
        o.insert(info("y", "", "a"), False)    # plugin: in front
        self.assertEqual([i["id"] for i in o.infos()], ["y", "a", "x", "b", "c"])

    def test_replace_delete_and_move(self):
        o = R.InfoOrder()
        for iid, prev in (("a", ""), ("b", "a"), ("c", "b")):
            o.insert(info(iid, prev), False)
        o.insert(info("b", "a", text="new"), False)   # same prev: replaced in place
        self.assertEqual([(i["id"], i["text"]) for i in o.infos()], [("a", ""), ("b", "new"), ("c", "")])
        o.insert(info("a", "c"), False)               # new prev: moved after c
        self.assertEqual([i["id"] for i in o.infos()], ["b", "c", "a"])
        o.insert(info("c", "b"), True)                # deleted
        self.assertEqual([i["id"] for i in o.infos()], ["b", "a"])

    def test_unknown_neighbours_append(self):
        o = R.InfoOrder()
        o.insert(info("a"), False)
        o.insert(info("z", "missing", "alsomissing"), False)
        self.assertEqual([i["id"] for i in o.infos()], ["a", "z"])


class SelectTests(unittest.TestCase):
    def test_function_rule(self):
        self.assertEqual(R.parse_select("01500", 1), (0, 50, 0, "", 1))

    def test_variable_rules(self):
        self.assertEqual(R.parse_select("23sX3metpc", 2)[1:4], (R.CF_LOCAL, 3, "metpc"))
        self.assertEqual(R.parse_select("14JX4quest", 10)[1], R.CF_JOURNAL)
        self.assertEqual(R.parse_select("0BLX0Balmora", 0)[1], R.CF_NOT_CELL)
        self.assertEqual(R.parse_select("0CsX0x", 0)[1], R.CF_NOT_LOCAL)

    def test_bad_rules(self):
        self.assertIsNone(R.parse_select("0199", 0))        # too short
        self.assertIsNone(R.parse_select("01999", 0))       # function out of range
        self.assertIsNone(R.parse_select("02sX9x", 0))      # bad comparison
        self.assertIsNone(R.parse_select("0ZsX0x", 0))      # bad type
        self.assertIsNone(R.parse_select("02sX0", 0))       # missing variable


class TextTests(unittest.TestCase):
    def test_book_html(self):
        raw = '<DIV ALIGN="CENTER">Title<BR>\r\n<BR>\r\n<FONT COLOR="000000">Line &amp; more<P>Next<IMG SRC="x.tga">'
        self.assertEqual(R.book_text(raw), "Title\n\nLine & more\n\nNext")

    def test_dialogue_links_flattened(self):
        self.assertEqual(R.dialogue_text("Ask about @Vivec#, friend."), "Ask about Vivec, friend.")

    def test_blob_store_blocks(self):
        b = R.BlobStore(block_size=64)
        ids = [b.add_text("x" * 40) for _ in range(1)] + [b.add_text("y" * 40), b.add_text("x" * 40)]
        self.assertEqual(ids[0], ids[2])            # deduplicated
        self.assertEqual(ids[1] >> 16, 1)          # second block
        self.assertEqual(ids[1] & 0xFFFF, 0)


class CookTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        data = make_testworld.make(cls.tmp.name)
        cls.out = os.path.join(cls.tmp.name, "cooked")
        cls.report = R.cook_rpg([os.path.join(data, "TestWorld.esm")], cls.out)
        cls.pack = dmcommon.DPakReader(dmcommon.read_file(os.path.join(cls.out, "rpg.dpak")))
        cls.core = R.read_rpg(cls.pack.get("rpg/core"))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def obj(self, oid):
        h = dmcommon.fnv1a32(oid)
        return next(o for o in self.core["objects"] if o[0] == h)

    def topic(self, name):
        h = dmcommon.fnv1a32(name)
        return next(i for i, t in enumerate(self.core["topics"]) if t[0] == h)

    def test_report(self):
        r = self.report
        self.assertEqual(r["scripts"], 4)
        self.assertEqual(r["script_errors"], {})
        self.assertEqual(r["quests"], 1)
        self.assertEqual(r["dialogues"], 16)  # 7 topics + 9 Persuasion-type
        self.assertEqual(r["infos"], 31)  # + the guard's greetings and crime answers
        self.assertEqual(r["result_script_failures"], 0)
        self.assertGreater(r["script_warnings"], 0)  # the deliberately broken script

    def test_tables_sorted(self):
        for key in ("objects", "factions", "globals", "gmsts", "scripts", "spells"):
            hashes = [row[0] for row in self.core[key]]
            self.assertEqual(hashes, sorted(hashes), key)
        names = [self.core["name"](t[1]).lower() for t in self.core["topics"]]
        self.assertEqual(names, sorted(names))
        by_hash = [self.core["topics"][i][0] for i in self.core["topic_hash_index"]]
        self.assertEqual(by_hash, sorted(by_hash))

    def test_objects(self):
        name = self.core["name"]
        ring = self.obj("tw_ring")
        self.assertEqual(name(ring[1]), "Weaver's Ring")
        self.assertEqual(ring[2], R.OT["MISC"])
        self.assertEqual(ring[9], 120)
        self.assertEqual(ring[6], dmcommon.fnv1a32("icons/m/tw_ring"))
        self.assertTrue(ring[5] & R.OF_UNIQUE)
        weaver = self.obj("tw_villager_f")
        self.assertEqual(weaver[3], 1)  # female
        self.assertEqual(weaver[10:13], (dmcommon.fnv1a32("tw_folk"), dmcommon.fnv1a32("tw_weaver"),
                                         dmcommon.fnv1a32("tw_weavers")))
        actor = self.core["actors"][weaver[17]]
        self.assertEqual((actor[0], actor[21], actor[22], actor[23]), (3, 40, 5, 2))  # level disp rep rank
        inv = self.core["lists"][weaver[15]:weaver[15] + weaver[13]]
        self.assertEqual(inv, [(dmcommon.fnv1a32("tw_coin"), 2)])
        dagger = self.obj("tw_dagger")
        self.assertEqual(dagger[10], dmcommon.fnv1a32("tw_spark_en"))
        self.assertEqual(dagger[11] & 0xFF, 2)          # chop min
        self.assertEqual(dagger[17:19], (0, 300))       # type, health

    def test_player_record(self):
        self.assertNotEqual(self.core["player"], R.NONE32)
        self.assertEqual(self.core["objects"][self.core["player"]][0], dmcommon.fnv1a32("player"))

    def test_conditions(self):
        t = self.core["topics"][self.topic("lost ring")]
        infos = self.core["infos"][t[4]:t[4] + t[5]]
        first = infos[0]
        conds = self.core["conds"][first[3]:first[3] + first[4]]
        self.assertEqual([c[0] for c in conds], [R.CF_ACTOR, 50])  # header filter first, then Choice
        self.assertEqual(conds[0][4], dmcommon.fnv1a32("tw_villager_f"))
        self.assertEqual((conds[1][1], conds[1][5]), (0, 1))
        self.assertTrue(first[5] & R.IF_HAS_ACTOR)
        reward = infos[4]
        rc = self.core["conds"][reward[3]:reward[3] + reward[4]]
        self.assertEqual([c[0] for c in rc], [R.CF_ACTOR, R.CF_JOURNAL, R.CF_ITEM])
        self.assertEqual(rc[2][1], 3)  # >=
        self.assertNotEqual(reward[1], R.NONE32)  # result script

    def test_journal(self):
        t = self.core["topics"][self.topic("tw_lostring")]
        self.assertEqual(t[2], R.TT_JOURNAL)
        infos = self.core["infos"][t[4]:t[4] + t[5]]
        self.assertEqual([i[7] for i in infos], [0, 10, 20, 100])
        self.assertTrue(infos[0][5] & R.IF_QUEST_NAME)
        self.assertTrue(infos[3][5] & R.IF_QUEST_FINISHED)
        self.assertEqual(R.read_text(self.pack, self.core, infos[0][0]), "The Weaver's Lost Ring")

    def test_result_script_blob(self):
        t = self.core["topics"][self.topic("lost ring")]
        reward = self.core["infos"][t[4] + 4]
        blob = R.read_blob(self.pack, self.core, reward[1])
        flags, _p, code, strings = struct.unpack_from(mwscript.SNIPPET_HEADER_FMT, blob)
        hdr = struct.calcsize(mwscript.SNIPPET_HEADER_FMT)
        listing = mwscript.disassemble(blob[hdr:hdr + code], blob[hdr + code:hdr + code + strings])
        text = "\n".join(listing)
        self.assertIn("->removeitem/2", text)
        self.assertIn("call journal/2", text)
        self.assertIn("stored", text)  # speaker local metpc by name
        self.assertTrue(flags & mwscript.SF_DYNAMIC)

    def test_scripts_and_globals(self):
        name = self.core["name"]
        scripts = {name(s[1]).lower(): s for s in self.core["scripts"]}
        ring = scripts["tw_ring_script"]
        self.assertTrue(ring[5] & mwscript.SF_ONACTIVATE)
        code = self.pack.get("script/tw_ring_script")
        self.assertEqual(len(code), ring[8] + ring[9])
        weaver = scripts["tw_weaver_script"]
        self.assertEqual(weaver[2:5], (2, 0, 1))
        vars_ = self.core["vars"][weaver[6]:weaver[6] + weaver[7]]
        self.assertEqual(sorted(v[0] for v in vars_), [v[0] for v in vars_])
        self.assertEqual(len(self.core["starts"]), 1)
        globals_ = {g[0]: g for g in self.core["globals"]}
        gh = globals_[dmcommon.fnv1a32("gamehour")]
        self.assertEqual(gh[1], 2)
        self.assertEqual(struct.unpack("<f", struct.pack("<I", gh[2]))[0], 9.0)

    def test_text_blocks_round_trip(self):
        for i, block in enumerate(self.core["blocks"]):
            raw = R.zlib.decompress(self.pack.get(f"text/{i}"), -15)
            self.assertEqual(len(raw), block[1])
            self.assertEqual(block[0], dmcommon.fnv1a32(f"text/{i}"))

    def test_faction(self):
        fac = self.core["factions"][0]
        name = self.core["name"]
        self.assertEqual(name(fac[1]), "Weavers' Circle")
        self.assertEqual([name(o) for o in fac[2:6]], ["Novice", "Spinner", "Weaver", "Master Weaver"])
        self.assertEqual(fac[12:14], (4, 6))                       # favoured attributes
        self.assertEqual(fac[14:21], (9, 24, 16, -1, -1, -1, -1))  # favoured skills
        react = self.core["reactions"][fac[23]:fac[23] + fac[22]]
        self.assertEqual(react, [(dmcommon.fnv1a32("tw_weavers"), 2)])


    # ------------------------------------------------ DRPG v2 rules tables
    def test_skills_table(self):
        import mwdata
        skills = self.core["skills"]
        self.assertEqual(len(skills), 27)
        name = self.core["name"]
        for i, row in enumerate(skills):
            self.assertEqual(row[1:3], (mwdata.SKILLS[i][2], mwdata.SKILLS[i][3]), mwdata.SKILLS[i][0])
        self.assertEqual(name(skills[5][0]), "Long Blade")        # GMST sSkillLongblade fallback
        self.assertEqual(skills[5][1:3], (0, 0))                   # strength, combat
        self.assertEqual(skills[16][1:3], (1, 1))                  # alchemy: intelligence, magic

    def test_magic_effects_table(self):
        import mwdata
        mgefs = self.core["mgefs"]
        self.assertEqual(len(mgefs), 143)
        frost = mgefs[16]
        self.assertEqual(frost[5], 1)                              # MGEF record present
        self.assertAlmostEqual(frost[2], 5.0)                      # base cost from the record
        self.assertEqual(frost[4], 2)                              # destruction
        self.assertEqual(frost[6], mwdata.RESISTED_BY[16])        # resisted by Resist Frost
        self.assertTrue(frost[1] & mwdata.MF_HARMFUL)              # hard-coded flag merged in
        self.assertTrue(frost[1] & 0x200)                          # spellmaking (record flags)
        missing = mgefs[1]                                         # no MGEF record in the test world
        self.assertEqual(missing[5], 0)
        self.assertEqual(self.core["name"](frost[0]), "Frost Damage")

    def test_races_table(self):
        races = self.core["races"]
        self.assertEqual([r[0] for r in races], sorted(r[0] for r in races))
        by = {r[0]: r for r in races}
        folk = by[dmcommon.fnv1a32("tw_folk")]
        beast = by[dmcommon.fnv1a32("tw_beast")]
        self.assertEqual(self.core["name"](folk[1]), "Folk")
        self.assertEqual(folk[2:4], (5, 19))                       # long blade, sneak bonuses
        self.assertEqual(folk[9:11], (10, 5))
        self.assertEqual(folk[16:32:2], (40,) * 8)                 # male attributes
        self.assertEqual(folk[36] & 3, 1)                          # playable
        self.assertEqual(beast[36] & 3, 3)                         # playable + beast
        self.assertEqual(R.read_text(self.pack, self.core, folk[37]), "The Folk of the test shore.")
        powers = self.obj("tw_folk")
        spells = self.core["lists"][powers[16]:powers[16] + powers[14]]
        self.assertEqual([h for h, _ in spells], [dmcommon.fnv1a32("tw_power")])

    def test_birthsign_and_classes(self):
        sign = self.obj("tw_sign_blade")
        self.assertEqual(sign[2], R.OT["BSGN"])
        spells = [h for h, _ in self.core["lists"][sign[16]:sign[16] + sign[14]]]
        self.assertIn(dmcommon.fnv1a32("tw_ability_attack"), spells)
        self.assertTrue(self.obj("tw_warrior")[5] & R.OF_PLAYABLE)
        self.assertFalse(self.obj("tw_elder")[5] & R.OF_PLAYABLE)   # NPC-only class

    def test_travel_destinations(self):
        elder = self.obj("tw_villager")
        first, count = elder[18], elder[19]
        self.assertEqual(count, 1)
        x, y, z, _rz, cell = self.core["travel"][first]
        self.assertEqual(cell, dmcommon.fnv1a32("test hamlet, cellar"))
        self.assertEqual((round(x), round(y), round(z)), (0, -300, 0))
        self.assertEqual(self.obj("tw_villager_f")[19], 0)

    def test_markers(self):
        divine = [m for m in self.core["markers"] if m[5] == 0]
        self.assertEqual(len(divine), 1)
        self.assertEqual(divine[0][4], 0)                          # exterior

    def test_npc_autocalc(self):
        elder = self.obj("tw_villager")
        actor = self.core["actors"][elder[17]]
        self.assertEqual(actor[1], 52)                             # autocalculated health
        self.assertEqual(actor[-1] & 0x3C803, 0x3C803)             # AIDT services kept
        self.assertEqual(actor[4], 300)                            # barter gold


    # ------------------------------------------------ DRPG v3 tables
    def test_ai_packages(self):
        guard = self.obj("tw_guard")
        first, count = guard[20], guard[21]
        self.assertEqual(count, 1)
        pkg = self.core["aipkgs"][first]
        self.assertEqual(pkg[7], R.AI_WANDER)
        self.assertEqual(pkg[5], 500)                               # wander distance
        self.assertEqual(pkg[9:12], (40, 20, 10))                   # idle chances
        snapper = self.obj("tw_snapper")
        self.assertEqual(self.core["aipkgs"][snapper[20]][5], 300)
        actor = self.core["actors"][snapper[17]]
        self.assertEqual(actor[25], 100)                            # fight

    def test_sounds(self):
        sounds = {s[0]: s for s in self.core["sounds"]}
        swish = sounds[dmcommon.fnv1a32("weapon swish")]
        self.assertEqual(swish[1], dmcommon.fnv1a32("sound/fx/tw/swish"))
        self.assertEqual(swish[2], 255)
        waves = sounds[dmcommon.fnv1a32("tw waves")]
        self.assertEqual(waves[3:5], (10, 40))
        gens = self.core["soundgens"]
        self.assertEqual([g[2] for g in gens], [0, 1, 4, 5, 6])
        self.assertTrue(all(g[0] == dmcommon.fnv1a32("tw_critter") for g in gens))
        frost = self.core["mgefs"][16]
        self.assertEqual(frost[9:13], tuple(dmcommon.fnv1a32("destruction " + k)
                                            for k in ("cast", "bolt", "hit", "area")))
        door = self.obj("tw_door")
        self.assertEqual(door[17] & 0xFFFFFFFF, dmcommon.fnv1a32("door wood open"))
        lantern = self.obj("tw_lantern")
        self.assertEqual(lantern[12], dmcommon.fnv1a32("fire"))
        region = self.obj("tw region")
        sounds_ = self.core["lists"][region[16]:region[16] + region[14]]
        self.assertEqual(sounds_, [(dmcommon.fnv1a32("tw waves"), 30)])

    def test_pathgrids(self):
        import struct as st
        for key, npts in (("pgrd/test hamlet, cellar", 9), ("pgrd/0,0", 11)):
            data = self.pack.get(key)
            magic, version, count, edges = st.unpack_from("<IHHI", data, 0)
            self.assertEqual((magic, version, count), (R.PGRD_MAGIC, 1, npts))
            firsts = st.unpack_from(f"<{count + 1}H", data, 12 + 12 * count)
            self.assertEqual(firsts[-1], edges)
        cellar = self.pack.get("pgrd/test hamlet, cellar")
        firsts = st.unpack_from("<10H", cellar, 12 + 12 * 9)
        self.assertEqual([firsts[i + 1] - firsts[i] for i in range(9)], [2, 3, 2, 3, 4, 3, 2, 3, 2])

    def test_prison_marker(self):
        kinds = sorted(m[5] for m in self.core["markers"])
        self.assertEqual(kinds, [0, 2])


class LoadOrderTests(unittest.TestCase):
    def test_plugin_overrides_and_merges(self):
        master = esm.record("DIAL", [esm.sub("NAME", esm.zs("hello")), esm.sub("DATA", b"\x00")])
        i1 = esm.record("INFO", [esm.sub("INAM", esm.zs("1")), esm.sub("PNAM", esm.zs("")),
                                 esm.sub("NNAM", esm.zs("")), esm.sub("NAME", b"first")])
        plugin_i = esm.record("INFO", [esm.sub("INAM", esm.zs("2")), esm.sub("PNAM", esm.zs("")),
                                       esm.sub("NNAM", esm.zs("1")), esm.sub("NAME", b"plugin first")])
        g1 = esm.record("GLOB", [esm.sub("NAME", esm.zs("g")), esm.sub("FNAM", b"l"),
                                 esm.sub("FLTV", struct.pack("<f", 1.0))])
        g2 = esm.record("GLOB", [esm.sub("NAME", esm.zs("G")), esm.sub("FNAM", b"l"),
                                 esm.sub("FLTV", struct.pack("<f", 7.0))])
        gd = R.GameData()
        gd.load_bytes(esm.tes3_header(3) + master + i1 + g1, "a.esm")
        gd.load_bytes(esm.tes3_header(3) + master + plugin_i + g2, "b.esp")
        self.assertEqual([i["text"] for i in gd.dialogues["hello"]["order"].infos()],
                         ["plugin first", "first"])
        self.assertEqual(gd.globals["g"][2], 7.0)


class ToolTests(unittest.TestCase):
    def test_generated_header_is_fresh(self):
        rc = subprocess.call([sys.executable, os.path.join(os.path.dirname(HERE), "gen_mwfuncs.py"),
                              "--check"])
        self.assertEqual(rc, 0, "src/mwfuncs.h is stale: run gen_mwfuncs.py")


if __name__ == "__main__":
    unittest.main()
