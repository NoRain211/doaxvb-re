# VRR frame pacing for a fixed 60 Hz game

Status: researched on 2026-09-30. API contracts are confirmed from primary
sources; recommendations are deductions, and unresolved behavior is marked
**unverified**. This is source research, not a new runtime test. The scope is
a windowed D3D11 presenter on Windows 10/11, a GeForce RTX-class GPU, and a
high-refresh VRR display. No private run evidence is included.

## Decision

Keep the guest's 60 Hz clock and the existing fixed-refresh fallback. Retain
`Present(0, DXGI_PRESENT_ALLOW_TEARING)` as the opt-in VRR policy while measuring
display-change times. It is Microsoft's documented windowed VRR route;
changing to `Present(1, 0)` should be a separate NVIDIA-specific comparison,
not an assumed correction. A latency waitable object controls queueing and
does not replace the game's clock. These are recommendations based on
[Microsoft's VRR contract][vrr] and [waitable-chain guidance][latency].

**A refresh-count delta of 2 does not establish a 33.3 ms game-frame hold.**
The counters count vblanks, not milliseconds. Neither the DXGI contract nor
the supplied histogram identifies which NVIDIA refreshes were counted.
Ranking LFC, delayed display, and counter/reporting behavior by likelihood is
**unverified**. Measure the interval between displayed presents to assess
the pacing changes below. [DXGI_FRAME_STATISTICS][stats]

## What the counters establish

| Field | Documented meaning |
| --- | --- |
| `PresentCount` | Reported presentation count; it need not equal the number of `Present` calls. The flip-model guide also describes it as the reported present ID. [Structure][stats], [present-ID correlation][flip] |
| `PresentRefreshCount` | Vblank counter at the presentation of the last reported image; windowed counting is relative to swap-chain creation. [Structure][stats] |
| `SyncRefreshCount` | Vblank counter when the scheduler sampled the performance counter. [Structure][stats] |
| `SyncQPCTime` | The accompanying QPC clock sample, not a promised timestamp of the image's first visible pixel. `SyncGPUTime` is reserved and zero. [Structure][stats] |

Microsoft's flip-model guide places `SyncRefreshCount` at the submission
vblank and describes its QPC time as approximate. Consequently, subtracting
successive `SyncQPCTime` values is not automatically a measurement of
successive display changes. Likewise, extrapolating display time from
`PresentRefreshCount - SyncRefreshCount` using a constant refresh period is
not justified for variable-duration refreshes. This is a deduction from the
[documented synchronization relationship][flip].

Correlate by present ID rather than assuming a query after `Present` describes
that same call. The inspected histogram accepts only consecutive reported
IDs; stalled queries and jumps therefore disappear from its percentages.
Report accepted, stale, skipped, and failed sample counts alongside the
histogram. Sources: [DXGI correlation procedure][flip],
[`submitPresent` filtering][local-presenter].

LFC can repeat an image to keep physical refresh within a display's supported
range. AMD's first-party explanation gives a 40 fps / 80 Hz example: two
refreshes still represent one 25 ms content interval. This confirms the
mechanism, **not NVIDIA's threshold, hysteresis, or its DXGI counter mapping**;
those remain **unverified** here. In particular, a 240 Hz maximum does not
identify the panel's lower VRR limit. [AMD's LFC and range explanation][lfc]

For this investigation, distinguish these hypotheses:

| Candidate explanation | Evidence that would distinguish it |
| --- | --- |
| Extra refreshes without delayed content | Displayed present IDs continue at approximately 16.667 ms while the vblank delta changes. This excludes a doubled content interval in the trace; it does not alone prove LFC. [PresentMon screen-time tracking][pm-source] |
| Actual late or repeated game image | The previous image remains displayed substantially longer, for example approximately 33.3 ms, or a submitted present is dropped. Compare CPU-present and GPU-ready timing to locate the delay. [PresentMon legacy metrics][pm-v1] |
| Fallback to fixed-refresh presentation | Display changes become quantized to fixed refresh periods; a presentation-mode transition supplies additional evidence. Mode alone does not identify VRR activation. [PresentMon event model][pm-events] |
| Stale or discontinuous statistics | DXGI repeats an old present ID, skips observations, or reports a disjoint sequence while the independent display trace advances. Multi-monitor unreliability is documented. [GetFrameStatistics][get-stats] |

These are proposed discriminating tests, not diagnoses. A genuine panel
repeat and a counting artifact can both leave content cadence unchanged.
PresentMon derives screen times from OS events; it is not an instrument
inside the panel. Proving the exact physical repeat mechanism needs additional
display-side evidence. [PresentMon trace implementation][pm-source]

## Windowed G-SYNC, independent flip, and MPO

These are related but separate states. NVIDIA documents enabling G-SYNC for
windowed applications and for the selected display, selecting G-SYNC monitor
technology, and configuring the capable display as primary. Its indicator is
the documented way to check activation. Independent-flip capability or
tearing support alone is not that activation check. [NVIDIA setup][nv-setup],
[NVIDIA indicator][nv-indicator], [hardware-composition query][hw-composition]

| Condition | What the primary sources support |
| --- | --- |
| Flip-model setup | At least two buffers; a single-sampled swap chain, with MSAA resolved separately; rebind the D3D11 back buffer after presenting. Avoid mixing GDI or other presentation APIs into the same HWND. [Flip-model requirements][flip-blog], [HWND restrictions][flip] |
| DirectFlip | Matching screen-sized buffers and a client area covering the screen can bypass normal composition. Matching only the client size is insufficient for this particular route. [DirectFlip scenarios][flip-blog] |
| Hardware scaling | Panel fitters can scale a screen-covering window's buffers within hardware limits. Therefore, unequal buffer and window sizes do not universally rule out independent flip. [DirectFlip scenarios][flip-blog] |
| MPO | A hardware overlay plane can scan out a subregion, with hardware-dependent scaling. MPO provides a route for smaller windows; allocation is conditional. [DirectFlip scenarios][flip-blog] |
| Overlays and DWM | Other content can trigger composition, reverse composition, or continued independent flip through MPO. It is incorrect to claim every overlay always disables independent flip. [DWM transitions][flip-blog] |
| Capability versus current state | `CheckHardwareCompositionSupport` reports support flags, not a guarantee that the current window has a plane. Observe actual presentation mode separately. [API contract][hw-composition], [PresentMon event model][pm-events] |

Recommendation: keep application downsampling into a client-sized chain,
avoiding a hardware scaling dependency. Independent flip remains
**unverified until measured**.
[Microsoft's rendering/output-resolution guidance][flip-blog]

DWM remains part of modern Windows; programmatically disabling composition
has had no effect since Windows 8. Older NVIDIA help text mentioning disabled
DWM must not be treated as Windows 10/11 setup instructions.
[DwmEnableComposition][dwm-enable], [NVIDIA legacy wording][nv-overview]

Windows' windowed-game optimization upgrades legacy blt presentation to flip.
It does not supply a missing flip model to this already flip-model design;
toggling it is not a demonstrated fix for this problem. The latter is a
deduction from [Microsoft's description of the optimization][windowed].

### Focus and scripted launch

`SetForegroundWindow` is restricted by foreground ownership, input history,
active menus, and other conditions; Windows can deny it even when the listed
conditions appear satisfied. Its return value and the actual foreground HWND
must be checked. An Alt key action is not a documented G-SYNC activation
protocol. [SetForegroundWindow][foreground]

**Unverified:** NVIDIA's exact foreground/activation heuristic, whether it
distinguishes synthetic input, and why a particular scripted launch differs
from a manual one. A lost foreground transition or overlapping launcher
window is a testable hypothesis, not a confirmed driver rule. Log foreground
ownership and presentation-mode changes instead of inferring activation from
a successful focus request. [Foreground contract][foreground],
[NVIDIA activation check][nv-indicator]

There is also a source-level timing detail worth recording: `submitPresent`
shows the window after the first successful `Present`. A launcher acting
earlier may observe a different startup state. Its effect on G-SYNC is
**unverified**. Source: [`submitPresent`][local-presenter].

### Secondary-monitor statistics

Microsoft explicitly warns that `GetFrameStatistics` is unreliable in many
multi-monitor configurations and when other fullscreen applications run.
It also says Hardware Flip Queue systems may require `DwmFlush` before
statistics update. This provides documented candidates for stale results;
it does not diagnose a particular stuck counter. [GetFrameStatistics][get-stats]

Hardware Flip Queue can defer CPU notifications while the display controller
processes queued frames. Its presence on the investigated system is
**unverified**. [Windows DDK description][hw-queue]

Use `DwmFlush` only in a separate diagnostic check for stale statistics: it
blocks for the calling application's outstanding display updates, so adding
it to every frame changes the experiment. Keep the window entirely on one
output; DXGI synchronizes a straddling window to the output containing its
largest client-area portion. [DwmFlush][dwm-flush], [Present][present]

## Pacing choices

| Policy | Recommendation and limits |
| --- | --- |
| CPU-paced `Present(0, ALLOW_TEARING)` | Keep as the portable windowed VRR candidate. Query tearing support, create with `ALLOW_TEARING`, preserve it on resize, and use sync interval zero. The flags permit the path; they do not guarantee G-SYNC activation or even frame delivery. [VRR requirements][vrr] |
| CPU-paced `Present(1, 0)` | Valid as a separate NVIDIA comparison, retaining the 60 Hz clock. Sync interval one specifies a minimum vblank interval, not a 60 fps cap. Never combine interval one with the present-time tearing flag. Whether G-SYNC stays active with this policy on the target setup is **unverified**. [Present][present], [VRR flag rules][vrr] |
| Frame-latency waitable object | A readiness/back-pressure mechanism to combine with a pacing policy. Wait before rendering each frame, including the first. It does not specify a 16.667 ms deadline. Latency one minimizes queue depth; increasing it can restore CPU/GPU overlap if needed. [Waitable-chain guidance][latency], [wait-handle contract][wait-handle] |
| Fixed-refresh `Present(refresh / 60, 0)` | Keep as fallback for supported integer multiples. At exact 240 Hz, four refresh periods equal 16.667 ms; at 120 Hz, two do. This arithmetic assumes the actual rate and timely delivery. DXGI intervals specify a minimum, not an unconditional exact hold. [Present][present] |

NVIDIA recommends G-SYNC with V-Sync enabled and Reflex or Ultra Low Latency
for a tear-free configuration; its latency guide describes an automatic cap
below the refresh ceiling. That guide specifically directs windowed users to
in-game V-Sync because its control-panel V-Sync override is documented there
as fullscreen-only. Treat this as NVIDIA guidance, not a portable DXGI
contract or proof that `Present(1)` improves this presenter.
[NVIDIA latency guide][nv-latency]

NVIDIA's cap guidance says slightly below the **display maximum**, without
prescribing a universal three-fps margin. A 60 fps game is already far below
a 240 Hz ceiling. Recommendation: do not reduce the guest to 57/59 fps or add
a second 60 fps driver limiter to address an unproven refresh-count problem.
Neither fixes a lower VRR boundary. [NVIDIA Max Frame Rate guidance][nv-cap],
[lower-bound compensation mechanism][lfc]

**Which is most even is unverified.** The decision criterion should be
approximately 16.667 ms between distinct displayed frames, low variation,
and no unexplained drops. Queue depth and input latency are separate criteria.
Microsoft's waitable-object guidance concerns latency; it does not establish
the winner of this application's cadence comparison. [Latency guidance][latency],
[display-duration metrics][pm-console]

## Measuring display cadence

| Method | Useful evidence and limitation |
| --- | --- |
| In-app `GetFrameStatistics` | Present-ID/vblank correlation without an external trace session. Handle failures, disjoint epochs, repeated samples, and skipped IDs. It is not a complete per-present history, and multi-monitor reliability is limited. [Flip statistics][flip], [method caveats][get-stats] |
| `IDXGISwapChainMedia::GetFrameStatisticsMedia` | Query optionally and check both interface and method results. It adds `CompositionMode` and `ApprovedPresentDuration` to the same basic counters; it does not add a per-present physical scanout timestamp. [Media method][media-method], [media structure][media-stats] |
| Media `CompositionMode` | The documented composed/overlay values concern media/decode surfaces and YUV conversion. Treat availability and interpretation for this ordinary RGB chain as **unverified**; it is not a documented general G-SYNC or PresentMon-mode query. [Media enum][media-mode] |
| `DwmGetCompositionTimingInfo` | Composition timing, with `hwnd == NULL` required since Windows 8.1. It cannot select this HWND or a secondary output through that argument. Do not treat its desktop timing as this independent-flip chain's display history. [API][dwm-timing], [independent-flip behavior][flip-blog] |
| Desktop Duplication | `LastPresentTime` timestamps desktop-image updates; pointer-only acquisitions can have zero image-update time and multiple updates can accumulate. Therefore capture timestamps are not a one-to-one trace of this chain's displayed frames. [Duplication frame information][duplication] |

For software measurement, prefer an overlay-free PresentMon console capture.
Filter by process and swap chain. Its event model tracks runtime submission
through kernel/compositor events; independent-flip paths use flip and
VSync/HSync events. Its implementation also uses immediate-flip timestamps.
Thus it observes the display pipeline more directly than CPU `Present`
duration, but cannot promise physical pixel timing or expose every internal
panel repeat. [Event model][pm-events], [implementation][pm-source]

For physical-light confirmation, NVIDIA documents an external luminance-sensor
approach through LDAT. A controlled changing marker is a possible follow-up
when software timing disagrees with visible motion; a working cadence
measurement protocol for this presenter is **unverified**. Optical changes
alone would not identify refreshes that redraw identical pixels. This last
point is a measurement deduction, not a claim about NVIDIA's LFC internals.
[NVIDIA's optical measurement description][ldat]

Read `PresentMode` per frame: `Hardware: Independent Flip` identifies direct
surface flipping without exclusive ownership; `Hardware Composed: Independent
Flip` identifies a hardware overlay plane; `Composed: Flip` identifies DWM
composition. None is a G-SYNC-active flag. [PresentMon mode definitions][pm-console]

Use `msBetweenDisplayChange` in the legacy schema (newer schemas also expose
`MsBetweenDisplayChange`) for elapsed time since the preceding displayed
present. It describes the **previous** frame's duration. Compare
`msBetweenPresents`, `msUntilDisplayed`, `msUntilRenderComplete`, and `Dropped`.
Keep dropped rows: a high submission rate can hide missing displayed frames.
[Legacy CSV definitions][pm-v1]

Newer `DisplayedTime` describes the current frame's displayed duration and
is unavailable for an undisplayed frame. Record the tool version and exact
CSV header; do not silently mix schemas. Retain process/swap-chain identity,
`SyncInterval`, flags, mode, and QPC timing for correlation.
[Current console schema][pm-console]

PresentMon is not infallible: its consumer marks missing-event sequences as
lost, and the output path excludes lost/failed presents. Reject an incomplete
trace rather than calling the remaining distribution smooth. Its README
also cautions about GPU timing accuracy with Hardware-Accelerated GPU
Scheduling. [Consumer][pm-events], [output handling][pm-output],
[measurement limitations][pm-readme]

### Without administrator rights

PresentMon can run without elevation when the account already belongs to
**Performance Log Users**. Adding that membership requires administrative
setup and a new sign-in; it is not a workaround available to an arbitrary
restricted account. Non-elevated capture has process-name visibility limits,
so target the process ID. [PresentMon access requirements][pm-readme],
[Windows trace-session permissions][etw]

An already authorized PresentMon service exposes its measurements to clients,
but that does not make service installation or trace provisioning
unprivileged. With neither trace permission nor an authorized service, use
the in-app queries and label unresolved display cadence **unverified**.
[Service architecture][pm-service], [trace permissions][etw]

Example for a compatible current console build using its legacy CSV schema;
substitute the runner's process ID and an ignored local CSV destination.
This is a proposed command, not an executed capture. `--help` should confirm
the installed version's switches.
[Console options][pm-console]

```powershell
PresentMon.exe --process_id <PID> --v1_metrics --qpc_time --timed 30 --terminate_after_timed --no_console_stats --output_file <LOCAL_CSV>
```

## Implemented changes and remaining recommendations

This branch changes runtime source as well as adding this research note:

- Scaled VRR output is downsampled into a client-sized swap chain.
- Blocking flip presentation creates a waitable chain and requests per-chain
  frame latency one, checking the result. Immediate presentation retains the
  device default. The worker does not wait on a chain readiness handle.
- `RECOMP_D3D_VRR=1` selects flip presentation when tearing is supported,
  including direct launches without `--vsync`.
- Fixed-refresh presentation selects interval two at 120-121 Hz and four at
  240-241 Hz; lower and other rates retain interval one. Monitor refresh is
  rechecked using elapsed time, once per second. [Per-chain latency][chain-latency]
- The VRR worker flushes rendering and waits on its own nominal 60 Hz grid
  before `Present`, in addition to the guest's existing wait. Thread-local
  waitable timers now close on thread exit.
- Optional CSV logging records raw present/statistics counters, query HRESULT,
  and QPC before/after `Present`. The histogram baseline resets on query
  failures. Its label remains `holds`; stale/skipped sample counts and the
  other diagnostics below are still open work.

These changes follow from the [presenter][local-presenter] and
[timer][local-vblank] source. Their effect on displayed cadence is
**unverified**. The following recommendations describe remaining work and
controls for a comparison; this branch is not an unchanged timing baseline.

1. **Fix the diagnostic meaning first, in `submitPresent`.** Rename the
   refresh histogram to `present_refresh_delta`; report it as a count, not
   displayed duration. Preserve raw `PresentCount`, `PresentRefreshCount`,
   `SyncRefreshCount`, `SyncQPCTime`, query HRESULT, and QPC before/after
   `Present`. Record `GetLastPresentCount` after successful submission to
   correlate returned statistics with earlier frames. Reset the comparison
   baseline after disjoint/error or output/chain changes; count stale samples
   and skipped IDs separately. Retain the zero-delta bucket. Source:
   [`submitPresent`][local-presenter]; rationale: [present-ID mapping][flip].
2. **Keep a control build with guest pacing only.** The guest
   wait releases a packet to the worker; `publish` permits up to two pending
   packets including one executing, and the patched worker renders and adds
   its own VRR wait before calling `Present`. Compare it with a control that
   has the same diagnostics but retains the pre-patch timing and output size.
   Therefore a steady guest clock is not a display clock. Record
   the existing guest/queue diagnostics alongside worker-side timestamps.
   The timer uses a nominal integer-nanosecond 60 Hz interval and discards
   timing debt after lateness; it does not guarantee exact arrival times.
   Sources: [guest wait][local-vblank], [swap adapter][local-adapter],
   [packet publication and execution][local-thread].
3. **Add a readiness wait only if queueing is implicated.** The flip chain
   now uses `FRAME_LATENCY_WAITABLE_OBJECT` alongside its tearing flag and
   checks `IDXGISwapChain2::SetMaximumFrameLatency(1)`. Obtain its readiness
   handle to integrate the wait. The device-level latency setting is not the
   control for this chain. Preserve flags across resize; this flag requires
   creation, not later addition through resize. [Swap-chain flags][chain-flags],
   [per-chain latency][chain-latency], [creation guidance][latency]
4. **Place any readiness wait at the actual frame boundary.** For this
   implementation that means before replaying a frame packet in the worker,
   not merely inside `submitPresent` after drawing. Keep message processing
   and shutdown responsive during the wait, and close the handle on teardown.
   Retain guest pacing; do not introduce another unconditional 16.667 ms
   sleep. This is an integration recommendation based on the
   [worker flow][local-thread] and [wait-before-render contract][wait-handle].
5. **Improve state evidence, not activation tricks.** Log foreground changes,
   client/buffer dimensions, selected output, and exact present HRESULT
   including `DXGI_STATUS_OCCLUDED`. Keep the client-sized downsample and
   fixed-refresh fallback. If later comparing `Present(1)`, change only the
   interval/present flag policy and verify NVIDIA activation again. Benefits
   remain **unverified** until that comparison. [Foreground API][foreground],
   [Present results][present], [NVIDIA indicator][nv-indicator]

Implementation source links above are repository-relative source URLs from
the inspected worktree. They establish local behavior; external links
establish API contracts. In particular, neither `SetMaximumFrameLatency(1)`
nor a future waitable chain eliminates the separate packet queue.
[Local queue][local-thread], [DXGI queue limit][chain-latency]

## Next gate

Run **one 30-second, manually focused VRR capture** of this branch and a
matching capture of a separate control build with only raw diagnostics added
to the pre-patch runtime. Record each build's commit, output dimensions,
latency policy, present flags/interval, and guest/worker waits. Keep driver
settings, scene, and display setup identical. The current branch includes all
runtime changes listed above, so this comparison measures their combined
effect; isolate one timing change at a time before attributing a difference.
Use the NVIDIA indicator to verify activation and
PresentMon console capture without its GUI overlay; retain QPC timing and
all dropped rows. This protocol follows the [activation check][nv-indicator]
and [console capture controls][pm-console].

Correlate each refresh delta of 2 with its preceding displayed frame's
duration. Compare median, tail, and maximum display intervals against
16.667 ms, and segment at mode/focus changes. A cluster near 33.3 ms supports
a real doubled content hold; intervals near 16.667 ms refute that explanation
for those samples while leaving LFC versus counter mapping **unverified**.
If trace permissions are unavailable or display events are incomplete,
report the experiment as inconclusive. This is a proposed falsifiable test
using [PresentMon duration semantics][pm-v1] and
[trace-access requirements][pm-readme].

[stats]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ns-dxgi-dxgi_frame_statistics
[get-stats]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics
[flip]: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model
[flip-blog]: https://devblogs.microsoft.com/directx/dxgi-flip-model/
[vrr]: https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
[present]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present
[lfc]: https://www.amd.com/en/products/graphics/technologies/freesync.html
[nv-setup]: https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-us/mergedProjects/Display/To_use_variable_refresh_rates.htm
[nv-indicator]: https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-us/mergedProjects/Display/To_know_if_VRR_is_turned_on_in_your_game.htm
[nv-overview]: https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-us/mergedProjects/Display/Variable_Refresh_Rate.htm
[nv-latency]: https://www.nvidia.com/en-us/geforce/guides/system-latency-optimization-guide/
[nv-cap]: https://nvidia.custhelp.com/app/answers/detail/a_id/4958
[ldat]: https://developer.nvidia.com/nvidia-latency-display-analysis-tool
[foreground]: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow
[hw-composition]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/nf-dxgi1_6-idxgioutput6-checkhardwarecompositionsupport
[hw-queue]: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/hardware-flip-queue
[windowed]: https://devblogs.microsoft.com/directx/updates-in-graphics-and-gaming/
[latency]: https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
[chain-flags]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_chain_flag
[chain-latency]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-setmaximumframelatency
[wait-handle]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject
[media-method]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchainmedia-getframestatisticsmedia
[media-stats]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/ns-dxgi1_3-dxgi_frame_statistics_media
[media-mode]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/ne-dxgi1_3-dxgi_frame_presentation_mode
[dwm-timing]: https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmgetcompositiontiminginfo
[dwm-flush]: https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmflush
[dwm-enable]: https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmenablecomposition
[duplication]: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_outdupl_frame_info
[etw]: https://learn.microsoft.com/en-us/windows/win32/api/evntrace/nf-evntrace-starttracew
[pm-readme]: https://github.com/GameTechDev/PresentMon/blob/00db2caba0efb4fa5f6c847fced2d8189edc87e4/README.md
[pm-console]: https://github.com/GameTechDev/PresentMon/blob/00db2caba0efb4fa5f6c847fced2d8189edc87e4/README-ConsoleApplication.md
[pm-v1]: https://github.com/GameTechDev/PresentMon/blob/v1.9.2/README.md#csv-columns
[pm-events]: https://github.com/GameTechDev/PresentMon/blob/00db2caba0efb4fa5f6c847fced2d8189edc87e4/PresentData/PresentMonTraceConsumer.hpp
[pm-source]: https://github.com/GameTechDev/PresentMon/blob/00db2caba0efb4fa5f6c847fced2d8189edc87e4/PresentData/PresentMonTraceConsumer.cpp
[pm-output]: https://github.com/GameTechDev/PresentMon/blob/00db2caba0efb4fa5f6c847fced2d8189edc87e4/PresentMon/OutputThread.cpp
[pm-service]: https://github.com/GameTechDev/PresentMon/blob/00db2caba0efb4fa5f6c847fced2d8189edc87e4/README-Service.md
[local-presenter]: ../../../recomp-runtime/d3d_presenter_d3d11.cpp
[local-vblank]: ../../../recomp-runtime/d3d_vblank.cpp
[local-adapter]: ../../../recomp-runtime/d3d_frame_adapter.c
[local-thread]: ../../../recomp-runtime/d3d_presenter_thread.cpp
