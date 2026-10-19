import unittest

import decomp_progress


class DecompProgressTest(unittest.TestCase):
    def test_ledger_matches_runtime(self):
        errors = decomp_progress.check(decomp_progress.load_ledger(),
                                       decomp_progress.runtime_sources(),
                                       decomp_progress.load_units())
        self.assertEqual(errors, [])

    def test_replaced_entry_cannot_call_its_generated_body(self):
        rows = [{"address": "0x00012340", "name": "f", "status": "replaced",
                 "source": "recomp-runtime/a.c", "note": ""}]
        units = [{"name": "game", "kind": "game", "start": 0x11000, "end": 0x180000}]
        clean = {"recomp-runtime/a.c": "case 0x00012340u: return f;"}
        wrapper = {"recomp-runtime/a.c": "case 0x00012340u: sub_00012340(); return;"}
        self.assertEqual(decomp_progress.check(rows, clean, units), [])
        self.assertIn("still calls sub_00012340", decomp_progress.check(rows, wrapper, units)[0])

    def test_entry_must_lie_in_a_unit(self):
        rows = [{"address": "0x00001000", "name": "f", "status": "wrapped",
                 "source": "recomp-runtime/a.c", "note": ""}]
        errors = decomp_progress.check(rows, {"recomp-runtime/a.c": "0x00001000u"}, [])
        self.assertIn("outside every unit", errors[0])


if __name__ == "__main__":
    unittest.main()

