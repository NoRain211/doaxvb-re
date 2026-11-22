# SPDX-License-Identifier: GPL-3.0-or-later
"""When the launcher build folder is reused or reconfigured."""
import json
from pathlib import Path
import tempfile
import unittest

from build_launcher import configure_needed

WANTED = {"source": "/game", "generator": "Ninja", "args": ["-DX=1"]}


class ConfigureNeededTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.build = Path(temp.name)
        self.key = self.build / "doaxbv-configure.json"

    def test_missing_key_reconfigures(self):
        self.assertTrue(configure_needed(self.build, WANTED))

    def test_matching_key_reuses(self):
        self.key.write_text(json.dumps(WANTED), encoding="utf-8")
        self.assertFalse(configure_needed(self.build, WANTED))

    def test_changed_key_reconfigures(self):
        for field, value in (("source", "/moved"), ("generator", "Unix Makefiles"), ("args", [])):
            with self.subTest(field=field):
                self.key.write_text(json.dumps({**WANTED, field: value}), encoding="utf-8")
                self.assertTrue(configure_needed(self.build, WANTED))

    def test_invalid_key_reconfigures(self):
        self.key.write_text("{", encoding="utf-8")
        self.assertTrue(configure_needed(self.build, WANTED))


if __name__ == "__main__":
    unittest.main()
