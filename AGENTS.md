# Project Agent Rules

## Goal

Produce readable, hand-written source for a native PC port of DOAXBV. The
whole-program recomp is a temporary scaffold: generated game code runs on a
hand-written runtime with native kernel, input, audio, and D3D8 replacements.

The 0.4 Beta runner is fully playable. Current work fixes the remaining known
issues, replaces game-owned generated functions in coherent clusters, and
modernizes presentation. `docs/public-status.md` states what the public tree
and local builds currently prove. `docs/building.md` defines the public and
authenticated local build routes.

## Architecture

The tracked runtime under `recomp-runtime/` owns guest memory, registers,
dispatch, kernel adapters, device models, D3D8 replacements, and host
presentation. The pinned `tools/xboxrecomp` submodule lifts a user-owned XBE
into an ignored local directory.

D3D8 is statically linked into the game image. Replace it at the API level
through `recomp_lookup_manual()`. Keep device models callable as plain
functions, independent of interception. Do not build an NV2A emulator beneath
the game.

Host presentation is D3D11 on a render worker thread. A new backend, such as
D3D12 ray tracing or Vulkan, must leave D3D11 working as the fallback.

## Decomp loop

1. Select a game-owned function that already runs.
2. Read its local generated form beside bounded static analysis.
3. Write clear source with real names, types, and control flow.
4. Register it in `recomp_lookup_manual()` so it wins over generated dispatch.
5. Run the same bounded gate and retain the change only when behavior holds.

Work in data or call clusters. Generated neighbors still depend on fixed guest
addresses and layouts, so redesign a structure only after replacing every
function that owns it.

## Hard rules

1. Never hand-edit generated C. Fix `xboxrecomp` and regenerate locally.
2. Never commit an XBE, generated game C, retail instruction bytes, extracted
   assets or filenames, saves, BIOS data, private paths, or run evidence.
3. Keep private inputs and output under `private/`; only
   `private/README.md` is public.
4. Report observed behavior. Forced state is a hypothesis, not a result.
5. Keep models separable from interception and host delivery details.
6. Replace library code such as D3D8, XAPI, CRT, and middleware wholesale;
   decompile only game-owned logic. Until a library is replaced, an adapter may
   wrap or call its generated functions at a documented seam. Record each seam
   in a comment beside the adapter's manual lookup and in the feature's `docs/`
   page, and treat wholesale replacement as open work.
7. Base every public branch on `origin/main`. A local branch that does not
   contain the public root commit `ef3bc2e` carries private history: never
   push it or merge it into a public branch. Port the change onto `main`.

## Progress and verification

A runtime iteration is bounded when its observable completes in under a
minute. It is forward when the program reaches a later natural event. A test,
receipt, refactor, or crash-free run alone is not gameplay progress.

Public-only changes must configure without private inputs. Before pushing a
public change, run the `public-ci` checks. The `tools` tests need the
`tools/xboxrecomp` submodule and Capstone 5.0.9. Players build with llvm-mingw
clang, so run the runtime tests with both MSVC and the clang in
`tools/llvm-mingw` (setup downloads it).

```powershell
python -m unittest discover -s tools -p "test_*.py"
python tools/public_export.py verify --require-public-tree
cmake -S recomp-runtime -B build/recomp-runtime
cmake --build build/recomp-runtime --config Debug
ctest --test-dir build/recomp-runtime -C Debug --output-on-failure
$env:PATH = "$PWD\tools\llvm-mingw\bin;$env:PATH"
cmake -S recomp-runtime -B build/clang -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/clang --parallel 4
ctest --test-dir build/clang --output-on-failure
```

Changes tested with local generated code must also preserve the authenticated
SHA and manifest failures and report the observed frontier.

The user's play test of a named build accepts gameplay, rendering, and audio
fixes. An agent smoke run shows only that the build starts and presents
frames; report it that way.

After two attempts stop before the same required event, stop varying runtime
runs. Compare both receipts, identify the earliest divergence, state one
falsifiable mechanism, and test the smallest safe change.

## Lifter

Builds use only the `tools/xboxrecomp` submodule, pinned to a commit on the
`codex/doaxbv-recipe` branch of `NoRain211/xboxrecomp`. Other local lifter
checkouts do not feed the build. Lifter fixes are commits on that branch.

Move the pin only with `tools/update_lifter_pin.py`. It regenerates the old
and new programs, updates the recipe, `LIFTER_REVISION` and the submodule
together, and lists the changed game functions. Play-test the new program
before committing.

Take `sp00nznet/xboxrecomp` changes one fix at a time when a game bug calls
for it. Moving the pin onto the upstream line in September 2026 broke player
animation, a texture and hotel routing. Offer general fixes upstream as
focused pull requests. Do not vendor `xboxrecomp` source or generated output
into this repository.

## Tool routing

- Static addresses, xrefs, or function bounds: use one bounded Ghidra query and
  record the binary identity.
- Real kernel or hardware behavior: use xemu as a live oracle.
- XDK or NV2A semantics: consult public Cxbx, nxdk, or public headers, then
  implement independently.

## Branches, pull requests, and releases

- When reporting work, say where each change lives: a merged PR, a pushed
  branch, or an uncommitted worktree.
- Keep extra worktrees under `private/` or the Codex worktree directory.
  Remove a branch or worktree only after its tip is reachable from
  `origin/main` or a pushed branch and its uncommitted changes are accounted
  for.
- Before merging, address valid review comments and wait for `public-ci` to
  pass. Recent PRs land as GitHub merge commits.
- Release notes and `CHANGELOG.md` name each real fix with its issue or PR.
  Update `docs/public-status.md` with what the release proves.
- The `readme-media` release hosts the README images. Keep it.

## Agent skills

### Issue tracker

Issues, specs, and Wayfinder maps live in GitHub Issues for
`NoRain211/doaxbv-re`. See `docs/agents/issue-tracker.md`.

### Triage labels

Use `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human`, and
`wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

This is a single-context repository: use root `CONTEXT.md` and, when present,
system-wide ADRs under `docs/adr/`. See `docs/agents/domain.md`.

## Governance

Changes to this file must keep the custody rules at least as strict. Propose
any self-initiated governance change before editing it.
