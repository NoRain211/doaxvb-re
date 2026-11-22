DOAXBV 0.7.0 BETA - Linux and Steam Deck

A native port of Dead or Alive Xtreme Beach Volleyball for x86-64 Linux,
including SteamOS on the Steam Deck. This package holds source and setup
tools only. You need your own copy of the game as an ISO; the build turns it
into a playable runner.

Smoke runs of RC1 on a Steam Deck reached the save-select and settings menus
at 60 fps; Linux has not been play-tested. SMAA and custom soundtracks are
not available on Linux yet. Point lights, the title sea shimmer, sun
shadows, per-pixel lighting and RECOMP_GAME_HZ are Windows-only for now
(issue #105); fog now works on Linux. The free camera, model viewer, title
screen idle modes and swimsuit unlock work here too. Online play has not
been tested on Linux.

UPGRADING
The game program changed in 0.7.0. Extract this ZIP into a new folder and
build again. Saves are unchanged; copy your private saves over.

SETUP
1. Install the build tools (compiler, CMake, Ninja, Git, Python and SDL3's
   build dependencies) with apt, dnf or pacman:
     ./install_deps.sh
   ./build_game.sh run in a terminal also does this when a tool is missing.
2. In this folder: ./launcher.sh, then choose your ISO on its Build page
   and build the game, or run ./build_game.sh /path/to/game.iso instead.
   The first build installs Capstone into private/venv, builds extract-xiso,
   and builds SDL3 when your distribution has none. Setup compiles with
   clang.

STEAM DECK
SteamOS has no compiler, so the build runs in an Ubuntu 24.04 distrobox that
shares your home folder; you then play on SteamOS itself. Extract this ZIP
under your home folder and, in Desktop Mode, run ./launcher.sh and build
from its Build page, or run in Konsole:
     ./build_steamdeck.sh /path/to/game.iso
Either one installs distrobox when needed, creates the container and
installs the build tools in it.

PLAY
The game compiles its shaders with shaderc at runtime, so the system needs
libshaderc_shared.so.1 (SteamOS includes it; on Ubuntu install libshaderc1).
./launcher.sh opens a gamepad-friendly launcher, then starts the game. It is
prebuilt, so it opens on SteamOS itself without a container.
./run_game.sh starts the game with the saved choices. For Steam Game Mode,
add launcher.sh with "Add a Non-Steam Game" in Desktop Mode; if one
controller drives two players, turn Steam Input off for that shortcut.
A built folder can be moved or copied; saves live in private/.

See docs/building.md for details and CHANGELOG.md for what changed.
Support the project: https://ko-fi.com/norainsrecomps
