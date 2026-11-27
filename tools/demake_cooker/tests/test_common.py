"""dmcommon: hashing, BSA, DataSource, DPAK, and the C <-> Python layout contract."""

import os
import re
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
COOKER = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(COOKER))
sys.path.insert(0, COOKER)

import dmcommon  # noqa: E402
import dmesh  # noqa: E402
import anim_demaker  # noqa: E402
import dzone  # noqa: E402
import sound_demaker  # noqa: E402
import mwscript  # noqa: E402
import rpg_compiler  # noqa: E402
import tex_demaker  # noqa: E402


class HashTests(unittest.TestCase):
    def test_fnv_reference_values(self):
        self.assertEqual(dmcommon.fnv1a32(""), 0x811C9DC5)
        self.assertEqual(dmcommon.fnv1a32("a"), 0xE40C292C)
        self.assertEqual(dmcommon.fnv1a32("foobar"), 0xBF9CF968)

    def test_hash_is_case_and_separator_insensitive(self):
        self.assertEqual(dmcommon.fnv1a32("Icons\\M\\Tx_Gold"), dmcommon.fnv1a32("icons/m/tx_gold"))

    def test_keys(self):
        self.assertEqual(dmcommon.texture_key("Textures\\Tx_Wood_01.TGA"), "tx_wood_01")
        self.assertEqual(dmcommon.texture_key("textures/menu/foo.dds"), "menu/foo")
        self.assertEqual(dmcommon.texture_key("icons/m/tx_gold.dds"), "icons/m/tx_gold")
        self.assertEqual(dmcommon.mesh_key("Meshes\\x\\Ex_Hlaalu_B_01.NIF"), "x/ex_hlaalu_b_01")
        self.assertEqual(dmcommon.normalize_path("Data Files/Meshes/A.nif"), "meshes/a.nif")


class BsaTests(unittest.TestCase):
    def test_hash_matches_known_tes3_value(self):
        # Value computed by the reference TES3 algorithm (OpenMW BSAFile::getHash).
        low, high = dmcommon.bsa_hash("meshes\\a.nif")
        self.assertEqual(low, ord("m") ^ (ord("e") << 8) ^ (ord("s") << 16) ^ (ord("h") << 24)
                         ^ ord("e") ^ (ord("s") << 8))
        self.assertIsInstance(high, int)

    def test_roundtrip(self):
        files = {"meshes\\a.nif": b"AAAA", "textures/B.dds": b"bbbbbb", "icons\\c.dds": b""}
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "Test.bsa")
            dmcommon.write_bsa(path, files)
            arc = dmcommon.BSAArchive(path)
            self.assertEqual(sorted(arc.names()), ["icons/c.dds", "meshes/a.nif", "textures/b.dds"])
            self.assertEqual(arc.read("Meshes/A.nif"), b"AAAA")
            self.assertEqual(arc.read("textures\\b.dds"), b"bbbbbb")
            self.assertEqual(arc.read("icons/c.dds"), b"")

    def test_rejects_non_tes3(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "bad.bsa")
            with open(path, "wb") as f:
                f.write(struct.pack("<III", 0x67, 0, 0))
            with self.assertRaises(ValueError):
                dmcommon.BSAArchive(path)

    def test_datasource_layering(self):
        with tempfile.TemporaryDirectory() as d:
            dmcommon.write_bsa(os.path.join(d, "Morrowind.bsa"),
                               {"meshes\\a.nif": b"base", "meshes\\b.nif": b"base-b"})
            dmcommon.write_bsa(os.path.join(d, "Tribunal.bsa"), {"meshes\\a.nif": b"tribunal"})
            os.makedirs(os.path.join(d, "Meshes"))
            with open(os.path.join(d, "Meshes", "B.NIF"), "wb") as f:
                f.write(b"loose")
            src = dmcommon.DataSource(dmcommon.DataConfig((d,)))
            self.assertEqual(src.read("meshes/a.nif"), b"tribunal")  # later BSA wins
            self.assertEqual(src.read("meshes/b.nif"), b"loose")  # loose wins
            self.assertEqual(src.list("meshes/", (".nif",)), ["meshes/a.nif", "meshes/b.nif"])
            self.assertFalse(src.exists("meshes/c.nif"))


class DPakTests(unittest.TestCase):
    def test_roundtrip_sorted_aligned(self):
        w = dmcommon.DPakWriter(dmcommon.DPAK_KIND_TEXTURE)
        for i in range(20):
            w.add(f"tx_item_{i}", bytes([i]) * (i * 7 + 1))
        data = w.build()
        r = dmcommon.DPakReader(data)
        self.assertEqual(r.kind, dmcommon.DPAK_KIND_TEXTURE)
        self.assertEqual(len(r.entries), 20)
        for i in range(20):
            self.assertEqual(r.get(f"tx_item_{i}"), bytes([i]) * (i * 7 + 1))
        for _h, ofs, _s in r.entries.values():
            self.assertEqual(ofs % dmcommon.DPAK_ENTRY_ALIGN, 0)

    def test_reader_rejects_unsorted_directory(self):
        w = dmcommon.DPakWriter(dmcommon.DPAK_KIND_GENERIC)
        w.add("a", b"1")
        w.add("b", b"2")
        data = bytearray(w.build())
        dir_ofs = struct.unpack_from("<I", data, 12)[0]
        first, second = data[dir_ofs:dir_ofs + 16], data[dir_ofs + 16:dir_ofs + 32]
        data[dir_ofs:dir_ofs + 32] = second + first
        with self.assertRaises(ValueError):
            dmcommon.DPakReader(bytes(data))


class LayoutContractTests(unittest.TestCase):
    """Every PW_STATIC_ASSERT(sizeof(X) == N) in src/fmt.h must match the
    struct format the Python writers use."""

    PY_SIZES = {
        "DPakHeader": struct.calcsize(dmcommon.DPAK_HEADER_FMT),
        "DPakEntry": struct.calcsize(dmcommon.DPAK_ENTRY_FMT),
        "DTexLevel": struct.calcsize(tex_demaker.DTEX_LEVEL_FMT),
        "DTexHeader": tex_demaker.DTEX_HEADER_SIZE,
        "DAtlasHeader": struct.calcsize("<4sHHIHHII8x"),
        "DAtlasEntry": struct.calcsize("<IHHHHHH"),
        "DMeshHeader": dmesh.SIZES["header"],
        "DMeshNode": dmesh.SIZES["node"],
        "DMeshVertex": dmesh.SIZES["vertex"],
        "DMeshBatch": dmesh.SIZES["batch"],
        "DMeshTexture": dmesh.SIZES["texture"],
        "DMeshEmitter": dmesh.SIZES["emitter"],
        "DMeshCollision": dmesh.SIZES["collision"],
        **dzone.SIZES,
        **anim_demaker.SIZES,
        **rpg_compiler.SIZES,
        **sound_demaker.SIZES,
        "DRpgSnippet": struct.calcsize(mwscript.SNIPPET_HEADER_FMT),
    }

    def test_sizes_match_c_header(self):
        with open(os.path.join(REPO, "src", "fmt.h"), encoding="utf-8") as f:
            text = f.read()
        found = dict((m.group(1), int(m.group(2))) for m in
                     re.finditer(r"PW_STATIC_ASSERT\(sizeof\((\w+)\) == (\d+)", text))
        self.assertEqual(set(found), set(self.PY_SIZES))
        for name, size in found.items():
            self.assertEqual(self.PY_SIZES[name], size, name)

    def test_dtex_header_prefix(self):
        self.assertEqual(struct.calcsize(tex_demaker.DTEX_HEADER_FMT), 24)

    def test_bytecode_opcodes_match(self):
        with open(os.path.join(REPO, "src", "fmt.h"), encoding="utf-8") as f:
            text = f.read()
        for cname, value in (("MWBC_RET", mwscript.OP_RET), ("MWBC_PUSHS", mwscript.OP_PUSHS),
                             ("MWBC_LOADM", mwscript.OP_LOADM), ("MWBC_STORED", mwscript.OP_STORED),
                             ("MWBC_DIVF", mwscript.OP_DIVF), ("MWBC_CMPI", mwscript.OP_CMPI),
                             ("MWBC_CMPF", mwscript.OP_CMPF), ("MWBC_JZ", mwscript.OP_JZ),
                             ("MWBC_CALLX", mwscript.OP_CALLX), ("MWBC_MSGBOX", mwscript.OP_MSGBOX),
                             ("MWBC_OP_COUNT", mwscript.OP_COUNT), ("DRPG_CF_GLOBAL", rpg_compiler.CF_GLOBAL),
                             ("DRPG_CF_NOT_LOCAL", rpg_compiler.CF_NOT_LOCAL),
                             ("DRPG_CF_ACTOR", rpg_compiler.CF_ACTOR), ("DRPG_CF_CELL", rpg_compiler.CF_CELL),
                             ("DRPG_SF_ONACTIVATE", mwscript.SF_ONACTIVATE)):
            m = re.search(r"\b" + cname + r"\s*=\s*(0x[0-9A-Fa-f]+|\d+|1 << \d+)", text)
            self.assertIsNotNone(m, cname)
            raw = m.group(1)
            got = 1 << int(raw.split("<<")[1]) if "<<" in raw else int(raw, 0)
            self.assertEqual(got, value, cname)
        for name, ot in rpg_compiler.OT.items():
            cname = "DRPG_OT_" + name.rstrip("_")
            self.assertRegex(text, r"\b" + cname + r"\b", cname)
        order = re.search(r"enum \{\s*DRPG_OT_NPC = 0,([^}]*)\}", text).group(1)
        names = ["NPC"] + [n.strip().replace("DRPG_OT_", "") for n in order.replace("\n", " ").split(",")
                           if n.strip()]
        self.assertEqual(names[:len(rpg_compiler.OBJ_TYPES)],
                         [t.rstrip("_") for t in rpg_compiler.OBJ_TYPES])

    def test_enum_values_match(self):
        with open(os.path.join(REPO, "src", "fmt.h"), encoding="utf-8") as f:
            text = f.read()
        for cname, value in (("DMESH_BATCH_F_SEGMENT", dmesh.BATCH_F_SEGMENT),
                             ("DMESH_BATCH_F_ADDITIVE", dmesh.BATCH_F_ADDITIVE),
                             ("DMESH_NODE_F_EMITTER", dmesh.NODE_F_EMITTER),
                             ("DMESH_F_PARTICLES", dmesh.DMESH_F_PARTICLES),
                             ("DTEX_F_ATLAS_PAGE", tex_demaker.DTEX_F_ATLAS_PAGE)):
            m = re.search(cname + r"\s*=\s*1 << (\d+)", text)
            self.assertIsNotNone(m, cname)
            self.assertEqual(1 << int(m.group(1)), value, cname)

    def test_m6_enums_match(self):
        import mwdata
        with open(os.path.join(REPO, "src", "fmt.h"), encoding="utf-8") as f:
            text = f.read()

        def enum_values(first):
            body = re.search(r"enum \{\s*" + first + r"\s*=\s*0,([^}]*)\}", text).group(1)
            return [first] + [n.split("=")[0].strip() for n in body.replace("\n", " ").split(",") if n.strip()]

        self.assertEqual(enum_values("DRPG_AI_WANDER")[:5],
                         ["DRPG_AI_WANDER", "DRPG_AI_TRAVEL", "DRPG_AI_ESCORT", "DRPG_AI_FOLLOW", "DRPG_AI_ACTIVATE"])
        self.assertEqual((rpg_compiler.AI_WANDER, rpg_compiler.AI_TRAVEL, rpg_compiler.AI_ESCORT,
                          rpg_compiler.AI_FOLLOW, rpg_compiler.AI_ACTIVATE), (0, 1, 2, 3, 4))
        self.assertEqual(len([n for n in enum_values("DRPG_SNDG_LEFT") if n != "DRPG_SNDG_COUNT"]),
                         len(mwdata.SNDG_TYPES))
        self.assertRegex(text, r"DRPG_MARKER_PRISON = 2")
        self.assertRegex(text, r"DSND_CODEC_ADPCM = %d, DSND_CODEC_MP3 = %d" % (sound_demaker.CODEC_ADPCM,
                                                                                sound_demaker.CODEC_MP3))
        self.assertIn("0x%08Xu" % sound_demaker.DSND_MAGIC, text)
        self.assertIn("0x%08Xu" % rpg_compiler.PGRD_MAGIC, text)


if __name__ == "__main__":
    unittest.main()
