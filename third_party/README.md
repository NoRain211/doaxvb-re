# Third-Party Tools

Downloaded public tooling can live here locally. Everything except this README,
the dependency submodules and the copied FSR headers below is ignored.

## Renderer

- [SMAA](https://github.com/iryoku/smaa): the `RECOMP_D3D_SMAA` post-process.
  MIT licensed and optional; without it the runtime builds without SMAA.

```sh
git submodule update --init third_party/smaa
```

- [FidelityFX FSR 1.0](https://github.com/GPUOpen-Effects/FidelityFX-FSR): the
  `RECOMP_D3D_UPSCALE=fsr` window upscale. MIT licensed. `third_party/fsr1`
  holds `ffx_a.h`, `ffx_fsr1.h` and `license.txt` unmodified from upstream
  v1.0.2 (commit `a21ffb8`). They are copied because the upstream repository
  nests about 1.4 GB of sample media as submodules, which a recursive clone
  would download.

## Rollback dependencies

- [rbengine](https://github.com/RetroPortingToolKit/rbengine): host pacing,
  input history, hash confirmation, and opaque snapshot storage.
- [recomp-net](https://github.com/RetroPortingToolKit/recomp-net): sessions,
  transport, input correction, and rollback episode state.

Both are MIT licensed and pinned by their gitlinks. Initialize the recorded
versions from the repository root:

```sh
git submodule update --init third_party/rbengine third_party/recomp-net
```

They are selected dependencies, not yet linked into the native game runtime.
The recomp-net gitlink includes the seal repair in commit `694e493`.
That commit is published in [our fork](https://github.com/NoRain211/recomp-net/commit/694e493460740ceb7f6e0171bad6ca484c57f631); the
[source patch](../tools/recomp-net-patches/README.md) preserves the repair
against its published base. Do not reapply it to the repaired commit.
See the [engine rollback audit](../docs/research/rollback-engine-audit.md) for
the tested pins, repaired failures, and required DOAXBV integration work.

Prefer recording the upstream URL, release/tag, and install notes in
`docs/tooling.md` instead of committing downloaded archives.
