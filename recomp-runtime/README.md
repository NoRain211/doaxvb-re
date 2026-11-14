# Recomp runtime

Configure without private inputs to build and run the public runtime, model,
and adapter tests:

```powershell
cmake -S recomp-runtime -B build/recomp-runtime
cmake --build build/recomp-runtime --config Debug
ctest --test-dir build/recomp-runtime -C Debug --output-on-failure
```

The public test target uses a small hand-written leaf fixture at the same
dispatch seam as an authenticated generated function. It tests guest register,
memory, and dispatch behavior without containing generated game code.

The runtime also includes transactional save recovery, host input handover,
native audio output, movie timing/color conversion and D3D11 texture/render
target support. Use `--vsync` for flip presentation and set
`RECOMP_AUDIO_GAIN=0.2` for 20% game volume. The default audio gain is muted.

To test an authenticated generated function instead, supply its source and
receipt hash:

```powershell
cmake -S recomp-runtime -B build/recomp-runtime `
  -DRECOMP_FUNCTION_SOURCE="<authenticated-extracted-function.c>" `
  -DRECOMP_FUNCTION_SHA256="<receipt-sha256>" `
  -DRECOMP_BOUNDARY_SOURCE="<authenticated-extracted-boundary.c>" `
  -DRECOMP_BOUNDARY_SHA256="<boundary-sha256>"
cmake --build build/recomp-runtime --config Debug
ctest --test-dir build/recomp-runtime -C Debug --output-on-failure
```

Configure rejects a function source that does not match its supplied receipt
SHA-256. Supplying either the source or hash without the other also fails. The
boundary inputs are optional and enable the boundary runtime integration tests;
those tests do not claim original-x86 parity.

The boundary harness is frozen as regression coverage. Do not add new branch
fixtures to `runtime_boundary_test.c`; new recomp work belongs in the runner
or a tracked import/model adapter.

Supply an exact extracted `xbe_entry_point` to build the runner:

```powershell
cmake -S recomp-runtime -B build/recomp-runner `
  -DRECOMP_FUNCTION_SOURCE="<authenticated-extracted-function.c>" `
  -DRECOMP_FUNCTION_SHA256="<receipt-sha256>" `
  -DRECOMP_ENTRY_SOURCE="<authenticated-entry.c>" `
  -DRECOMP_ENTRY_SHA256="<entry-sha256>"
cmake --build build/recomp-runner --config Debug
build/recomp-runner/Debug/recomp_runner.exe --xbe "<private-xbe>"
```

The first runner slice loads the XBE into program-owned 64 MiB guest RAM,
dispatches its authenticated entry point, records the first import request,
and stops at the unimplemented guest thread start. It does not run the native
host or claim boot, kernel, or game-output parity.

## Whole-program runner

Build the complete authenticated snapshot with its tree-manifest identity:

```powershell
cmake -S recomp-runtime -B build/recomp-program `
  -DRECOMP_FUNCTION_SOURCE="<authenticated-extracted-function.c>" `
  -DRECOMP_FUNCTION_SHA256="<leaf-receipt-sha256>" `
  -DRECOMP_PROGRAM_DIR="<authenticated-generated-snapshot>" `
  -DRECOMP_PROGRAM_MANIFEST_SHA256="<snapshot-manifest-sha256>"
cmake --build build/recomp-program --config Debug --target recomp_program_runner
build/recomp-program/Debug/recomp_program_runner.exe --xbe "<private-xbe>"
```

### Asserting where a run stops

A run reports success through its exit code rather than through a human
reading the log:

```powershell
build/recomp-program/Debug/recomp_program_runner.exe --xbe "<private-xbe>" `
  --expect-stop "memory:0xffffffdc" --milestone-log "<local-milestone-log>"
```

Exit 0 only when the stop matches, 3 on any other stop, so a run that halts
somewhere plausible-looking still fails. Identifiers are matched by prefix, so
`--expect-stop import:` accepts any unimplemented-import stop and
`--expect-stop import:NtOpenFile` accepts exactly one. The identifiers are
listed in `stop_report.h`. Omitting `--expect-stop` preserves the historical
exit codes.

`--milestone-log` appends one tab-separated line per run with the stop, the
expectation, the result, and the number of kernel adapter calls serviced —
a coarse forward-progress measure that makes a regression a diff.

Note that `HalReturnToFirmware` exits 2, not 0. It is reached through
`XapiBootToDash`, the startup error boundary, so treating it as a clean halt
once made a failed run look like a success.

This target compiles all 16 generated chunks and `recomp_dispatch.c`. At
configure time it emits fail-loud stubs for referenced `sub_*` symbols whose
bodies are absent from the authenticated snapshot. Reaching one exits with
code 2 and reports the guest address. The stubs make snapshot coverage gaps
observable; they do not implement or validate the missing guest code.

Configure authenticates the snapshot manifest, then derives private build
copies that initialize every `uint32_t ebp;` prologue local from `g_seh_ebp`.
The lifter leaves that local undefined for `push ebp; mov ebp, esp` frames.
The count depends on the locally generated snapshot, so configure fails closed
with the applied count and the expected value must be overridden when a
receipt-derived count differs. A locally reproduced snapshot validated 11,052
replacements; the default matches that receipt. The private snapshot is never
modified.

Two diagnostics keep host stops self-describing. `host_diagnostics.cpp` routes
MSVC runtime-check failures to stderr and names the generated function from a
symbolized backtrace, instead of letting `_CrtDbgBreak` surface as a bare
`STATUS_BREAKPOINT`. Runtime checks stay enabled. A generated `__debugbreak`
reports its member, line, enclosing `sub_`, nearest `loc_` label, guest ESP and
EBP, and the last dispatched guest address, then exits 2.

Whole-program lookup checks tracked manual adapters, generated dispatch, then
kernel adapters. The initial startup set contains the Ghidra-recovered
`0x0018322D` thread entry and synchronous ordinal 255
`PsCreateSystemThreadEx`. Its synchronization subset owns guest-visible RTL
critical-section fields and handles ordinal 258 thread termination. Contention
stops the runner. This is a single-thread bring-up policy, not an Xbox
scheduler model.

## Reading stop and crash logs

Every controlled stop writes `event=runtime-stop` to stderr and, when enabled,
the milestone log. `kind` is `normal-exit`, `diagnostic-boundary`, or
`runtime-error`. `fallback_code` preserves the original status; `exit_code`
records the actual process status after `--expect-stop`. The separate `result`
field is `unchecked`, `match`, or `mismatch`. A matched runtime error can exit
zero; a normal user close can exit three for an expectation mismatch. Neither
an expectation match nor exit zero alone proves successful gameplay.

Windows unhandled exceptions write `event=host-crash kind=unhandled-exception`
in Release and Debug, with the exception code and address. A stack overflow
writes `kind=stack-overflow`, `abort()` (also reached by `std::terminate` and
pure virtual calls) writes `kind=abort`, and an invalid CRT argument writes
`kind=invalid-crt-parameter` before the process ends. Expected-stop rules
do not apply to these crashes. Debug `event=exception-observed terminal=unknown`
means an exception was observed before its final handling decision; a subsequent
handler may recover. Debug `event=runtime-check terminal=false` reports a CRT
check while execution continues. Do not classify either diagnostic as a crash
without a terminal event or external evidence of process termination.

These are local stderr logs, not automatic uploads. Forced termination, other
fail-fast paths, and failures that bypass these handlers may leave no terminal
log; absence of a crash event is not proof of a clean exit.
Bug reports should include the exact build identity and configuration, stop
line, process exit status and reproduction steps. Review logs for private
paths or game-derived data before sharing them.
