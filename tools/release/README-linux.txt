DOAXBV 0.6.2 BETA - Linux and Steam Deck

A native port of Dead or Alive Xtreme Beach Volleyball for x86-64 Linux,
including SteamOS on the Steam Deck. This package holds source and setup
tools only. You need your own copy of the game as an ISO; the build turns it
into a playable runner.

Smoke runs of RC1 on a Steam Deck reached the save-select and settings menus
at 60 fps; Linux has not been play-tested. SMAA and custom soundtracks are
not available on Linux yet. Fog, point lights, the title sea shimmer, sun
shadows, per-pixel lighting and RECOMP_GAME_HZ are Windows-only for now
(issue #105). The free camera, model viewer, title screen idle modes and
swimsuit unlock work here too.

UPGRADING
The game program is the same as 0.6.0 and 0.6.1. If your build works, you
don't need this release. Otherwise extract this ZIP into a new folder and
build again. Saves are unchanged; copy your private saves over.

SETUP (Ubuntu 24.04 or similar; on SteamOS, use an Ubuntu distrobox)
1. Install build tools and SDL3's build dependencies:
     sudo apt install cmake ninja-build clang git python3-venv \
       libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev \
       libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev \
       libdecor-0-dev libasound2-dev libpulse-dev libvulkan-dev libshaderc1
2. In this folder: ./build_game.sh /path/to/game.iso
   The first run installs Capstone into private/venv, builds extract-xiso,
   and builds SDL3 when your distribution has none. Setup compiles with
   clang.

PLAY
The game compiles its shaders with shaderc at runtime, so the system needs
libshaderc_shared.so.1 (SteamOS includes it; on Ubuntu install libshaderc1).
./launcher.sh opens a gamepad-friendly settings screen, then starts the
game. ./run_game.sh starts it with the saved choices. For Steam Game Mode,
add launcher.sh with "Add a Non-Steam Game" in Desktop Mode; if one
controller drives two players, turn Steam Input off for that shortcut.
Keep this folder in place after setup; saves live in private/.

See docs/building.md for details and CHANGELOG.md for what changed.
Support the project: https://ko-fi.com/norainsrecomps
