DOAXBV 0.6.2 BETA
Fully playable, with known issues. See CHANGELOG.md for the full history.

QUICK START
1. Extract the entire ZIP into a new folder with a short path.
2. Open Launcher.cmd, go to BUILD, choose your supported USA game ISO and
   press Build game. (Dragging the ISO onto BuildGame.cmd does the same.)
   The first build downloads the compiler. Wait for "Build complete".
3. Choose your settings and press PLAY. After setup, Launcher.exe opens the
   launcher and DOAXBV.exe starts the game with your saved settings.
   RunGame.cmd starts the game with the default settings.
Once a build exists, an Xbox or PlayStation controller also works in the
launcher: LB/RB switch sections, the d-pad moves and changes options,
A selects, Start plays and B quits.

REQUIREMENTS
Windows 10/11 x64, a supported USA game ISO, internet access and enough disk
space for extracted game files and compilation. Python, Capstone, Git,
CMake and extract-xiso are included. Setup downloads the llvm-mingw
compiler (190 MB, about 715 MB extracted), the DXC shader compiler (32 MB)
and SDL3. No administrator approval, Visual Studio or Windows SDK is needed.
If extract-xiso reports a missing VCRUNTIME140.dll, install the included
Prerequisites/vc_redist.x86.exe and retry (it is for the 32-bit extractor).

UPGRADING
The game program is the same as 0.6.0 and 0.6.1. If your build works, you
don't need this release. Otherwise extract this ZIP into a new folder and
build again. Saves are unchanged; copy your private saves over from your
previous install. Saves are not imported automatically.

WHAT CHANGED SINCE 0.5.3
Setup: no Visual Studio needed. The launcher has a new look, builds the game
from its BUILD page, works with a controller, and setup adds DOAXBV.exe.

Rendering: fog, point lights, specular highlights, sharper textures at steep
angles, smoother foliage and hair edges with MSAA, the title screen sea
shimmer, and no halo around characters in depth-of-field scenes. Scene
transitions show the last frame instead of going black.

Launcher options (all off by default): sun shadows, per-pixel lighting,
smooth normals, title screen idle modes (no cutscenes, or endless CPU
Exhibition matches), shorter demo cuts, every swimsuit in Exhibition, a free
camera with a model viewer and screenshots, texture replacement and VRR.

High refresh (opt-in, set as a Windows environment variable before starting
the game): RECOMP_SPLIT_RATE=auto shows extra frames on displays above 60 Hz
with gameplay timing unchanged. RECOMP_GAME_HZ=120 is an experimental
whole-game version that still shows slight streaking. Leave both unset on
60 Hz displays.

macOS and Linux (including Steam Deck): download the separate ZIPs.

CONTROLLERS: DO NOT USE DS4WINDOWS
Connect PlayStation controllers directly and close DS4Windows (or disable
its controller output) before playing. DS4Windows adds an emulated Xbox
controller, so one pad would appear twice and take two player ports. If you
launch the game through Steam, turn Steam Input off for it.

This package contains source and setup tools. It includes no prebuilt game
runner, game files or generated game code. These are produced locally from
your ISO. Keep the folder in place after setup. Saves and logs remain under
private/. Default game volume is 100%.

KNOWN ISSUES
- Shore textures can blink on the island, and the cut to a goal celebration
  can flash black (#159).
- A second Hopping Game can be playable before loading finishes (#54).
- An upside-down map can flash when leaving the pool (#55).
- Some underwater beach textures may be missing (#56).
- PlayStation controllers have not yet been confirmed on physical hardware.

LICENSES AND SOURCE
See LICENSE, NOTICE and BUNDLED-TOOLS.md for third-party licenses, versions
and source access. No game assets or game-related rights are granted by this
package.

REPORT ISSUES
https://github.com/NoRain211/doaxbv-re/issues
Include reproduction steps, hardware/controller details and relevant errors.
Remove private paths. Never upload ISOs, extracted game files, generated game
code, saves or BIOS. Build identities appear in build-receipt.json.

AI ASSISTANCE
This project uses AI coding agents extensively. Their output can contain
mistakes; passing tests does not establish complete fidelity.

SUPPORT
https://ko-fi.com/norainsrecomps
