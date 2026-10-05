# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic coverage for private texture indexing and model joins."""
import csv
import json
from pathlib import Path
import struct
import tempfile
import unittest

from texture_rip import png
from texture_sort import image_features, layout_groups, sort_textures


class TextureSortTests(unittest.TestCase):
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
