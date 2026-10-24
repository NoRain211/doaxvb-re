# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic disc checks for launcher art and icon extraction."""
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import launcher_art
from texture_rip import texture_key, texture_payloads
from test_texture_rip import bundle


def afs(*entries):
    table, body = b"", b""
    for entry in entries:
        table += struct.pack("<II", 2048 + len(body), len(entry))
        body += entry + bytes(-len(entry) % 2048)
    header = b"AFS\0" + struct.pack("<I", len(entries)) + table
    return header + bytes(2048 - len(header)) + body


class LauncherArtTests(unittest.TestCase):
    def test_installs_missing_art_and_icon_once(self):
        xpr = bundle()
        _, texture, payload, palette, _ = next(texture_payloads(xpr))
        key = texture_key(texture, payload, palette)
        with tempfile.TemporaryDirectory() as temp:
            disc, private = Path(temp, "disc"), Path(temp, "private")
            disc.mkdir()
            (disc / "a.AFS").write_bytes(afs(b"not a texture", xpr))
            art = {"logo.png": key, "beach.png": key, "button-a.png": "0b-missing"}
            with patch.dict(launcher_art.ART, art, clear=True), patch.object(launcher_art, "EMBLEM", (0, 0, 4, 2)):
                written = launcher_art.install(disc, private)
                self.assertEqual({p.name for p in written}, {"logo.png", "beach.png", "doaxbv.ico"})
                self.assertTrue((private / "launcher/logo.png").read_bytes().startswith(b"\x89PNG"))
                icon = (private / "doaxbv.ico").read_bytes()
                self.assertEqual(struct.unpack_from("<HHH", icon), (0, 1, 4))
                sizes = [icon[6 + 16 * i] or 256 for i in range(4)]
                self.assertEqual(sizes, list(launcher_art.ICON_SIZES))
                _, _, _, _, _, _, length, offset = struct.unpack_from("<BBBBHHII", icon, 6)
                self.assertTrue(icon[offset:offset + length].startswith(b"\x89PNG"))
                (private / "launcher/beach.png").write_bytes(b"mine")
                self.assertEqual(launcher_art.install(disc, private), [])
                self.assertEqual((private / "launcher/beach.png").read_bytes(), b"mine")


if __name__ == "__main__":
    unittest.main()
