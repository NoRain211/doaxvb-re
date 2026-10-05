# Recomp launchers and setup flows

Research date: 2026-10-05. Sources are first-party repositories, docs, code, and committed screenshots pinned to exact revisions below. I did not run the projects; implementation notes are based on code/docs, and visual notes are limited to screenshots I inspected. This is UX research, not a recommendation to copy game assets or marks.

## Comparison

| Project | Launcher pattern | Useful for DOAXBV |
|---|---|---|
| [Project8Recomp](https://github.com/theokyr/Project8Recomp/tree/c8c21e2781acd134de6b2ccf12b666145e00ea6c) — Tony Hawk’s Project 8 | Single-game desktop launcher, RmlUi markup, first-run setup and ready home. | Verify chosen input before copying; show distinct errors; skip setup on later launches. ([setup](https://github.com/theokyr/Project8Recomp/blob/c8c21e2781acd134de6b2ccf12b666145e00ea6c/docs/SETUP.md), [first-run page](https://github.com/theokyr/Project8Recomp/blob/c8c21e2781acd134de6b2ccf12b666145e00ea6c/src/launcher/assets/ui/first_run.rml)) |
| [Pinyon Shift](https://github.com/arcanite24/pinyon-shift/tree/c6b1f7ebab7c887f5ab147ae90b8eee02ccc577a) — Forza Horizon | Separate Windows WPF shell to validate user input and build locally. | Clear ready state, actionable progress/errors, brief settings summaries. ([README](https://github.com/arcanite24/pinyon-shift/blob/c6b1f7ebab7c887f5ab147ae90b8eee02ccc577a/README.md), [WPF project](https://github.com/arcanite24/pinyon-shift/blob/c6b1f7ebab7c887f5ab147ae90b8eee02ccc577a/launcher/PinyonShift.Launcher/PinyonShift.Launcher.csproj)) |
| [recomp-ui](https://github.com/RetroPortingToolKit/recomp-ui/tree/d3151bf5f4fbb3e63faf7e204ab2937d16fe5324) — shared frontend | Dear ImGui launcher and optional in-game menu with a plain-C host contract. | Keep UI state separate from feature behavior; make defaults and persistence explicit. ([README](https://github.com/RetroPortingToolKit/recomp-ui/blob/d3151bf5f4fbb3e63faf7e204ab2937d16fe5324/README.md), [API](https://github.com/RetroPortingToolKit/recomp-ui/blob/d3151bf5f4fbb3e63faf7e204ab2937d16fe5324/src/recomp_launcher.h)) |
| [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp/tree/b65c482ed672258eb684ab94a4e8c8aa662615ee) — Majora’s Mask | RmlUi main-menu import plus extensive in-game settings. | Consume one selected game image directly when possible; group settings by user intent. ([README](https://github.com/Zelda64Recomp/Zelda64Recomp/blob/b65c482ed672258eb684ab94a4e8c8aa662615ee/README.md), [launcher markup](https://github.com/Zelda64Recomp/Zelda64Recomp/blob/b65c482ed672258eb684ab94a4e8c8aa662615ee/assets/launcher.rml)) |
| [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp/tree/cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c) — Sonic Unleashed | In-app install wizard plus controller-friendly in-game options; no separate desktop shell. | Guide first-run content setup and keep later settings easy to find. ([README](https://github.com/hedge-dev/UnleashedRecomp/blob/cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c/README.md), [wizard source](https://github.com/hedge-dev/UnleashedRecomp/blob/cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c/UnleashedRecomp/ui/installer_wizard.cpp)) |

## Confirmed behavior and observed visuals

**Project8Recomp:** First run asks for an ISO and hashes the executable inside it before copying any data. A mismatch is reported as one of three cases: not a disc image, a different game, or the correct game from an unsupported release. After setup, the ordinary entry goes to Play; the optional --gui argument opens the full home screen. The docs explain that checking first avoids copying several gigabytes for a bad input. ([setup guide](https://github.com/theokyr/Project8Recomp/blob/c8c21e2781acd134de6b2ccf12b666145e00ea6c/docs/SETUP.md), [README](https://github.com/theokyr/Project8Recomp/blob/c8c21e2781acd134de6b2ccf12b666145e00ea6c/README.md))

Its Display page leaves every value at “Game default” until the user changes it, saves on change, and says the setting applies on the next Play. The setup guide documents gamepad controls: stick/D-pad moves, A activates, B backs out, bumpers move among controls. The repository identifies RmlUi as a menu dependency; first-run and home screens are RML documents styled by a shared RCSS file. ([settings/input](https://github.com/theokyr/Project8Recomp/blob/c8c21e2781acd134de6b2ccf12b666145e00ea6c/docs/SETUP.md), [RML](https://github.com/theokyr/Project8Recomp/blob/c8c21e2781acd134de6b2ccf12b666145e00ea6c/src/launcher/assets/ui/first_run.rml), [dependencies](https://github.com/theokyr/Project8Recomp/blob/c8c21e2781acd134de6b2ccf12b666145e00ea6c/README.md))

I inspected its committed screenshots: first-run is a single setup card with one primary “Choose disc image” action and a “Not now” path; the ready home shows a “Ready to play” status, Play, Display, save-folder, and About actions. The Display screenshot pairs each control with “Game default” and explains when changes take effect. ([first-run screenshot](https://github.com/theokyr/Project8Recomp/blob/c8c21e2781acd134de6b2ccf12b666145e00ea6c/docs/images/launcher-first-run.png), [home](https://github.com/theokyr/Project8Recomp/blob/c8c21e2781acd134de6b2ccf12b666145e00ea6c/docs/images/launcher-home.png), [display](https://github.com/theokyr/Project8Recomp/blob/c8c21e2781acd134de6b2ccf12b666145e00ea6c/docs/images/launcher-display.png))

**Pinyon Shift:** This is the closest native Windows shell comparison. The launcher project targets .NET 8 Windows with WPF. Its first-run flow accepts drag/drop or file selection, confirms ownership, validates the ISO, installs build tools, then extracts, recompiles, and builds locally. The README says this can take 20–60 minutes and needs about 25 GB free. For setup failures it reports the failed step and exit code, the first underlying compiler/CMake/copy error, a hint, structured report, and full logs. It also documents a selectable install root and a portable mode. ([WPF project](https://github.com/arcanite24/pinyon-shift/blob/c6b1f7ebab7c887f5ab147ae90b8eee02ccc577a/launcher/PinyonShift.Launcher/PinyonShift.Launcher.csproj), [README](https://github.com/arcanite24/pinyon-shift/blob/c6b1f7ebab7c887f5ab147ae90b8eee02ccc577a/README.md))

Its launcher settings are deliberately few: graphics API, internal resolution, output scaling, and Treasure Map. The README says it explains render/output dimensions; most settings are in-game. In-launcher updating is listed as a future item at this revision. In inspected screenshots, the ready screen foregrounds “Ready to drive,” a configuration summary, Play and Settings, with saves/logs/report/GitHub links in the footer. The settings view shows three labeled graphics controls, the resulting resolution, and Save/Cancel/reset/backup actions. ([README](https://github.com/arcanite24/pinyon-shift/blob/c6b1f7ebab7c887f5ab147ae90b8eee02ccc577a/README.md), [ready screenshot](https://github.com/arcanite24/pinyon-shift/blob/c6b1f7ebab7c887f5ab147ae90b8eee02ccc577a/.github/launcher-ready.png), [settings screenshot](https://github.com/arcanite24/pinyon-shift/blob/c6b1f7ebab7c887f5ab147ae90b8eee02ccc577a/.github/launcher-settings.png))

**recomp-ui:** This is an architecture reference, not a recommendation to adopt the framework. Its README describes an ImGui launcher assembled from system profiles. A plain-C API supplies game facts, settings, defaults, config location, and optional verification callbacks. The UI owns navigation and presentation; hosts define what settings do. The documented persistence behavior includes explaining when values save, preserving unrelated INI content, and a confirmed Restore Defaults action when the host provides defaults. The README self-reports Mega Man X and Ape Escape integrations; I did not inspect those consumers, so I treat that adoption claim as maintainer-reported, not independent proof for DOAXBV. ([README](https://github.com/RetroPortingToolKit/recomp-ui/blob/d3151bf5f4fbb3e63faf7e204ab2937d16fe5324/README.md), [API](https://github.com/RetroPortingToolKit/recomp-ui/blob/d3151bf5f4fbb3e63faf7e204ab2937d16fe5324/src/recomp_launcher.h))

**Integrated-menu comparisons:** Zelda64Recomp says the player selects the supported North American game in the main menu; the program loads its assets directly without a separate extraction step. Its gameplay, graphics, input, audio, and mod settings are available in a menu that supports mouse, keyboard, or controller. ([README](https://github.com/Zelda64Recomp/Zelda64Recomp/blob/b65c482ed672258eb684ab94a4e8c8aa662615ee/README.md))

UnleashedRecomp guides users through providing game files and a title update, runs integrity checks, and can reopen the installer from the title screen to add DLC. It distinguishes Add Files from Add Folder and documents what each accepts. Its options menu opens from title or pause and has full controller navigation. Configuration lives in the user config directory; updates are manual file replacement that retains configuration and saves. These are in-game UI precedents, not proof of a standalone launcher. ([README and install flow](https://github.com/hedge-dev/UnleashedRecomp/blob/cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c/README.md), [installer source](https://github.com/hedge-dev/UnleashedRecomp/blob/cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c/UnleashedRecomp/ui/installer_wizard.cpp))

## Implications for DOAXBV (inference)

Based on these examples and the supplied description of DOAXBV’s current launcher/runner:

- Give first run and repeat launches distinct screens. First run should state what is missing and offer one next action. A ready home should foreground Play, show a compact settings summary, and link to Settings, Help, and logs.
- Gate Play on checks the UI can actually observe. Use truthful states such as ready, missing input, verification failed, or runtime unavailable. Show “verified” only after a real preflight/receipt check.
- Group current controls into Graphics, Audio, Textures, and Experimental, with Home and Help for launching and troubleshooting. Add a Controls page only when the launcher can configure supported bindings. Explain specialist rendering features in a short help sentence and say why an unavailable option is disabled.
- Make persistence clear. Project8Recomp saves on change and explains “next Play”; Pinyon’s screenshot shows Save/Cancel; recomp-ui documents which exit paths save. For DOAXBV, make Save/Apply independent from Play and say whether changes take effect now or next launch.
- Show the actual failed check or runner stage and a direct log action instead of a generic launch error. Keep the runner authoritative for authenticated input checks and reflect its result in the UI.
- Use the screenshots for hierarchy and spacing only. Build DOAXBV’s own identity; do not reuse logos, characters, cover art, fonts, or other copyrighted game assets.

## Native desktop recommendation

The user selected a polished native Windows desktop launcher. The current
`tools/launcher.ps1` uses WinForms, a fixed-size form, and absolute control
positions. It does not select a UI font or call `Application.EnableVisualStyles`.
Settings are saved only after Play; closing the form discards edits. The form
does not inspect the selected build receipt, show setup progress, or monitor a
running game. Authentication happens afterward in `tools/run_game.py`.

Start with the existing WinForms shell. Enable operating-system visual styles
before constructing controls, use a consistent Segoe UI font, and replace
fixed coordinates with layout panels and sensible minimum sizes.
Microsoft documents [visual styles](https://learn.microsoft.com/en-us/dotnet/api/system.windows.forms.application.enablevisualstyles)
and [TableLayoutPanel](https://learn.microsoft.com/en-us/dotnet/desktop/winforms/controls/tablelayoutpanel-control-overview)
for themed controls and layouts that respond to resizing and text length.
This is an implementation recommendation; no launcher code was changed by
this research.

Use two distinct experiences: first-run preparation and everyday launch.
Preparation should show DOAXBV's real extraction, generation, and compilation
phases, with a direct route to the failed stage's log. Its current custody
policy requires game-derived content to be prepared locally; Zelda's simpler
asset-loading workflow does not remove that requirement. Keep existing
authenticated checks authoritative and use their actual results for readiness.
See [the build route](../../building.md) and
[the host-shell boundary](host-shell-redistribution.md).

For everyday use, prefer a Home page with one Play action, a small selected
build/settings summary, and truthful ready/running/error states. Put controls
on the four settings pages above. Save independently of Play, offer Restore
Defaults, and label changes as applying on the next launch. Keep fog on and
new rendering features opt-in. Describe unsupported options using an actual
capability probe or known build metadata; do not infer DXR support from the
GPU vendor. Microsoft's [navigation guidance](https://learn.microsoft.com/en-us/windows/apps/design/basics/navigation-basics)
supports a shallow, familiar hierarchy, and its
[settings guidance](https://learn.microsoft.com/en-us/windows/apps/design/app-settings/guidelines-for-app-settings)
supports grouping related options and revealing dependent settings on demand.

Test keyboard focus, text scaling, mixed-DPI monitors, long folder names,
missing/corrupt receipts, setup failure, and a second launch while running.
High-DPI behavior needs specific testing in both PowerShell hosts: enhanced
.NET Framework WinForms scaling is opt-in and depends on application
configuration. Do not promise that a newer .NET-only API fixes the
PowerShell 5 host. See [WinForms high-DPI support](https://learn.microsoft.com/en-us/dotnet/desktop/winforms/high-dpi-support-in-windows-forms?view=netframeworkdesktop-4.8).

WPF remains a reasonable later option for a larger native shell, as Pinyon
demonstrates. Framework migration is not necessary to fix the current
hierarchy and launch flow. WinUI 3 would introduce a separate deployment
decision: framework-dependent unpackaged Windows App SDK applications must
deploy and initialize its runtime. See
[Microsoft's deployment guide](https://learn.microsoft.com/en-us/windows/apps/windows-app-sdk/deploy-unpackaged-apps).

No PRODUCT.md or DESIGN.md currently defines the launcher. After choosing a
direction, `$impeccable init` can record its design rules. The accompanying
Launch first and Compact tabs concepts are interaction previews with example
state, not screenshots of a working replacement launcher.

## Exact source revisions

Commit author dates checked on 2026-10-05: Project8Recomp c8c21e2781acd134de6b2ccf12b666145e00ea6c (2026-08-24); Pinyon Shift c6b1f7ebab7c887f5ab147ae90b8eee02ccc577a (2026-10-03); recomp-ui d3151bf5f4fbb3e63faf7e204ab2937d16fe5324 (2026-10-05); Zelda64Recomp b65c482ed672258eb684ab94a4e8c8aa662615ee (2026-09-25); UnleashedRecomp cf829a9eca8fb680fba4b0409ddeb6ca92f22e3c (2026-06-29).
