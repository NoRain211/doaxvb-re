# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests drive_game.py against a fake game: a thread that takes presses from
the press file and answers with log lines. No runner or game data."""
from pathlib import Path
import tempfile
import threading
import time
import unittest

import drive_game
from drive_game import DriveError, Session


class FakeSession(Session):
    def running(self):
        return True


class FakeGame(threading.Thread):
    """Takes each press and appends whatever `answer` returns for it."""

    def __init__(self, session, answer):
        super().__init__(daemon=True)
        self.session, self.answer, self.presses = session, answer, []
        self.stopping = threading.Event()

    def run(self):
        while not self.stopping.is_set():
            if self.session.press_path.exists():
                code = self.session.press_path.read_text(encoding="ascii")
                self.session.press_path.unlink()
                self.presses.append(code)
                lines = self.answer(code, len(self.presses))
                if lines:
                    with open(self.session.state["log"], "a", encoding="utf-8") as log:
                        log.writelines(line + "\n" for line in lines)
            time.sleep(0.01)


class DriveGameTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.log = self.folder / "run.log"
        self.log.write_text("", encoding="utf-8")
        self.session = FakeSession(self.folder / "session")
        self.session.state = {"pid": 1, "log": str(self.log), "mark": -1}
        self.session.save_state()
        self.game = None

    def tearDown(self):
        if self.game is not None:
            self.game.stopping.set()
            self.game.join()
        self.temporary.cleanup()

    def play(self, answer):
        self.game = FakeGame(self.session, answer)
        self.game.start()

    def append(self, *lines):
        with self.log.open("a", encoding="utf-8") as log:
            log.writelines(line + "\n" for line in lines)

    def test_helpers(self):
        self.assertEqual(drive_game.first_match(["a", "b", "a"], 0, "a"), 2)
        self.assertIsNone(drive_game.first_match(["a", "b"], 0, "a"))
        self.assertEqual(drive_game.direction(0, 5), "right")
        self.assertEqual(drive_game.direction(0, 7), "left")
        self.assertEqual(drive_game.direction(5, 7), "right")

    def test_waits_look_after_the_mark(self):
        self.append("[auto:ui] frame 1: scene title", "[auto:ui] frame 2: scene menu")
        self.assertEqual(self.session.wait("scene menu", 0), "[auto:ui] frame 2: scene menu")
        # The line is behind the mark now: the same wait finds nothing new.
        self.assertIsNone(self.session.wait("scene menu", 0))
        self.assertEqual(Session(self.session.folder).state["mark"], 1)
        with self.assertRaises(DriveError):
            self.session.expect("scene island", 0.2)

    def test_press_writes_codes_the_game_takes(self):
        self.play(lambda code, count: None)
        self.session.press("a")
        self.session.press("right", 2, 0)
        deadline = time.monotonic() + 2
        while len(self.game.presses) < 3 and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(self.game.presses, ["0", "d0008", "d0008"])
        with self.assertRaises(DriveError):
            self.session.press("z")

    def test_press_until_repeats_lost_presses(self):
        # The first two presses are lost; the third lands.
        self.play(lambda code, count: ["[auto:ui] frame 9: scene menu"] if count == 3 else None)
        line = self.session.press_until("start", "scene menu", 5, 0.1)
        self.assertEqual(line, "[auto:ui] frame 9: scene menu")
        self.assertEqual(self.game.presses, ["d0010"] * 3)

    def test_press_until_ignores_lines_from_earlier_presses(self):
        # A separate press moved the cursor through Options onto Delete. The
        # Options line from before that press must not end `until` at once:
        # it has to press up and see Options again.
        answers = {"d0002": "[auto:ui] frame 5: list 2 cursor 2 of 3: Delete a saved game",
                   "d0001": "[auto:ui] frame 7: list 2 cursor 1 of 3: Options"}
        self.play(lambda code, count: [answers[code]])
        self.append("[auto:ui] frame 3: list 2 cursor 1 of 3: Options")
        self.session.press("down")
        # As separate commands do: the next one starts from the saved mark,
        # with no wait in between to move it.
        deadline = time.monotonic() + 2
        while "Delete" not in self.log.read_text(encoding="utf-8") and time.monotonic() < deadline:
            time.sleep(0.01)
        self.session = FakeSession(self.session.folder)
        self.game.session = self.session
        line = self.session.press_until("up", "cursor 1 of 3: Options$", 5, 0.5)
        self.assertEqual(line, "[auto:ui] frame 7: list 2 cursor 1 of 3: Options")
        self.assertEqual(self.game.presses, ["d0002", "d0001"])

    def test_goto_takes_the_shorter_way(self):
        self.append("[auto:ui] frame 1: map location 0 Niki Beach")
        stops = {"d0004": -1, "d0008": 1}
        position = [0]

        def answer(code, count):
            position[0] = (position[0] + stops[code]) % 10
            if position[0] == 8:  # the map skips 8 in the morning
                position[0] = (position[0] + stops[code]) % 10
            return [f"[auto:ui] frame {count + 1}: map location {position[0]} x"]

        self.play(answer)
        self.session.goto("Poolside")
        self.assertEqual(self.game.presses, ["d0004", "d0004"])

    def test_select_wakes_a_hidden_list_and_confirms(self):
        self.append("[auto:ui] frame 1: list 0 open",
                    "[auto:ui] frame 1: list 0 cursor 0 of 5: Leave the poolside",
                    "[auto:ui] frame 2: list 0 closed")
        labels = ["Leave the poolside", "Take a rest", "Play Hopping game", "View items", "Quit the game"]
        state = {"open": False, "cursor": 0}

        def answer(code, count):
            if not state["open"]:
                state["open"] = True  # the waking press does nothing else
                return [f"[auto:ui] frame {count}: list 0 open",
                        f"[auto:ui] frame {count}: list 0 cursor 0 of 5: {labels[0]}"]
            if code == "d0002":
                state["cursor"] += 1
                return [f"[auto:ui] frame {count}: list 0 cursor {state['cursor']} of 5: {labels[state['cursor']]}"]
            return [f"[auto:ui] frame {count}: list 0 closed"]

        self.play(answer)
        self.session.select("View items")
        deadline = time.monotonic() + 2
        while len(self.game.presses) < 5 and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(self.game.presses, ["d0002", "d0002", "d0002", "d0002", "0"])

    def test_rest_presses_a_once_and_waits(self):
        self.append("[auto:ui] frame 1: list 0 open",
                    "[auto:ui] frame 1: list 0 cursor 0 of 5: Leave the poolside")
        labels = ["Leave the poolside", "Take a rest"]
        state = {"cursor": 0}

        def answer(code, count):
            if code == "d0002":
                state["cursor"] += 1
                return [f"[auto:ui] frame {count}: list 0 cursor {state['cursor']} of 5: {labels[state['cursor']]}"]
            if state["cursor"] == 1 and count == 2:
                state["cursor"] = 2
                return [f"[auto:ui] frame {count}: list 0 closed", f"[auto:ui] frame {count}: island rest"]
            # The A that skips the rest; night falls on the way back.
            return [f"[auto:ui] frame {count}: day part night", f"[auto:ui] frame {count}: island hotel"]

        self.play(answer)
        drive_game.REST_SKIP_DELAY, delay = 0, drive_game.REST_SKIP_DELAY
        try:
            line = self.session.rest()
        finally:
            drive_game.REST_SKIP_DELAY = delay
        self.assertTrue(line.endswith("island hotel"))
        self.assertEqual(self.game.presses, ["d0002", "0", "0"])

    def test_finish_dialogs_stops_pressing_once_closed(self):
        self.append("[auto:ui] frame 1: dialog open")
        self.play(lambda code, count: [f"[auto:ui] frame {count}: dialog closed"] if count == 3 else
                  [f"[auto:ui] frame {count}: island map"])
        self.session.finish_dialogs(10)
        time.sleep(0.3)
        self.assertEqual(self.game.presses, ["0", "0", "0"])

    def test_new_vacation_needs_an_empty_slot(self):
        self.append("[auto:ui] frame 1: save vacation 1")
        self.play(lambda code, count: [f"[auto:ui] frame {count}: list 2 cursor 0 of 3: Continue"])
        with self.assertRaises(DriveError):
            drive_game.new_vacation(self.session, 1, "Kasumi")
        self.assertEqual(self.game.presses, ["0"])

    def test_buy_walks_the_prompts(self):
        state = {"step": "list", "cursor": 0}

        def answer(code, count):
            lines = []
            if code == "0":
                state["step"] = {"list": "buy", "buy": "wrap", "wrap": "list"}[state["step"]]
                state["cursor"] = 0
                lines.append(f"[auto:ui] frame {count}: shop step {state['step']}")
                if state["step"] != "list":
                    label = "Buy" if state["step"] == "buy" else "Wrap"
                    lines.append(f"[auto:ui] frame {count}: shop prompt cursor 0: {label}")
            elif code == "d0008":
                state["cursor"] = 1
                lines.append(f"[auto:ui] frame {count}: shop prompt cursor 1: No")
            return lines

        self.play(answer)
        self.session.buy()
        self.assertEqual(self.game.presses, ["0", "0", "d0008", "0"])

    def test_stop_puts_back_saves_the_run_wrote(self):
        saves, backup = self.folder / "UDATA", self.folder / "backup"
        (saves / "slot").mkdir(parents=True)
        (saves / "slot/data").write_bytes(b"before")
        drive_game.shutil.copytree(saves, backup)
        (saves / "slot/data").write_bytes(b"written")
        self.session.state.update(saves=str(saves), backup=str(backup))
        self.session.running = lambda: False
        self.session.stop()
        self.assertEqual((saves / "slot/data").read_bytes(), b"before")
        written = list(self.session.folder.glob("saves-written-*"))
        self.assertEqual(len(written), 1)
        self.assertEqual((written[0] / "slot/data").read_bytes(), b"written")
        self.assertTrue(drive_game.same_tree(saves, backup))

    def test_stop_puts_back_the_controller_mode_preference(self):
        self.session.running = lambda: False
        preference = self.folder / "controller-mode-v1"
        preference.write_bytes(b"DCP1\x01\x01\x01\x01")
        self.session.state.update(preference=str(preference), preference_bytes=b"DCP1\0\0\0\0".hex())
        self.session.stop()
        self.assertEqual(preference.read_bytes(), b"DCP1\0\0\0\0")
        # A file the run created goes again.
        self.session.state.update(pid=1, preference_bytes=None)
        self.session.stop()
        self.assertFalse(preference.exists())
        # --keep-save keeps it.
        preference.write_bytes(b"DCP1\x01\0\0\0")
        self.session.state.update(pid=1)
        self.session.stop(keep_save=True)
        self.assertTrue(preference.exists())


if __name__ == "__main__":
    unittest.main()
