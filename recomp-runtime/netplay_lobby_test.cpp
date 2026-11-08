#include "netplay.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
using RawSocket = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
using RawSocket = int;
#endif

static int failures;
static std::atomic<bool> cancelNow;
static bool cancelled()
{
    return cancelNow;
}

// Each side cancels at a fixed point of the exchange from its own thread, so load cannot move the cancel.
static const RecompNetplay *metHost;
static bool hostMet()
{
    return metHost->peer_port != 0u; // set as the host welcomes the joiner, before any profile arrives
}
static const RecompNetplayLobby *welcomed;
static bool joinerWelcomed()
{
    return welcomed->delay != 0xffu; // the host's welcome overwrites the joiner's marker
}

static std::uint64_t blockedPlayer;
static bool isBlocked(std::uint64_t id)
{
    return id == blockedPlayer;
}

static void check(const char *name, bool passed)
{
    if (!passed) {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

// A socket no open reached yet, which closing leaves alone: an open that fails
// short-circuits the ones after it.
static RecompNetplay unopened()
{
    RecompNetplay net{};
    net.socket = UINTPTR_MAX; // INVALID_SOCKET, as recomp_netplay_close checks it
    return net;
}

// The lobby runs on a socket its caller opens, as a room's is handed over.
static RecompNetplayLobbyResult hostOn(RecompNetplay *net, std::uint16_t port, const RecompNetplayLobby *offered,
    RecompNetplayProfiles *profiles, std::uint32_t timeout, bool (*cancel)())
{
    return recomp_netplay_listen(net, port) ? recomp_netplay_host(net, offered, profiles, timeout, cancel)
                                            : RECOMP_NETPLAY_LOBBY_SOCKET;
}

static RecompNetplayLobbyResult joinTo(RecompNetplay *net, std::uint16_t port, RecompNetplayLobby *asked,
    RecompNetplayProfiles *profiles, std::uint32_t timeout, bool (*cancel)())
{
    return recomp_netplay_open(net, 0u, "127.0.0.1", port) ? recomp_netplay_join(net, asked, profiles, timeout, cancel)
                                                         : RECOMP_NETPLAY_LOBBY_SOCKET;
}

static RecompNetplayLobby lobby(char build, std::uint8_t delay, std::uint8_t flags)
{
    RecompNetplayLobby value;
    std::memset(value.digest, build, sizeof value.digest);
    value.delay = delay;
    value.flags = flags;
    return value;
}

// A socket standing in for doa2u-re's tools/matchmaker.py: it pairs two rooms on 127.0.0.1 and relays between them.
static void sendFrom(const RecompNetplay &from, std::uint16_t port, const std::vector<std::uint8_t> &packet)
{
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(0x7f000001u);
    sendto(static_cast<RawSocket>(from.socket), reinterpret_cast<const char *>(packet.data()),
        static_cast<int>(packet.size()), 0, reinterpret_cast<const sockaddr *>(&to), sizeof to);
}

static std::vector<std::uint8_t> peerPacket(std::uint16_t port)
{
    std::vector<std::uint8_t> packet = {'D', '2', 'M', 'M', 1u, 6u};
    const std::uint8_t endpoint[6] = {127u, 0u, 0u, 1u, static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port)};
    if (port != 0u) packet.insert(packet.end(), endpoint, endpoint + 6);
    else packet.resize(packet.size() + 6u);
    packet.resize(packet.size() + 6u + 1u + RECOMP_NETPLAY_NAME_BYTES); // no LAN endpoint, no name
    return packet;
}

// Pairs a host room on 47620 and a joiner room on 47621, telling each the other's port (0 for none).
static bool matchRooms(std::uint16_t toHost, std::uint16_t toJoiner, int milliseconds, bool &relayed)
{
    RecompNetplay server = unopened(), host = unopened(), joiner = unopened();
    bool opened = recomp_netplay_listen(&server, 47622u) && recomp_netplay_listen(&host, 47620u) &&
        recomp_netplay_listen(&joiner, 47621u) && recomp_netplay_match_server(&host, "127.0.0.1", 47622u) &&
        recomp_netplay_match_server(&joiner, "127.0.0.1", 47622u);
    sendFrom(server, 47620u, peerPacket(toHost));
    sendFrom(server, 47621u, peerPacket(toJoiner));
    RecompNetplayRoom hostRoom{}, joinerRoom{};
    hostRoom.hosting = true;
    char digest[RECOMP_NETPLAY_DIGEST_BYTES];
    std::memset(digest, 'a', sizeof digest);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (opened && std::chrono::steady_clock::now() < end && !(hostRoom.pinged && joinerRoom.pinged)) {
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", false, false);
        std::uint8_t packet[2048];
        sockaddr_in from{};
#ifdef _WIN32
        int size = sizeof from;
#else
        socklen_t size = sizeof from;
#endif
        for (int got; (got = static_cast<int>(recvfrom(static_cast<RawSocket>(server.socket),
                           reinterpret_cast<char *>(packet), sizeof packet, 0, reinterpret_cast<sockaddr *>(&from),
                           &size))) > 0;) {
            if (got > 6 && packet[5] == 9u) // relay to the other room, as the server does
                sendFrom(server, ntohs(from.sin_port) == 47620u ? 47621u : 47620u,
                    std::vector<std::uint8_t>(packet, packet + got));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    relayed = host.match.relay && joiner.match.relay;
    const bool met = hostRoom.pinged && joinerRoom.pinged && std::strcmp(hostRoom.name, "JOINER") == 0 &&
        std::strcmp(joinerRoom.name, "HOST") == 0 && !host.match.punching && !joiner.match.punching;
    if (!met) // one failure under load has not been explained; this says which side stalled next time
        std::fprintf(stderr, "rooms %u/%u: opened %d, pinged %d/%d, names %s/%s, punching %d/%d, relay %d/%d\n",
            toHost, toJoiner, (int)opened, (int)hostRoom.pinged, (int)joinerRoom.pinged, hostRoom.name,
            joinerRoom.name, (int)host.match.punching, (int)joiner.match.punching, (int)host.match.relay,
            (int)joiner.match.relay);
    recomp_netplay_close(&server);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);
    return met;
}

// A lobby packet with no payload past its header and index, as a joiner sends it.
static std::vector<std::uint8_t> lobbyPacket(std::uint8_t kind, std::uint32_t index)
{
    return {'D', '2', 'L', 'B', 5u, kind, static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(index >> 8),
        static_cast<std::uint8_t>(index >> 16), static_cast<std::uint8_t>(index >> 24)};
}

// The profile chunks a socket receives within 300 ms.
static int chunksArriving(const RecompNetplay &net)
{
    int chunks = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < end) {
        std::uint8_t packet[2048];
        for (int got; (got = static_cast<int>(recv(static_cast<RawSocket>(net.socket),
                           reinterpret_cast<char *>(packet), sizeof packet, 0))) > 0;)
            chunks += got > 10 && packet[5] == 4u;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return chunks;
}

// A host serves the next window of its profile at once, a replayed one after 50 ms, and eight copies in all.
static void serveBounds()
{
    RecompNetplay host = unopened(), fake = unopened();
    RecompNetplayLobbyResult hosted = RECOMP_NETPLAY_LOBBY_SOCKET;
    const RecompNetplayLobby offered = lobby('a', 2u, 0u);
    std::vector<std::uint8_t> mine(16u * 1024u + 1000u), theirs(RECOMP_NETPLAY_MAX_PROFILE); // 17 chunks
    RecompNetplayProfiles profiles{mine.data(), static_cast<std::uint32_t>(mine.size()), theirs.data(), 0u};
    if (!recomp_netplay_listen(&host, 47655u) || !recomp_netplay_open(&fake, 0u, "127.0.0.1", 47655u)) {
        check("serve sockets", false);
        recomp_netplay_close(&host);
        recomp_netplay_close(&fake);
        return;
    }
    std::thread wait([&] { hosted = recomp_netplay_host(&host, &offered, &profiles, 10000u, nullptr); });
    std::vector<std::uint8_t> hello = lobbyPacket(1u, 0u);
    hello.resize(6u);
    hello.insert(hello.end(), RECOMP_NETPLAY_DIGEST_BYTES, 'a');
    hello.resize(hello.size() + 6u); // delay, flags and an empty profile
    sendFrom(fake, 47655u, hello);
    (void)chunksArriving(fake); // the welcome
    sendFrom(fake, 47655u, lobbyPacket(3u, 0u));
    sendFrom(fake, 47655u, lobbyPacket(3u, 16u));
    sendFrom(fake, 47655u, lobbyPacket(3u, 0u));
    check("next window served at once, replay held", chunksArriving(fake) == 17);
    sendFrom(fake, 47655u, lobbyPacket(3u, 0u));
    sendFrom(fake, 47655u, lobbyPacket(3u, 16u));
    check("a replay does not move the served window back", chunksArriving(fake) == 16);
    // 33,768 bytes sent so far; seven more windows pass eight copies of 17,384.
    int replayed = 0;
    for (int i = 0; i < 9; ++i) {
        sendFrom(fake, 47655u, lobbyPacket(3u, 0u));
        replayed += chunksArriving(fake);
    }
    check("eight copies end serving", replayed == 7 * 16);
    sendFrom(fake, 47655u, lobbyPacket(3u, 17u)); // holds them all
    wait.join();
    check("serve lobby ends", hosted == RECOMP_NETPLAY_LOBBY_OK);
    recomp_netplay_close(&host);
    recomp_netplay_close(&fake);
}

// A ROOMS answer listing one room named HOST, with its host at 127.0.0.1:port, or without an
// endpoint, as an older server sends it.
static std::vector<std::uint8_t> roomsPacket(std::uint32_t id, std::uint16_t port, bool endpoints)
{
    std::vector<std::uint8_t> packet = {'D', '2', 'M', 'M', 1u, 4u, 1u};
    for (int shift = 24; shift >= 0; shift -= 8) packet.push_back(static_cast<std::uint8_t>(id >> shift));
    const std::uint8_t rest[] = {0u, 7u, 2u, 4u, 'H', 'O', 'S', 'T'}; // age, delay, name length, name
    packet.insert(packet.end(), rest, rest + sizeof rest);
    packet.resize(7u + 40u);
    if (endpoints) {
        const std::uint8_t at[6] = {127u, 0u, 0u, 1u, static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port)};
        packet.insert(packet.end(), at, at + 6);
    }
    return packet;
}

// FIND MATCH pings a listed host's room at the endpoint in the list, and keeps the ping when the
// room is listed again. A room its host does not list stays quiet, and an older server's list has
// no endpoint to ping.
static void probeRooms()
{
    using std::chrono::steady_clock;
    RecompNetplay server = unopened(), host = unopened(), joiner = unopened();
    const bool opened = recomp_netplay_listen(&server, 47623u) && recomp_netplay_listen(&host, 47624u) &&
        recomp_netplay_listen(&joiner, 47625u) && recomp_netplay_match_server(&joiner, "127.0.0.1", 47623u);
    check("probe sockets", opened);
    const auto closeAll = [&] {
        recomp_netplay_close(&server);
        recomp_netplay_close(&host);
        recomp_netplay_close(&joiner);
    };
    if (!opened) return closeAll();
    RecompNetplayRoom room{};
    room.hosting = true;
    char digest[RECOMP_NETPLAY_DIGEST_BYTES];
    std::memset(digest, 'a', sizeof digest);
    const RecompNetplayListedRoom &listed = joiner.match.rooms[0];
    const auto list = [&](std::uint32_t id, std::uint16_t port, bool endpoints) {
        sendFrom(server, 47625u, roomsPacket(id, port, endpoints));
        const auto end = steady_clock::now() + std::chrono::seconds(2);
        while (!joiner.match.listed && steady_clock::now() < end) {
            recomp_netplay_match_poll(&joiner);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const bool heard = joiner.match.listed && joiner.match.room_count == 1u && listed.id == id &&
            std::strcmp(listed.name, "HOST") == 0;
        joiner.match.listed = false;
        return heard;
    };
    const auto probe = [&](int milliseconds) {
        recomp_netplay_match_probe(&joiner);
        const auto end = steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (!listed.pinged && steady_clock::now() < end) {
            recomp_netplay_room_tick(&host, &room, digest, "HOST", false, false);
            recomp_netplay_match_poll(&joiner);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return listed.pinged;
    };
    host.match.room_id = 0x01020304u; // as the server's HOSTED answer sets it
    check("list carries the host's endpoint", list(0x01020304u, 47624u, true) &&
        listed.address == htonl(0x7f000001u) && listed.port == htons(47624u) && !listed.pinged);
    check("listed room answers a ping", probe(2000) && listed.ping_ms < 1000u);
    check("a room listed again keeps its ping", list(0x01020304u, 47624u, true) && listed.pinged);
    host.match.room_id = 0u; // private, or not listed yet
    check("an unlisted room stays quiet", list(0x01020305u, 47624u, true) && !probe(300));
    check("an older server's list has no endpoint", list(0x01020304u, 0u, false) && listed.port == 0u &&
        !listed.pinged);
    closeAll();
}

int main()
{
    // The joiner adopts the host's settings, the two swap profiles, and the host learns the joiner's address.
    RecompNetplay host, joiner;
    RecompNetplayLobbyResult hosted = RECOMP_NETPLAY_LOBBY_SOCKET;
    const RecompNetplayLobby offered = lobby('a', 2u, 3u);
    std::vector<std::uint8_t> hostSent(70000u), joinerSent(5000u), hostGot(RECOMP_NETPLAY_MAX_PROFILE),
        joinerGot(RECOMP_NETPLAY_MAX_PROFILE);
    for (std::size_t i = 0; i < hostSent.size(); ++i) hostSent[i] = static_cast<std::uint8_t>(i * 7u + i / 1024u);
    for (std::size_t i = 0; i < joinerSent.size(); ++i) joinerSent[i] = static_cast<std::uint8_t>(i * 13u + 1u);
    RecompNetplayProfiles hostProfiles{hostSent.data(), static_cast<std::uint32_t>(hostSent.size()), hostGot.data(), 0u};
    RecompNetplayProfiles joinerProfiles{joinerSent.data(), static_cast<std::uint32_t>(joinerSent.size()),
        joinerGot.data(), 0u};
    std::thread wait([&] { hosted = hostOn(&host, 47611u, &offered, &hostProfiles, 5000u, nullptr); });
    RecompNetplayLobby asked = lobby('a', 0u, 0u);
    const RecompNetplayLobbyResult joined =
        joinTo(&joiner, 47611u, &asked, &joinerProfiles, 5000u, nullptr);
    wait.join();
    check("join", joined == RECOMP_NETPLAY_LOBBY_OK && hosted == RECOMP_NETPLAY_LOBBY_OK);
    check("settings adopted", asked.delay == 2u && asked.flags == 3u);
    check("host profile copied", joinerProfiles.theirs_size == hostSent.size() &&
        std::memcmp(joinerGot.data(), hostSent.data(), hostSent.size()) == 0);
    check("joiner profile copied", hostProfiles.theirs_size == joinerSent.size() &&
        std::memcmp(hostGot.data(), joinerSent.data(), joinerSent.size()) == 0);
    check("host knows joiner", host.peer_port != 0u);

    // Pads then cross both ways on the lobby's sockets.
    RecompRollbackSession a, b;
    recomp_rollback_init(&a, 0u);
    recomp_rollback_init(&b, 0u);
    RecompInputGamepad pad{};
    pad.buttons = 5u;
    recomp_rollback_add_local(&a, &pad);
    recomp_rollback_add_local(&b, &pad);
    for (int i = 0; i < 50 && (a.remote_count == 0u || b.remote_count == 0u); ++i) {
        recomp_netplay_send(&host, &a, 0);
        recomp_netplay_send(&joiner, &b, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        recomp_netplay_receive(&host, &a);
        recomp_netplay_receive(&joiner, &b);
    }
    check("pads cross", a.remote_count == 1u && b.remote_count == 1u);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // Players without profiles send none.
    RecompNetplayProfiles none{nullptr, 0u, hostGot.data(), 1u};
    RecompNetplayProfiles noneJoiner{nullptr, 0u, joinerGot.data(), 1u};
    wait = std::thread([&] { hosted = hostOn(&host, 47614u, &offered, &none, 5000u, nullptr); });
    asked = lobby('a', 0u, 0u);
    check("no profile", joinTo(&joiner, 47614u, &asked, &noneJoiner, 5000u, nullptr) ==
        RECOMP_NETPLAY_LOBBY_OK && noneJoiner.theirs_size == 0u);
    wait.join();
    check("no profile host", hosted == RECOMP_NETPLAY_LOBBY_OK && none.theirs_size == 0u);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A different build is refused on both sides.
    const RecompNetplayLobby other = lobby('b', 2u, 0u);
    wait = std::thread([&] { hosted = hostOn(&host, 47612u, &offered, &none, 5000u, nullptr); });
    asked = other;
    const RecompNetplayLobbyResult refused =
        joinTo(&joiner, 47612u, &asked, &noneJoiner, 5000u, nullptr);
    wait.join();
    check("mismatch", refused == RECOMP_NETPLAY_LOBBY_MISMATCH && hosted == RECOMP_NETPLAY_LOBBY_MISMATCH);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // Nobody hosting: the joiner gives up.
    asked = offered;
    check("timeout", joinTo(&joiner, 47613u, &asked, &noneJoiner, 300u, nullptr) ==
        RECOMP_NETPLAY_LOBBY_TIMEOUT);
    recomp_netplay_close(&joiner);

    // A host cancelling alone ends its wait; one cancelling mid-exchange sends the joiner back too.
    cancelNow = true;
    check("host cancels alone", hostOn(&host, 47617u, &offered, &none, 5000u, cancelled) ==
        RECOMP_NETPLAY_LOBBY_CANCELLED && !host.peer_left);
    recomp_netplay_close(&host);
    cancelNow = false;
    std::vector<std::uint8_t> big(RECOMP_NETPLAY_MAX_PROFILE, 7u);
    RecompNetplayProfiles bigJoiner{big.data(), static_cast<std::uint32_t>(big.size()), joinerGot.data(), 0u};
    // The host cancels as it meets the joiner, while the 1 MiB profile is in flight.
    metHost = &host;
    wait = std::thread([&] { hosted = hostOn(&host, 47617u, &offered, &none, 5000u, hostMet); });
    asked = offered;
    const RecompNetplayLobbyResult sentBack =
        joinTo(&joiner, 47617u, &asked, &bigJoiner, 5000u, nullptr);
    wait.join();
    check("host cancels mid-exchange", hosted == RECOMP_NETPLAY_LOBBY_CANCELLED && !host.peer_left &&
        sentBack == RECOMP_NETPLAY_LOBBY_CANCELLED && joiner.peer_left);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A joiner cancelling as the host's welcome arrives, with its profile in flight, sends the host back.
    wait = std::thread([&] { hosted = hostOn(&host, 47618u, &offered, &none, 5000u, nullptr); });
    asked = offered;
    asked.delay = 0xffu;
    welcomed = &asked;
    check("joiner cancels", joinTo(&joiner, 47618u, &asked, &bigJoiner, 5000u,
        joinerWelcomed) == RECOMP_NETPLAY_LOBBY_CANCELLED);
    wait.join();
    check("host hears the cancel", hosted == RECOMP_NETPLAY_LOBBY_CANCELLED && host.peer_left);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A room's host knows its joiner, so the joiner's goodbye ends the wait even when its hello was lost.
    check("goodbye before hello sockets", recomp_netplay_listen(&host, 47626u) &&
        recomp_netplay_open(&joiner, 0u, "127.0.0.1", 47626u));
    host.peer_address = htonl(0x7f000001u);
    host.peer_port = htons(recomp_netplay_local_port(&joiner));
    recomp_netplay_send_goodbye(&joiner);
    check("goodbye before hello", recomp_netplay_host(&host, &offered, &none, 5000u, nullptr) ==
        RECOMP_NETPLAY_LOBBY_CANCELLED && host.peer_left);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A delay past the rollback window could never run, so the joiner refuses it.
    const RecompNetplayLobby tooLate = lobby('a', RECOMP_ROLLBACK_WINDOW + 1u, 0u);
    wait = std::thread([&] { hosted = hostOn(&host, 47627u, &tooLate, &none, 500u, nullptr); });
    asked = offered;
    check("delay past the window", joinTo(&joiner, 47627u, &asked, &noneJoiner, 5000u, nullptr) ==
        RECOMP_NETPLAY_LOBBY_MISMATCH);
    wait.join();
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A host back in its room sends a joiner still calling the cancelled match back to its room.
    RecompNetplayRoom backRoom{};
    backRoom.hosting = true;
    std::atomic<bool> calling{true};
    check("room for a late joiner", recomp_netplay_listen(&host, 47619u));
    wait = std::thread([&] {
        while (calling) {
            recomp_netplay_room_tick(&host, &backRoom, offered.digest, "HOST", false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    asked = offered;
    check("late joiner sent back", joinTo(&joiner, 47619u, &asked, &noneJoiner, 2000u,
        nullptr) == RECOMP_NETPLAY_LOBBY_CANCELLED && joiner.peer_left);
    calling = false;
    wait.join();
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // In a room the two hear each other's names, readiness and ping, and the joiner hears the start.
    char digest[RECOMP_NETPLAY_DIGEST_BYTES], otherDigest[RECOMP_NETPLAY_DIGEST_BYTES];
    std::memset(digest, 'a', sizeof digest);
    std::memset(otherDigest, 'b', sizeof otherDigest);
    RecompNetplayRoom hostRoom{}, joinerRoom{};
    hostRoom.hosting = true;
    hostRoom.id = 0x1111222233334444ull;
    joinerRoom.id = 0x5555666677778888ull;
    check("room opens", recomp_netplay_listen(&host, 47615u) &&
        recomp_netplay_open(&joiner, 0u, "127.0.0.1", 47615u));
    for (int i = 0; i < 100 && !(joinerRoom.start && hostRoom.pinged && joinerRoom.pinged); ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", true, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, hostRoom.ready);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("room names", std::strcmp(hostRoom.name, "JOINER") == 0 && std::strcmp(joinerRoom.name, "HOST") == 0);
    check("room ready and start", hostRoom.ready && joinerRoom.start && !joinerRoom.ready);
    check("room ping", hostRoom.pinged && joinerRoom.pinged && hostRoom.ping_ms < 1000u &&
        joinerRoom.ping_ms < 1000u);
    check("room same build", hostRoom.joined && joinerRoom.joined && !hostRoom.mismatch && !joinerRoom.mismatch);
    check("room ids", hostRoom.peer_id == joinerRoom.id && joinerRoom.peer_id == hostRoom.id);

    // A chat line arrives once, and one that is not printable ASCII never does.
    recomp_netplay_room_say(&joinerRoom, "gg, one more?");
    for (int i = 0; i < 50 && !hostRoom.heard_new; ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", true, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("chat arrives", hostRoom.heard_new && std::strcmp(hostRoom.heard, "gg, one more?") == 0);
    hostRoom.heard_new = false;
    recomp_netplay_room_say(&joinerRoom, "bell\a");
    for (int i = 0; i < 25; ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", true, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("chat once, printable only", !hostRoom.heard_new);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // A host never answers a joiner it blocked, takes the same joiner once unblocked,
    // and a joiner that blocked the host stops calling it.
    hostRoom = RecompNetplayRoom{};
    joinerRoom = RecompNetplayRoom{};
    hostRoom.hosting = true;
    hostRoom.id = 1u;
    joinerRoom.id = 2u;
    hostRoom.blocked = isBlocked;
    blockedPlayer = 2u;
    check("block room opens", recomp_netplay_listen(&host, 47624u) &&
        recomp_netplay_open(&joiner, 0u, "127.0.0.1", 47624u));
    for (int i = 0; i < 25; ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", false, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("host refuses a blocked joiner", hostRoom.refused && !hostRoom.joined && !joinerRoom.joined &&
        host.peer_port == 0u);
    blockedPlayer = 0u;
    for (int i = 0; i < 50 && !(hostRoom.joined && joinerRoom.joined); ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", false, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("unblocked joiner gets in", hostRoom.joined && joinerRoom.joined && hostRoom.peer_id == 2u);
    recomp_netplay_room_drop(&host, &hostRoom);
    check("host drops the joiner", !hostRoom.joined && host.peer_port == 0u);
    joinerRoom.blocked = isBlocked;
    blockedPlayer = 1u;
    joinerRoom.joined = false;
    for (int i = 0; i < 50 && !joinerRoom.refused; ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", false, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("joiner refuses a blocked host", joinerRoom.refused && !joinerRoom.joined && joiner.peer_port == 0u);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    hostRoom = RecompNetplayRoom{};
    joinerRoom = RecompNetplayRoom{};
    hostRoom.hosting = true;
    check("room reopens", recomp_netplay_listen(&host, 47616u) &&
        recomp_netplay_open(&joiner, 0u, "127.0.0.1", 47616u));
    for (int i = 0; i < 100 && !(hostRoom.joined && joinerRoom.joined); ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, otherDigest, "JOINER", false, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("room other build", hostRoom.mismatch && joinerRoom.mismatch);
    recomp_netplay_room_drop(&host, &hostRoom);
    check("drop forgets the other build", !hostRoom.mismatch);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);

    // Matched rooms punch through to each other, or meet through the relay without
    // endpoints, or after 5 s of punching at endpoints that never answer.
    bool relayed = true;
    // Each waits only until both rooms meet; punching must win inside the 5 s before the relay takes over.
    check("matched rooms punch through", matchRooms(47621u, 47620u, 4500, relayed) && !relayed);
    check("matched rooms relay without endpoints", matchRooms(0u, 0u, 5000, relayed) && relayed);
    check("matched rooms relay when punching fails", matchRooms(47629u, 47628u, 10000, relayed) && relayed);
    probeRooms();
    serveBounds();

    if (failures != 0) return 1;
    std::puts("PASS netplay lobby joins one build, swaps profiles, refuses another and runs a room");
    return 0;
}
