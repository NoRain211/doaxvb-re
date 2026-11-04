/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Adapted from doa2u-re's netplay_session.cpp: the frame loop, prediction,
   frame sync and desync checks are the same; the frame boundary, hidden
   frames and pads use this runtime's seams. */
#include "netplay_session.h"

#include "d3d_frame_adapter.h"
#include "d3d_vblank.h"
#include "input_host_win32.h"
#include "input_pulse_source.h"
#include "netplay.h"
#include "netplay_game.h"
#include "rollback.h"
#include "synctest.h"
#include "xapi_time_adapter.h"
extern "C" {
#include "game_scheduler.h"
#include "kernel_abi.h"
#include "runtime.h"
#include "stop_report.h"
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

/* RECOMP_ROLLBACK=loopback plays a generated remote player on port 0 whose
   pads arrive RECOMP_ROLLBACK_LAG frames late. RECOMP_ROLLBACK=udp:... plays a
   peer runner; RECOMP_ROLLBACK_PLAYER (0 or 1) is the local port and
   RECOMP_ROLLBACK_DELAY the input delay, both matching on the two sides.
   Sessions start at loop pass RECOMP_ROLLBACK_START. At session frame
   RECOMP_ROLLBACK_FRAMES every pad is confirmed and guest RAM goes to
   RECOMP_ROLLBACK_DUMP. */
enum class RollbackMode { Off, Loopback, Udp };
struct RollbackRun {
    RollbackMode mode = RollbackMode::Off;
    bool active = false;
    bool hidden = false;
    bool interrupted = false;
    bool walking = false; // the menus are walked to Two Players for both sides
    std::uint32_t pass = 0u; // loop passes since boot
    std::uint32_t start = 0u, stop = UINT32_MAX, lag = 4u, delay = 0u, player = 0u;
    std::uint32_t frontier = 0u; // first session frame never run
    std::uint32_t rollbacks = 0u, replayed = 0u, waited = 0u, saves = 0u;
    std::uint32_t wait = 0u, paced = UINT32_MAX;
    std::chrono::steady_clock::time_point next_frame{};
    std::int32_t advantage = 0;
    std::chrono::steady_clock::time_point started;
    const char *dump = nullptr;
    std::uint32_t checkFrame = 0u, compared = 0u, matched = 0u;
    std::uint64_t checkHash = 0u;
    double slowestHashMs = 0.0;
    RecompRollbackSession session;
    RecompNetplay net;
    RecompInputGamepad pads[2]{}; // the running frame's pads by guest port
};
static RollbackRun rollback;
static RecompNetplaySessionHost host;

// A new pad nearly every frame, so repeating the last pad mispredicts.
static RecompInputGamepad generatedPad(std::uint32_t frame)
{
    std::uint32_t x = frame * 2654435761u;
    x ^= x >> 13;
    RecompInputGamepad pad{};
    pad.buttons = static_cast<std::uint16_t>(x & 0x000fu); // d-pad
    pad.analog_buttons[(x >> 4) & 3u] = (x & 0x40u) != 0u ? 0xffu : 0u; // A, B, X or Y
    return pad;
}

static RecompInputGamepad localPad(std::uint32_t frame)
{
    RecompInputGamepad pad{};
    if (rollback.mode == RollbackMode::Loopback) return pad;
    /* Scripted two-runner tests give each player its own stream in live
       rallies and press A now and then in character select, so both pick. */
    if (std::getenv("RECOMP_ROLLBACK_GENERATED") != nullptr) {
        if (recomp_netplay_game_rally_live()) return generatedPad(frame * 2u + rollback.player);
        if (recomp_netplay_game_character_select() && frame % 30u == rollback.player * 15u)
            pad.analog_buttons[RECOMP_INPUT_ANALOG_A] = RECOMP_INPUT_ANALOG_PRESSED;
        return pad;
    }
    (void)recomp_input_host_sample(0u, &pad);
    return pad;
}

static void receivePads()
{
    RecompRollbackSession &s = rollback.session;
    if (rollback.mode == RollbackMode::Udp) {
        recomp_netplay_receive(&rollback.net, &s);
        return;
    }
    const std::uint32_t lag = s.frame >= rollback.stop ? 0u : rollback.lag;
    while (s.remote_count + lag <= s.frame) {
        const RecompInputGamepad pad = generatedPad(s.remote_count);
        recomp_rollback_add_remote(&s, &pad);
    }
}

static void endSession(const char *reason)
{
    if (rollback.mode == RollbackMode::Udp) recomp_netplay_send_goodbye(&rollback.net);
    recomp_stop(4, "%s", reason);
}

/* A stalled session ends after its grace: 15 s once the other player has
   been heard, 120 s while their runner may still be booting. A goodbye from
   the other runner ends it at once. */
static void stalled(std::chrono::steady_clock::time_point began)
{
    using namespace std::chrono;
    if (rollback.net.peer_left) {
        std::fprintf(stderr, "recomp netplay: the other player left at frame %u\n", rollback.session.frame);
        recomp_stop(4, "rollback:peer-left");
    }
    const bool heard = rollback.net.peer_seen;
    const auto waited = steady_clock::now() - began;
    if (waited > seconds(heard ? 15 : 120)) endSession("rollback:peer-timeout");
    if (waited >= seconds(1) && !rollback.interrupted && heard) {
        std::fprintf(stderr, "recomp netplay: the other player went quiet at frame %u\n", rollback.session.frame);
        rollback.interrupted = true;
    }
}

static void resumed(std::chrono::steady_clock::time_point began)
{
    if (!rollback.interrupted) return;
    rollback.interrupted = false;
    std::fprintf(stderr, "recomp netplay: the other player is back after %.1f s\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
}

static void compareChecks();

/* At the newest frame: gathers pads until the frame may run or a rewind is
   due, and returns the frame to load or RECOMP_ROLLBACK_NONE. */
static std::uint32_t awaitFrontier(bool last)
{
    using Clock = std::chrono::steady_clock;
    RecompRollbackSession &s = rollback.session;
    const bool udp = rollback.mode == RollbackMode::Udp;
    Clock::time_point sent{};
    const Clock::time_point began = Clock::now();
    for (;;) {
        receivePads();
        if (udp) compareChecks();
        // The local pad is read at the frame's start, so pacing adds no input lag.
        const bool started = !udp || Clock::now() >= rollback.next_frame;
        const std::uint32_t target = started ? recomp_rollback_take_rewind(&s) : RECOMP_ROLLBACK_NONE;
        if (target != RECOMP_ROLLBACK_NONE) {
            resumed(began);
            return target;
        }
        if (started) {
            while (s.local_count <= s.frame + rollback.delay) {
                const RecompInputGamepad pad = localPad(s.local_count);
                recomp_rollback_add_local(&s, &pad);
            }
        }
        const bool ready = last
            ? s.remote_count >= rollback.stop && (!udp || rollback.net.peer_ack >= rollback.stop)
            : started && recomp_rollback_can_run(&s) && (!udp || rollback.net.peer_seen);
        if (ready) {
            resumed(began);
            return RECOMP_ROLLBACK_NONE;
        }
        if (!udp) recomp_stop(4, "rollback:stalled");
        const Clock::time_point now = Clock::now();
        if (now - sent >= std::chrono::milliseconds(rollback.net.match.relay ? 16 : 4)) {
            recomp_netplay_send(&rollback.net, &s, rollback.advantage);
            sent = now;
        }
        stalled(began);
        Clock::time_point until = now + std::chrono::milliseconds(2);
        if (rollback.next_frame > now && rollback.next_frame < until) until = rollback.next_frame;
        recomp_d3d_sleep_until(std::chrono::duration_cast<std::chrono::nanoseconds>(until.time_since_epoch()).count());
    }
}

// Guest RAM without SyncTest's host-scheduled ranges, to RECOMP_ROLLBACK_DUMP when set.
static void writeDump()
{
    if (rollback.dump == nullptr) return;
    std::vector<std::uint8_t> ram(host.guestRam, host.guestRam + host.guestRamSize);
    recomp_synctest_mask_ignored(ram.data(), ram.size());
    std::ofstream out(rollback.dump, std::ios::binary);
    out.write(reinterpret_cast<const char *>(ram.data()), static_cast<std::streamsize>(ram.size()));
    if (!out) recomp_stop(4, "rollback:dump");
}

static void finishRollback()
{
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - rollback.started).count();
    std::fprintf(stderr, "recomp rollback: frame %u reached after %u rollbacks "
        "(%u frames run again, %u waited, %u saves) in %.1f s\n",
        rollback.session.frame, rollback.rollbacks, rollback.replayed, rollback.waited, rollback.saves, seconds);
    writeDump();
    if (rollback.mode == RollbackMode::Udp) {
        // shortcut: the peer may still need our acknowledgement; repeat it briefly instead of a close handshake.
        for (int i = 0; i < 50; ++i) {
            recomp_netplay_send(&rollback.net, &rollback.session, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    recomp_stop(0, "rollback:done");
}

/* Schedules each new frame on a 60 Hz grid; a frame sync wait moves the grid. */
static void scheduleFrame()
{
    using namespace std::chrono;
    constexpr steady_clock::duration interval = nanoseconds(1000000000 / 60);
    const steady_clock::time_point now = steady_clock::now();
    // Late frames may catch up by the two the presenter queues.
    rollback.next_frame = std::max(rollback.next_frame + interval, now - 2 * interval);
    if (rollback.wait != 0u) rollback.next_frame = std::max(rollback.next_frame, now) + rollback.wait * interval;
    rollback.wait = 0u;
}

/* Every 30th frame of a UDP session, each side hashes guest RAM as the frame
   starts, without SyncTest's host-scheduled ranges. Once the other player's
   pads before that frame have all arrived, no rewind reaches it again, so the
   hash is final and rides in each packet. */
constexpr std::uint32_t desyncCheckFrames = 30u;

static bool isCheckFrame(std::uint32_t frame)
{
    return rollback.mode == RollbackMode::Udp && frame != 0u && frame % desyncCheckFrames == 0u;
}

static void hashCheckFrame()
{
    const std::uint32_t frame = rollback.session.frame;
    if (!isCheckFrame(frame)) return;
    const auto began = std::chrono::steady_clock::now();
    RecompRamRange ignored[RECOMP_SNAPSHOT_HASH_RANGES];
    const std::size_t count = recomp_synctest_ignored_ranges(ignored, RECOMP_SNAPSHOT_HASH_RANGES);
    if (!recomp_rollback_ring_hash(ignored, count, &rollback.checkHash)) recomp_stop(4, "rollback:hash");
    rollback.checkFrame = frame;
    rollback.slowestHashMs = std::max(rollback.slowestHashMs,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
}

/* Only frames starting without the other player's pad need a save, plus
   check frames, whose hash reads the newest save. */
static bool saveFrame()
{
    const RecompRollbackSession &s = rollback.session;
    if (s.frame < s.remote_count && !isCheckFrame(s.frame)) return true;
    ++rollback.saves;
    return recomp_rollback_ring_save(s.frame);
}

static void confirmCheck()
{
    if (rollback.checkFrame != 0u && rollback.session.remote_count >= rollback.checkFrame) {
        recomp_netplay_add_check(&rollback.net, rollback.checkFrame, rollback.checkHash);
        rollback.checkFrame = 0u;
    }
}

static void compareChecks()
{
    RecompNetplay &net = rollback.net;
    std::uint64_t mine = 0u;
    if (!recomp_netplay_own_check(&net, &mine) || net.peer_check_frame == rollback.compared) return;
    rollback.compared = net.peer_check_frame;
    if (mine == net.peer_check_hash) {
        if (++rollback.matched % 20u == 0u) {
            std::fprintf(stderr, "recomp netplay: guest RAM matches the other player's through frame %u "
                "(slowest check %.2f ms)\n", rollback.compared, rollback.slowestHashMs);
            rollback.slowestHashMs = 0.0;
        }
        return;
    }
    std::fprintf(stderr, "recomp netplay: desync at frame %u: guest RAM hash %016llx here, %016llx there\n",
        rollback.compared, static_cast<unsigned long long>(mine),
        static_cast<unsigned long long>(net.peer_check_hash));
    writeDump();
    for (int i = 0; i < 3; ++i) recomp_netplay_send(&net, &rollback.session, rollback.advantage);
    endSession("rollback:desync");
}

static void setHidden(bool hidden)
{
    rollback.hidden = hidden;
    recomp_d3d_frame_adapter_hold_present(RECOMP_PRESENT_HOLD_ROLLBACK, hidden);
}

static void setWindow(std::uint32_t window)
{
    RecompRollbackSession &s = rollback.session;
    if (s.window == window) return;
    s.window = window;
    std::fprintf(stderr, "recomp netplay: %s from frame %u\n", window != 0u ? "rollback" : "lockstep", s.frame);
}

/* A profile write cannot be rewound (synctest_modules.c), so it waits until
   the other player's pads through the running frame have arrived. The window
   is normally closed long before, and this never waits. A pad that differs
   from the prediction means the frame is already wrong; the session then
   ends before the write. */
static void confirmSaveFrame()
{
    RecompRollbackSession &s = rollback.session;
    if (!rollback.active || rollback.mode != RollbackMode::Udp || s.frame == 0u) return;
    const std::uint32_t running = s.frame - 1u;
    if (s.remote_count <= running)
        std::fprintf(stderr, "recomp netplay: save in frame %u waits for the other player's input\n", running);
    const auto began = std::chrono::steady_clock::now();
    while (s.remote_count <= running) {
        recomp_netplay_receive(&rollback.net, &s);
        recomp_netplay_send(&rollback.net, &s, rollback.advantage);
        stalled(began);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    resumed(began);
    if (s.rewind_to <= running) endSession("rollback:save-mispredicted");
}

static void onFrame()
{
    recomp_xapi_frame_clock_advance();
    RecompRollbackSession &s = rollback.session;
    if (!rollback.active) {
        if (++rollback.pass < rollback.start) return;
        recomp_rollback_init(&s, rollback.delay);
        if (!recomp_rollback_ring_init(RECOMP_ROLLBACK_WINDOW)) recomp_stop(4, "rollback:ring");
        rollback.active = true;
        rollback.walking = rollback.mode == RollbackMode::Udp && rollback.start <= 1u;
        rollback.started = std::chrono::steady_clock::now();
        // The session paces every frame; the scheduler's own 60 Hz wait would only add lag.
        recomp_game_set_paced(false);
        std::fprintf(stderr, "recomp rollback: session starts at pass %u as player %u\n",
            rollback.pass, rollback.player);
    }
    bool saved = true;
    if (s.frame == rollback.frontier) {
        if (rollback.mode == RollbackMode::Udp)
            setWindow(recomp_netplay_game_rally_live() ? RECOMP_ROLLBACK_WINDOW : 0u);
        if (rollback.mode == RollbackMode::Udp && rollback.paced != s.frame) {
            scheduleFrame();
            rollback.paced = s.frame;
        }
        const std::uint32_t target = awaitFrontier(s.frame == rollback.stop);
        if (target != RECOMP_ROLLBACK_NONE) {
            if (!recomp_rollback_ring_load(target)) endSession("rollback:unrestorable");
            ++rollback.rollbacks;
            rollback.replayed += rollback.frontier - target;
        } else if (s.frame == rollback.stop) {
            finishRollback();
            return;
        } else {
            if (rollback.mode == RollbackMode::Udp) {
                confirmCheck();
                compareChecks();
            }
            saved = saveFrame();
            if (rollback.mode == RollbackMode::Udp) {
                rollback.advantage = static_cast<std::int32_t>(s.frame - rollback.net.peer_frame);
                recomp_netplay_send(&rollback.net, &s, rollback.advantage);
                rollback.wait = recomp_netplay_wait_frames(&rollback.net, rollback.advantage);
                rollback.waited += rollback.wait;
            }
        }
    } else {
        saved = saveFrame();
    }
    if (!saved) recomp_stop(4, "rollback:save");
    hashCheckFrame();
    // Frames run again were already shown and heard with the predicted pads.
    setHidden(s.frame < rollback.frontier);
    recomp_rollback_run(&s, &rollback.pads[rollback.player], &rollback.pads[1u - rollback.player]);
    // Both sides set the walk's pads the same way from the same guest state.
    if (rollback.walking) {
        if (recomp_netplay_game_walk(s.frame, &rollback.pads[0])) {
            rollback.walking = false;
            std::fprintf(stderr, "recomp netplay: Two Players character select from frame %u\n", s.frame);
        }
        rollback.pads[1] = RecompInputGamepad{};
    }
    if (s.frame > rollback.frontier) rollback.frontier = s.frame;
}

bool recomp_netplay_session_read_env()
{
    const char *mode = std::getenv("RECOMP_ROLLBACK");
    if (mode == nullptr || *mode == '\0' || std::strcmp(mode, "0") == 0) return true;
    const auto number = [](const char *name, std::uint32_t &value) {
        if (const char *text = std::getenv(name); text != nullptr && *text != '\0')
            value = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10));
    };
    number("RECOMP_ROLLBACK_START", rollback.start);
    number("RECOMP_ROLLBACK_FRAMES", rollback.stop);
    rollback.dump = std::getenv("RECOMP_ROLLBACK_DUMP");
    if (std::strcmp(mode, "loopback") == 0) {
        rollback.mode = RollbackMode::Loopback;
        rollback.player = 1u;
        if (std::getenv("RECOMP_ROLLBACK_FRAMES") == nullptr) rollback.stop = 600u;
        number("RECOMP_ROLLBACK_LAG", rollback.lag);
        if (rollback.lag <= RECOMP_ROLLBACK_WINDOW) return true;
        std::fprintf(stderr, "recomp runner: RECOMP_ROLLBACK_LAG is at most %u\n", RECOMP_ROLLBACK_WINDOW);
        return false;
    }
    // udp:<local port>:<peer host>:<peer port>
    const std::string spec = std::strncmp(mode, "udp:", 4u) == 0 ? mode + 4 : "";
    const std::size_t hostAt = spec.find(':'), portAt = spec.rfind(':');
    if (hostAt == std::string::npos || portAt == hostAt) {
        std::fprintf(stderr, "recomp runner: RECOMP_ROLLBACK is loopback or udp:<local port>:<peer host>:<peer port>\n");
        return false;
    }
    const unsigned long localPort = std::strtoul(spec.substr(0u, hostAt).c_str(), nullptr, 10);
    const unsigned long peerPort = std::strtoul(spec.substr(portAt + 1u).c_str(), nullptr, 10);
    const std::string peerHost = spec.substr(hostAt + 1u, portAt - hostAt - 1u);
    number("RECOMP_ROLLBACK_PLAYER", rollback.player);
    number("RECOMP_ROLLBACK_DELAY", rollback.delay);
    if (localPort == 0u || localPort > 65535u || peerPort == 0u || peerPort > 65535u ||
        rollback.player > 1u || rollback.delay > RECOMP_ROLLBACK_WINDOW) {
        std::fprintf(stderr, "recomp runner: bad RECOMP_ROLLBACK ports, player or delay\n");
        return false;
    }
    rollback.mode = RollbackMode::Udp;
    if (recomp_netplay_open(&rollback.net, static_cast<std::uint16_t>(localPort), peerHost.c_str(),
            static_cast<std::uint16_t>(peerPort)))
        return true;
    std::fprintf(stderr, "recomp runner: cannot open UDP port %lu\n", localPort);
    return false;
}

bool recomp_netplay_session_enabled()
{
    return rollback.mode != RollbackMode::Off;
}

void recomp_netplay_session_install(const RecompNetplaySessionHost &runner)
{
    host = runner;
    // Guest time must be a function of the frame from boot, so both runners read the same times.
    recomp_xapi_frame_clock_set_enabled(true);
    recomp_synctest_install_default_modules();
    recomp_synctest_set_guest_ram(host.guestRam, host.guestRamSize, host.writeWatch);
    recomp_game_set_frame_boundary(onFrame);
    recomp_kernel_profile_mutation_hook = confirmSaveFrame;
}

bool recomp_netplay_session_pad(std::uint32_t port, RecompInputGamepad *pad, bool *present)
{
    if (!rollback.active) return false;
    // Both runners must see the same controllers: the session's two ports.
    // Loopback has no local player, so its port stays empty.
    *present = port < 2u && !(rollback.mode == RollbackMode::Loopback && port == rollback.player);
    if (*present) *pad = rollback.pads[port];
    return true;
}

void recomp_netplay_session_exit()
{
    if (rollback.mode == RollbackMode::Udp && rollback.active && rollback.session.frame < rollback.stop)
        recomp_netplay_send_goodbye(&rollback.net);
}
