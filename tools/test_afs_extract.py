# SPDX-License-Identifier: GPL-3.0-or-later
"""AFS checks using only synthetic archives."""
import io
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from afs_extract import extract, read_entries, safe_name


def archive(pointer=24, names=True):
    data = bytearray(224)
    struct.pack_into("<4sIIIII", data, 0, b"AFS\0", 2, 64, 3, 96, 4)
    data[64:67], data[96:100] = b"one", b"two!"
    if names:
        struct.pack_into("<II", data, pointer, 128, 96)
        data[128:136] = b"../a.txt"
        data[176:181] = b"b.txt"
    return bytes(data)


class AfSTests(unittest.TestCase):
    def test_names_and_both_pointer_locations(self):
        for pointer in (24, 56):
            entries = read_entries(io.BytesIO(archive(pointer)))
            self.assertEqual([(e.offset, e.size, e.name) for e in entries],
                             [(64, 3, "../a.txt"), (96, 4, "b.txt")])
        self.assertEqual(safe_name("../a.txt"), "_a.txt")

    def test_nameless_and_empty(self):
        self.assertTrue(all(not e.name for e in read_entries(io.BytesIO(archive(names=False)))))
        self.assertEqual(read_entries(io.BytesIO(b"AFS\0" + bytes(4))), [])

    def test_reject_bad_bounds(self):
        for data in (b"", b"not AFS!", b"AFS\0" + struct.pack("<I", 9999),
                     archive()[:98]):
            with self.assertRaises(ValueError):
                read_entries(io.BytesIO(data))
        data = bytearray(archive())
        struct.pack_into("<I", data, 8, 4)
        with self.assertRaises(ValueError):
            read_entries(io.BytesIO(data))

    def test_extract_and_custody(self):
        with tempfile.TemporaryDirectory() as folder, patch("afs_extract.ROOT", Path(folder)):
            root = Path(folder)
            source = root / "input.afs"
            source.write_bytes(archive())
            output = root / "private/rip/synthetic"
            self.assertEqual(extract(source, output, names=True), 2)
            self.assertEqual((output / "000000__a.txt").read_bytes(), b"one")
            self.assertEqual((output / "000001_b.txt").read_bytes(), b"two!")
            self.assertEqual(source.read_bytes(), archive())
            with self.assertRaises(FileExistsError):
                extract(source, output)
            for target in (root / "public", root / "private", root / "private/../escape"):
                with self.assertRaises(ValueError):
                    extract(source, target)


if __name__ == "__main__":
    unittest.main()
