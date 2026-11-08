#include "netplay.h"

#include <algorithm>
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

// netplay.c's lobby packets: "D2LB", LOBBY_VERSION, kind, then the body.
static std::vector<std::uint8_t> helloPacket()
{
    std::vector<std::uint8_t> hello = {'D', '2', 'L', 'B', 5u, 1u};
    hello.insert(hello.end(), RECOMP_NETPLAY_DIGEST_BYTES, static_cast<std::uint8_t>('a'));
    hello.resize(hello.size() + 6u); // delay, flags, no profile
    return hello;
}

// Counts the lobby packets of one kind waiting on a socket.
static int drainKind(const RecompNetplay &net, std::uint8_t kind)
{
    int count = 0;
    std::uint8_t packet[2048];
    for (int got; (got = static_cast<int>(recv(static_cast<RawSocket>(net.socket), reinterpret_cast<char *>(packet),
                       sizeof packet, 0))) > 0;)
        count += got > 6 && std::memcmp(packet, "D2LB", 4) == 0 && packet[5] == kind;
    return count;
}

// PEER, to the socket whose nonce it carries.
static std::vector<std::uint8_t> peerPacket(std::uint16_t port, const std::uint8_t *nonce)
{
    std::vector<std::uint8_t> packet = {'D', '2', 'M', 'M', 2u, 6u};
    packet.insert(packet.end(), nonce, nonce + RECOMP_NETPLAY_TOKEN_BYTES);
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
    sendFrom(server, 47620u, peerPacket(toHost, host.match.nonce));
    sendFrom(server, 47621u, peerPacket(toJoiner, joiner.match.nonce));
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
            // Relay to the other room, as the server does: the header, the receiver's nonce, then what came
            // after the sender's nonce and cookie.
            if (got >= 6 + 2 * RECOMP_NETPLAY_TOKEN_BYTES && packet[5] == 9u) {
                const bool toJoiner = ntohs(from.sin_port) == 47620u;
                const std::uint8_t *nonce = toJoiner ? joiner.match.nonce : host.match.nonce;
                std::vector<std::uint8_t> relayed(packet, packet + 6);
                relayed.insert(relayed.end(), nonce, nonce + RECOMP_NETPLAY_TOKEN_BYTES);
                relayed.insert(relayed.end(), packet + 6 + 2 * RECOMP_NETPLAY_TOKEN_BYTES, packet + got);
                sendFrom(server, toJoiner ? 47621u : 47620u, relayed);
            }
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

// A ROOMS answer, to the socket whose nonce it carries, listing one room named HOST whose host reported
// a round trip of hostRtt ms.
static std::vector<std::uint8_t> roomsPacket(const std::uint8_t *nonce, std::uint32_t id, std::uint16_t hostRtt)
{
    std::vector<std::uint8_t> packet = {'D', '2', 'M', 'M', 2u, 4u};
    packet.insert(packet.end(), nonce, nonce + RECOMP_NETPLAY_TOKEN_BYTES);
    packet.push_back(1u);
    for (int shift = 24; shift >= 0; shift -= 8) packet.push_back(static_cast<std::uint8_t>(id >> shift));
    const std::uint8_t rest[] = {0u, 7u, 2u, 4u, 'H', 'O', 'S', 'T'}; // age, delay, name length, name
    packet.insert(packet.end(), rest, rest + sizeof rest);
    packet.resize(7u + RECOMP_NETPLAY_TOKEN_BYTES + 40u);
    packet.push_back(static_cast<std::uint8_t>(hostRtt >> 8));
    packet.push_back(static_cast<std::uint8_t>(hostRtt));
    return packet;
}

// A host times HOST to HOSTED and says it in its next HOST. FIND MATCH estimates a room's ping as
// its own LIST to ROOMS round trip plus the host's, and ignores a list it did not ask for.
static void estimatePings()
{
    using std::chrono::steady_clock;
    RecompNetplay server = unopened(), host = unopened(), joiner = unopened();
    const bool opened = recomp_netplay_listen(&server, 47623u) && recomp_netplay_listen(&host, 47624u) &&
        recomp_netplay_listen(&joiner, 47625u) && recomp_netplay_match_server(&host, "127.0.0.1", 47623u) &&
        recomp_netplay_match_server(&joiner, "127.0.0.1", 47623u);
    check("estimate sockets", opened);
    const auto closeAll = [&] {
        recomp_netplay_close(&server);
        recomp_netplay_close(&host);
        recomp_netplay_close(&joiner);
    };
    if (!opened) return closeAll();
    char digest[RECOMP_NETPLAY_DIGEST_BYTES];
    std::memset(digest, 'a', sizeof digest);
    // The server's next packet, as the server sees it.
    const auto asked = [&]() {
        std::uint8_t packet[2048];
        const auto end = steady_clock::now() + std::chrono::seconds(2);
        for (int got; steady_clock::now() < end; std::this_thread::sleep_for(std::chrono::milliseconds(5)))
            if ((got = static_cast<int>(recv(static_cast<RawSocket>(server.socket), reinterpret_cast<char *>(packet),
                     sizeof packet, 0))) > 0)
                return std::vector<std::uint8_t>(packet, packet + got);
        return std::vector<std::uint8_t>();
    };
    const auto waitFor = [&](RecompNetplay &net, const bool &done, int milliseconds) {
        const auto end = steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (!done && steady_clock::now() < end) {
            recomp_netplay_match_poll(&net);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done;
    };
    const auto rtt = [](const std::vector<std::uint8_t> &host) {
        return host.size() < 2u ? -1 : host[host.size() - 2u] << 8 | host.back();
    };

    recomp_netplay_match_host(&host, digest, 2u, true, "HOST");
    check("a first HOST knows no round trip", rtt(asked()) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::vector<std::uint8_t> hostedPacket = {'D', '2', 'M', 'M', 2u, 2u};
    hostedPacket.insert(hostedPacket.end(), host.match.nonce, host.match.nonce + RECOMP_NETPLAY_TOKEN_BYTES);
    const std::uint8_t hostedBody[] = {1u, 2u, 3u, 4u, 127u, 0u, 0u, 1u, 0xbau, 0x18u}; // room id, endpoint
    hostedPacket.insert(hostedPacket.end(), hostedBody, hostedBody + sizeof hostedBody);
    sendFrom(server, 47624u, hostedPacket);
    bool hosted = false;
    const auto end = steady_clock::now() + std::chrono::seconds(2);
    while (!(hosted = host.match.room_id == 0x01020304u) && steady_clock::now() < end) {
        recomp_netplay_match_poll(&host);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check("HOSTED times the HOST", hosted && host.match.host_rtt_ms >= 20u && host.match.host_rtt_ms < 1000u);
    recomp_netplay_match_host(&host, digest, 2u, true, "HOST");
    check("the next HOST says it", rtt(asked()) == host.match.host_rtt_ms);

    const RecompNetplayListedRoom &listed = joiner.match.rooms[0];
    sendFrom(server, 47625u, roomsPacket(joiner.match.nonce, 0x01020304u, 30u));
    check("an unasked list is ignored", !waitFor(joiner, joiner.match.listed, 300));
    recomp_netplay_match_list(&joiner, digest);
    check("LIST is padded", asked().size() == 512u);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sendFrom(server, 47625u, roomsPacket(joiner.match.nonce, 0x01020304u, 30u));
    check("list estimates the ping", waitFor(joiner, joiner.match.listed, 2000) && joiner.match.room_count == 1u &&
        listed.id == 0x01020304u && std::strcmp(listed.name, "HOST") == 0 && listed.pinged &&
        listed.ping_ms >= 50u && listed.ping_ms < 1030u);
    joiner.match.listed = false;
    recomp_netplay_match_list(&joiner, digest);
    (void)asked();
    sendFrom(server, 47625u, roomsPacket(joiner.match.nonce, 0x01020304u, 0u));
    check("an unknown host round trip shows no ping", waitFor(joiner, joiner.match.listed, 2000) && !listed.pinged);
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

    // A room packet from an address that never heard this host draws one room packet, not ten a second.
    {
        RecompNetplay forger = unopened();
        RecompNetplayRoom doubtedRoom{};
        doubtedRoom.hosting = true;
        const bool opened = recomp_netplay_listen(&host, 47660u) && recomp_netplay_open(&forger, 0u, "127.0.0.1", 47660u);
        check("doubted room opens", opened);
        std::vector<std::uint8_t> roomPacket = {'D', '2', 'L', 'B', 5u, 5u};
        roomPacket.insert(roomPacket.end(), RECOMP_NETPLAY_DIGEST_BYTES, static_cast<std::uint8_t>('a'));
        roomPacket.resize(167u); // flags, name, clocks with none heard, no chat, sender id
        sendFrom(forger, 47660u, roomPacket);
        int answers = 0;
        for (int i = 0; opened && i < 25; ++i) {
            recomp_netplay_room_tick(&host, &doubtedRoom, digest, "HOST", false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            answers += drainKind(forger, 5u);
        }
        check("doubted peer answered one for one", doubtedRoom.joined && answers == 1);
        recomp_netplay_close(&forger);
        recomp_netplay_close(&host);
    }

    // A room's socket arrives with its peer, so a hello from any other address cannot take the match.
    {
        RecompNetplay forger = unopened();
        const bool opened = recomp_netplay_listen(&host, 47661u) && recomp_netplay_open(&forger, 0u, "127.0.0.1", 47661u);
        check("handed-over socket", opened);
        host.peer_address = htonl(0x7f000001u);
        host.peer_port = htons(47662u);
        wait = std::thread([&] { hosted = recomp_netplay_host(&host, &offered, &none, 5000u, nullptr); });
        int welcomes = 0;
        for (int i = 0; i < 10; ++i) {
            sendFrom(forger, 47661u, helloPacket());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            welcomes += drainKind(forger, 2u);
        }
        asked = offered;
        const bool joinedPeer = recomp_netplay_open(&joiner, 47662u, "127.0.0.1", 47661u) &&
            recomp_netplay_join(&joiner, &asked, &noneJoiner, 5000u, nullptr) == RECOMP_NETPLAY_LOBBY_OK;
        wait.join();
        welcomes += drainKind(forger, 2u);
        check("forged hello ignored", opened && welcomes == 0 && joinedPeer && hosted == RECOMP_NETPLAY_LOBBY_OK &&
            host.peer_port == htons(47662u));
        recomp_netplay_close(&forger);
        recomp_netplay_close(&host);
        recomp_netplay_close(&joiner);
    }

    // A packet from the matchmaker's address that does not echo the socket's nonce is forged, and ignored.
    {
        RecompNetplay server = unopened(), room = unopened();
        const bool opened = recomp_netplay_listen(&server, 47663u) && recomp_netplay_listen(&room, 47664u) &&
            recomp_netplay_match_server(&room, "127.0.0.1", 47663u);
        std::uint8_t wrong[RECOMP_NETPLAY_TOKEN_BYTES];
        std::memcpy(wrong, room.match.nonce, sizeof wrong);
        wrong[0] ^= 1u;
        sendFrom(server, 47664u, peerPacket(47630u, wrong));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recomp_netplay_match_poll(&room);
        const bool ignored = !room.match.met;
        sendFrom(server, 47664u, peerPacket(47630u, room.match.nonce));
        for (int i = 0; i < 50 && !room.match.met; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            recomp_netplay_match_poll(&room);
        }
        check("forged matchmaker packet ignored", opened && ignored && room.match.met);

        // A HOST without a cookie draws one, and the room says HOST again with it.
        const auto heardHost = [&] {
            std::vector<std::uint8_t> got(2048);
            for (int i = 0; i < 50; ++i, std::this_thread::sleep_for(std::chrono::milliseconds(10))) {
                const int size = static_cast<int>(recv(static_cast<RawSocket>(server.socket),
                    reinterpret_cast<char *>(got.data()), static_cast<int>(got.size()), 0));
                if (size > 6 + 2 * RECOMP_NETPLAY_TOKEN_BYTES && got[5] == 1u) return got;
            }
            return std::vector<std::uint8_t>();
        };
        recomp_netplay_match_host(&room, digest, 2u, true, "HOST");
        const std::vector<std::uint8_t> first = heardHost();
        std::vector<std::uint8_t> cookie = {'D', '2', 'M', 'M', 2u, 10u};
        cookie.insert(cookie.end(), room.match.nonce, room.match.nonce + RECOMP_NETPLAY_TOKEN_BYTES);
        for (std::uint8_t i = 1u; i <= RECOMP_NETPLAY_TOKEN_BYTES; ++i) cookie.push_back(i);
        sendFrom(server, 47664u, cookie);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recomp_netplay_match_poll(&room);
        const std::vector<std::uint8_t> again = heardHost();
        check("cookie sent again", !first.empty() && !again.empty() &&
            std::all_of(first.begin() + 14, first.begin() + 22, [](std::uint8_t b) { return b == 0u; }) &&
            std::equal(cookie.begin() + 14, cookie.end(), again.begin() + 14));
        recomp_netplay_close(&server);
        recomp_netplay_close(&room);
    }

    // Matched rooms punch through to each other, or meet through the relay without
    // endpoints, or after 5 s of punching at endpoints that never answer.
    bool relayed = true;
    // Each waits only until both rooms meet; punching must win inside the 5 s before the relay takes over.
    check("matched rooms punch through", matchRooms(47621u, 47620u, 4500, relayed) && !relayed);
    check("matched rooms relay without endpoints", matchRooms(0u, 0u, 5000, relayed) && relayed);
    check("matched rooms relay when punching fails", matchRooms(47629u, 47628u, 10000, relayed) && relayed);
    estimatePings();

    if (failures != 0) return 1;
    std::puts("PASS netplay lobby joins one build, swaps profiles, refuses another and runs a room");
    return 0;
}
