# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive the game with scripted pad presses, step by step, by its log.

Each step presses a button and waits for the [auto:ui] line that shows the
press landed (RECOMP_AUTO_LOG; see docs/ui-log.md), instead of sleeping. A
session folder keeps the running game between commands, so steps can be run
one at a time from a shell or chained in a route. See docs/automation.md."""
import argparse
from datetime import datetime
import filecmp
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time

from extract_iso import package_root
import run_game

# Codes for --input-paused-analog-file: an analog button's index, or "d" and
# a digital mask in four hex digits.
BUTTONS = {"a": "0", "b": "1", "x": "2", "y": "3", "black": "4", "white": "5", "lt": "6", "rt": "7",
           "up": "d0001", "down": "d0002", "left": "d0004", "right": "d0008",
           "start": "d0010", "back": "d0020", "ls": "d0040", "rs": "d0080"}
# The island map's stops by the log's location number; 8 has no name yet.
LOCATIONS = ("Niki Beach", "Private Beach", "Jungle", "Bass Island", "Accessory Shop", "Sports Shop",
             "Zack of All Trades", "Poolside", None, "Radio Station")
# Shows a list menu that has hidden itself; the game takes the press only
# to show the menu again.
WAKE_BUTTON = "down"
# Seconds into a rest before its scene takes the A that skips it.
REST_SKIP_DELAY = 3
# Advances the dialogs that open the hotel's night; B does not.
HOTEL_DIALOG_BUTTON = "a"
AUTO_LINE = re.compile(r"^\[auto:")
POLL = 0.1


class DriveError(Exception):
    pass


def say(text):
    print(f"[auto:driver] {text}", flush=True)


def first_match(lines, mark, pattern):
    """The index of the first line after index `mark` matching `pattern`."""
    expression = re.compile(pattern)
    for index in range(mark + 1, len(lines)):
        if expression.search(lines[index]):
            return index
    return None


def direction(current, target):
    """The shorter way round the map from one stop to another."""
    return "right" if (target - current) % len(LOCATIONS) <= len(LOCATIONS) // 2 else "left"


def same_tree(left, right):
    """Whether two folders hold the same files with the same contents."""
    compared = filecmp.dircmp(left, right)
    if compared.left_only or compared.right_only or compared.funny_files:
        return False
    _, mismatch, errors = filecmp.cmpfiles(left, right, compared.common_files, shallow=False)
    return not mismatch and not errors and all(
        same_tree(Path(left) / name, Path(right) / name) for name in compared.common_dirs)


def alive(pid):
    if os.name == "nt":
        listed = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"],
                                capture_output=True, text=True).stdout
        return re.search(rf"\b{pid}\b", listed) is not None
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def find_game(root, runner=None, image=None):
    """The runner and game executable: given, from the selected build
    receipt, or the separately installed runner run_game.py also accepts."""
    if runner is None:
        built = run_game.build_inputs(root)
        if built is not None:
            return built[0], built[1]
        candidates = [root / "recomp_program_runner.exe", root / "recomp_program_runner",
                      root / "build/recomp-program/Release/recomp_program_runner.exe",
                      root / "build/recomp-program/recomp_program_runner"]
        runner = next((path for path in candidates if path.is_file()), None)
        if runner is None:
            raise DriveError("no runner found; pass --runner")
    if image is None:
        images = list((root / "private/imported-disc/disc").glob("*.xbe"))
        if len(images) != 1:
            raise DriveError("cannot choose the game executable; pass --xbe")
        image = images[0]
    return Path(runner).resolve(strict=True), Path(image).resolve(strict=True)


def other_runners(image):
    """The ids of runner processes started on `image`, where /proc lists
    them (Linux); an empty list elsewhere."""
    found, wanted = [], Path(image).resolve()
    proc = Path("/proc")
    if not proc.is_dir():
        return found
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            args = (entry / "cmdline").read_bytes().decode("utf-8", "replace").split("\0")
        except OSError:
            continue
        if args and "recomp_program_runner" in Path(args[0]).name and "--xbe" in args:
            index = args.index("--xbe") + 1
            if index >= len(args):
                continue
            path = Path(args[index])
            if not path.is_absolute():
                try:
                    path = Path(os.readlink(entry / "cwd")) / path
                except OSError:
                    continue
            if path.resolve() == wanted:
                found.append(int(entry.name))
    return found


class Session:
    """A running game and what the steps have seen of its log so far."""

    def __init__(self, folder):
        self.folder = Path(folder)
        self.state_path = self.folder / "session.json"
        self.press_path = self.folder / "press"
        self.trigger_path = self.folder / "shot-trigger"
        self.frames = self.folder / "frames"
        self.shots = self.folder / "shots"
        self.process = None
        try:
            self.state = json.loads(self.state_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            self.state = {}

    def save_state(self):
        self.folder.mkdir(parents=True, exist_ok=True)
        self.state_path.write_text(json.dumps(self.state, indent=2), encoding="utf-8")

    def running(self):
        if self.process is not None:
            return self.process.poll() is None
        return "pid" in self.state and alive(self.state["pid"])

    # Starting and stopping

    def start(self, runner, image, log=None, env=None):
        if self.running():
            raise DriveError(f"a game is already running (pid {self.state['pid']}); stop it first")
        others = other_runners(image)
        if others:
            # A person may be playing it: its saves are in use, and a backup
            # taken now would be older than what that game writes next.
            raise DriveError(f"another game is running on this disc (pid {others[0]}); not starting")
        if "pid" in self.state:
            # The last run ended without stop (it crashed or was killed):
            # its backup still holds the player's saves, so put them back
            # before taking a new one.
            say("the last run was not stopped; putting back what it changed")
            self.put_back(keep=False)
            self.state.pop("pid")
            self.save_state()
        self.folder.mkdir(parents=True, exist_ok=True)
        self.frames.mkdir(exist_ok=True)
        for path in (self.press_path, self.trigger_path, self.folder / "resume"):
            path.unlink(missing_ok=True)
        log = Path(log or self.folder / ("run-" + datetime.now().strftime("%Y%m%d-%H%M%S") + ".log")).resolve()
        # A scripted run must not change the player's saves: stop puts this back.
        saves = image.parent / ".recomp-storage/partition1/UDATA"
        backup = self.folder / "saves-at-start"
        shutil.rmtree(backup, ignore_errors=True)
        if saves.is_dir():
            shutil.copytree(saves, backup)
        # So must the Analog/Digital choice, which the runtime keeps beside
        # the saves as a PC preference (docs/recomp-controls.md).
        preference = image.parent / ".recomp-storage/controller-mode-v1"
        preference_bytes = preference.read_bytes().hex() if preference.is_file() else None
        environment = dict(os.environ, RECOMP_AUTO_LOG="1", RECOMP_AUDIO_GAIN="0", RECOMP_D3D_SCALE="1",
                           RECOMP_SCREENSHOT_TRIGGER=str(self.trigger_path),
                           RECOMP_SCREENSHOT_DIR=str(self.frames))
        environment.update(env or {})
        command = [str(runner), "--xbe", str(image), "--vsync", "--input-wait-after-poll", "1",
                   "--input-resume-file", str(self.folder / "resume"),
                   "--input-paused-analog-file", str(self.press_path)]
        detach = ({"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt"
                  else {"start_new_session": True})
        with log.open("w", encoding="utf-8") as output:
            self.process = subprocess.Popen(command, cwd=image.parent, env=environment, stdout=output,
                                            stderr=subprocess.STDOUT, **detach)
        self.state = {"pid": self.process.pid, "log": str(log), "mark": -1, "saves": str(saves),
                      "backup": str(backup) if backup.is_dir() else None,
                      "preference": str(preference), "preference_bytes": preference_bytes}
        self.save_state()
        say(f"started pid {self.process.pid}, log {log}")
        # The runner checks that no press is waiting before it polls the
        # pad; presses go in once it does.
        try:
            polled = self.wait(r"^recomp input: script waiting after poll", 60)
        except DriveError:
            polled = None
        if polled is None and not self.running():
            # The game stopped before it ran, so it changed nothing; drop
            # the backup, which a later stop or start would otherwise put
            # back over saves written since.
            for key in ("pid", "backup", "preference", "preference_bytes"):
                self.state.pop(key, None)
            self.save_state()
            raise DriveError(f"the game exited before it polled its pad; see {log}")
        if polled is None:
            raise DriveError("the game never polled its pad")

    def stop(self, keep_save=False):
        pid = self.state.get("pid")
        if pid is not None and self.running():
            if os.name == "nt":
                subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)
            else:
                os.kill(pid, signal.SIGTERM)
            deadline = time.monotonic() + 5
            while self.running() and time.monotonic() < deadline:
                time.sleep(POLL)
            if self.running() and os.name != "nt":
                os.kill(pid, signal.SIGKILL)
        for path in (self.press_path, self.trigger_path):
            path.unlink(missing_ok=True)
        self.put_back(keep_save)
        self.state.pop("pid", None)
        self.save_state()
        say("stopped")

    def put_back(self, keep):
        """Puts back the saves and the controller-mode preference as they
        were at start, unless `keep`."""
        saves, backup = self.state.get("saves"), self.state.get("backup")
        if backup and Path(backup).is_dir() and not same_tree(saves, backup):
            if keep:
                say("the run changed the saves; kept them")
            else:
                written = self.folder / ("saves-written-" + datetime.now().strftime("%Y%m%d-%H%M%S"))
                shutil.move(saves, written)
                shutil.copytree(backup, saves)
                say(f"the run wrote the saves; restored them (the written ones are in {written})")
        self.restore_preference(keep)

    def restore_preference(self, keep):
        """Puts back the controller-mode preference as it was at start: the
        same bytes, or no file."""
        path = self.state.get("preference")
        if not path:
            return
        path, before = Path(path), self.state.get("preference_bytes")
        now = path.read_bytes().hex() if path.is_file() else None
        if now == before:
            return
        if keep:
            say("the run changed the controller-mode preference; kept it")
        elif before is None:
            path.unlink()
            say("the run wrote a controller-mode preference; removed it")
        else:
            path.write_bytes(bytes.fromhex(before))
            say("the run changed the controller-mode preference; restored it")

    # Reading the log

    def lines(self):
        try:
            return Path(self.state["log"]).read_text(encoding="utf-8", errors="replace").splitlines()
        except (KeyError, OSError):
            return []

    def auto_lines(self, count=10):
        return [line for line in self.lines() if AUTO_LINE.match(line)][-count:]

    def seen(self, pattern):
        """The first line after the mark matching `pattern`, which becomes
        the mark: later waits only look after it. None if there is none."""
        lines = self.lines()
        index = first_match(lines, self.state.get("mark", -1), pattern)
        if index is None:
            return None
        self.state["mark"] = index
        self.save_state()
        return lines[index]

    def latest(self, pattern):
        """The last line in the whole log matching `pattern`, or None."""
        expression = re.compile(pattern)
        return next((line for line in reversed(self.lines()) if expression.search(line)), None)

    def wait(self, pattern, timeout=20.0):
        deadline = time.monotonic() + timeout
        while True:
            line = self.seen(pattern)
            if line is not None:
                return line
            if not self.running():
                raise DriveError(f"the game exited while waiting for /{pattern}/")
            if time.monotonic() >= deadline:
                return None
            time.sleep(POLL)

    def expect(self, pattern, timeout=20.0):
        line = self.wait(pattern, timeout)
        if line is None:
            raise DriveError(f"no /{pattern}/ within {timeout:g} s")
        say(f"saw {line}")
        return line

    # Pressing

    def press(self, button, count=1, gap=0.6):
        """Presses `button` `count` times. The mark moves to the end of the
        log first, so a wait after it only sees what followed this press: a
        line an earlier press caused, as from a separate `press` command,
        cannot stand in for this one's."""
        code = BUTTONS.get(button)
        if code is None:
            raise DriveError(f"unknown button {button}; one of {', '.join(BUTTONS)}")
        self.finish()
        for number in range(count):
            # The runner removes the file when it takes the press.
            deadline = time.monotonic() + 5
            while self.press_path.exists():
                if time.monotonic() >= deadline:
                    raise DriveError("the game is not taking presses")
                time.sleep(POLL / 2)
            temporary = self.press_path.with_suffix(".tmp")
            temporary.write_text(code, encoding="ascii")
            os.replace(temporary, self.press_path)
            if number + 1 < count:
                time.sleep(gap)

    def press_until(self, button, pattern, timeout=30.0, gap=1.5):
        """Presses `button` every `gap` s until a line after the mark matches.
        A match already logged since the last press counts without a press."""
        line = self.seen(pattern)
        deadline = time.monotonic() + timeout
        while line is None:
            if time.monotonic() >= deadline:
                raise DriveError(f"pressed {button} for {timeout:g} s and never saw /{pattern}/")
            self.press(button)
            line = self.wait(pattern, gap)
        say(f"saw {line}")
        return line

    def finish(self):
        """Moves the mark to the end of the log."""
        self.state["mark"] = len(self.lines()) - 1
        self.save_state()

    # Screens

    def goto(self, target):
        """Steps the island map to a location; a press the map ignores while
        it loads is repeated."""
        if target not in LOCATIONS:
            raise DriveError(f"unknown location {target}")
        wanted = LOCATIONS.index(target)
        for _ in range(40):
            line = self.latest(r"\[auto:ui\] .*map location \d+")
            if line is None:
                raise DriveError("not on the island map")
            current = int(re.search(r"map location (\d+)", line).group(1))
            if current == wanted:
                self.finish()
                say(f"on {target}")
                return
            # Only a line after this press shows the map moved.
            self.finish()
            self.press(direction(current, wanted))
            self.wait(r"map location", 1.5)
        raise DriveError(f"could not reach {target}")

    def wake_list(self):
        """Shows the list menu again when it has hidden itself (the pool's
        menu does after a few idle seconds)."""
        last = self.latest(r"\[auto:ui\] .*list 0 (open|closed)$")
        if last is not None and last.endswith("closed"):
            self.finish()
            self.press_until(WAKE_BUTTON, r"list 0 open$", 10, 1.5)

    def select(self, label, timeout=30.0):
        """Moves the open list menu's (list 0's) cursor onto the item `label`
        and confirms it."""
        expression = re.compile(r"list 0 cursor (\d+) of (\d+): " + re.escape(label) + "$")
        deadline = time.monotonic() + timeout
        while True:
            self.wake_list()
            line = self.latest(r"\[auto:ui\] .*list 0 cursor \d+ of \d+: ")
            if line is not None and expression.search(line):
                break
            if time.monotonic() >= deadline:
                raise DriveError(f"the list never reached {label}")
            self.finish()
            self.press("down")
            self.wait(r"list 0 (cursor|closed)", 1.5)
        say(f"on {label}")
        self.finish()
        self.press("a")

    def rest(self):
        """Takes a rest at the pool, which uses up a part of the day, and
        waits until it is over: back at the pool, or at the hotel once night
        falls. One A skips the rest's scene; no more are pressed, since the
        pool's menu takes the next A as Leave the poolside."""
        self.select("Take a rest")
        self.expect(r"island rest$", 20)
        time.sleep(REST_SKIP_DELAY)
        self.press("a")
        return self.expect(r"island (pool|hotel)$", 60)

    def buy(self, wrap=False):
        """Buys the item under the shop's cursor: Buy, then Wrap or No."""
        self.finish()
        self.press_until("a", r"shop step buy$", 10)
        self.press_until("left", r"shop prompt cursor 0: Buy$", 10, 0.6)
        self.press_until("a", r"shop step wrap$", 10)
        if wrap:
            self.press_until("left", r"shop prompt cursor 0: Wrap$", 10, 0.6)
        else:
            self.press_until("right", r"shop prompt cursor 1: No$", 10, 0.6)
        self.press_until("a", r"shop step list$", 10)

    def finish_dialogs(self, timeout=180.0):
        """Presses A while a dialog is open, one press a line, until it
        closes. Nothing is pressed once it has closed, so no press reaches
        the screen behind it."""
        deadline = time.monotonic() + timeout
        while True:
            last = self.latest(r"\[auto:ui\] .*dialog (open|closed)$")
            if last is None or last.endswith("closed"):
                self.finish()
                return
            if time.monotonic() >= deadline:
                raise DriveError(f"a dialog stayed open for {timeout:g} s")
            self.finish()
            self.press("a")
            self.wait(r"\[auto:ui\]", 1.2)

    def shot(self, name):
        """Saves the next frame (without the runtime's overlay) as shots/NAME.png."""
        before = set(self.frames.glob("*.png"))
        self.trigger_path.touch()
        deadline = time.monotonic() + 10
        while True:
            new = sorted(set(self.frames.glob("*.png")) - before, key=lambda path: path.stat().st_mtime)
            if new:
                break
            if time.monotonic() >= deadline or not self.running():
                raise DriveError("no screenshot arrived")
            time.sleep(POLL)
        # The file is complete once its size holds still.
        size = -1
        while new[-1].stat().st_size != size:
            size = new[-1].stat().st_size
            time.sleep(POLL * 2)
        self.shots.mkdir(exist_ok=True)
        target = self.shots / f"{name}.png"
        os.replace(new[-1], target)
        say(f"shot {target}")
        return target

    def fail(self, error):
        """Reports a failed step with a screenshot of where it stopped."""
        print(f"[auto:driver] failed: {error}", file=sys.stderr, flush=True)
        if self.running():
            try:
                self.shot("fail-" + datetime.now().strftime("%H%M%S"))
            except DriveError:
                pass
        for line in self.auto_lines(5):
            print(line, file=sys.stderr)


def to_save_select(session):
    """From launch to the save select, Vacation 1 under the cursor."""
    # The opening ignores early presses; START until the menus.
    session.press_until("start", r"\[auto:ui\] frame \d+: scene menu", 60, 2)
    session.expect(r"list 0 cursor 0 of 2: GO TO ZACK ISLAND")
    session.press_until("a", r"menu screen save-select")
    session.expect(r"save vacation 1")


def choose_vacation(session, number):
    """Moves the save select's cursor to Vacation `number` and opens its
    prompt; returns the prompt's first item, Continue or New Game."""
    current = int(re.search(r"save vacation (\d+)$", session.latest(r"\[auto:ui\] .*save vacation \d+$"))[1])
    if number != current:
        session.press_until("right" if number > current else "left", rf"save vacation {number}$", 10, 1)
    return session.press_until("a", r"list 2 cursor 0 of \d+: ", 10).rsplit(": ", 1)[1]


def new_vacation(session, number, girl):
    """Starts a new vacation in the empty Vacation `number` as `girl`, past
    the opening scene and the welcome dialogs."""
    if choose_vacation(session, number) != "New Game":
        raise DriveError(f"Vacation {number} has a save; choose an empty one")
    session.press_until("a", r"island opening$", 10)
    # START skips the opening scene.
    session.press_until("start", r"island pick$", 30, 2)
    if not session.expect(r"pick cursor \d+ ", 10).endswith(f" {girl}"):
        session.press_until("right", rf"pick cursor \d+ {re.escape(girl)}$", 20, 0.6)
    session.press_until("a", r"island pool$", 30, 4)
    session.expect(r"dialog open$", 30)
    session.finish_dialogs()


def route(session, target, vacation=1, girl="Kasumi"):
    """From launch to the Sports Shop, the island map, the pool's menu, the
    items screen or the hotel's menu, each step waiting for the line that
    shows it landed. "new" starts a new vacation instead, in an empty
    Vacation `vacation` as `girl`, and ends where its welcome leaves off."""
    to_save_select(session)
    if target == "new":
        new_vacation(session, vacation, girl)
        place = session.latest(r"\[auto:ui\] .*island \w+$").rsplit(" ", 1)[1]
        say(f"route new: done, at the {place}")
        return
    if choose_vacation(session, vacation) != "Continue":
        raise DriveError(f"Vacation {vacation} has no save to continue")
    # One A takes Continue; the island takes a while to load.
    session.press_until("a", r"menu screen -1|scene island", 30, 4)
    session.expect(r"map location", 60)
    if target in ("pool", "items"):
        session.goto("Poolside")
        session.press_until("a", r"island pool")
        session.expect(r"list 0 cursor 0 of 5", 30)
        if target == "items":
            session.select("View items")
            session.expect(r"island items")
    elif target == "hotel":
        # Rest at the pool until night falls; the evening rest ends at the
        # hotel. Its gifts come first, one A each, then its menu opens.
        session.goto("Poolside")
        session.press_until("a", r"island pool")
        for _ in range(4):
            session.expect(r"list 0 cursor 0 of 5", 30)
            if session.rest().endswith("hotel"):
                break
        else:
            raise DriveError("night never came")
        session.press_until(HOTEL_DIALOG_BUTTON, r"list 0 cursor \d+ of 7: ", 60, 2.5)
        if session.latest(r"\[auto:ui\] .*island (hotel|casino)$").endswith("casino"):
            session.select("Go back to the Room")
            session.expect(r"list 0 cursor \d+ of 7: ", 30)
    elif target == "shop":
        session.goto("Sports Shop")
        session.press_until("a", r"shop 0 cursor")
    say(f"route {target}: done")


def main(argv=None):
    root = package_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", type=Path, default=root / "private/drive",
                        help="session folder (default private/drive)")
    commands = parser.add_subparsers(dest="command", required=True)

    def add_start_options(command):
        command.add_argument("--runner", type=Path, help="runner to start (default: the selected build)")
        command.add_argument("--xbe", type=Path, help="game executable (default: the imported disc's)")
        command.add_argument("--log", type=Path, help="runner log (default: in the session folder)")
        command.add_argument("--env", action="append", default=[], metavar="NAME=VALUE",
                             help="extra environment for the runner, such as RECOMP_SHOP_PANEL=1")

    add_start_options(commands.add_parser("start", help="start the game"))
    route_command = commands.add_parser("route", help="start the game and walk to a screen")
    route_command.add_argument("target", choices=("shop", "map", "pool", "items", "hotel", "new"))
    route_command.add_argument("--vacation", type=int, default=None,
                               help="save slot: 1 to continue (default), an empty one for new (default 2)")
    route_command.add_argument("--girl", default="Kasumi", help="who to play in a new vacation")
    add_start_options(route_command)
    command = commands.add_parser("press", help="press a button")
    command.add_argument("button", choices=BUTTONS)
    command.add_argument("-n", "--count", type=int, default=1)
    command.add_argument("--gap", type=float, default=0.6)
    command = commands.add_parser("wait", help="wait for a log line after the mark")
    command.add_argument("pattern")
    command.add_argument("--timeout", type=float, default=20)
    command = commands.add_parser("until", help="press a button until a log line after the mark matches")
    command.add_argument("button", choices=BUTTONS)
    command.add_argument("pattern")
    command.add_argument("--timeout", type=float, default=30)
    command.add_argument("--gap", type=float, default=1.5)
    command = commands.add_parser("goto", help="step the island map to a location")
    command.add_argument("location", choices=[name for name in LOCATIONS if name])
    command = commands.add_parser("select", help="confirm an item of the open list menu")
    command.add_argument("label")
    commands.add_parser("rest", help="take a rest at the pool")
    command = commands.add_parser("buy", help="buy the item under the shop's cursor")
    command.add_argument("--wrap", action="store_true", help="gift wrap it")
    command = commands.add_parser("shot", help="save the next frame as shots/NAME.png")
    command.add_argument("name")
    command = commands.add_parser("log", help="the last [auto:...] lines")
    command.add_argument("-n", "--count", type=int, default=15)
    command = commands.add_parser("stop", help="stop the game and put back the saves it wrote")
    command.add_argument("--keep-save", action="store_true")
    args = parser.parse_args(argv)

    session = Session(args.session)
    try:
        if args.command in ("start", "route"):
            runner, image = find_game(root, args.runner, args.xbe)
            env = dict(item.split("=", 1) for item in args.env)
            session.start(runner, image, args.log, env)
            if args.command == "route":
                vacation = args.vacation or (2 if args.target == "new" else 1)
                route(session, args.target, vacation, args.girl)
        elif args.command == "stop":
            session.stop(args.keep_save)
        elif args.command == "log":
            print("\n".join(session.auto_lines(args.count)))
        elif not session.running():
            raise DriveError("no game is running; start one first")
        elif args.command == "press":
            session.press(args.button, args.count, args.gap)
        elif args.command == "wait":
            session.expect(args.pattern, args.timeout)
        elif args.command == "until":
            session.press_until(args.button, args.pattern, args.timeout, args.gap)
        elif args.command == "goto":
            session.goto(args.location)
        elif args.command == "select":
            session.select(args.label)
        elif args.command == "rest":
            session.rest()
        elif args.command == "buy":
            session.buy(args.wrap)
        elif args.command == "shot":
            session.shot(args.name)
    except (DriveError, OSError, ValueError) as error:
        session.fail(error)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
