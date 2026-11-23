DOAXBV 0.8.0 BETA - macOS

A native port of Dead or Alive Xtreme Beach Volleyball for Apple Silicon and
Intel Macs. This package holds source and setup tools only. You need your
own copy of the game as an ISO; the build turns it into a playable runner.

A contributor's smoke run of RC1 reached the save-select and options menus;
macOS has not been play-tested. The new launcher in 0.7.0 has been drawn
only in CI on macOS, not used on a Mac. SMAA is not available on macOS
yet. Custom soundtracks: put MP3, WAV or FLAC files in private/UserMusic
(the launcher's Audio page opens it) and pick them at the Radio Station. Point lights, the title sea shimmer, sun shadows,
per-pixel lighting and RECOMP_GAME_HZ are Windows-only for now (issue #105);
fog now works on macOS. Online play has not been tested on macOS.

UPGRADING
The game program is the same as 0.7.0, but the runtime changed. Extract
this ZIP into a new folder and build again. Saves are unchanged; copy your
private saves over. Online, 0.8.0 meets only other 0.8.0 players. The
launcher now tells you when a newer release is out; it downloads nothing.

SETUP
1. Install the Xcode command line tools: xcode-select --install
2. Install Homebrew packages: brew install cmake ninja sdl3 python
3. In this folder: ./launcher.sh
   The first run builds the launcher, which opens on its Build page: choose
   your ISO and build the game there, or run ./build_game.sh /path/to/game.iso
   instead. The first build installs Capstone into private/venv and builds
   extract-xiso; both need internet access.

PLAY
./launcher.sh picks resolution, anti-aliasing, fog, volume and music
options, then starts the game. ./run_game.sh starts it with the saved
choices. A built folder can be moved or copied; saves live in private/.

See docs/building.md for details and CHANGELOG.md for what changed.
Support the project: https://ko-fi.com/norainsrecomps
