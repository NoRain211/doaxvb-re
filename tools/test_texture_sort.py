# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic coverage for private texture indexing and model joins."""
import csv
import json
from pathlib import Path
import struct
import tempfile
import unittest

from texture_rip import png
from test_model_export import triangle_model
from texture_sort import (assign_outfit_designs, classify_record, head_part, image_features,
                          layout_groups, sort_textures, texture_bone_mass)


def companion(palette):
    """One primary pass whose script draws object 0 with a palette recipe."""
    data = bytearray(56)
    struct.pack_into("<5I", data, 0, 0, 20, 0, 0, 0)
    struct.pack_into("<2I", data, 20, 1, 16)
    struct.pack_into("<2I", data, 36, 1, 32)
    data[52:55] = bytes([2, palette, 0])
    return bytes(data)


def bone_record(entries, visual=None):
    return {"key": "k", "bone_entries": entries, "girl_names": {"aa": "alpha"},
            "body_codes": {"aa", "bb"}, "visual": visual or {"average_rgb": "#f0c0a0"},
            "layout": ("", 1)}


class TextureSortTests(unittest.TestCase):
    def test_palette_weights_become_body_region_mass(self):
        palettes = [[[1, []], [17, []]], [[2, []], [7, []]]]
        head, approximate = texture_bone_mass(triangle_model(), companion(0), palettes, {0: "k"})
        self.assertEqual(approximate, set())
        self.assertEqual(head["k"], {"head": 3.0})
        mixed, _ = texture_bone_mass(triangle_model(), companion(1), palettes, {0: "k"})
        self.assertEqual(mixed["k"], {"torso": 0.75, "legs": 2.25})

    def test_body_and_item_bone_classification(self):
        body = {"code": "aa", "body": True, "approximate": False, "stem": "xaa00"}
        record = bone_record([dict(body, fractions={"legs": 0.9, "torso": 0.1})])
        classify_record(record, set())
        self.assertEqual((record["category"], record["confidence"]), ("characters/alpha/skin", "high"))
        record = bone_record([dict(body, fractions={"head": 1.0})],
                             {"alpha_transparent": 0.3, "average_rgb": "#202020"})
        classify_record(record, set())
        self.assertEqual(record["category"], "characters/alpha/hair")
        self.assertEqual(head_part({"average_rgb": "#f0c0a0"})[0], "face")
        item = {"body": False, "approximate": False, "fractions": {"torso": 1.0}}
        records = [bone_record([dict(item, code=code, stem=stem + code)
                                for code in ("aa", "bb") for stem in stems])
                   for stems in (["sw01"], ["sw02"], ["sw01", "sw02"])]
        for record in records:
            classify_record(record, set())
        assign_outfit_designs(records)
        self.assertEqual([record["category"] for record in records],
                         ["outfits/sw01/sw01/swimsuit", "outfits/sw01/sw02/swimsuit",
                          "outfits/sw01/shared/swimsuit"])

    def test_synthetic_archive_model_frame_and_gallery(self):
        with tempfile.TemporaryDirectory() as temporary:
            project = Path(temporary)
            rip = project / "private" / "rip"
            textures = rip / "textures"
            textures.mkdir(parents=True)
            source_path = r"private\rip\afs\datahdd\000000"
            container = rip / "afs" / "datahdd" / "000000"
            container.parent.mkdir(parents=True)
            data = bytearray(0x200)
            struct.pack_into("<II", data, 0xA0, 0x80000000, 0x90)
            struct.pack_into("<I", data, 0xA0 + 0x70, 0)
            container.write_bytes(data)

            keys = ["06-8x8-m1-" + format(number, "064x") for number in (1, 2)]
            colors = [(255, 0, 0, 255), (0, 255, 0, 255)]
            records = []
            for index, (key, color) in enumerate(zip(keys, colors)):
                rgba = bytes(color) * 64
                (textures / f"{key}.png").write_bytes(png(8, 8, rgba))
                records.append({
                    "source": source_path,
                    "record": 100 + index * 20,
                    "key": key,
                    "texture": {"format": 6, "width": 8, "height": 8, "mips": 1},
                })
            (textures / "manifest.json").write_text(
                json.dumps({"textures": records, "errors": []}), encoding="utf-8"
            )
            (rip / "asset-metadata.json").write_text(json.dumps({
                source_path: {"name": "sample_font.xpr", "extension": ".xpr",
                              "archive": "datahdd.afs", "index": 0},
            }), encoding="utf-8")
            (rip / "archive-keys.json").write_text(json.dumps({
                key: [{"path": source_path, "record": 100 + index * 20}]
                for index, key in enumerate(keys)
            }), encoding="utf-8")
            (rip / "model-objects.json").write_text(json.dumps([
                {"path": source_path, "obj": 0},
            ]), encoding="utf-8")

            frame_root = project / "private" / "frame-dumps"
            frame = frame_root / "frame-12"
            frame.mkdir(parents=True)
            (frame / f"{keys[0]}.png").write_bytes((textures / f"{keys[0]}.png").read_bytes())
            features = [image_features(textures / f"{key}.png") for key in keys]
            self.assertEqual(features[0]["phash"], features[1]["phash"])
            flat_groups = layout_groups([
                {"key": key, "visual": feature}
                for key, feature in zip(keys, features)
            ])
            self.assertNotEqual(flat_groups[keys[0]], flat_groups[keys[1]])
            checker = bytearray()
            for y in range(8):
                for x in range(8):
                    checker.extend((0, 0, 0, 255) if (x + y) % 2 else (255, 255, 255, 255))
            checker_path = project / "checker.png"
            checker_path.write_bytes(png(8, 8, bytes(checker)))
            checker_features = image_features(checker_path)
            pattern_groups = layout_groups([
                {"key": key, "visual": checker_features}
                for key in keys
            ])
            self.assertEqual(pattern_groups[keys[0]][1], 2)
            self.assertEqual(pattern_groups[keys[0]][0], pattern_groups[keys[1]][0])

            output = rip / "sorted"
            sorted_records, missing = sort_textures(rip, output, [frame_root])
            self.assertEqual(missing, 0)
            self.assertEqual(len(sorted_records), 2)
            self.assertEqual(sorted_records[0]["category"], "ui-hud-fonts/fonts")
            self.assertEqual(sorted_records[0]["model_objects"], [
                "datahdd.afs#000000 (sample_font.xpr) OBJ@0x0",
            ])
            self.assertEqual(sorted_records[0]["runtime_frames"], ["frame-12"])
            for key in keys:
                self.assertTrue(list(output.rglob(f"{key}.png")))
            with (output / "index.csv").open(encoding="utf-8", newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 2)
            self.assertIn("record@0x64", rows[0]["XPR records"])
            self.assertIn(keys[0], (output / "index.html").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
