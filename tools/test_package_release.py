# SPDX-License-Identifier: GPL-3.0-or-later
"""Platform membership rules for the release packager."""
import unittest

import package_release


class PlatformMemberTests(unittest.TestCase):
    def test_each_platform_ships_only_its_own_root_launchers(self):
        common = {name: b"" for name in ("build_game.sh", "BuildGame.cmd", "README.md",
                                         "tools/cmake/share/run.sh", "tools/helper.cmd")}
        self.assertEqual(set(package_release.windows_members(common)),
                         {"BuildGame.cmd", "README.md", "tools/cmake/share/run.sh", "tools/helper.cmd"})
        self.assertEqual(set(package_release.unix_members(common)),
                         {"build_game.sh", "README.md", "tools/cmake/share/run.sh", "tools/helper.cmd"})

    def test_packaging_inputs_never_ship(self):
        for name, packaging in (("tools/package_release.py", True), ("tools/test_package_release.py", True),
                                ("tools/release/README-macos.txt", True), ("tools/build_game.py", False),
                                ("tools/releases.md", False)):
            with self.subTest(name=name):
                self.assertEqual(package_release.is_packaging(name), packaging)


if __name__ == "__main__":
    unittest.main()
