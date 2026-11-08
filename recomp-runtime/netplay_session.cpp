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
#include "netplay_launch.h"
#include "netplay_menu.h"
#include "netplay_profile.h"
#include "netplay_settings.h"
#include "overlay_screen.h"
#include "replay.h"
#include "rollback.h"
#include "save_transaction.h"
#include "synctest.h"
#include "xapi_time_adapter.h"
extern "C" {
#include "dsound_service_adapter.h"
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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
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
enum class RollbackMode { Off, Loopback, Udp, Replay };
struct RollbackRun {
    RollbackMode mode = RollbackMode::Off;
    bool active = false;
    bool hidden = false;
    bool interrupted = false;
    bool walking = false; // the menus are walked to Two Players for both sides
    bool fromRoom = false; // a room started this runner, so an ending session goes back to the menu
    RecompNetplayLaunch launch; // a room's session hands its socket and state back (recomp_netplay_session_return_args)
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
    // A lobby session's play folder, build, host options and saves; a replay keeps them.
    bool lobby = false;
    std::filesystem::path disc;
    std::string digest;
    std::uint8_t flags = 0u;
    std::vector<std::uint8_t> hostSaves;
    std::ofstream replayFile; // the replay being recorded (startReplay)
    RecompReplayWriter replayWriter{};
    std::uint32_t replayFrames = 0u;
    std::vector<std::uint8_t> replayData; // --replay's file
    RecompReplayReader replayReader{};
    // The corner display (drawSessionOverlay).
    int networkDisplay = 0;
    std::uint32_t rewindDepth = 0u;
    std::chrono::steady_clock::time_point rewoundAt{};
    bool padSeen = false; // this player has used a gamepad, so losing it shows a notice
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

static void restoreSettings();
static void closeReplay();

/* Sessions from the command line stop with exit code 4; a room's session
   starts the runner again offline, back on the online menu. */
static void endSession(const char *reason)
{
    if (rollback.mode == RollbackMode::Udp) recomp_netplay_send_goodbye(&rollback.net);
    closeReplay();
    if (!rollback.fromRoom) recomp_stop(4, "%s", reason);
    std::fprintf(stderr, "recomp netplay: session ended (%s); back to the room\n", reason);
    recomp_netplay_close(&rollback.net);
    restoreSettings();
    recomp_runner_relaunch(recomp_netplay_session_return_args(rollback.launch, reason));
}

// When the panel over the last frame was last drawn (showNotice).
static std::chrono::steady_clock::time_point noticeShown;

// A panel over the last frame, shown again now, while the game waits at a frame boundary.
static void showNotice(const char *title, const std::string &detail)
{
    if (rollback.walking) recomp_overlay_rect(-1000.0f, 0.0f, 2640.0f, 480.0f, 0xff000000u); // the walk stays covered
    // opaque, so each redraw looks the same
    recomp_overlay_rect(120.0f, 194.0f, 400.0f, 92.0f, 0xff181820u);
    recomp_overlay_rect(120.0f, 194.0f, 400.0f, 4.0f, 0xffd8283cu);
    recomp_overlay_text(320.0f - recomp_overlay_text_width(title, 3.0f) / 2.0f, 214.0f, 3.0f, 0xffffffffu, title);
    recomp_overlay_text(320.0f - recomp_overlay_text_width(detail.c_str(), 2.0f) / 2.0f, 252.0f, 2.0f, 0xffa8a8b0u,
        detail.c_str());
    recomp_overlay_screen_publish();
    recomp_d3d_frame_adapter_present_again();
    noticeShown = std::chrono::steady_clock::now();
}

/* A stalled session shows a notice over its last frame after 1 s, which also
   keeps the window answering, and ends after its grace: 15 s once the other
   player has been heard, 120 s while their runner may still be booting. A
   goodbye from the other runner ends it at once. Mid-frame waits (atBoundary
   false) only log: the frame being drawn is not finished. */
static void stalled(std::chrono::steady_clock::time_point began, bool atBoundary = true)
{
    using namespace std::chrono;
    if (rollback.net.peer_left) {
        std::fprintf(stderr, "recomp netplay: the other player left at frame %u\n", rollback.session.frame);
        if (atBoundary) {
            showNotice("OPPONENT LEFT", "ENDING THE SESSION");
            std::this_thread::sleep_for(seconds(2));
        }
        endSession("rollback:peer-left");
    }
    const auto now = steady_clock::now();
    const bool heard = rollback.net.peer_seen;
    const seconds grace(heard ? 15 : 120);
    const auto waited = now - began;
    if (waited > grace) endSession("rollback:peer-timeout");
    if (waited < seconds(1)) return;
    if (!rollback.interrupted && heard)
        std::fprintf(stderr, "recomp netplay: the other player went quiet at frame %u\n", rollback.session.frame);
    rollback.interrupted = true;
    if (!atBoundary || now - noticeShown < milliseconds(100)) return;
    showNotice(heard ? "WAITING FOR OPPONENT" : "CONNECTING",
        "ENDING THE SESSION IN " + std::to_string(duration_cast<seconds>(grace - waited).count() + 1) + " S");
}

static void resumed(std::chrono::steady_clock::time_point began)
{
    if (!rollback.interrupted) return;
    rollback.interrupted = false;
    // Takes the notice down; the network display, when on, draws again at the next Swap.
    recomp_overlay_screen_publish();
    std::fprintf(stderr, "recomp netplay: the other player is back after %.1f s\n",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count());
}

static void compareChecks();

/* Each lobby session records a replay into the player's own
   .recomp-storage/replays. It stays on this computer: it holds the host's
   saves. Frames go in once they have run and both pads have arrived, so the
   file holds only confirmed input. */
static void startReplay()
{
    namespace fs = std::filesystem;
    const fs::path folder = rollback.disc / ".recomp-storage" / "replays";
    char stamp[24], name[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    std::error_code error;
    fs::create_directories(folder, error);
    // A second session within the same second gets a suffix instead of truncating the first one's replay.
    std::snprintf(name, sizeof name, "%s.dxbr", stamp);
    for (unsigned n = 2u; fs::exists(folder / name, error); ++n)
        std::snprintf(name, sizeof name, "%s-%u.dxbr", stamp, n);
    rollback.replayFile.open(folder / name, std::ios::binary | std::ios::trunc);
    RecompReplayInfo info{};
    std::memcpy(info.digest, rollback.digest.data(), std::min(rollback.digest.size(), sizeof info.digest));
    info.player = static_cast<std::uint8_t>(rollback.player);
    info.delay = static_cast<std::uint8_t>(rollback.delay);
    info.flags = rollback.flags;
    info.start_frame = rollback.pass;
    info.blob_size[0] = static_cast<std::uint32_t>(rollback.hostSaves.size()); // the joiner sends none
    std::vector<std::uint8_t> header(RECOMP_REPLAY_HEADER_BYTES + rollback.hostSaves.size());
    header.resize(recomp_replay_header(&rollback.replayWriter, &info, rollback.hostSaves.data(),
        rollback.hostSaves.data(), header.data()));
    rollback.replayFile.write(reinterpret_cast<const char *>(header.data()),
        static_cast<std::streamsize>(header.size()));
    rollback.replayFile.flush();
    rollback.replayFrames = 0u;
    if (rollback.replayFile) {
        std::fprintf(stderr, "recomp netplay: recording replay %s\n", name);
        return;
    }
    std::fprintf(stderr, "recomp netplay: cannot write replay %s; playing without one\n", name);
    rollback.replayFile.close();
}

static void recordReplay()
{
    if (!rollback.replayFile.is_open()) return;
    const RecompRollbackSession &s = rollback.session;
    const std::uint32_t confirmed = std::min({s.local_count, s.remote_count, rollback.frontier});
    std::uint8_t bytes[RECOMP_REPLAY_FRAME_BYTES];
    for (; rollback.replayFrames < confirmed; ++rollback.replayFrames) {
        RecompInputGamepad pads[2];
        pads[rollback.player] = s.local[rollback.replayFrames % RECOMP_ROLLBACK_HISTORY];
        pads[1u - rollback.player] = s.remote[rollback.replayFrames % RECOMP_ROLLBACK_HISTORY];
        rollback.replayFile.write(reinterpret_cast<const char *>(bytes),
            static_cast<std::streamsize>(recomp_replay_frame(&rollback.replayWriter, pads, bytes)));
        // About once a second, so a crash loses little.
        if (rollback.replayFrames % 60u == 59u) {
            rollback.replayFile.write(reinterpret_cast<const char *>(bytes),
                static_cast<std::streamsize>(recomp_replay_flush(&rollback.replayWriter, bytes)));
            rollback.replayFile.flush();
        }
    }
}

static void closeReplay()
{
    if (!rollback.replayFile.is_open()) return;
    recordReplay();
    std::uint8_t bytes[RECOMP_REPLAY_FRAME_BYTES];
    rollback.replayFile.write(reinterpret_cast<const char *>(bytes),
        static_cast<std::streamsize>(recomp_replay_flush(&rollback.replayWriter, bytes)));
    rollback.replayFile.close();
    std::fprintf(stderr, "recomp netplay: replay %s with %u frames\n", rollback.replayFile ? "kept" : "cut short",
        rollback.replayFrames);
}

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
    closeReplay();
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
    const std::size_t count = recomp_synctest_ignored_ranges(ignored);
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
        stalled(began, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    resumed(began);
    if (s.rewind_to <= running) endSession("rollback:save-mispredicted");
}

// Both sides, and a replay, set the walk's pads the same way from the same guest state.
static void walkPads()
{
    if (!rollback.walking) return;
    if (recomp_netplay_game_walk(rollback.session.frame, &rollback.pads[0])) {
        rollback.walking = false;
        recomp_dsound_service_adapter_set_muted(0);
        std::fprintf(stderr, "recomp netplay: Two Players character select from frame %u\n", rollback.session.frame);
    }
    rollback.pads[1] = RecompInputGamepad{};
}

/* Starts the session or replay at loop pass RECOMP_ROLLBACK_START; false
   before that pass. */
static bool startSession()
{
    if (rollback.active) return true;
    if (++rollback.pass < rollback.start) return false;
    const bool replay = rollback.mode == RollbackMode::Replay;
    recomp_rollback_init(&rollback.session, replay ? 0u : rollback.delay);
    if (!replay && !recomp_rollback_ring_init(RECOMP_ROLLBACK_WINDOW)) recomp_stop(4, "rollback:ring");
    rollback.active = true;
    rollback.walking = rollback.mode != RollbackMode::Loopback && rollback.start <= 1u;
    // The walk to character select is neither seen (drawSessionOverlay) nor heard.
    recomp_dsound_service_adapter_set_muted(rollback.walking);
    rollback.started = std::chrono::steady_clock::now();
    std::fprintf(stderr, "recomp rollback: %s starts at pass %u as player %u\n", replay ? "replay" : "session",
        rollback.pass, rollback.player);
    if (rollback.lobby) startReplay();
    return true;
}

static void onFrame()
{
    recomp_xapi_frame_clock_advance();
    if (!startSession()) return;
    RecompRollbackSession &s = rollback.session;
    recordReplay();
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
            const auto now = std::chrono::steady_clock::now();
            const std::uint32_t depth = rollback.frontier - target;
            const bool recent = now - rollback.rewoundAt < std::chrono::seconds(1);
            rollback.rewindDepth = recent ? std::max(rollback.rewindDepth, depth) : depth;
            rollback.rewoundAt = now;
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
    walkPads();
    if (s.frame > rollback.frontier) rollback.frontier = s.frame;
}

/* --replay plays a recorded session's pads from the same first pass, as
   onFrame runs confirmed ones, at the scheduler's own 60 Hz, and stops at
   the end of the file. */
static void onReplayFrame()
{
    recomp_xapi_frame_clock_advance();
    if (!startSession()) return;
    RecompRollbackSession &s = rollback.session;
    if (s.frame == rollback.stop) finishRollback();
    if (!recomp_replay_next(&rollback.replayReader, rollback.pads)) {
        std::fprintf(stderr, "recomp netplay: replay ends at frame %u\n", s.frame);
        writeDump();
        recomp_stop(0, "replay:end");
    }
    ++s.frame;
    walkPads();
}

/* Each Swap: STARTING MATCH over the walk to character select, and the
   corner display from NETPLAY SETTINGS: ping, or also the input delay, the
   deepest rewind of the last second and how many frames this side runs
   ahead of the other. */
static void drawSessionOverlay()
{
    if (rollback.walking) {
        const char *title = "STARTING MATCH";
        recomp_overlay_rect(-1000.0f, 0.0f, 2640.0f, 480.0f, 0xff000000u); // past the edges of a wide window
        recomp_overlay_text(320.0f - recomp_overlay_text_width(title, 3.0f) / 2.0f, 228.0f, 3.0f, 0xffffffffu, title);
    }
    // Only this side sees its pad go missing; both guests keep the session's two pads.
    const bool connected = recomp_input_host_pad_connected(0u);
    rollback.padSeen = rollback.padSeen || connected;
    if (rollback.padSeen && !connected && rollback.mode != RollbackMode::Replay) {
        const char *missing = "CONTROLLER DISCONNECTED";
        const float width = recomp_overlay_text_width(missing, 2.0f);
        recomp_overlay_rect(320.0f - width / 2.0f - 6.0f, 410.0f, width + 12.0f, 24.0f, 0xa0000000u);
        recomp_overlay_text(320.0f - width / 2.0f, 415.0f, 2.0f, 0xffff5050u, missing);
    }
    if (rollback.networkDisplay == 0 || rollback.mode == RollbackMode::Replay) {
        recomp_overlay_screen_publish();
        return;
    }
    const RecompNetplay &net = rollback.net;
    std::string text = "PING " + (net.rtt_known ? std::to_string(net.rtt_ms) + " MS" : std::string("--"));
    if (rollback.networkDisplay == 2) {
        const bool recent = std::chrono::steady_clock::now() - rollback.rewoundAt < std::chrono::seconds(1);
        text += "  DELAY " + std::to_string(rollback.delay) + "  ROLLBACK " +
            std::to_string(recent ? rollback.rewindDepth : 0u) + "F  AHEAD " + std::to_string(rollback.advantage) + 'F';
    }
    const std::uint32_t color = !net.rtt_known || net.rtt_ms < 80u ? 0xff60e080u
        : net.rtt_ms < 150u ? 0xffffd040u : 0xffff5050u;
    const float width = recomp_overlay_text_width(text.c_str(), 2.0f);
    recomp_overlay_rect(628.0f - width - 12.0f, 446.0f, width + 12.0f, 24.0f, 0xa0000000u);
    recomp_overlay_text(628.0f - width - 6.0f, 451.0f, 2.0f, color, text.c_str());
    recomp_overlay_screen_publish();
}

static void envNumber(const char *name, std::uint32_t &value)
{
    if (const char *text = std::getenv(name); text != nullptr && *text != '\0')
        value = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10));
}

// Digits only; 0, which no port uses, for anything else.
static unsigned long portNumber(const std::string &text)
{
    return text.empty() || text.find_first_not_of("0123456789") != std::string::npos
        ? 0u : std::strtoul(text.c_str(), nullptr, 10);
}

// Test hooks for every kind of session.
static void readTestHooks()
{
    envNumber("RECOMP_ROLLBACK_FRAMES", rollback.stop);
    rollback.dump = std::getenv("RECOMP_ROLLBACK_DUMP");
}

bool recomp_netplay_session_read_env(const char *mode)
{
    envNumber("RECOMP_ROLLBACK_START", rollback.start);
    readTestHooks();
    if (std::strcmp(mode, "loopback") == 0) {
        rollback.mode = RollbackMode::Loopback;
        rollback.player = 1u;
        if (std::getenv("RECOMP_ROLLBACK_FRAMES") == nullptr) rollback.stop = 600u;
        envNumber("RECOMP_ROLLBACK_LAG", rollback.lag);
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
    const unsigned long localPort = portNumber(spec.substr(0u, hostAt));
    const unsigned long peerPort = portNumber(spec.substr(portAt + 1u));
    const std::string peerHost = spec.substr(hostAt + 1u, portAt - hostAt - 1u);
    envNumber("RECOMP_ROLLBACK_PLAYER", rollback.player);
    envNumber("RECOMP_ROLLBACK_DELAY", rollback.delay);
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

/* Settings that change guest RAM, which the joiner takes from the host. */
enum : std::uint8_t { LobbyUnlockSwimsuits = 1u, LobbyNormalScreen = 2u, LobbyMusicShuffle = 4u };

static bool settingIs(const char *name, const char *value)
{
    const char *set = std::getenv(name);
    return set != nullptr && std::strcmp(set, value) == 0;
}

// The player's own values, put back before a room's session starts the runner offline again.
static std::vector<std::pair<std::string, std::string>> ownSettings;

static void putSetting(const char *name, const char *value)
{
    if (std::none_of(ownSettings.begin(), ownSettings.end(), [name](const auto &own) { return own.first == name; })) {
        const char *own = std::getenv(name);
        ownSettings.emplace_back(name, own != nullptr ? own : "");
    }
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void restoreSettings()
{
    for (const auto &[name, value] : ownSettings) {
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str()); // an empty value removes it
#else
        if (value.empty()) unsetenv(name.c_str());
        else setenv(name.c_str(), value.c_str(), 1);
#endif
    }
    ownSettings.clear();
}

// Each module reads its setting once, after this, as the guest boots.
static void adoptSettings(std::uint8_t flags, const std::filesystem::path &storage)
{
    putSetting("RECOMP_UNLOCK_SWIMSUITS", (flags & LobbyUnlockSwimsuits) != 0u ? "1" : "0");
    putSetting("RECOMP_D3D_WIDESCREEN", (flags & LobbyNormalScreen) != 0u ? "0" : "1");
    putSetting("RECOMP_MUSIC_SHUFFLE", (flags & LobbyMusicShuffle) != 0u ? "1" : "0");
    // Sessions run 60 Hz, start from the game's own title and keep the camera the game's.
    putSetting("RECOMP_GAME_HZ", "60");
    putSetting("RECOMP_TITLE_IDLE", "default");
    putSetting("RECOMP_FREE_CAMERA", "0");
    // The game lists custom soundtracks in guest RAM; a session's folder has none.
    putSetting("RECOMP_USER_MUSIC", (storage / "netplay" / "UserMusic").string().c_str());
}

// A fresh copy of the host's saves each session, so neither player's own saves change.
static bool useSessionSaves(const std::uint8_t *saves, std::size_t size, const std::filesystem::path &storage)
{
    std::error_code error;
    std::filesystem::remove_all(storage / "netplay", error);
    std::vector<std::string> names;
    if (error || !recomp_netplay_unpack_profile(saves, size, recomp_save_partition1(storage, "netplay"), names)) {
        std::cerr << "recomp runner: the host's saves are damaged or unsafe\n";
        return false;
    }
    recomp_save_set_session("netplay");
    rollback.hostSaves.assign(saves, saves + size);
    return true;
}

/* A room's socket, as --netplay-room-socket carries it to the session's
   runner and back: <local port>,<peer address>,<peer port>,<server
   address>,<server port>,<relay>, all but the local port in network order. */
struct RoomSocket {
    std::uint16_t localPort = 0u;
    std::uint32_t peerAddress = 0u, serverAddress = 0u;
    std::uint16_t peerPort = 0u, serverPort = 0u;
    bool relay = false;

    bool parse(const std::string &text)
    {
        unsigned long field[6] = {};
        if (std::sscanf(text.c_str(), "%lu,%lu,%lu,%lu,%lu,%lu", &field[0], &field[1], &field[2], &field[3],
                &field[4], &field[5]) != 6 || field[0] == 0u || field[0] > 65535u)
            return false;
        localPort = static_cast<std::uint16_t>(field[0]);
        peerAddress = static_cast<std::uint32_t>(field[1]);
        peerPort = static_cast<std::uint16_t>(field[2]);
        serverAddress = static_cast<std::uint32_t>(field[3]);
        serverPort = static_cast<std::uint16_t>(field[4]);
        relay = field[5] != 0u;
        return true;
    }

    static std::string format(const RecompNetplay &socket)
    {
        char text[80];
        std::snprintf(text, sizeof text, "%u,%lu,%u,%lu,%u,%u", recomp_netplay_local_port(&socket),
            static_cast<unsigned long>(socket.peer_address), socket.peer_port,
            static_cast<unsigned long>(socket.match.server_address), socket.match.server_port,
            socket.match.relay ? 1u : 0u);
        return text;
    }

    // The room's peer and matchmaker, so relayed packets keep arriving through it.
    void applyTo(RecompNetplay &net) const
    {
        net.peer_address = peerAddress;
        net.peer_port = peerPort;
        net.match.server_address = serverAddress;
        net.match.server_port = serverPort;
        net.match.relay = relay;
    }
};

bool recomp_netplay_session_connect(const RecompNetplayLaunch &launch)
{
    const bool hosting = launch.hostPort != 0u;
    rollback.launch = launch;
    rollback.fromRoom = !launch.roomSocket.empty();
    RoomSocket room;
    if (rollback.fromRoom && !room.parse(launch.roomSocket)) {
        std::cerr << "recomp runner: bad --netplay-room-socket\n";
        return false;
    }
    if (hosting) {
        const auto port = static_cast<std::uint16_t>(launch.hostPort);
        // A room's runner removes its forward before starting this one, so this asks again.
        if (recomp_netplay_open_router_port(port, nullptr, 0u)) std::atexit(recomp_netplay_close_router_port);
        std::fprintf(stderr, "recomp netplay: hosting on UDP port %u; waiting for the other player\n", launch.hostPort);
        if (!recomp_netplay_listen(&rollback.net, port)) {
            std::cerr << "recomp runner: netplay could not open its UDP port\n";
            return false;
        }
    } else {
        const std::size_t colon = launch.join.rfind(':');
        const unsigned long port = colon == std::string::npos ? 0u : portNumber(launch.join.substr(colon + 1u));
        if (colon == std::string::npos || colon == 0u || port == 0u || port > 65535u) {
            std::cerr << "recomp runner: --netplay-join needs <host>:<port>\n";
            return false;
        }
        std::fprintf(stderr, "recomp netplay: joining %s\n", launch.join.c_str());
        if (!recomp_netplay_open(&rollback.net, room.localPort, launch.join.substr(0u, colon).c_str(),
                static_cast<std::uint16_t>(port))) {
            std::cerr << "recomp runner: netplay could not open its UDP port\n";
            return false;
        }
    }
    if (rollback.fromRoom) room.applyTo(rollback.net);
    /* The game keeps all three vacations in one save, so both sides play
       from the host's, and the joiner sends none. Recovery first, so an
       interrupted save is never sent. */
    const std::filesystem::path storage = launch.disc / ".recomp-storage",
        partition1 = recomp_save_partition1(storage, nullptr);
    std::vector<std::uint8_t> mine, theirs(RECOMP_NETPLAY_MAX_PROFILE);
    if (hosting && (!recomp_save_initialize(launch.disc.string().c_str()) ||
            !recomp_netplay_pack_profile(partition1, recomp_netplay_latest_profile(partition1), true, mine) ||
            mine.size() > RECOMP_NETPLAY_MAX_PROFILE)) {
        std::cerr << "recomp runner: cannot read your saves to send, or they are over 1 MiB\n";
        recomp_netplay_close(&rollback.net); // a room's runner starts again on this port
        return false;
    }
    RecompNetplayLobby lobby{};
    std::memcpy(lobby.digest, launch.digest.data(), sizeof lobby.digest);
    lobby.delay = static_cast<std::uint8_t>(launch.delay);
    lobby.flags = static_cast<std::uint8_t>((settingIs("RECOMP_UNLOCK_SWIMSUITS", "1") ? LobbyUnlockSwimsuits : 0u) |
        (settingIs("RECOMP_D3D_WIDESCREEN", "0") ? LobbyNormalScreen : 0u) |
        (settingIs("RECOMP_MUSIC_SHUFFLE", "1") ? LobbyMusicShuffle : 0u));
    RecompNetplayProfiles profiles{mine.data(), static_cast<std::uint32_t>(mine.size()), theirs.data(), 0u};
    const RecompNetplayLobbyResult result = hosting
        ? recomp_netplay_host(&rollback.net, &lobby, &profiles, 10u * 60u * 1000u, nullptr)
        : recomp_netplay_join(&rollback.net, &lobby, &profiles, 2u * 60u * 1000u, nullptr);
    if (result != RECOMP_NETPLAY_LOBBY_OK) {
        std::cerr << "recomp runner: netplay " << (result == RECOMP_NETPLAY_LOBBY_CANCELLED
            ? "cancelled by the other player"
            : result == RECOMP_NETPLAY_LOBBY_MISMATCH ? "refused: the other player runs a different build"
            : result == RECOMP_NETPLAY_LOBBY_TIMEOUT ? "timed out" : "could not open its UDP port") << '\n';
        recomp_netplay_close(&rollback.net);
        return false;
    }
    if (!useSessionSaves(hosting ? mine.data() : theirs.data(), hosting ? mine.size() : profiles.theirs_size, storage)) {
        recomp_netplay_close(&rollback.net);
        return false;
    }
    adoptSettings(lobby.flags, storage);
    rollback.mode = RollbackMode::Udp;
    rollback.player = hosting ? 0u : 1u;
    rollback.delay = lobby.delay;
    rollback.lobby = true;
    rollback.disc = launch.disc;
    rollback.digest = launch.digest;
    rollback.flags = lobby.flags;
    readTestHooks();
    std::fprintf(stderr, "recomp netplay: connected as player %u with %u frames of input delay\n",
        rollback.player + 1u, rollback.delay);
    return true;
}

bool recomp_netplay_session_load_replay(const std::string &path, const RecompNetplayLaunch &launch)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    rollback.replayData.assign(std::istreambuf_iterator<char>(file), {});
    RecompReplayInfo info;
    const std::uint8_t *hostSaves = nullptr, *joinerSaves = nullptr;
    if (!recomp_replay_open(&rollback.replayReader, rollback.replayData.data(), rollback.replayData.size(), &info,
            &hostSaves, &joinerSaves) || info.player > 1u) {
        std::cerr << "recomp runner: --replay needs a replay file this runner can read\n";
        return false;
    }
    if (std::string(info.digest, sizeof info.digest) != launch.digest) {
        std::cerr << "recomp runner: the replay was recorded by another build\n";
        return false;
    }
    const std::filesystem::path storage = launch.disc / ".recomp-storage";
    if (!useSessionSaves(hostSaves, info.blob_size[0], storage)) return false;
    adoptSettings(info.flags, storage);
    rollback.mode = RollbackMode::Replay;
    rollback.player = info.player;
    rollback.delay = info.delay;
    rollback.start = info.start_frame;
    readTestHooks();
    std::fprintf(stderr, "recomp netplay: replaying a session as player %u\n", rollback.player + 1u);
    return true;
}

bool recomp_netplay_session_room_launch(std::vector<std::string> &args)
{
    RecompNetplayRoomState state;
    std::uint32_t port = 0u, delay = 0u;
    RecompNetplay socket;
    if (!recomp_netplay_menu_take_launch(state, port, delay, socket)) return false;
    args = {"--netplay-room-socket", RoomSocket::format(socket), "--netplay-room",
        recomp_netplay_room_state_format(state)};
    if (state.hosting) {
        args.insert(args.end(), {"--netplay-host", std::to_string(port), "--netplay-delay", std::to_string(delay)});
    } else {
        // The socket's peer is the host, also for a joiner FIND MATCH paired, who typed no address.
        const auto *peer = reinterpret_cast<const std::uint8_t *>(&socket.peer_address); // network order
        const unsigned peerPort = (socket.peer_port & 0xffu) << 8 | socket.peer_port >> 8;
        char endpoint[32];
        std::snprintf(endpoint, sizeof endpoint, "%u.%u.%u.%u:%u", peer[0], peer[1], peer[2], peer[3], peerPort);
        args.insert(args.end(), {"--netplay-join", endpoint});
    }
    // The new runner takes the port; the router forward goes with this one.
    recomp_netplay_close(&socket);
    recomp_netplay_close_router_port();
    std::fprintf(stderr, "recomp netplay: the room starts the match; starting again as the %s\n",
        state.hosting ? "host" : "joiner");
    return true;
}

std::vector<std::string> recomp_netplay_session_return_args(const RecompNetplayLaunch &launch,
    const std::string &reason)
{
    std::vector<std::string> args{"--netplay-return", reason};
    if (!launch.room.empty())
        args.insert(args.end(), {"--netplay-room-socket", launch.roomSocket, "--netplay-room", launch.room});
    return args;
}

bool recomp_netplay_session_reopen_room(const RecompNetplayLaunch &launch, const std::string &reason)
{
    RecompNetplayRoomState state;
    RoomSocket room;
    if (!recomp_netplay_room_state_parse(launch.room, state) || !room.parse(launch.roomSocket)) return false;
    RecompNetplay socket;
    // The session's runner may still hold the port while it exits.
    bool open = false;
    for (int tries = 0; tries < 20 && !(open = recomp_netplay_listen(&socket, room.localPort)); ++tries) {
        recomp_netplay_close(&socket);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!open) {
        recomp_netplay_close(&socket);
        std::fprintf(stderr, "recomp netplay: cannot open UDP port %u again; the room stays closed\n", room.localPort);
        return false;
    }
    room.applyTo(socket);
    std::fprintf(stderr, "recomp netplay: back in the room as the %s\n", state.hosting ? "host" : "joiner");
    recomp_netplay_menu_return_to_room(socket, state, reason);
    return true;
}

void recomp_netplay_session_install(const RecompNetplaySessionHost &runner)
{
    host = runner;
    // Guest time must be a function of the frame from boot, so both runners read the same times.
    recomp_xapi_frame_clock_set_enabled(true);
    recomp_synctest_install_default_modules();
    recomp_synctest_set_guest_ram(host.guestRam, host.guestRamSize, host.writeWatch);
    /* A replay plays at the scheduler's 60 Hz; a session paces every frame
       itself, and the scheduler's own wait would only add lag. */
    if (rollback.mode == RollbackMode::Replay) recomp_game_set_frame_boundary(onReplayFrame, true);
    else recomp_game_set_frame_boundary(onFrame, false);
    recomp_kernel_profile_mutation_hook = confirmSaveFrame;
    if (rollback.lobby) {
        RecompNetplaySettings settings;
        recomp_netplay_settings_load(rollback.disc / ".recomp-storage" / "netplay.txt", settings);
        rollback.networkDisplay = settings.display;
    }
    if (rollback.lobby || rollback.mode == RollbackMode::Replay) recomp_d3d_swap_hook = drawSessionOverlay;
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
    closeReplay();
}
