# SPDX-License-Identifier: GPL-3.0-or-later
"""Platform membership rules for the release packager."""
import unittest

import package_release


class PlatformMemberTests(unittest.TestCase):
    def test_each_platform_ships_only_its_own_root_launchers(self):
        windows = {"BuildGame.cmd", "Launcher.cmd", "RunGame.cmd", "ExtractIso.cmd"}
        unix = {"build_game.sh", "launcher.sh", "run_game.sh"}
        shared = {"README.md", "tools/cmake/share/run.sh", "tools/helper.cmd"}
        common = {name: b"" for name in windows | unix | shared}
        self.assertEqual(set(package_release.windows_members(common)), windows | shared)
        self.assertEqual(set(package_release.unix_members(common)), unix | shared)

    def test_macos_entry_is_an_extensionless_script_for_the_launcher(self):
        self.assertTrue(package_release.MACOS_ENTRY.startswith(b"#!"))
        self.assertIn(b"launcher.sh", package_release.MACOS_ENTRY)

    def test_only_the_macos_zip_gets_an_executable_doaxbv_entry(self):
        self.assertEqual(set(package_release.UNIX_PLATFORMS), {"macOS", "Linux"})
        common, modes = {"launcher.sh": b""}, {"launcher.sh": "100755"}
        kept, mode = package_release.unix_package("macOS", common, modes)
        self.assertEqual((kept["DOAXBV"], mode["DOAXBV"]), (package_release.MACOS_ENTRY, "100755"))
        kept, mode = package_release.unix_package("Linux", common, modes)
        self.assertNotIn("DOAXBV", kept)
        self.assertNotIn("DOAXBV", mode)
        self.assertNotIn("DOAXBV", modes)

    def test_only_the_linux_zip_gets_the_package_and_steam_deck_scripts(self):
        common = {name: b"" for name in ("launcher.sh", *package_release.LINUX_ONLY)}
        self.assertEqual(set(package_release.unix_package("Linux", common, {})[0]), set(common))
        self.assertEqual(set(package_release.unix_package("macOS", common, {})[0]), {"launcher.sh", "DOAXBV"})

    def test_packaging_inputs_never_ship(self):
        for name, packaging in (("tools/package_release.py", True), ("tools/test_package_release.py", True),
                                ("tools/release/README-macos.txt", True), ("tools/build_game.py", False),
                                ("tools/releases.md", False)):
            with self.subTest(name=name):
                self.assertEqual(package_release.is_packaging(name), packaging)

    def test_launcher_sources_read_like_sha256sum(self):
        common = {"tools/launcher_sdl.cpp": b"a", "tools/build_linux_launcher.sh": b"b"}
        self.assertEqual(package_release.launcher_sources(common),
                         "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb  launcher_sdl.cpp\n"
                         "3e23e8160039594a33894f6564e1b1348bbd7a0088d42c4acb73eeaed59c009d  build_linux_launcher.sh\n")

    def test_prebuilt_launcher_must_be_x86_64_linux(self):
        elf = b"\x7fELF\x02\x01" + bytes(12) + b"\x3e\x00"
        self.assertEqual(package_release.prebuilt_launcher(elf), elf)
        for name, data in (("arm64", elf[:18] + b"\xb7\x00"), ("32-bit", elf[:4] + b"\x01" + elf[5:]),
                           ("Mach-O", b"\xcf\xfa\xed\xfe" + elf[4:]), ("empty", b"")):
            with self.subTest(name=name), self.assertRaises(SystemExit):
                package_release.prebuilt_launcher(data)


if __name__ == "__main__":
    unittest.main()
