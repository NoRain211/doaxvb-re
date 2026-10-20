# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic Xbox surface, block decoder and replacement-key checks."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zlib

from texture_rip import (Texture, decode, decode_dxt, describe, deswizzle, png,
                         resources, rip, texture_key, texture_payloads)


def bundle(palettized=False, user=False):
    header = bytearray(b"XPR0" + bytes(8))
    if user:
        header += struct.pack("<II", 0x80000000, 3) + b"abc"
    fmt = 11 if palettized else 12
    header += struct.pack("<IIIII", 0x40001, 0, 0, (2 << 24) | (2 << 20) | (1 << 16) | (fmt << 8) | 32, 0)
    payload = bytes(range(16)) if palettized else struct.pack("<HHI", 0xf800, 0x7e0, 0)
    if palettized:
        header += struct.pack("<III", 0x30001, len(payload), 0)
        payload += b"\0\0\xff\xff" * 256
    header += struct.pack("<I", 0xffffffff)
    struct.pack_into("<II", header, 4, len(header) + len(payload), len(header))
    return bytes(header) + payload


class TextureTests(unittest.TestCase):
    def test_rectangular_morton_known_order(self):
        self.assertEqual(deswizzle(bytes(range(16)), 8, 2, 1),
                         bytes([0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15]))
        self.assertEqual(deswizzle(bytes(range(8)), 2, 4, 1), bytes(range(8)))
        with self.assertRaises(ValueError):
            deswizzle(bytes(6), 3, 2, 1)

    def test_dxt1_four_colors_and_transparency(self):
        indices = sum((i % 4) << (2 * i) for i in range(16))
        data = struct.pack("<HHI", 0xf800, 0x7e0, indices)
        pixels = decode_dxt(data, 4, 4, 12)
        self.assertEqual(pixels[:16], bytes([255, 0, 0, 255, 0, 255, 0, 255,
                                           170, 85, 0, 255, 85, 170, 0, 255]))
        data = struct.pack("<HHI", 0, 0xffff, indices)
        pixels = decode_dxt(data, 4, 4, 12)
        self.assertEqual(pixels[8:16], bytes([127, 127, 127, 255, 0, 0, 0, 0]))
        self.assertEqual(decode_dxt(data, 1, 1, 12), bytes([0, 0, 0, 255]))

    def test_dxt3_explicit_alpha_and_four_color_rule(self):
        alphas = sum(i << (4 * i) for i in range(16)).to_bytes(8, "little")
        data = alphas + struct.pack("<HHI", 0, 0xffff, 0xffffffff)
        pixels = decode_dxt(data, 4, 4, 14)
        self.assertEqual(pixels[3::4], bytes(i * 17 for i in range(16)))
        self.assertEqual(pixels[:3], bytes([170, 170, 170]))

    def test_dxt5_both_alpha_modes(self):
        indices = sum((i % 8) << (3 * i) for i in range(16)).to_bytes(6, "little")
        for endpoints, expected in ((bytes([255, 0]), [255, 0, 218, 182, 145, 109, 72, 36]),
                                    (bytes([0, 255]), [0, 255, 51, 102, 153, 204, 0, 255])):
            data = endpoints + indices + struct.pack("<HHI", 0xffff, 0, 0)
            self.assertEqual(decode_dxt(data, 4, 4, 15)[3::4], bytes(expected * 2))

    def test_uncompressed_formats_and_linear_padding(self):
        cases = [(2, 0xfc00, (255, 0, 0, 255)), (2, 0x7c00, (255, 0, 0, 0)),
                 (3, 0x7c00, (255, 0, 0, 255)), (4, 0x8f00, (255, 0, 0, 136)),
                 (5, 0x7e0, (0, 255, 0, 255)), (6, 0x80123456, (18, 52, 86, 128)),
                 (7, 0x00123456, (18, 52, 86, 255)), (0x19, 127, (255, 255, 255, 127)),
                 (0x3c, 0x12345678, (18, 52, 86, 120))]
        for fmt, value, expected in cases:
            texture = Texture(fmt, 1, 1, 1)
            self.assertEqual(decode(texture, value.to_bytes(texture.spans()[0], "little")), bytes(expected))
        texture = Texture(0x12, 1, 2, 1, 64)
        payload = bytes([3, 2, 1, 255]) + bytes(60) + bytes([6, 5, 4, 128])
        self.assertEqual(decode(texture, payload), bytes([1, 2, 3, 255, 4, 5, 6, 128]))
        self.assertEqual(texture.spans(), [68])

    def test_key_all_mips_and_palette(self):
        texture = Texture(11, 2, 2, 2)
        payload, palette = b"abcde", bytes(1024)
        expected = "0b-2x2-m2-" + hashlib.sha256(payload + palette).hexdigest()
        self.assertEqual(texture_key(texture, payload, palette), expected)
        self.assertNotEqual(texture_key(texture, payload, palette),
                            texture_key(texture, b"abcdx", palette))
        self.assertNotEqual(texture_key(texture, payload, palette),
                            texture_key(texture, payload, bytes(1023) + b"x"))
        with self.assertRaises(ValueError):
            texture_key(texture, payload)
        self.assertEqual(Texture(12, 8, 4, 4).spans(), [16, 8, 8, 8])

    def test_palette_channels_and_missing_binding(self):
        palette = bytearray(1024)
        palette[4:8] = bytes([30, 20, 10, 40])
        self.assertEqual(decode(Texture(11, 1, 1, 1), b"\1", bytes(palette)),
                         bytes([10, 20, 30, 40]))
        data = bytearray(bundle(True))
        struct.pack_into("<I", data, 32, 0xffffffff)
        self.assertIn("palette", list(texture_payloads(data))[0][-1])

    def test_xpr_and_unaligned_user_data(self):
        for palettized in (False, True):
            for user in (False, True):
                data = bundle(palettized, user)
                rows = list(texture_payloads(data))
                self.assertEqual(len(rows), 1)
                _, texture, payload, palette, error = rows[0]
                self.assertIsNone(error)
                self.assertEqual(decode(texture, payload, palette), b"\xff\0\0\xff" * 16)
                with self.assertRaises(ValueError):
                    resources(b"bad!" + data[4:])
                self.assertEqual(resources(b"bad!" + data[4:], relaxed=True), resources(data))

    def test_invalid_shapes_and_bounds(self):
        for word in (32 | 4, 48, (15 << 20) | 32, (15 << 16) | 32):
            with self.assertRaises(ValueError):
                describe(word, 0)
        for data in (b"", bundle()[:20], bundle()[:-1]):
            with self.assertRaises(ValueError):
                resources(data)
        data = bytearray(bundle())
        struct.pack_into("<I", data, 16, len(data))
        with self.assertRaises(ValueError):
            resources(data)
        data = bytearray(bundle())
        struct.pack_into("<I", data, 24, (8 << 24) | (8 << 20) | (1 << 16) | (12 << 8) | 32)
        self.assertIn("span", list(texture_payloads(data))[0][-1])

    def test_png_chunks_and_private_rip(self):
        encoded = png(1, 1, b"\xff\0\0\xff")
        offset, chunks = 8, {}
        while offset < len(encoded):
            size = struct.unpack_from(">I", encoded, offset)[0]
            kind = encoded[offset + 4:offset + 8]
            data = encoded[offset + 8:offset + 8 + size]
            crc = struct.unpack_from(">I", encoded, offset + 8 + size)[0]
            self.assertEqual(crc, zlib.crc32(kind + data))
            chunks[kind] = data
            offset += 12 + size
        self.assertEqual(zlib.decompress(chunks[b"IDAT"]), b"\0\xff\0\0\xff")
        with tempfile.TemporaryDirectory() as folder, patch("afs_extract.ROOT", Path(folder)):
            root = Path(folder)
            source = root / "synthetic.xpr"
            source.write_bytes(bundle(True))
            output = root / "private/textures"
            self.assertEqual(rip([source, source], output, raw=True), (1, 2, 0))
            manifest = json.loads((output / "manifest.json").read_text())
            key = manifest["textures"][0]["key"]
            self.assertTrue((output / f"{key}.png").is_file())
            self.assertEqual((output / f"{key}.bin").stat().st_size, 16 + 1024)
            with self.assertRaises(FileExistsError):
                rip([source], output)
            with self.assertRaises(ValueError):
                rip([source], root / "public")


if __name__ == "__main__":
    unittest.main()
