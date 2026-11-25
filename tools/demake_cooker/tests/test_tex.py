"""tex_demaker: sizing, quantisation, alpha modes, mips, swizzle, atlas, CLI."""

import io
import json
import os
import random
import sys
import tempfile
import unittest

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import dmcommon  # noqa: E402
import tex_demaker as T  # noqa: E402


def gradient(w, h):
    img = Image.new("RGBA", (w, h))
    img.putdata([((x * 255) // max(w - 1, 1), (y * 255) // max(h - 1, 1), 96, 255)
                 for y in range(h) for x in range(w)])
    return img


def flat_colors(w, h, colors):
    img = Image.new("RGBA", (w, h))
    img.putdata([colors[((x // 8) + (y // 8)) % len(colors)] for y in range(h) for x in range(w)])
    return img


def dds_bytes(img):
    buf = io.BytesIO()
    img.save(buf, "DDS")
    return buf.getvalue()


class SwizzleTests(unittest.TestCase):
    def test_block_order(self):
        width, height = 32, 16  # bytes x rows -> 2 x 2 blocks
        data = bytes((y * width + x) & 0xFF for y in range(height) for x in range(width))
        sw = T.swizzle(data, width, height)
        self.assertEqual(sw[0:16], data[0:16])  # block (0,0) row 0
        self.assertEqual(sw[16:32], data[32:48])  # block (0,0) row 1
        self.assertEqual(sw[128:144], data[16:32])  # block (1,0) row 0
        self.assertEqual(sw[256:272], data[8 * 32:8 * 32 + 16])  # block (0,1) row 0
        self.assertEqual(T.unswizzle(sw, width, height), data)

    def test_rejects_bad_dims(self):
        with self.assertRaises(ValueError):
            T.swizzle(bytes(24 * 8), 24, 8)

    def test_t4_nibble_order(self):
        self.assertEqual(T.pack_indices(bytes([1, 2, 3, 4]), T.DTEX_PSM_T4), bytes([0x21, 0x43]))
        self.assertEqual(T.unpack_indices(bytes([0x21, 0x43]), T.DTEX_PSM_T4, 4), bytes([1, 2, 3, 4]))


class SizingTests(unittest.TestCase):
    def test_target_sizes(self):
        self.assertEqual(T.target_size(512, 512, 64), (64, 64))
        self.assertEqual(T.target_size(512, 256, 64), (64, 32))
        self.assertEqual(T.target_size(128, 128, 32), (32, 32))
        self.assertEqual(T.target_size(16, 16, 64), (32, 16))  # min swizzle width
        self.assertEqual(T.target_size(1024, 8, 64), (64, 8))

    def test_size_rules(self):
        cfg = T.TexConfig()
        self.assertEqual(T.max_dim_for("tx_wood_01", 512, 512, cfg), 64)
        self.assertEqual(T.max_dim_for("tx_small", 128, 128, cfg), 32)
        self.assertEqual(T.max_dim_for("splash/splash_a", 1024, 512, cfg), 256)
        self.assertEqual(T.max_dim_for("menu_head_block", 64, 64, cfg), 128)


class DemakeTests(unittest.TestCase):
    def test_few_colors_choose_t4(self):
        img = flat_colors(256, 256, [(200, 30, 30, 255), (30, 200, 30, 255), (30, 30, 200, 255)])
        data, info = T.demake_image(img, "tx_tiles", T.TexConfig())
        self.assertEqual(info["psm"], "T4")
        self.assertEqual(info["size"], [64, 64])
        self.assertGreater(info["psnr"], 40)
        parsed = T.parse_dtex(data)
        self.assertEqual(parsed["psm"], T.DTEX_PSM_T4)
        self.assertTrue(parsed["flags"] & T.DTEX_F_SWIZZLED)
        self.assertEqual(len(parsed["palette"]), 16)
        self.assertEqual([(l.width, l.height) for l in parsed["levels"]], [(64, 64), (32, 32)])
        self.assertEqual(parsed["name_hash"], dmcommon.fnv1a32("tx_tiles"))
        self.assertEqual(parsed["clut_ofs"] % 16, 0)
        # decoded level 0 reproduces the source colours
        dec = T.dtex_to_image(data)
        src = img.resize((64, 64), Image.NEAREST)
        self.assertEqual(dmcommon.image_pixels(dec)[:64], dmcommon.image_pixels(src)[:64])

    def test_rich_image_chooses_t8_with_three_levels(self):
        data, info = T.demake_image(gradient(512, 512), "tx_grad", T.TexConfig())
        self.assertEqual(info["psm"], "T8")
        parsed = T.parse_dtex(data)
        self.assertEqual([(l.width, l.height) for l in parsed["levels"]],
                         [(64, 64), (32, 32), (16, 16)])
        for lv in parsed["levels"]:
            self.assertEqual(len(lv.indices), lv.width * lv.height)
            self.assertTrue(all(i < 256 for i in lv.indices))

    def test_forced_color_modes(self):
        cfg16 = T.TexConfig(colors="16")
        _, info = T.demake_image(gradient(256, 256), "a", cfg16)
        self.assertEqual(info["psm"], "T4")
        cfg256 = T.TexConfig(colors="256")
        _, info = T.demake_image(flat_colors(64, 64, [(1, 2, 3, 255)]), "b", cfg256)
        self.assertEqual(info["psm"], "T8")

    def test_cutout_alpha(self):
        img = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
        for y in range(256):
            for x in range(256):
                if (x // 32 + y // 32) % 2:
                    img.putpixel((x, y), (40, 150, 40, 255))
        data, info = T.demake_image(img, "tx_leaves", T.TexConfig())
        self.assertEqual(info["alpha"], "cutout")
        parsed = T.parse_dtex(data)
        self.assertTrue(parsed["flags"] & T.DTEX_F_ALPHA_TEST)
        self.assertEqual(parsed["palette"][0][3], 0)
        dec = T.dtex_to_image(data)
        self.assertEqual(dec.getpixel((0, 0))[3], 0)  # transparent square
        self.assertEqual(dec.getpixel((10, 0))[3], 255)  # opaque square
        for lv in parsed["levels"]:
            self.assertIn(0, lv.indices)

    def test_blend_alpha(self):
        img = Image.new("RGBA", (128, 128))
        img.putdata([(100, 150, 200, (x * 2) & 0xFF) for y in range(128) for x in range(128)])
        data, info = T.demake_image(img, "tx_glass", T.TexConfig(colors="256"))
        self.assertEqual(info["alpha"], "blend")
        parsed = T.parse_dtex(data)
        self.assertTrue(parsed["flags"] & T.DTEX_F_ALPHA_BLEND)
        alphas = {c[3] for c in parsed["palette"]}
        self.assertGreater(len(alphas), 8)

    def test_dither_and_unswizzled(self):
        cfg = T.TexConfig(colors="16", dither=True, swizzle=False, mips=False)
        data, info = T.demake_image(gradient(64, 64), "tx_d", cfg)
        parsed = T.parse_dtex(data)
        self.assertFalse(parsed["flags"] & T.DTEX_F_SWIZZLED)
        self.assertEqual(len(parsed["levels"]), 1)
        self.assertEqual(info["levels"], 1)

    def test_nearest_downscale_keeps_crisp_pixels(self):
        img = flat_colors(512, 512, [(255, 0, 0, 255), (0, 0, 255, 255)])
        data, _ = T.demake_image(img, "tx_check", T.TexConfig())
        colors = set(dmcommon.image_pixels(T.dtex_to_image(data)))
        self.assertLessEqual(len(colors), 2)  # no blended in-between colours


class AtlasTests(unittest.TestCase):
    def test_pack_and_index(self):
        rng = random.Random(7)
        icons = []
        for i in range(70):
            c = (rng.randrange(256), rng.randrange(256), rng.randrange(256), 255)
            size = 32 if i % 5 else 16
            icons.append((f"icons/m/item_{i}", Image.new("RGBA", (size, size), c)))
        files, info = T.cook_atlas("icons", icons, T.TexConfig(), 256, 32)
        self.assertEqual(info["icons"], 70)
        self.assertGreaterEqual(info["pages"], 1)
        idx = T.parse_datl(files["atlas/icons.datl"])
        self.assertEqual(len(idx["entries"]), 70)
        self.assertEqual(len(idx["pages"]), info["pages"])
        hashes = [e[0] for e in idx["entries"]]
        self.assertEqual(hashes, sorted(hashes))
        lookup = {e[0]: e for e in idx["entries"]}
        for key, img in icons:
            h, page, x, y, w, hh = lookup[dmcommon.fnv1a32(key)]
            self.assertEqual((w, hh), img.size)
            page_img = T.dtex_to_image(files[f"atlas/icons/{page}"])
            got = page_img.getpixel((x + w // 2, y + hh // 2))
            want = img.getpixel((0, 0))
            self.assertLess(sum(abs(a - b) for a, b in zip(got[:3], want[:3])), 40)

    def test_rects_do_not_overlap(self):
        icons = [(f"i{k}", Image.new("RGBA", (32, 24 if k % 2 else 32))) for k in range(90)]
        _pages, entries = T.pack_atlas(icons, 256, 256)
        by_page = {}
        for e in entries:
            for o in by_page.get(e.page, []):
                overlap = not (e.x + e.w <= o.x or o.x + o.w <= e.x or
                               e.y + e.h <= o.y or o.y + o.h <= e.y)
                self.assertFalse(overlap, (e, o))
            by_page.setdefault(e.page, []).append(e)
            self.assertLessEqual(e.x + e.w, 256)
            self.assertLessEqual(e.y + e.h, 256)


class CliTests(unittest.TestCase):
    def test_end_to_end_from_bsa(self):
        with tempfile.TemporaryDirectory() as d:
            data_dir = os.path.join(d, "Data Files")
            os.makedirs(os.path.join(data_dir, "Textures"))
            dmcommon.write_bsa(os.path.join(data_dir, "Morrowind.bsa"), {
                "textures\\tx_wood_01.dds": dds_bytes(gradient(256, 256).convert("RGB")),
                "textures\\tx_leaf.dds": dds_bytes(flat_colors(64, 64, [(0, 99, 0, 255), (0, 0, 0, 0)])),
                "icons\\m\\tx_gold.dds": dds_bytes(Image.new("RGBA", (32, 32), (250, 200, 0, 255))),
            })
            # loose TGA overrides nothing but adds a texture; a broken file is reported
            gradient(128, 64).save(os.path.join(data_dir, "Textures", "Tx_Loose.tga"))
            with open(os.path.join(data_dir, "Textures", "tx_broken.dds"), "wb") as f:
                f.write(b"not a dds")
            out = os.path.join(d, "out")
            rc = T.main(["--data", data_dir, "--out", out, "--jobs", "2"])
            self.assertEqual(rc, 0)
            pack = dmcommon.DPakReader(dmcommon.read_file(os.path.join(out, "textures.dpak")))
            names = set(pack.entries)
            self.assertTrue({"tx_wood_01", "tx_leaf", "tx_loose", "atlas/icons/0",
                             "atlas/icons.datl"} <= names)
            self.assertNotIn("tx_broken", names)
            with open(os.path.join(out, "textures_manifest.json"), encoding="utf-8") as f:
                manifest = json.load(f)
            self.assertEqual([f["key"] for f in manifest["failures"]], ["tx_broken"])
            loose = T.parse_dtex(pack.get("tx_loose"))
            self.assertEqual((loose["width"], loose["height"]), (32, 16))
            self.assertEqual(T.main(["--data", data_dir, "--out", out, "--jobs", "1",
                                     "--strict"]), 1)

    def test_only_keys_filter(self):
        with tempfile.TemporaryDirectory() as d:
            dmcommon.write_bsa(os.path.join(d, "Morrowind.bsa"), {
                "textures\\a.dds": dds_bytes(gradient(64, 64).convert("RGB")),
                "textures\\b.dds": dds_bytes(gradient(64, 64).convert("RGB")),
                "textures\\splash\\splash_x.dds": dds_bytes(gradient(64, 64).convert("RGB")),
            })
            keys = os.path.join(d, "keys.json")
            with open(keys, "w", encoding="utf-8") as f:
                json.dump({"textures": ["A"]}, f)
            out = os.path.join(d, "out")
            self.assertEqual(T.main(["--data", d, "--out", out, "--jobs", "1",
                                     "--only-keys", keys]), 0)
            pack = dmcommon.DPakReader(dmcommon.read_file(os.path.join(out, "textures.dpak")))
            self.assertEqual(set(pack.entries), {"a", "splash/splash_x"})


if __name__ == "__main__":
    unittest.main()
