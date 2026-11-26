"""esm.py + world_demaker.py + dzone.py."""

import math
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import dmcommon  # noqa: E402
import dzone  # noqa: E402
import esm  # noqa: E402
import make_testworld  # noqa: E402
import world_demaker as W  # noqa: E402


def ref(index, oid, pos=(0.0, 0.0, 0.0), rot=(0.0, 0.0, 0.0), extra=()):
    subs = [esm.sub("FRMR", struct.pack("<I", index)), esm.sub("NAME", esm.zs(oid))]
    subs += list(extra)
    subs.append(esm.sub("DATA", struct.pack("<6f", *pos, *rot)))
    return subs


def cell(name, flags, x, y, refs=()):
    subs = [esm.sub("NAME", esm.zs(name)), esm.sub("DATA", struct.pack("<iii", flags, x, y))]
    for r in refs:
        subs += r
    return esm.record("CELL", subs)


def stat(oid, model):
    return esm.record("STAT", [esm.sub("NAME", esm.zs(oid)), esm.sub("MODL", esm.zs(model))])


class LandCodecTests(unittest.TestCase):
    def test_vhgt_roundtrip(self):
        heights = [400.0 * math.sin(i * 0.11) + 96.0 * math.cos(i * 0.007)
                   for i in range(esm.LAND_NUM_VERTS)]
        heights = [round(h / 8.0) * 8.0 for h in heights]  # representable values
        dec = esm.decode_heights(esm.encode_heights(heights))
        self.assertEqual(len(dec), esm.LAND_NUM_VERTS)
        self.assertLess(max(abs(a - b) for a, b in zip(dec, heights)), 1e-3)

    def test_vhgt_decoding_rule(self):
        # offset 10, first column deltas accumulate per row, others per column
        deltas = [0] * esm.LAND_NUM_VERTS
        deltas[0] = 1
        deltas[1] = 2
        deltas[esm.LAND_SIZE] = -3
        raw = struct.pack("<f", 10.0) + struct.pack(f"<{esm.LAND_NUM_VERTS}b", *deltas) + b"\0\0\0"
        h = esm.decode_heights(raw)
        self.assertEqual(h[0], 11 * 8.0)
        self.assertEqual(h[1], 13 * 8.0)
        self.assertEqual(h[2], 13 * 8.0)
        self.assertEqual(h[esm.LAND_SIZE], 8 * 8.0)

    def test_vtex_transpose(self):
        grid = list(range(256))
        raw = esm.transpose_vtex(grid)
        self.assertEqual(esm.detranspose_vtex(raw), grid)
        # file order: first 16 entries are the top-left 4x4 block
        self.assertEqual(raw[:4], [0, 1, 2, 3])
        self.assertEqual(raw[4:8], [16, 17, 18, 19])


class EsmMergeTests(unittest.TestCase):
    def test_load_order_overrides(self):
        with tempfile.TemporaryDirectory() as d:
            base = os.path.join(d, "Base.esm")
            plug = os.path.join(d, "Plugin.esp")
            esm.write_esm(base, [
                stat("rock", "a\\rock.nif"),
                stat("tree", "a\\tree.nif"),
                cell("Hut", esm.CELL_INTERIOR, 0, 0, [ref(1, "rock"), ref(2, "tree")]),
                cell("", 0, 3, 4, [ref(3, "rock", (24600.0, 32800.0, 10.0))]),
            ])
            esm.write_esm(plug, [
                stat("rock", "b\\rock2.nif"),  # override model
                esm.record("STAT", [esm.sub("NAME", esm.zs("tree")), esm.sub("DELE", b"\0" * 4)]),
                # (master 1 << 24 | 2) deletes the tree ref; ref 1 of the plugin is new
                cell("hut", esm.CELL_INTERIOR, 0, 0, [
                    ref((1 << 24) | 2, "tree", extra=[esm.sub("DELE", b"\0" * 4)]),
                    ref(1, "rock", (5.0, 6.0, 7.0)),
                    ref((1 << 24) | 1, "rock", (1.0, 1.0, 1.0)),  # move base ref 1
                ]),
                # MVRF: base ref 3 moves from (3,4) to (5,4)
                esm.record("CELL", [
                    esm.sub("NAME", esm.zs("")), esm.sub("DATA", struct.pack("<iii", 0, 3, 4)),
                    esm.sub("MVRF", struct.pack("<I", (1 << 24) | 3)),
                    esm.sub("CNDT", struct.pack("<ii", 5, 4)),
                    *ref((1 << 24) | 3, "rock", (41000.0, 32800.0, 10.0))]),
            ], masters=[("Base.esm", 0)])
            w = esm.World().load([base, plug])
        self.assertEqual(w.objects["rock"].model, "b\\rock2.nif")
        self.assertNotIn("tree", w.objects)
        hut = w.cells[("int", "hut")]
        self.assertEqual(sorted(hut.refs), [(0, 1), (1, 1)])
        self.assertEqual(hut.refs[(0, 1)].pos, (1.0, 1.0, 1.0))
        self.assertEqual(hut.refs[(1, 1)].pos, (5.0, 6.0, 7.0))
        self.assertNotIn((0, 3), w.cells[("ext", 3, 4)].refs)
        self.assertIn((0, 3), w.cells[("ext", 5, 4)].refs)


class PlacementTests(unittest.TestCase):
    def test_rotation_convention(self):
        # Morrowind z rotation is clockwise seen from above: +Y turns to +X
        m = W.placement_matrix((0, 0, 0), (0, 0, math.pi / 2), 1.0)
        p = W._xf(m, (0.0, 1.0, 0.0))
        self.assertAlmostEqual(p[0], 1.0, places=6)
        self.assertAlmostEqual(p[1], 0.0, places=6)
        m = W.placement_matrix((10, 20, 30), (0.3, -0.2, 1.1), 2.0)
        # columns are orthogonal with length = scale
        cols = [[m[r][c] for r in range(3)] for c in range(3)]
        for c in cols:
            self.assertAlmostEqual(math.sqrt(sum(x * x for x in c)), 2.0, places=6)
        self.assertAlmostEqual(sum(a * b for a, b in zip(cols[0], cols[1])), 0.0, places=6)
        self.assertEqual(W._xf(m, (0, 0, 0)), (10, 20, 30))


class TestWorldTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        data = make_testworld.make(cls.tmp.name)
        import mesh_demaker
        out = os.path.join(cls.tmp.name, "cooked")
        mesh_demaker.main(["--data", data, "--out", out, "--jobs", "1"])
        cls.out = out
        cls.data = data
        cls.manifest = W.cook_world([os.path.join(data, "TestWorld.esm")],
                                    os.path.join(out, "meshes.dpak"), out, W.WorldConfig())
        cls.pack = dmcommon.DPakReader(dmcommon.read_file(os.path.join(out, "zones.dpak")))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_world_index(self):
        world = dzone.read_world(self.pack.get("world"))
        self.assertEqual(world["zone_size"], 3)
        self.assertEqual(len(world["zones"]), 5)  # 2x2 exterior zones + cellar
        self.assertEqual(len(world["ext"]), 36)
        self.assertEqual(world["ext"], sorted(world["ext"]))
        self.assertEqual(world["int"][0][0], dmcommon.fnv1a32("test hamlet, cellar"))
        for z in world["zones"]:
            entry = next(n for n in self.pack.entries if dmcommon.fnv1a32(n) == z["entry_hash"])
            self.assertEqual(z["bytes"], self.pack.entries[entry][2])

    def test_terrain_matches_land(self):
        world = esm.World().load([os.path.join(self.data, "TestWorld.esm")])
        land = world.lands[(0, 0)]
        z = dzone.read_zone(self.pack.get("zone/ext_0_0"))
        patch = next(t for t in z["terrain"] if (t["x"], t["y"]) == (0, 0))
        for j in (0, 5, 16):
            for i in (0, 7, 16):
                self.assertAlmostEqual(patch["heights"][j * 17 + i],
                                       land.heights[(j * 4) * 65 + i * 4], places=3)
        self.assertEqual(patch["origin"], (0.0, 0.0))
        fringe = [t for t in z["terrain"] if t["flags"] & dzone.CELL_F_FRINGE]
        self.assertEqual(len(fringe), 7)  # (-1..3)^2 ring clipped to existing land
        # terrain batches cover every quad exactly once
        tris = sum(len(z["batches"][b]["indices"]) // 3
                   for b in range(patch["first_batch"], patch["first_batch"] + patch["batch_count"]))
        self.assertEqual(tris, 16 * 16 * 2)
        names = {z["textures"][q]["name"] for q in patch["quad_tex"]}
        self.assertTrue(names <= {"tx_tw_grass", "tx_tw_dirt", "tx_tw_rock"})

    def test_doors_merging_lights(self):
        hamlet = dzone.read_zone(self.pack.get("zone/ext_0_0"))
        cellar = dzone.read_zone(self.pack.get("zone/int/test hamlet, cellar"))
        self.assertEqual(hamlet["doors"][0]["dest_zone"], 4)
        self.assertEqual(hamlet["doors"][0]["dest_cell_hash"], dmcommon.fnv1a32("Test Hamlet, Cellar"))
        self.assertEqual(cellar["doors"][0]["dest_zone"], 3)
        door_inst = hamlet["instances"][hamlet["doors"][0]["instance"]]
        self.assertEqual(door_inst["kind"], dzone.INST_DOOR)
        self.assertEqual(door_inst["door"], 0)
        merged = [i for i in hamlet["instances"] if i["flags"] & dzone.IF_MERGED]
        self.assertTrue(merged and all(i["kind"] == dzone.INST_STATIC for i in merged))
        light = hamlet["lights"][0]
        self.assertEqual(hamlet["instances"][light["instance"]]["kind"], dzone.INST_LIGHT)
        self.assertEqual(light["radius"], 384.0)
        self.assertEqual(cellar["kind"], dzone.KIND_INTERIOR)
        self.assertTrue(cellar["flags"] & dzone.F_WATER)
        self.assertAlmostEqual(cellar["water_height"], -150.0)
        self.assertLess(cellar["fog_far"], 5000.0)
        items = [i for i in cellar["instances"] if i["kind"] == dzone.INST_ITEM]
        self.assertEqual(len(items), 3)  # coin, ring, book
        coin = [i for i in items if i["object_hash"] == dmcommon.fnv1a32("tw_coin")]
        self.assertEqual(len(coin), 1)
        self.assertFalse(coin[0]["flags"] & dzone.IF_PERSISTENT)  # after NAM0
        self.assertTrue(set(self.manifest["textures"]) >= {"tx_tw_grass", "water/water00"})

    def test_budget_fallback_disables_merging(self):
        with tempfile.TemporaryDirectory() as out:
            cfg = W.WorldConfig(zone_budget=1000)
            man = W.cook_world([os.path.join(self.data, "TestWorld.esm")],
                               os.path.join(self.out, "meshes.dpak"), out, cfg)
            ham = next(z for z in man["zones"] if z["entry"] == "zone/ext_0_0")
            self.assertFalse(ham["merged"])
            self.assertTrue(any("exceeds zone budget" in w for w in man["warnings"]))
            pack = dmcommon.DPakReader(dmcommon.read_file(os.path.join(out, "zones.dpak")))
            z = dzone.read_zone(pack.get("zone/ext_0_0"))
            self.assertFalse(any(i["flags"] & dzone.IF_MERGED for i in z["instances"]))


if __name__ == "__main__":
    unittest.main()
