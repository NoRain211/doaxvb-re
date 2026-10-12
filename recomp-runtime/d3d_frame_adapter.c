#include "d3d_frame_adapter.h"
#include "cri_service_adapter.h"
#include "d3d_draw_adapter.h"
#include "d3d_frame_model.h"
#include "d3d_vblank.h"
#include "d3d_presenter.h"
#include "d3d_render_state_adapter.h"
#include "d3d_texture_adapter.h"
#ifdef RECOMP_FULL_PROGRAM
#include "fiber_adapter.h"
#endif
#include "stop_report.h"
#include "save_transaction.h"

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

enum {
    D3D_DEVICE_CLEAR_ADDRESS = 0x001e72d0u,
    D3D_DEVICE_SWAP_ADDRESS = 0x001e8f30u,
    D3D_DEVICE_SET_GAMMA_ADDRESS = 0x001e3640u,
};

static RecompD3dFrameState frame_state;
static RecompD3dPresenter *presenter;
static uint32_t frame_device_address;
#ifdef _WIN32
static volatile LONG console_break_requested;

void recomp_d3d_frame_adapter_request_console_break(void)
{
    InterlockedExchange(&console_break_requested, 1);
}
#endif

static uint32_t stack_argument(uint32_t entry_esp, uint32_t index)
{
    return *recomp_memory_u32(entry_esp + 4u + index * 4u);
}

uint32_t recomp_d3d_frame_adapter_swap_counter(void)
{
    return frame_state.swap_counter;
}

void recomp_d3d_frame_adapter_initialize(
    const RecompD3dPresenterConfig *config,
    uint32_t device_address)
{
    RecompD3dFrameError frame_error;
    RecompD3dPresenterError presenter_error;

    if (config == NULL || device_address == 0u) {
        recomp_stop(2, "d3d-frame-init:device");
    }
    frame_error = recomp_d3d_frame_initialize(
        &frame_state, config->width, config->height);
    if (frame_error != RECOMP_D3D_FRAME_OK) {
        recomp_stop(2, "d3d-frame-init:model:%u", (unsigned)frame_error);
    }
    presenter_error = recomp_d3d_presenter_create(config, &presenter);
    if (presenter_error != RECOMP_D3D_PRESENTER_OK) {
        recomp_d3d_frame_reset(&frame_state);
        recomp_stop(
            2,
            "d3d-frame-init:presenter:%u",
            (unsigned)presenter_error);
    }
    frame_device_address = device_address;
    recomp_d3d_vblank_reset();
}

void recomp_d3d_frame_adapter_reset(void)
{
    if (presenter != NULL) {
        RecompD3dPresenterError error =
            recomp_d3d_presenter_destroy(&presenter);

        if (error != RECOMP_D3D_PRESENTER_OK) {
            recomp_stop(
                2,
                "d3d-frame-reset:presenter:%u",
                (unsigned)error);
        }
    }
    recomp_d3d_frame_reset(&frame_state);
    frame_device_address = 0u;
}

/* The window can close between frames, so any command may see CLOSED. */
void recomp_d3d_frame_adapter_exit_if_closed(RecompD3dPresenterError error)
{
    if (error == RECOMP_D3D_PRESENTER_CLOSED) {
        recomp_d3d_frame_adapter_reset();
        fprintf(stderr, "recomp runner: window closed; exiting normally\n");
        recomp_stop(0, "host-window-close");
    }
}

RecompD3dPresenter *recomp_d3d_frame_adapter_presenter(void)
{
    return presenter;
}

bool recomp_d3d_frame_adapter_target(RecompD3dPresenterTarget *target)
{
    uint32_t color;
    uint32_t back;
    uint32_t depth;
    uint32_t default_depth;

    if (target == NULL || frame_device_address == 0u) {
        return false;
    }
    *target = (RecompD3dPresenterTarget){0};
    color = *recomp_memory_u32(frame_device_address + 0x21b4u);
    back = *recomp_memory_u32(frame_device_address + 0x21c0u);
    depth = *recomp_memory_u32(frame_device_address + 0x21b8u);
    default_depth = *recomp_memory_u32(frame_device_address + 0x21ccu);
    if (color == 0u || back == 0u) {
        goto reject;
    }
    target->offscreen = color != back;
    target->no_depth = depth == 0u;
    target->custom_depth = depth != 0u && depth != default_depth;
    if (target->offscreen &&
        (!recomp_d3d_texture_adapter_describe(color, &target->color) ||
         target->color.data == 0u || !target->color.render_target ||
         target->color.depth)) {
        goto reject;
    }
    if (target->custom_depth) {
        RecompD3dTextureDesc original = {0};

        if (!recomp_d3d_texture_adapter_describe(depth, &target->depth) ||
            target->depth.data == 0u || !target->depth.depth) {
            goto reject;
        }
        /* A surface wrapper can alias the default depth's pixel storage. */
        if (recomp_d3d_texture_adapter_describe(default_depth, &original) &&
            original.depth && original.data == target->depth.data &&
            original.format_byte == target->depth.format_byte &&
            original.width == target->depth.width &&
            original.height == target->depth.height &&
            original.linear == target->depth.linear &&
            original.pitch == target->depth.pitch) {
            target->custom_depth = false;
        }
    }
    return true;

reject:
    {
        static uint32_t reported;

        if (reported < 8u) {
            ++reported;
            fprintf(
                stderr,
                "recomp d3d target: rejected color=%08" PRIx32
                " back=%08" PRIx32 " data=%08" PRIx32
                " fmt=%02" PRIx32 " %ux%u depth=%08" PRIx32
                " default=%08" PRIx32 " data=%08" PRIx32
                " fmt=%02" PRIx32 " %ux%u\n",
                color, back, target->color.data, target->color.format_byte,
                target->color.width, target->color.height,
                depth, default_depth, target->depth.data,
                target->depth.format_byte,
                target->depth.width, target->depth.height);
        }
    }
    return false;
}

void recomp_d3d_frame_adapter_reset_buffers(void)
{
    RecompD3dFrameResult result =
        recomp_d3d_frame_reset_buffers(&frame_state);
    RecompD3dPresenterError presenter_error;

    if (result.error != RECOMP_D3D_FRAME_OK) {
        recomp_stop(
            2,
            "d3d-reset:model:%u",
            (unsigned)result.error);
    }
    presenter_error = recomp_d3d_presenter_submit(
        presenter, &result.command);
    recomp_d3d_frame_adapter_exit_if_closed(presenter_error);
    if (presenter_error != RECOMP_D3D_PRESENTER_OK) {
        recomp_stop(
            2,
            "d3d-reset:presenter:%u",
            (unsigned)presenter_error);
    }
}

void recomp_d3d_clear_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t saved_eax = recomp_runtime.registers.eax;
    RecompD3dFrameResult result = recomp_d3d_frame_clear(
        &frame_state,
        stack_argument(entry_esp, 0u),
        stack_argument(entry_esp, 1u),
        stack_argument(entry_esp, 2u),
        stack_argument(entry_esp, 3u),
        stack_argument(entry_esp, 4u),
        stack_argument(entry_esp, 5u));
    RecompD3dPresenterError presenter_error;

    if (result.error != RECOMP_D3D_FRAME_OK) {
        fprintf(
            stderr,
            "recomp d3d: Clear model rejected arguments (%u)\n",
            (unsigned)result.error);
        recomp_stop(2, "d3d-clear:model:%u", (unsigned)result.error);
    }
    if (!recomp_d3d_frame_adapter_target(&result.command.data.clear.target)) {
        recomp_stop(2, "d3d-clear:render-target");
    }
    presenter_error = recomp_d3d_presenter_submit(
        presenter, &result.command);
    recomp_d3d_frame_adapter_exit_if_closed(presenter_error);
    if (presenter_error != RECOMP_D3D_PRESENTER_OK) {
        fprintf(
            stderr,
            "recomp d3d: Clear presenter failed (%u)\n",
            (unsigned)presenter_error);
        recomp_stop(
            2,
            "d3d-clear:presenter:%u",
            (unsigned)presenter_error);
    }

    recomp_runtime.registers.eax = saved_eax;
    recomp_runtime.registers.esp = entry_esp + 28u;
}

void recomp_d3d_swap_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    RecompD3dFrameResult result = recomp_d3d_frame_swap(
        &frame_state, stack_argument(entry_esp, 0u));
    RecompD3dPresenterError presenter_error;

    /* KeTickCount is exported to the guest as a data symbol at
       kKernelDataBase + 0x40 (runner.cpp), but nothing ever advanced it, so
       guest code that polls it saw time frozen at zero for the life of the
       process. The frozen native host runs a dedicated 1 ms thread for this
       (hostKeTickCountThreadProc), but a host thread writing guest memory
       asynchronously would make runs non-reproducible, and the camera is
       measurably bit-identical across runs today - a property worth keeping.

       Advance it from presentation instead: one swap is one displayed frame,
       so a fixed step per swap gives a monotonic millisecond counter that is
       deterministic and independent of host scheduling. 16 ms per swap is
       the NTSC frame interval this title targets.

       This is a kernel data export, so it is written here rather than in the
       swap model, which must stay free of host and kernel knowledge. */
    {
        enum {
            KERNEL_DATA_BASE = 0x00740000u,
            KE_TICK_COUNT_ADDRESS = KERNEL_DATA_BASE + 0x40u,
            MILLISECONDS_PER_SWAP = 16u,
        };
        uint32_t *tick_count = recomp_memory_u32(KE_TICK_COUNT_ADDRESS);

        if (tick_count != NULL) {
            *tick_count += MILLISECONDS_PER_SWAP;
        }
    }

    /* Round 27. Rounds 22-26 measured every guest selector the scene and
       view machinery exposes and found all of them constant while two
       independent cameras alternate. Inferring from selectors has failed
       five times, so observe the caller directly instead.

       Ghidra reports exactly five call sites of D3DDevice_Swap, in four
       distinct functions: 0x00179A5A (FUN_00179890), 0x00179A92 and
       0x00179AC0 (both FUN_00179A90), 0x00179F34 (FUN_00179E70) and
       0x0017A283 (FUN_0017A200). This adapter replaces the guest function,
       so the return address the CALL pushed is still the dword at entry_esp
       and names the site outright. Two addresses alternating with frame
       parity would identify the culprit; a single address retires the whole
       'two present paths' family.

       The extra dwords are a heuristic scan, not a real unwind: a stale
       value that merely looks like a code address is expected, so treat a
       chain entry as evidence only when it is stable across frames and lands
       on a known function entry. Opt-in, off by default. */
    {
        static const char *who_trace;
        static bool who_trace_read;
        static uint32_t who_trace_lines;

        if (!who_trace_read) {
            who_trace_read = true;
            who_trace = getenv("RECOMP_D3D_WHOCALLS");
        }
        if (who_trace != NULL && who_trace_lines < 240u) {
            /* The first attempt read the dword at entry_esp expecting the
               return address a hardware CALL would have pushed. It read 0 on
               all 240 frames, because the lifter emits `PUSH32(esp, 0)` in
               place of the return address and transfers control through
               RECOMP_ICALL_SAFE. The guest stack therefore carries no return
               address at all, and a stack scan cannot recover one.

               The dispatch stack does carry the identity: every indirect
               site passes __FILE__ and __LINE__, and the runtime already
               resolves those to a generated function and guest label when it
               reports a stop. Walk that instead - it is the real caller
               chain rather than a heuristic over stale stack words. */
            char chain[192];
            size_t used = 0u;
            size_t depth;

            chain[0] = '\0';
            for (depth = 0u; depth < 6u; ++depth) {
                uint32_t target = 0u;
                const char *member = NULL;
                int line = 0;
                int written;

                if (!recomp_dispatch_frame_at(
                        depth, &target, &member, &line)) {
                    break;
                }
                written = snprintf(
                    chain + used,
                    sizeof chain - used,
                    " %08" PRIx32 "@%s:%d",
                    target,
                    member != NULL ? member : "<adapter>",
                    line);
                if (written <= 0 || (size_t)written >= sizeof chain - used) {
                    break;
                }
                used += (size_t)written;
            }
            ++who_trace_lines;
            fprintf(
                stderr,
                "recomp d3d whocalls: swap=%lu esp=%08" PRIx32
                " chain=%s\n",
                (unsigned long)result.command.data.present.swap_counter,
                entry_esp,
                chain);
        }
    }

    if (result.error != RECOMP_D3D_FRAME_OK) {
        fprintf(
            stderr,
            "recomp d3d: Swap model rejected arguments (%u)\n",
            (unsigned)result.error);
        recomp_stop(2, "d3d-swap:model:%u", (unsigned)result.error);
    }
    /* Animation and gameplay fibers each advance once per Swap. Host VSync
       follows the monitor's refresh rate, not the guest's 60 Hz cadence. */
    recomp_d3d_wait_vblank();
    presenter_error = recomp_d3d_presenter_submit(
        presenter, &result.command);
    recomp_d3d_draw_adapter_capture_present(
        result.command.data.present.swap_counter, (uint32_t)presenter_error);
    recomp_d3d_frame_adapter_exit_if_closed(presenter_error);
    if (presenter_error != RECOMP_D3D_PRESENTER_OK) {
        fprintf(
            stderr,
            "recomp d3d: Swap presenter failed (%u)\n",
            (unsigned)presenter_error);
        recomp_stop(
            2,
            "d3d-swap:presenter:%u",
            (unsigned)presenter_error);
    }

#ifdef _WIN32
    if (InterlockedCompareExchange(&console_break_requested, 0, 0) != 0 &&
        !recomp_save_pending()) {
        recomp_d3d_frame_adapter_reset();
        fprintf(stderr, "recomp runner: console break; exiting normally\n");
        recomp_stop(0, "host-console-break");
    }
#endif

    *recomp_memory_u32(frame_device_address + 0x2c10u) =
        result.command.data.present.swap_counter;
#ifdef RECOMP_FULL_PROGRAM
    /* Scene state machine at 0x000A3820: 0x5DECE0 is the scene currently
       running and 0x5DED55 is the scene requested for the next iteration.
       START writes 0x5DED55 from two scene handlers (0x000A4DA0 -> 4,
       0x000A4DC0 -> 3). Reporting every change of the running scene shows
       whether input actually advances the title or the request is dropped. */
    {
        static bool scene_seen;
        static uint8_t last_scene;
        static uint8_t last_request;
        static uint32_t seen_buttons;
        if (frame_device_address != 0u) {
            const uint8_t *scene = (const uint8_t *)(const void *)
                recomp_memory_i8(0x005dece0u);
            const uint8_t *request = (const uint8_t *)(const void *)
                recomp_memory_i8(0x005ded55u);

            if (scene != NULL && request != NULL &&
                (!scene_seen || *scene != last_scene ||
                 *request != last_request)) {
                scene_seen = true;
                last_scene = *scene;
                last_request = *request;
                fprintf(
                    stderr,
                    "recomp scene: swap=%lu running=%u requested=%u "
                    "ready=%u menu=%u sub=%u\n",
                    (unsigned long)result.command.data.present.swap_counter,
                    (unsigned)*scene,
                    (unsigned)*request,
                    (unsigned)*(const uint8_t *)(const void *)
                        recomp_memory_i8(0x004d567cu),
                    (unsigned)*(const uint8_t *)(const void *)
                        recomp_memory_i8(0x004d966au),
                    (unsigned)*(const uint8_t *)(const void *)
                        recomp_memory_i8(0x004d9534u));
            }
            /* The title accumulates its own held-button mask at 0x5F3138.
               Reporting each newly observed bit shows whether a synthesized
               press reaches the game at all, separately from whether the
               scene acts on it. A is 0x0100, START is 0x0010. */
            {
                /* 0x5F3140 is the rising-edge word the scene and menu
                   handlers actually test. Reporting first-seen bits only
                   hides every repeat press, so report each edge instead and
                   cap the volume rather than the distinct bit set. */
                /* The budget is 4000, not 40, for the same reason the uitick
                   probe below carries 4000: at 40 lines this went silent at
                   swap 6107, so round 87 could not name which mask unlocked
                   screen 3 and had to attribute it by timing instead. */
                static uint32_t button_lines;
                uint32_t held = *recomp_memory_u32(0x005f3138u);
                uint32_t rising = *recomp_memory_u32(0x005f3140u);

                if (rising != 0u && button_lines < 4000u) {
                    seen_buttons |= held;
                    ++button_lines;
                    fprintf(
                        stderr,
                        "recomp buttons: swap=%lu rising=0x%08" PRIx32
                        " held=0x%08" PRIx32 "\n",
                        (unsigned long)
                            result.command.data.present.swap_counter,
                    rising,
                    held);
                }
            }
            /* The scene-3 menu only forwards a press to its button callback
               at 0x0009D9DB4 once its own UI state block at 0x009D9D80 is
               live. Report that block once so a menu that never arms is
               distinguishable from one that arms and ignores the press. */
            {
                /* 0x000F27C0 dispatches on the UI state word 0x009D9DA0 and
                   only its value 3 reaches the branch that can deliver an
                   event to the callback. 0x000F26B0 arms a screen by writing
                   -1 into both phase (+0x18) and event (+0x1c), so an event
                   of -1 is the armed idle value rather than a failed arm.
                   Report every transition of the gating words so a state that
                   never advances is distinguishable from an event that is
                   produced and then dropped. */
                static uint32_t last_state = 0xfffffffeu;
                static uint32_t last_event = 0xfffffffeu;
                static uint32_t last_phase = 0xfffffffeu;
                static int last_screen = -2;
                static uint32_t ui_lines;
                uint32_t state = *recomp_memory_u32(0x009d9da0u);
                uint32_t event = *recomp_memory_u32(0x009d9d9cu);
                uint32_t phase = *recomp_memory_u32(0x009d9d98u);
                int screen = (int)*recomp_memory_i8(0x009d9d80u);

                if (ui_lines < 4000u &&
                    (state != last_state || event != last_event ||
                     phase != last_phase || screen != last_screen)) {
                    last_state = state;
                    last_event = event;
                    last_phase = phase;
                    last_screen = screen;
                    ++ui_lines;
                    fprintf(
                        stderr,
                        "recomp menu: swap=%lu state=%" PRIu32
                        " event=0x%08" PRIx32 " phase=0x%08" PRIx32
                        " screen=%d callback=0x%08" PRIx32
                        " entries=%" PRIu32 " table=0x%08" PRIx32 "\n",
                        (unsigned long)
                            result.command.data.present.swap_counter,
                        state, event, phase, screen,
                       *recomp_memory_u32(0x009d9db4u),
                       *recomp_memory_u32(0x009d9df4u),
                       *recomp_memory_u32(0x009d9db0u));
               }
                /* The transition-completion gate sub_000F2750 returns
                   "not finished" if ANY entry in the callback table at
                   0x009D9DBC returns 0 -- a sticky-zero accumulator, so a
                   single unfinished callback holds the whole transition
                   open. Static analysis found six possible callbacks, and
                   two of them (0xE0290 registered by sub_000CE0C0, and
                   0xE02B0 registered by sub_000D8160) have NO body anywhere
                   in the generated tree and no dispatch entry -- the bodies
                   jump from sub_000E0260 straight to sub_000E0330.

                   Log the live slots so a stall names a function instead of
                   a button: which callbacks are registered, and which
                   done-flags are still zero. Count is at 0x009D9DE0,
                   callbacks at 0x009D9DBC, done-flags at 0x009D9DDC. */
                {
                    static uint32_t last_sig;
                    static uint32_t cb_lines;
                    uint32_t count = *recomp_memory_u32(0x009d9de0u);

                    if (count != 0u && count <= 8u && cb_lines < 4000u) {
                        uint32_t sig = count;
                        uint32_t i;

                        for (i = 0u; i < count; ++i) {
                            sig = (sig * 31u) +
                                  *recomp_memory_u32(0x009d9dbcu + i * 4u);
                            sig = (sig * 31u) +
                                  *recomp_memory_u32(0x009d9ddcu + i * 4u);
                        }

                        if (sig != last_sig) {
                            last_sig = sig;
                            ++cb_lines;
                            fprintf(stderr, "recomp uicb: swap=%lu count=%" PRIu32,
                                    (unsigned long)
                                        result.command.data.present.swap_counter,
                                    count);
                            for (i = 0u; i < count; ++i) {
                                fprintf(stderr,
                                        " [%" PRIu32 "]=0x%08" PRIx32
                                        ":done=%" PRIu32,
                                        i,
                                        *recomp_memory_u32(0x009d9dbcu + i * 4u),
                                        *recomp_memory_u32(0x009d9ddcu + i * 4u));
                            }
                            fprintf(stderr, "\n");
                        }
                    }
                }
                /* Screen 1's per-frame update at 0x000F4330 gates on
                   0x000F0760, which switches on the first dword of the
                   object at 0x009D99C8 (0x3c-byte stride, index 0 here).
                   That word is the screen's own step state, so report its
                   transitions to tell a screen that is waiting on a load
                   from one that finished and is idling. */
                {
                    static uint32_t last_step = 0xfffffffeu;
                    static uint32_t step_lines;
                    uint32_t step = *recomp_memory_u32(0x009d99c8u);

                   if (step != last_step && step_lines < 40u) {
                       last_step = step;
                       ++step_lines;
                       fprintf(
                           stderr,
                           "recomp uiobj: swap=%lu step=%" PRIu32
                            " prev=%" PRIu32 " tick=%" PRIu32
                            " limit=%" PRIu32 "\n",
                            (unsigned long)
                                result.command.data.present.swap_counter,
                            step,
                            *recomp_memory_u32(0x009d99ccu),
                           *recomp_memory_u32(0x009d9da8u),
                           *recomp_memory_u32(0x009d9dacu));
                  }
              }
                /* Screen 4 (the island activity) runs its per-frame update
                   through 0x000F0760 with index 1, not index 0, so the
                   "uiobj" probe above watches the wrong object for this
                   screen. Index 1's state word is 0x009D99C8 + 0x3C =
                   0x009D9A04, written 2 at 0x000F085F; only then does
                   0x000F0760 return 0 and the action interpreter
                   sub_000E1380 get called at 0x000E0F16. Report that word
                   next to the normalized input state so a screen that never
                   becomes interactive is distinguishable from one that is
                   interactive and ignores the delivered edge.

                   0x005F3138 is the aggregate held mask and 0x005F3140 the
                   rising-edge mask produced by the normalizer 0x000B58A0;
                   0x004D9548 is port 0's translated UI flag word. Printing
                   all four on any change makes the baseline-versus-input
                   differential a single grep: an edge that appears in
                   0x005F3140 but changes nothing else localizes the stop at
                   the consumer rather than at delivery. Gated on
                   RECOMP_ACTPROBE so the default run is unchanged. */
                {
                    static bool act_read;
                    static const char *act_env;
                    static uint32_t act_lines;
                    static uint32_t last_act_sig = 0xfffffffeu;

                    if (!act_read) {
                        act_read = true;
                        act_env = getenv("RECOMP_ACTPROBE");
                    }
                    if (act_env != NULL && act_lines < 4000u) {
                        uint32_t obj1 = *recomp_memory_u32(0x009d9a04u);
                        uint32_t held = *recomp_memory_u32(0x005f3138u);
                        uint32_t rise = *recomp_memory_u32(0x005f3140u);
                        uint32_t flag = *recomp_memory_u32(0x004d9548u);
                        uint32_t sig = obj1;

                        sig = (sig * 31u) + held;
                        sig = (sig * 31u) + rise;
                        sig = (sig * 31u) + flag;
                        if (sig != last_act_sig) {
                            last_act_sig = sig;
                            ++act_lines;
                            fprintf(
                                stderr,
                                "recomp act: swap=%lu obj1=%" PRIu32
                                " held=0x%08" PRIx32 " rise=0x%08" PRIx32
                                " flag=0x%08" PRIx32 " screen=%d\n",
                                (unsigned long)
                                    result.command.data.present.swap_counter,
                                obj1, held, rise, flag,
                                (int)*recomp_memory_i8(0x009d9d80u));
                            {
                                /* r112: with the A press proven to classify
                                   as confirm (0x004D9548 rising word reads
                                   0x00350100, and 0x10000 is the A flag set
                                   at 0x000A5129), the step-4 arm can only
                                   reject at 0x000E14EA (record word ==
                                   0xFFFF) or 0x000E14F5 (record state byte
                                   == 2).  Both read the 16-byte record that
                                   sub_000E12B0/sub_000E1230 resolve, and the
                                   table at 0x005D9398 is BSS filled from
                                   assets, so neither is statically knowable.
                                   Recompute the same index and print both. */
                                int32_t grp = *recomp_memory_i8(0x00b5b209u);
                                int32_t cnt = *recomp_memory_i8(
                                    0x00b5b1a8u + (uint32_t)grp);
                                uint32_t bas = (uint32_t)(uint8_t)
                                    *recomp_memory_i8(
                                        0x00b5b1b7u + (uint32_t)grp);
                                uint32_t obj = *recomp_memory_u32(0x00b5b128u);
                                uint32_t cur = obj != 0u
                                    ? (uint32_t)*recomp_memory_u16(obj + 0xcu)
                                    : 0xffffu;
                                uint32_t tbl = *recomp_memory_u32(0x00317634u);
                                uint32_t slot = cur & 0xffu;
                                uint32_t slotword = 0x30u;
                                uint32_t recw = 0xffffu;
                                uint32_t recb = 0xffu;

                                if ((int32_t)slot < cnt) {
                                    uint32_t t = (bas + slot) & 0xffu;

                                    if (t >= 0x30u) {
                                        t = 0u;
                                    }
                                    slotword = (uint32_t)(uint8_t)
                                        *recomp_memory_i8(0x00b5b148u + t);
                                }
                                if (tbl != 0u) {
                                    uint32_t rec = tbl +
                                        (((0x33u * 4u) + slotword) * 0x10u);

                                    recw = *recomp_memory_u16(rec);
                                    recb = (uint32_t)(uint8_t)
                                        *recomp_memory_i8(rec + 4u);
                                }
                                fprintf(
                                    stderr,
                                    "recomp entryrec: swap=%lu grp=%d cnt=%d"
                                    " base=0x%02" PRIx32 " cur=0x%04" PRIx32
                                    " slotword=0x%02" PRIx32
                                    " rawcat=0x%02" PRIx32
                                    " tbl=0x%08" PRIx32
                                    " word=0x%04" PRIx32
                                    " state=0x%02" PRIx32 "\n",
                                    (unsigned long)result.command.data.present
                                        .swap_counter,
                                    (int)grp, (int)cnt, bas, cur, slotword,
                                    (uint32_t)(uint8_t)*recomp_memory_i8(
                                        0x009d9939u),
                                    tbl, recw, recb);
                            }
                        }
                    }
                }
                /* r105 found the screen-4 consume gate. sub_000E0EA0 reads
                   0x00B5B140 first and, when it is 0 (or 0x1B), returns 1
                   immediately -- "action consumed" -- WITHOUT ever calling
                   sub_000F0760 or the interpreter sub_000E1380. Its caller
                   sub_000D5380 then treats that 1 as a real consumption at
                   0x000D53BE, so a delivered and correctly translated A press
                   is swallowed with no effect. That exactly matches r105C: A
                   translated to flag 0x00350100 on screen 4 and nothing moved.

                   0x00B5B140 is written 1 only by sub_000E09E0 (the UI list
                   initializer) and 0 by sub_000CC65B. 0x00B5B144 is the
                   companion mode word set beside it, and 0x00B5B128 is the
                   screen-4 context pointer whose list the interpreter walks.
                   Report all three so the next run measures whether the
                   activity screen ever ran its list initializer, instead of
                   inferring it. Same RECOMP_ACTPROBE gate. */
                {
                    static uint32_t gate_lines;
                    static uint32_t last_gate_sig = 0xfffffffeu;
                    static bool gate_read;
                    static const char *gate_env;

                    if (!gate_read) {
                        gate_read = true;
                        gate_env = getenv("RECOMP_ACTPROBE");
                    }
                    if (gate_env != NULL && gate_lines < 400u) {
                        uint32_t gate = *recomp_memory_u32(0x00b5b140u);
                        uint32_t mode = *recomp_memory_u32(0x00b5b144u);
                        uint32_t ctx = *recomp_memory_u32(0x00b5b128u);
                        uint32_t sig = gate;

                       sig = (sig * 31u) + mode;
                       sig = (sig * 31u) + ctx;
                        /* r105D falsified the 0x00B5B140 short-circuit: it
                           reads 4 on screen 4, so sub_000E0EA0 does call
                           through. The next branch that can swallow the press
                           is sub_000F0760's guard at 0x000F0798 -- it reaches
                           the 0x90000/0x10 test on the translated flag word
                           only when sub_000EF5D0 returns 0. sub_000EF5D0
                           switches on 0x005C5BA0 and returns 1 for most of
                           its eight cases (loc_000EF609), discarding the
                           press before any flag test. Its one zero-return
                           path also requires the counter at 0x005C5FAC to
                           have reached 0x3C. Sample both so the next run
                           names the blocking value instead of the branch. */
                        sig = (sig * 31u) + *recomp_memory_u32(0x005c5ba0u);
                        /* r105E printed t=0 at swap 14471 and never printed
                           again, but t was NOT part of the signature, so that
                           0 is only the last latched value and says nothing
                           about the press at 15007. Fold the counter in --
                           bucketed at the 0x3C threshold sub_000EF5D0 tests,
                           so the line reprints when it crosses, without
                           emitting a line every frame. */
                        sig = (sig * 31u) +
                              (*recomp_memory_u32(0x005c5facu) >= 0x3cu ? 1u
                                                                        : 0u);
                        /* r105F left one ambiguity: the bucketed counter
                           cannot distinguish "frozen at 0" from "cycling
                           below 0x3C". Report the raw counter once every 300
                           swaps after the screen-4 entry swap so a frozen
                           incrementer is separable from a re-arming one,
                           without emitting a line per frame. */
                        {
                            static uint32_t tfreeze_lines;
                            unsigned long tswap = (unsigned long)
                                result.command.data.present.swap_counter;

                            if (tswap >= 14400ul && tswap % 300ul == 0ul &&
                                tfreeze_lines < 200u) {
                                ++tfreeze_lines;
                                /* r105G proved the counter is frozen because
                                   0x005C5BA0 is 0: sub_0008EF30 decrements it
                                   before its 8-case switch (0x0008EFA7), so 0
                                   wraps past the bound and skips every case,
                                   then loc_0008F18B returns at 0x0008F191
                                   before the increment at 0x0008F2F7. The
                                   lane itself is driven -- sub_0008AB4A is
                                   reached from the per-frame path via
                                   sub_0008AAF0/sub_0008AB1F -- so this is
                                   guest state, not a missing host adapter.
                                   0x005C6078 is the request word those mode
                                   cases compare against (0x0008EFC1 wants 2),
                                   and 0x005C607C is its companion; report
                                   both to name what should have moved the
                                   mode off 0 on this scene. */
                                fprintf(
                                    stderr,
                                    "recomp tfreeze: swap=%lu t=%" PRIu32
                                    " sndmode=%" PRIu32 " req=%" PRIu32
                                    " req2=%" PRIu32 " screen=%d\n",
                                    tswap,
                                    *recomp_memory_u32(0x005c5facu),
                                    *recomp_memory_u32(0x005c5ba0u),
                                    *recomp_memory_u32(0x005c6078u),
                                    *recomp_memory_u32(0x005c607cu),
                                    (int)*recomp_memory_i8(0x009d9d80u));
                            }
                        }
                        if (sig != last_gate_sig) {
                            last_gate_sig = sig;
                            ++gate_lines;
                            fprintf(
                                stderr,
                                "recomp gate: swap=%lu b140=0x%08" PRIx32
                                " b144=0x%08" PRIx32 " ctx=0x%08" PRIx32
                                " sndmode=%" PRIu32 " t=%" PRIu32
                                " screen=%d\n",
                                (unsigned long)
                                    result.command.data.present.swap_counter,
                                gate, mode, ctx,
                                *recomp_memory_u32(0x005c5ba0u),
                                *recomp_memory_u32(0x005c5facu),
                                (int)*recomp_memory_i8(0x009d9d80u));
                        }
                    }
                }
                /* Round 108. r107 measured screen 4 parking on step 4 of its
                   scripted sequence with the A press accepted: the flag word
                   reads 0x00350100, and 0x00350100 & 0x10000 is exactly the
                   pass condition of the step-4 button gate sub_000CBEA0.
                   Control therefore reaches the next test,

                     0x000E14E3  cmp word ptr [esp+0x10], 0xffff
                     0x000E14EA  je  0x000E1488     ; no selection, step stays

                   where [esp+0x10] holds the value sub_000E1380 stored at
                   0x000E13BE -- the return value of the menu lookup
                   sub_000E12B0.

                   r107 proposed watching the entry count at 0x00B5B1A8 and
                   predicted it reads 0. Reading the lookup body shows that a
                   zero count is NOT sufficient to produce 0xFFFF, so the
                   watch could not have decided the question:

                     movzx ecx, byte [esp+4]           ; index argument
                     movsx edx, byte [eax + 0xB5B1A8]  ; per-group entry count
                     cmp   ecx, edx
                     jl    in_range
                     mov   ecx, 0x30                   ; out-of-range SLOT
                     jmp   tail
                   in_range:
                     movzx eax, byte [eax + 0xB5B1B7]  ; per-group base offset
                     add   eax, ecx
                     cmp   eax, 0x30
                     jb    ok
                     xor   eax, eax
                   ok:
                     movzx ecx, byte [eax + 0xB5B148]  ; slot -> entry id
                   tail:
                     mov   al, byte [0x9D9939]
                     call  sub_000EF330                ; -> small category
                     mov   edx, dword [0x317634]
                     imul  eax, eax, 0x33
                     add   eax, ecx
                     shl   eax, 4
                     movzx eax, word [eax + edx]       ; 16-bit result

                   BOTH paths end in the same table read. The out-of-range
                   path substitutes slot 0x30 and still indexes the table at
                   [0x317634], so 0xFFFF is a value STORED IN THAT TABLE, and
                   either path can return it. A write-watch on 0x00B5B1A8
                   would also most likely have recorded nothing at all: it is
                   a byte inside a structure written during screen setup, not
                   a word the per-frame audio lane touches, and the watch
                   budget is consumed by frame traffic long before screen 4.

                   So sample the inputs and the resolved table row directly,
                   from the same per-swap seam that already reports the gate
                   above. The group index is the signed byte at 0x00B5B209
                   that every caller loads into eax before the call. With the
                   count, the base offset, the slot the arithmetic selects and
                   the 16-bit word actually living in that table row, the next
                   receipt can name which of the two paths produced 0xFFFF, or
                   show the row is fine and move the divergence downstream.

                   Read-only: this reports guest memory and never writes it.
                   Gated on the existing RECOMP_ACTPROBE so the default run is
                   unchanged. */
                {
                    static uint32_t sel_lines;
                    static uint32_t last_sel_sig = 0xfffffffeu;
                    static bool sel_read;
                    static const char *sel_env;

                    if (!sel_read) {
                        sel_read = true;
                        sel_env = getenv("RECOMP_ACTPROBE");
                    }
                    if (sel_env != NULL && sel_lines < 200u) {
                        uint32_t group =
                            (uint32_t)(uint8_t)*recomp_memory_i8(0x00b5b209u);
                        int32_t count = (int32_t)*recomp_memory_i8(
                            (uint32_t)(0x00b5b1a8u + group));
                        uint32_t base = (uint32_t)(uint8_t)*recomp_memory_i8(
                            (uint32_t)(0x00b5b1b7u + group));
                        uint32_t table = *recomp_memory_u32(0x00317634u);
                        uint32_t category =
                            (uint32_t)(uint8_t)*recomp_memory_i8(0x009d9939u);
                        uint32_t raw_category = category;
                        /* Slot the in-range path picks for menu index 0, and
                           the entry id that slot maps to. Index 0 is the
                           first entry a freshly armed menu highlights. */
                        uint32_t slot = base < 0x30u ? base : 0u;
                        uint32_t entry = (uint32_t)(uint8_t)*recomp_memory_i8(
                            (uint32_t)(0x00b5b148u + slot));
                        /* The out-of-range path forces slot 0x30 instead. */
                        uint32_t sig = (uint32_t)count;
                        /* Round 109 measured count=1 and inword=0x009E, so the
                           selection word is a valid entry and the 0xFFFF arm at
                           0x000E14EA is NOT taken. The press therefore reaches
                           0x000E14EC, where the menu-mode byte [ebp+0xC3] --
                           absolute 0x00B5B1EB, the object base 0x00B5B128 plus
                           0xC3 -- selects which arm runs:

                             0x000E1500  mov al, byte [ebp+0xC3]
                             0x000E1506  cmp al, 4 / jne 0x000E1549
                             0x000E1549  cmp al, 2 / jne 0x000E166A

                           Only modes 4 and 2 have arms that can advance the
                           step; anything else falls through to 0x000E166A and
                           reaches the tail with the step unchanged. The same
                           byte is read by the menu builder at 0x000E2AB0, so it
                           also decides which entries were visible. Report it
                           alongside the step-1 counters at [ebp+0xB4]/[ebp+0xB0]
                           (absolute 0x00B5B1DC/0x00B5B1D8), which the step-1
                           handler compares to decide the sequence has advanced.
                           Read-only. */
                        uint32_t menu_mode =
                            (uint32_t)(uint8_t)*recomp_memory_i8(0x00b5b1ebu);
                        uint32_t step_cur = *recomp_memory_u32(0x00b5b1dcu);
                        uint32_t step_end = *recomp_memory_u32(0x00b5b1d8u);

                        sig = (sig * 31u) + group;
                        sig = (sig * 31u) + base;
                        sig = (sig * 31u) + entry;
                        sig = (sig * 31u) + table;
                        sig = (sig * 31u) + menu_mode;
                        if (sig != last_sel_sig) {
                            uint32_t in_word = 0u;
                            uint32_t oor_word = 0u;
                            const uint16_t *in_slot;
                            const uint16_t *oor_slot;

                            last_sel_sig = sig;
                            ++sel_lines;
                            /* row = (category * 0x33 + entry) << 4.

                               0x9D9939 is NOT the row multiplier: the guest
                               passes it through sub_000EF330, which maps the
                               domain 1..0x14 onto 0..7 via a byte table at
                               0xEF394 and a jump table at 0xEF370, and
                               returns the sentinel 0xFE for anything else
                               (0, >0x14), passing 0xFF through unchanged.
                               Round 108 read the raw byte as the multiplier
                               and so computed a row the guest never uses.
                               Reproduce the mapping here rather than the raw
                               value; report both so a bad category is
                               visible instead of silently mis-indexing. */
                            if (category == 0xFEu || category == 0xFFu ||
                                category == 0u || category > 0x14u) {
                                category = 0xFEu;
                            } else {
                                category = (uint32_t)(uint8_t)
                                    *recomp_memory_i8(0x000ef394u +
                                                      (category - 1u));
                            }
                            in_slot = recomp_memory_u16(
                                table + (((category * 0x33u) + entry) << 4));
                            oor_slot = recomp_memory_u16(
                                table + (((category * 0x33u) + 0x30u) << 4));
                            if (in_slot != NULL) {
                                in_word = *in_slot;
                            }
                            if (oor_slot != NULL) {
                                oor_word = *oor_slot;
                            }
                            fprintf(
                                stderr,
                                "recomp menusel: swap=%lu group=%" PRIu32
                                " count=%d base=0x%02" PRIx32
                                " slot=0x%02" PRIx32 " entry=0x%02" PRIx32
                                " rawcat=0x%02" PRIx32 " cat=0x%02" PRIx32
                                " table=0x%08" PRIx32
                                " inword=0x%04" PRIx32
                                " oorword=0x%04" PRIx32
                                " mode=0x%02" PRIx32 " b4=0x%08" PRIx32
                                " b0=0x%08" PRIx32 " screen=%d\n",
                                (unsigned long)
                                    result.command.data.present.swap_counter,
                                group, (int)count, base, slot, entry,
                                raw_category, category, table, in_word,
                                oor_word, menu_mode, step_cur, step_end,
                                (int)*recomp_memory_i8(0x009d9d80u));
                        }
                    }
                }
                /* Round 111: the confirm handler sub_000E1380 has exactly one
                   caller, 0x000E0F16 inside sub_000E0EA0, and that call is
                   gated twice before the menu-mode byte is ever read:

                     0x000E0EA1  mov  eax, [0xB5B140]   ; current step
                     0x000E0EAB  je   0x000E0EB2        ; step 0    -> return
                     0x000E0EB0  jne  0x000E0EBA        ; step 0x1B -> return
                     0x000E0ED0  call 0x000F0760        ; with ecx = 1
                     0x000E0EDF  jne  0x000E0F07        ; return != 1
                     0x000E0F0A  jne  0x000E0F20        ; state != 2 -> skip
                     0x000E0F16  call 0x000E1380        ; confirm handler

                   sub_000F0760 indexes 0x009D99C8 + 1*0x3C = 0x009D9A04 and
                   fills the caller's out parameter from [esi], so only state 2
                   reaches the handler at all. Rounds 108-110 read the menu-mode
                   byte without ever measuring this gate, so a handler that
                   never runs and one that runs and declines look identical.

                   Mode 0 is also not an unhandled value. 0x000E166A tests 1 and
                   then 0x000E1691 dispatches script opcode 0x11 through
                   sub_000E21F0, which assigns the step from its own edx
                   argument at 0x000E2222. Had an accepted press reached there,
                   the step would have left 4. It has not, so the divergence is
                   upstream of the mode test, not in it. Sample the step beside
                   the state so both are visible in one line.

                   Read-only, gated on the existing RECOMP_ACTPROBE. */
                {
                    static uint32_t gate_lines;
                    static uint32_t last_gate_sig = 0xfffffffeu;
                    static bool gate_read;
                    static const char *gate_env;

                    if (!gate_read) {
                        gate_read = true;
                        gate_env = getenv("RECOMP_ACTPROBE");
                    }
                    if (gate_env != NULL && gate_lines < 400u) {
                        uint32_t state = *recomp_memory_u32(0x009d9a04u);
                        uint32_t prev = *recomp_memory_u32(0x009d9a08u);
                        uint32_t step = *recomp_memory_u32(0x00b5b140u);
                        uint32_t b4 = *recomp_memory_u32(0x00b5b1dcu);
                        uint32_t pad =
                            (uint32_t)(uint8_t)*recomp_memory_i8(0x003af1d0u);
                        uint32_t desc =
                            *recomp_memory_u32(0x004d9548u + (pad * 0x2cu));
                        uint32_t sig = state;

                        sig = (sig * 31u) + prev;
                        sig = (sig * 31u) + step;
                        sig = (sig * 31u) + desc;
                        if (sig != last_gate_sig) {
                            last_gate_sig = sig;
                            ++gate_lines;
                            fprintf(
                                stderr,
                                "recomp menugate: swap=%lu state=%" PRIu32
                                " prev=%" PRIu32 " step=0x%02" PRIx32
                                " b4=0x%08" PRIx32 " pad=0x%02" PRIx32
                                " desc=0x%08" PRIx32 " screen=%d\n",
                                (unsigned long)
                                    result.command.data.present.swap_counter,
                                state, prev, step, b4, pad, desc,
                                (int)*recomp_memory_i8(0x009d9d80u));
                        }
                    }
                }
                /* 0x000F27C0 advances 0x009D9DA8 toward the limit at
                   0x009D9DAC on every call. Sampling that tick separates a
                   dispatcher that stopped running from one that runs and
                   declines to leave its state.
                   The budget is 4000, not 20: at one line per 300 swaps a
                   20-line budget goes silent at swap 6000, which made a run
                   that was still advancing look identical to one that had
                   stopped. A later presenter failure then had no swap number
                   attached to it. */
                {
                    static uint32_t last_tick = 0xffffffffu;
                    static uint32_t tick_lines;
                    uint32_t tick = *recomp_memory_u32(0x009d9da8u);
                    unsigned long swap = (unsigned long)
                        result.command.data.present.swap_counter;

                    if (swap % 300ul == 0ul && tick != last_tick &&
                        tick_lines < 4000u) {
                        last_tick = tick;
                        ++tick_lines;
                       fprintf(
                           stderr,
                           "recomp uitick: swap=%lu tick=%" PRIu32
                           " limit=%" PRIu32 "\n",
                           swap, tick,
                           *recomp_memory_u32(0x009d9dacu));
                   }
               }
                /* Screen 0's update at 0x000F3E40 walks a 4-entry per-port
                   descriptor table at 0x003AF1D0 and turns a press into a UI
                   event by testing the port's own flag word at
                   0x004D9548 + port * 0x2C for 0x10 (back) or 0x20
                   (confirm). That flag word, not the raw pad mask, is what
                   the menu consumes, so report port 0's copy alongside the
                   pad edge to show where a press stops being propagated. */
                {
                    static uint32_t last_flags = 0xffffffffu;
                    static uint32_t flag_lines;
                    uint32_t flags = *recomp_memory_u32(0x004d9548u);

                    if (flags != last_flags && flag_lines < 40u) {
                        last_flags = flags;
                        ++flag_lines;
                       fprintf(
                           stderr,
                           "recomp padflags: swap=%lu port0=0x%08" PRIx32
                           " port1=0x%08" PRIx32 " enabled=%u\n",
                           (unsigned long)
                               result.command.data.present.swap_counter,
                           flags,
                           *recomp_memory_u32(0x004d9574u),
                           (unsigned)*recomp_memory_i8(0x005dece4u));
                   }
               }
                /* Scene 3's update at 0x000A4C40 leaves the scene only when
                   0x004D567C reads 1, and that byte is written at 0x000F360F
                   from the menu callback's argument. Sampling it once per
                   swap can miss a value the scene consumes and clears in the
                   same frame, so latch every distinct value ever seen. */
                {
                    static uint32_t ready_seen;
                    static uint32_t ready_lines;
                    uint32_t ready =
                        (uint32_t)(uint8_t)*recomp_memory_i8(0x004d567cu);
                    uint32_t bit = 1u << (ready & 31u);

                    if ((ready_seen & bit) == 0u && ready_lines < 12u) {
                        ready_seen |= bit;
                        ++ready_lines;
                        fprintf(
                            stderr,
                            "recomp ready: swap=%lu value=%" PRIu32
                            " branch=%u scene=%u\n",
                            (unsigned long)
                                result.command.data.present.swap_counter,
                            ready,
                            (unsigned)(uint8_t)*recomp_memory_i8(0x004d966au),
                            (unsigned)(uint8_t)*recomp_memory_i8(0x005dece0u));
                    }
                }
                /* Screen 1 idles in mode 0x101 at 0x009DA130 and leaves it
                   only when the fade byte 0x009DADF8 reaches 0xFF
                   (0x000F4716). That byte is driven by 0x000F38D0, which
                   advances only while the frame counter 0x009DAE00 is below
                   0x1E and interpolates toward the target 0x009DADFA. Report
                   the whole chain so a fade that never starts is
                   distinguishable from one that runs and stops short.
                   The budget is 4000, not 48: an earlier fade on
                   screen 0 consumes most of a small budget, so the
                   probe stops printing near swap 952 while the run
                   continues to swap 6000; a completed fade then looks
                   identical to a frozen one. */
                {
                    static uint32_t fade_lines;
                    static uint32_t last_mode = 0xfffffffeu;
                    static uint32_t last_count = 0xfffffffeu;
                    static unsigned last_cur = 0x1ffu;
                    uint32_t mode = *recomp_memory_u32(0x009da130u);
                    uint32_t count = *recomp_memory_u32(0x009dae00u);
                    unsigned cur = (unsigned)(uint8_t)
                        *recomp_memory_i8(0x009dadf8u);

                    if (fade_lines < 4000u &&
                        (mode != last_mode || count != last_count ||
                         cur != last_cur)) {
                        last_mode = mode;
                        last_count = count;
                        last_cur = cur;
                        ++fade_lines;
                        fprintf(
                            stderr,
                            "recomp fade: swap=%lu mode=0x%08" PRIx32
                            " count=%" PRIu32 " cur=%u target=%u\n",
                            (unsigned long)
                                result.command.data.present.swap_counter,
                           mode, count, cur,
                           (unsigned)(uint8_t)
                               *recomp_memory_i8(0x009dadfau));
                   }
               }
                /* Screen 1's enter fn 0x000F41B0 writes 0x009D99C8 = 2 at
                   0xF41E4, then 0x009D9A00 = 0xE at 0xF425C, then calls
                   0x000A4FB0, and only afterwards zeroes the fade counter
                   0x009DAE00 at 0xF42BE and sets the mode word 0x009DA130 to
                   0x101 at 0xF42FE. Those four are in program order, so
                   reporting them together localizes how far the enter fn got
                   before it stopped. */
                {
                    static uint32_t enter_lines;
                    static uint32_t last_sig = 0xfffffffeu;
                    uint32_t marker = *recomp_memory_u32(0x009d9a00u);
                    unsigned live = (unsigned)(uint8_t)
                        *recomp_memory_i8(0x004d9658u);
                    uint32_t sig = marker ^ (live * 0x9e3779b9u);

                    if (enter_lines < 24u && sig != last_sig) {
                        last_sig = sig;
                        ++enter_lines;
                        fprintf(
                            stderr,
                            "recomp enter1: swap=%lu marker=0x%08" PRIx32
                            " live=%u step=%" PRIu32 " mode=0x%08" PRIx32
                            "\n",
                            (unsigned long)
                                result.command.data.present.swap_counter,
                           marker, live,
                           *recomp_memory_u32(0x009d99c8u),
                           *recomp_memory_u32(0x009da130u));
                   }
               }
                /* Screen 1's enter fn reaches 0x000F39E0, which matches the
                   screen id byte at 0x009DA120 against the 5-entry key table
                   at 0x002A7AC8 {1,4,5,6,3} and returns early at 0xF3A0F on a
                   miss, skipping the fade reset and the mode word. It also
                   records the resolved index at 0x009DADFC. Report the id and
                   that index so a key miss is distinguishable from a match
                   that then failed downstream. */
                {
                    static uint32_t id_lines;
                    static uint32_t last_id_sig = 0xfffffffeu;
                    unsigned id = (unsigned)(uint8_t)
                        *recomp_memory_i8(0x009da120u);
                    uint32_t resolved = *recomp_memory_u32(0x009dadfcu);
                    uint32_t sig = resolved ^ (id * 0x01000193u);

                    if (id_lines < 24u && sig != last_id_sig) {
                        last_id_sig = sig;
                        ++id_lines;
                        fprintf(
                            stderr,
                            "recomp scrid: swap=%lu id=%u resolved=0x%08"
                            PRIx32 " screen=%d\n",
                            (unsigned long)
                                result.command.data.present.swap_counter,
                           id, resolved,
                           (int)*recomp_memory_i8(0x009d9d80u));
                   }
               }
                /* 0x000F3A11 spins on loc_000F3A40 waiting for resource
                   index 0x009DADFC to load, calling the fade animator and
                   presenting a frame each pass, so a resource that never
                   arrives looks exactly like a live render loop. The slot is
                   MEM32(MEM32(0x00317764)) + index*12 + 8 and holds -1 until
                   loaded, then points at an 'XPR0' (0x30525058) blob. */
                {
                    static uint32_t res_lines;
                    static uint32_t last_slot = 0xfffffffeu;
                    const uint32_t *root = recomp_memory_u32(0x00317764u);
                    uint32_t index = *recomp_memory_u32(0x009dadfcu);

                    /* The async request mailbox at 0x005DDE78 and the
                       128-entry priority queue it drains into at 0x005DD884.
                       Byte 0x005DDE79 is 0xFE while a request is staged, and
                       0x005DD885 is the head request's state byte. Report the
                       queue alongside the slot so a load that is queued but
                       never serviced is distinguishable from one never queued. */
                    {
                        static uint32_t queue_lines;
                        static uint32_t last_state = 0xffffffffu;
                        uint32_t staged = *recomp_memory_i8(0x005dde79u) & 0xffu;
                        uint32_t head = *recomp_memory_i8(0x005dd885u) & 0xffu;
                        uint32_t limit = *recomp_memory_i8(0x005dd882u) & 0xffu;
                        uint32_t active = *recomp_memory_i8(0x005dd880u) & 0xffu;
                        /* 0x000A32F0 polls a request only when its channel
                           handle (0x005DEC58 + ch*0x14 + 0x10) is non-NULL,
                           so a queued request whose ADXF channel was never
                           opened is skipped forever. */
                        uint32_t ch0 = *recomp_memory_u32(0x005dec68u);
                        uint32_t ch1 = *recomp_memory_u32(0x005dec7cu);
                        uint32_t state = (staged << 24) | (head << 16) |
                                         (limit << 8) | active;
                        /* Fold the poll counter into the dedup key. Without
                           it a live pump and a dead one look identical once
                           the queue bytes settle. Sample it coarsely so a
                           busy pump does not flood the log. */
                        state ^= (recomp_cri_service_adxf_get_stat_calls() /
                                  256u) << 12;

                        if (state != last_state && queue_lines < 24u) {
                            last_state = state;
                            ++queue_lines;
                            fprintf(
                                stderr,
                                "recomp resq: swap=%lu staged=0x%02" PRIx32
                                " head=0x%02" PRIx32 " limit=%" PRIu32
                                " active=%" PRIu32 " stat=%" PRIu32
                                " ch0=0x%08" PRIx32 " ch1=0x%08" PRIx32
                                " wsteps=%" PRIu32 "\n",
                                (unsigned long)result.command.data.present
                                    .swap_counter,
                                staged, head, limit, active,
                                recomp_cri_service_adxf_get_stat_calls(),
                                ch0, ch1,
                                recomp_cri_service_file_worker_steps());
                        }
                    }

                    if (root != NULL && *root != 0u && index < 64u) {
                        const uint32_t *ctx = recomp_memory_u32(*root);
                        const uint32_t *slot =
                            ctx != NULL
                                ? recomp_memory_u32(*ctx + index * 12u + 8u)
                                : NULL;

                        if (slot != NULL && *slot != last_slot &&
                            res_lines < 20u) {
                            const uint32_t *blob =
                                *slot != 0xffffffffu
                                    ? recomp_memory_u32(*slot)
                                    : NULL;

                            last_slot = *slot;
                            ++res_lines;
                            fprintf(
                                stderr,
                                "recomp xpr: swap=%lu index=%" PRIu32
                                " slot=0x%08" PRIx32 " magic=0x%08" PRIx32
                                "\n",
                                (unsigned long)
                                    result.command.data.present.swap_counter,
                                index, *slot,
                                blob != NULL ? *blob : 0u);
                        }
                    }
                }
            }
        }
    }
#endif
    /* Observation only: the draw seam reports transforms from inside a draw,
       but a run that never reaches DrawIndexedVertices then says nothing
       about whether the transform slots densified. Dump them from the swap
       path, which every run reaches, so the packed-SSE question is
       answerable independently of the draw. The first swap happens before
       any SetTransform, so this waits for a late swap. */
    {
        static bool xform_reported;
        const char *at_text = getenv("RECOMP_XFORM_DUMP_AT");
        unsigned long dump_at = at_text != NULL ? strtoul(at_text, NULL, 10) : 550ul;

        if (!xform_reported && frame_device_address != 0u &&
            (unsigned long)result.command.data.present.swap_counter >= dump_at) {
            xform_reported = true;
            for (uint32_t slot = 0u; slot < 7u; ++slot) {
                uint32_t address =
                    frame_device_address + 0x0810u + slot * 0x0040u;
                const uint32_t *m = recomp_memory_u32(address);
                float f[16];
                uint32_t nonzero = 0u;

                if (m == NULL) {
                    continue;
                }
                memcpy(f, m, sizeof f);
                for (uint32_t i = 0u; i < 16u; ++i) {
                    if (f[i] != 0.0f) {
                        ++nonzero;
                    }
                }
                fprintf(
                    stderr,
                    "recomp d3d swap-xform[%u] nonzero=%u "
                    "%g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g\n",
                    slot, nonzero,
                    f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                    f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
            }
            fprintf(
                stderr,
                "recomp d3d swap-xform: draws submitted=%u declined=%u\n",
                (unsigned)recomp_d3d_draw_adapter_submitted(),
                (unsigned)recomp_d3d_draw_adapter_declined());
            recomp_d3d_draw_adapter_report_fvf();
            recomp_d3d_render_state_adapter_report();
            recomp_d3d_texture_adapter_report();
            recomp_d3d_presenter_report_draw_textures();
#ifdef RECOMP_FULL_PROGRAM
            recomp_fiber_adapter_report();
#endif
        }
    }
    recomp_runtime.registers.eax =
        result.command.data.present.swap_counter;
    recomp_runtime.registers.esp = entry_esp + 8u;
}

static void set_gamma_ramp(void)
{
    const uint32_t entry_esp = recomp_runtime.registers.esp;
    RecompD3dPresenterCommand command = {0};
    const uint32_t address = stack_argument(entry_esp, 1u);
    const uint8_t *ramp = recomp_memory(address, sizeof command.data.gamma);
    RecompD3dPresenterError error;

    if (address == 0u || ramp == NULL) {
        recomp_stop(2, "d3d-gamma:invalid-ramp");
    }
    command.type = RECOMP_D3D_PRESENTER_COMMAND_GAMMA;
    memcpy(command.data.gamma, ramp, sizeof command.data.gamma);
    error = recomp_d3d_presenter_submit(presenter, &command);
    recomp_d3d_frame_adapter_exit_if_closed(error);
    if (error != RECOMP_D3D_PRESENTER_OK) {
        recomp_stop(2, "d3d-gamma:presenter:%u", (unsigned)error);
    }
    recomp_runtime.registers.esp = entry_esp + 12u;
}

RecompFunction recomp_d3d_frame_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case D3D_DEVICE_SET_GAMMA_ADDRESS:
        return set_gamma_ramp;
    case D3D_DEVICE_CLEAR_ADDRESS:
        return recomp_d3d_clear_adapter;
    case D3D_DEVICE_SWAP_ADDRESS:
        return recomp_d3d_swap_adapter;
    default:
        return NULL;
    }
}
