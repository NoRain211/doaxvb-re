DOAXBV 0.6.2 BETA - macOS

A native port of Dead or Alive Xtreme Beach Volleyball for Apple Silicon and
Intel Macs. This package holds source and setup tools only. You need your
own copy of the game as an ISO; the build turns it into a playable runner.

A contributor's smoke run of RC1 reached the save-select and options menus;
macOS has not been play-tested. SMAA and custom soundtracks are not
available on macOS yet. Fog, point lights, the title sea shimmer, sun
shadows, per-pixel lighting and RECOMP_GAME_HZ are Windows-only for now
(issue #105).

UPGRADING
The game program is the same as 0.6.0 and 0.6.1. If your build works, you
don't need this release. Otherwise extract this ZIP into a new folder and
build again. Saves are unchanged; copy your private saves over.

SETUP
1. Install the Xcode command line tools: xcode-select --install
2. Install Homebrew packages: brew install cmake ninja sdl3 python
3. In this folder: ./build_game.sh /path/to/game.iso
   The first run installs Capstone into private/venv and builds
   extract-xiso; both need internet access.

PLAY
./launcher.sh picks resolution, anti-aliasing, volume and music options,
then starts the game. ./run_game.sh starts it with the saved choices.
Keep this folder in place after setup; saves live in private/.

See docs/building.md for details and CHANGELOG.md for what changed.
Support the project: https://ko-fi.com/norainsrecomps
