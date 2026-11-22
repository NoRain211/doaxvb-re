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

// The lobby runs on a socket its caller opens, as a room's is handed over.
// A socket no open reached yet, which closing leaves alone: an open that fails
// short-circuits the ones after it.
static RecompNetplay unopened()
{
    RecompNetplay net{};
    net.socket = UINTPTR_MAX; // INVALID_SOCKET, as recomp_netplay_close checks it
    return net;
}

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

// A socket standing in for tools/matchmaker.py: it pairs two rooms on 127.0.0.1 and relays between them.
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

// A caller's first room packet: flags, name, clocks with none heard, no chat, sender id.
static const std::size_t RoomClocks = 6u + RECOMP_NETPLAY_DIGEST_BYTES + 2u + RECOMP_NETPLAY_NAME_BYTES;
static std::vector<std::uint8_t> roomPacket()
{
    std::vector<std::uint8_t> packet = {'D', '2', 'L', 'B', 5u, 5u};
    packet.insert(packet.end(), RECOMP_NETPLAY_DIGEST_BYTES, static_cast<std::uint8_t>('a'));
    packet.resize(RoomClocks + 10u + 5u + RECOMP_NETPLAY_CHAT_BYTES + 8u);
    return packet;
}

// Runs a host's room for a while: a tick every 20 ms.
static void tickRoom(RecompNetplay &host, RecompNetplayRoom &room, const char *digest, int ticks)
{
    for (int i = 0; i < ticks; ++i) {
        recomp_netplay_room_tick(&host, &room, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

extern "C" void netplay_relay_on(RecompNetplay *net, const char *why);
extern "C" void netplay_send_to(const RecompNetplay *net, std::uint32_t address, std::uint16_t port,
    const std::uint8_t *packet, std::size_t size);

// A pair that met over IPv6 and then falls back to the relay sends through the matchmaker, not to ::1.
static bool relayOverridesIpv6()
{
    RecompNetplay server, host;
    const bool listening[2] = {recomp_netplay_listen(&server, 47640u), recomp_netplay_listen(&host, 47641u)};
    bool wrapped = false;
    if (listening[0] && listening[1] && recomp_netplay_match_server(&host, "127.0.0.1", 47640u)) {
        host.peer_address = htonl(0x7f000001u);
        host.peer_port = htons(47642u);
        host.match.direct6 = true;
        host.match.candidate6_port = htons(47643u);
        host.match.candidate6[15] = 1u;
        netplay_relay_on(&host, "the test relays");
        const std::uint8_t hello[2] = {'h', 'i'};
        netplay_send_to(&host, host.peer_address, host.peer_port, hello, sizeof hello);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::uint8_t packet[256];
        for (int got; (got = static_cast<int>(recv(static_cast<RawSocket>(server.socket),
                           reinterpret_cast<char *>(packet), sizeof packet, 0))) > 0;)
            wrapped = wrapped || (got > 6 && std::memcmp(packet, "D2MM", 4) == 0 && packet[5] == 9u);
    }
    recomp_netplay_close(&server);
    recomp_netplay_close(&host);
    return wrapped && !host.match.direct6;
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
static std::vector<std::uint8_t> peerPacket(std::uint16_t port, const std::uint8_t *nonce, std::uint16_t v6Port = 0u)
{
    std::vector<std::uint8_t> packet = {'D', '2', 'M', 'M', 2u, 6u};
    packet.insert(packet.end(), nonce, nonce + RECOMP_NETPLAY_TOKEN_BYTES);
    const std::uint8_t endpoint[6] = {127u, 0u, 0u, 1u, static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port)};
    if (port != 0u) packet.insert(packet.end(), endpoint, endpoint + 6);
    else packet.resize(packet.size() + 6u);
    packet.resize(packet.size() + 6u + 1u + RECOMP_NETPLAY_NAME_BYTES); // no LAN endpoint, no name
    if (v6Port != 0u) { // the other player's IPv6 endpoint, ::1
        packet.resize(packet.size() + 15u);
        packet.push_back(1u);
        packet.push_back(static_cast<std::uint8_t>(v6Port >> 8));
        packet.push_back(static_cast<std::uint8_t>(v6Port));
    }
    return packet;
}

// Pairs a host room on 47620 and a joiner room on 47621, telling each the other's port (0 for none).
static bool matchRooms(std::uint16_t toHost, std::uint16_t toJoiner, int milliseconds, bool &relayed,
    std::uint16_t v6ToHost = 0u, std::uint16_t v6ToJoiner = 0u)
{
    RecompNetplay server, host, joiner;
    // Every listen runs, even after one fails, so each socket is set before the closes below.
    const bool listening[3] = {recomp_netplay_listen(&server, 47622u), recomp_netplay_listen(&host, 47620u),
        recomp_netplay_listen(&joiner, 47621u)};
    bool opened = listening[0] && listening[1] && listening[2] &&
        recomp_netplay_match_server(&host, "127.0.0.1", 47622u) &&
        recomp_netplay_match_server(&joiner, "127.0.0.1", 47622u);
    if (v6ToHost != 0u && !(host.match.has_socket6 && joiner.match.has_socket6)) {
        std::fprintf(stderr, "no IPv6 loopback here; skipping the IPv6 match\n");
        relayed = false;
        recomp_netplay_close(&server);
        recomp_netplay_close(&host);
        recomp_netplay_close(&joiner);
        return true;
    }
    sendFrom(server, 47620u, peerPacket(toHost, host.match.nonce, v6ToHost));
    sendFrom(server, 47621u, peerPacket(toJoiner, joiner.match.nonce, v6ToJoiner));
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
    bool met = hostRoom.pinged && joinerRoom.pinged && std::strcmp(hostRoom.name, "JOINER") == 0 &&
        std::strcmp(joinerRoom.name, "HOST") == 0 && !host.match.punching && !joiner.match.punching;
    if (met && v6ToHost != 0u) { // a host that drops its quiet IPv6-only peer seats it again
        recomp_netplay_room_drop(&host, &hostRoom);
        for (int i = 0; i < 30 && !hostRoom.joined; ++i) {
            recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
            recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        met = hostRoom.joined && host.match.direct6 && joiner.match.direct6;
    }
    // A late pairing for another player leaves the host's talking peer alone.
    const std::uint16_t peer = host.peer_port;
    const bool hostRelay = host.match.relay;
    sendFrom(server, 47620u, peerPacket(47630u, host.match.nonce));
    for (int i = 0; i < 5; ++i) {
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    met = met && host.peer_port == peer && host.match.relay == hostRelay && !host.match.punching;
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
    RecompNetplay server, host, joiner;
    const bool listening[3] = {recomp_netplay_listen(&server, 47623u), recomp_netplay_listen(&host, 47624u),
        recomp_netplay_listen(&joiner, 47625u)};
    check("estimate sockets", listening[0] && listening[1] && listening[2] &&
        recomp_netplay_match_server(&host, "127.0.0.1", 47623u) &&
        recomp_netplay_match_server(&joiner, "127.0.0.1", 47623u));
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
    recomp_netplay_close(&server);
    recomp_netplay_close(&host);
    recomp_netplay_close(&joiner);
}

// A lobby packet with no payload past its header and index, as a joiner sends it.
static std::vector<std::uint8_t> lobbyPacket(std::uint8_t kind, std::uint32_t index)
{
    return {'D', '2', 'L', 'B', 5u, kind, static_cast<std::uint8_t>(index), static_cast<std::uint8_t>(index >> 8),
        static_cast<std::uint8_t>(index >> 16), static_cast<std::uint8_t>(index >> 24)};
}

// The profile chunks a socket receives within 300 ms; their payload bytes add to bytes.
static int chunksArriving(const RecompNetplay &net, int &bytes)
{
    int chunks = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < end) {
        std::uint8_t packet[2048];
        for (int got; (got = static_cast<int>(recv(static_cast<RawSocket>(net.socket),
                           reinterpret_cast<char *>(packet), sizeof packet, 0))) > 0;)
            if (got > 10 && packet[5] == 4u) {
                ++chunks;
                bytes += got - 10;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return chunks;
}

// A host serves the next window of its profile at once, a replayed one after 50 ms, and eight copies in all.
// A busy runner may stall the host past one hold, so the hold checks allow one extra served window; an
// unbounded host serves every replay.
static void serveBounds()
{
    RecompNetplay host = unopened(), fake = unopened();
    RecompNetplayLobbyResult hosted = RECOMP_NETPLAY_LOBBY_SOCKET;
    const RecompNetplayLobby offered = lobby('a', 2u, 0u);
    std::vector<std::uint8_t> mine(16u * 1024u + 1000u), theirs(RECOMP_NETPLAY_MAX_PROFILE); // 17 chunks
    RecompNetplayProfiles profiles{mine.data(), static_cast<std::uint32_t>(mine.size()), theirs.data(), 0u};
    // A port the system picks, so another test or runner holding a fixed one cannot fail this.
    const bool listening = recomp_netplay_listen(&host, 0u);
    const std::uint16_t port = listening ? recomp_netplay_local_port(&host) : 0u;
    if (port == 0u || !recomp_netplay_open(&fake, 0u, "127.0.0.1", port)) {
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
    sendFrom(fake, port, hello);
    int bytes = 0;
    (void)chunksArriving(fake, bytes); // the welcome
    sendFrom(fake, port, lobbyPacket(3u, 0u));
    sendFrom(fake, port, lobbyPacket(3u, 16u));
    for (int i = 0; i < 8; ++i) sendFrom(fake, port, lobbyPacket(3u, 0u));
    const int first = chunksArriving(fake, bytes);
    check("next window served at once, replays held", first == 17 || first == 33);
    sendFrom(fake, port, lobbyPacket(3u, 0u));
    for (int i = 0; i < 8; ++i) sendFrom(fake, port, lobbyPacket(3u, 16u));
    const int back = chunksArriving(fake, bytes);
    check("a replay does not move the served window back", back == 16 || back == 17);
    // 33,768 to 51,152 bytes sent so far; nine more requests reach eight copies of 17,384.
    for (int i = 0; i < 9; ++i) {
        sendFrom(fake, port, lobbyPacket(3u, 0u));
        (void)chunksArriving(fake, bytes);
    }
    check("eight copies end serving", bytes > 8 * 17384 - 1024 && bytes <= 8 * 17384);
    sendFrom(fake, port, lobbyPacket(3u, 17u)); // holds them all
    wait.join();
    check("serve lobby ends", hosted == RECOMP_NETPLAY_LOBBY_OK);
    recomp_netplay_close(&host);
    recomp_netplay_close(&fake);
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

    // A different build is refused, and the host waits on for the right one.
    const RecompNetplayLobby other = lobby('b', 2u, 0u);
    wait = std::thread([&] { hosted = hostOn(&host, 47612u, &offered, &none, 5000u, nullptr); });
    asked = other;
    const RecompNetplayLobbyResult refused =
        joinTo(&joiner, 47612u, &asked, &noneJoiner, 5000u, nullptr);
    recomp_netplay_close(&joiner);
    asked = offered;
    const RecompNetplayLobbyResult retried =
        joinTo(&joiner, 47612u, &asked, &noneJoiner, 5000u, nullptr);
    wait.join();
    check("mismatch", refused == RECOMP_NETPLAY_LOBBY_MISMATCH && retried == RECOMP_NETPLAY_LOBBY_OK &&
        hosted == RECOMP_NETPLAY_LOBBY_OK);
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
    // The next line waits out the receiver's second, then is read and dropped as not printable.
    recomp_netplay_room_say(&joinerRoom, "bell\a");
    const auto bellSaid = std::chrono::steady_clock::now();
    for (int i = 0; i < 100 && hostRoom.heard_id != joinerRoom.say_id; ++i) {
        recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", true, false);
        recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check("chat a line a second", hostRoom.heard_id == joinerRoom.say_id &&
        std::chrono::steady_clock::now() - bellSaid >= std::chrono::milliseconds(900));
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

    // A joiner that leaves frees the host; a host that leaves sends the joiner away for good,
    // so the room it opens again on the same port starts empty. A session's goodbye closes nothing.
    {
        const auto meet = [&](RecompNetplayRoom &a, RecompNetplayRoom &b) {
            for (int i = 0; i < 50 && !(a.joined && b.joined); ++i) {
                recomp_netplay_room_tick(&joiner, &a, digest, "JOINER", false, false);
                recomp_netplay_room_tick(&host, &b, digest, "HOST", false, false);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return a.joined && b.joined;
        };
        hostRoom = RecompNetplayRoom{};
        joinerRoom = RecompNetplayRoom{};
        hostRoom.hosting = true;
        check("leave room opens", recomp_netplay_listen(&host, 47626u) &&
            recomp_netplay_open(&joiner, 0u, "127.0.0.1", 47626u) && meet(joinerRoom, hostRoom));
        recomp_netplay_send_goodbye(&host);
        for (int i = 0; i < 10; ++i) {
            recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check("a session goodbye stays out of the room", !joinerRoom.left && joinerRoom.joined);

        recomp_netplay_room_leave(&joiner); // the socket stays open, so it can meet the host again
        for (int i = 0; i < 25 && !hostRoom.left; ++i) {
            recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check("host hears the joiner leave", hostRoom.left && !hostRoom.joined && host.peer_port == 0u);

        hostRoom = RecompNetplayRoom{};
        joinerRoom = RecompNetplayRoom{};
        hostRoom.hosting = true;
        check("leave room meets again", meet(joinerRoom, hostRoom));
        recomp_netplay_room_leave(&host);
        recomp_netplay_close(&host);
        for (int i = 0; i < 25 && !joinerRoom.left; ++i) {
            recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check("joiner hears the host leave", joinerRoom.left && !joinerRoom.joined && joiner.peer_port == 0u);
        hostRoom = RecompNetplayRoom{};
        hostRoom.hosting = true;
        check("leave room reopens", recomp_netplay_listen(&host, 47626u));
        for (int i = 0; i < 25; ++i) {
            recomp_netplay_room_tick(&joiner, &joinerRoom, digest, "JOINER", false, false);
            recomp_netplay_room_tick(&host, &hostRoom, digest, "HOST", false, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check("a reopened room starts empty", !hostRoom.joined && host.peer_port == 0u);
        recomp_netplay_close(&host);
        recomp_netplay_close(&joiner);
    }

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
        RecompNetplay forger;
        RecompNetplayRoom doubtedRoom{};
        doubtedRoom.hosting = true;
        check("doubted room opens", recomp_netplay_listen(&host, 47643u) &&
            recomp_netplay_open(&forger, 0u, "127.0.0.1", 47643u));
        sendFrom(forger, 47643u, roomPacket());
        int answers = 0;
        for (int i = 0; i < 25; ++i) {
            tickRoom(host, doubtedRoom, digest, 1);
            answers += drainKind(forger, 5u);
        }
        check("doubted peer answered one for one and not seated", !doubtedRoom.joined && answers == 1);
        // A joiner at its own address takes the seat while the forged packets keep coming, also
        // when a forged one is the last the host reads each tick.
        RecompNetplayRoom realRoom{};
        const bool called = recomp_netplay_open(&joiner, 0u, "127.0.0.1", 47643u);
        for (int i = 0; i < 100 && !(doubtedRoom.joined && realRoom.joined); ++i) {
            sendFrom(forger, 47643u, roomPacket());
            recomp_netplay_room_tick(&joiner, &realRoom, digest, "JOINER", false, false);
            sendFrom(forger, 47643u, roomPacket());
            tickRoom(host, doubtedRoom, digest, 1);
        }
        check("a joiner gets past a forged one", called && realRoom.joined && doubtedRoom.joined &&
            std::strcmp(doubtedRoom.name, "JOINER") == 0 && host.match.direct);
        recomp_netplay_close(&joiner);
        recomp_netplay_close(&forger);
        recomp_netplay_close(&host);
    }

    // A host back from a session keeps the seat from others, gives up on a last joiner who never
    // returns, and lets another player in.
    {
        RecompNetplayRoom backRoom{};
        backRoom.hosting = true;
        RecompNetplay stranger;
        const bool opened = recomp_netplay_listen(&host, 47643u) &&
            recomp_netplay_open(&stranger, 0u, "127.0.0.1", 47643u);
        host.peer_address = htonl(0x7f000001u);
        host.peer_port = htons(47642u);
        recomp_netplay_room_hold(&host, &backRoom);
        sendFrom(stranger, 47643u, roomPacket());
        tickRoom(host, backRoom, digest, 5);
        const bool kept = host.peer_port == htons(47642u) && host.match.busy && !backRoom.joined &&
            drainKind(stranger, 5u) == 0;
        backRoom.held_until_ms -= 10001u; // 10 s on
        recomp_netplay_room_tick(&host, &backRoom, digest, "HOST", false, false);
        check("a returned host drops a joiner who stays away", opened && kept && host.peer_port == 0u && !host.match.busy);
        recomp_netplay_close(&stranger);
        recomp_netplay_close(&host);
    }

    // A clock the host sent one address seats no one from another: each address is sent its own.
    {
        RecompNetplay visitor, forger;
        RecompNetplayRoom saltedRoom{};
        saltedRoom.hosting = true;
        const bool opened = recomp_netplay_listen(&host, 47646u) &&
            recomp_netplay_open(&visitor, 0u, "127.0.0.1", 47646u) && recomp_netplay_open(&forger, 0u, "127.0.0.1", 47646u);
        std::vector<std::uint8_t> echoing = roomPacket();
        sendFrom(visitor, 47646u, echoing);
        std::uint8_t shown[2048];
        std::size_t got = 0u;
        for (int i = 0; i < 25 && got != echoing.size(); ++i) {
            tickRoom(host, saltedRoom, digest, 1);
            got = static_cast<std::size_t>(recv(static_cast<RawSocket>(visitor.socket), reinterpret_cast<char *>(shown),
                sizeof shown, 0));
        }
        // The sender's clock comes first; the last clock it heard follows.
        std::memcpy(echoing.data() + RoomClocks + 4u, shown + RoomClocks, 4u);
        sendFrom(forger, 47646u, echoing);
        tickRoom(host, saltedRoom, digest, 5);
        check("another address's clock seats no one", opened && got == echoing.size() && !saltedRoom.joined);
        // The address the clock was sent to is seated by it.
        sendFrom(visitor, 47646u, echoing);
        tickRoom(host, saltedRoom, digest, 5);
        check("its own clock seats an address", saltedRoom.joined);
        recomp_netplay_close(&visitor);
        recomp_netplay_close(&forger);
        recomp_netplay_close(&host);
    }

    // A flood of chunk requests draws one 16-chunk burst per 50 ms, not 16 chunks per request.
    {
        RecompNetplay flooder;
        RecompNetplayProfiles served{hostSent.data(), static_cast<std::uint32_t>(hostSent.size()), hostGot.data(), 0u};
        cancelNow = false;
        wait = std::thread([&] { hosted = hostOn(&host, 47640u, &offered, &served, 5000u, cancelled); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        check("flood socket", recomp_netplay_open(&flooder, 0u, "127.0.0.1", 47640u));
        const std::vector<std::uint8_t> hello = helloPacket();
        const std::vector<std::uint8_t> request = {'D', '2', 'L', 'B', 5u, 3u, 0u, 0u, 0u, 0u};
        int chunks = 0;
        const auto drain = [&] {
            std::uint8_t packet[2048];
            for (int got; (got = static_cast<int>(recv(static_cast<RawSocket>(flooder.socket),
                               reinterpret_cast<char *>(packet), sizeof packet, 0))) > 0;)
                chunks += got > 6 && std::memcmp(packet, "D2LB", 4) == 0 && packet[5] == 4u;
        };
        // Say hello until the host welcomes it, so a slow host thread cannot drop the only one.
        bool welcomed = false;
        for (int i = 0; i < 100 && !welcomed; ++i) {
            sendFrom(flooder, 47640u, hello);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::uint8_t packet[2048];
            for (int got; (got = static_cast<int>(recv(static_cast<RawSocket>(flooder.socket),
                               reinterpret_cast<char *>(packet), sizeof packet, 0))) > 0;)
                welcomed = welcomed || (got > 6 && std::memcmp(packet, "D2LB", 4) == 0 && packet[5] == 2u);
        }
        check("flood welcomed", welcomed);
        for (int i = 0; i < 200; ++i) {
            sendFrom(flooder, 47640u, request);
            drain();
        }
        for (int i = 0; i < 15; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            drain();
        }
        cancelNow = true;
        wait.join();
        cancelNow = false;
        check("request flood throttled", chunks > 0 && chunks <= 160);
        if (chunks == 0 || chunks > 160) std::fprintf(stderr, "flood drew %d chunks\n", chunks);
        recomp_netplay_close(&flooder);
        recomp_netplay_close(&host);
    }

    // A room's socket arrives with its peer, so a hello from any other address cannot take the match.
    {
        RecompNetplay forger;
        check("handed-over socket", recomp_netplay_listen(&host, 47641u) &&
            recomp_netplay_open(&forger, 0u, "127.0.0.1", 47641u));
        host.peer_address = htonl(0x7f000001u);
        host.peer_port = htons(47642u);
        wait = std::thread([&] { hosted = recomp_netplay_host(&host, &offered, &none, 5000u, nullptr); });
        int welcomes = 0;
        for (int i = 0; i < 10; ++i) {
            sendFrom(forger, 47641u, helloPacket());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            welcomes += drainKind(forger, 2u);
        }
        asked = offered;
        const bool joinedPeer = recomp_netplay_open(&joiner, 47642u, "127.0.0.1", 47641u) &&
            recomp_netplay_join(&joiner, &asked, &noneJoiner, 5000u, nullptr) == RECOMP_NETPLAY_LOBBY_OK;
        wait.join();
        welcomes += drainKind(forger, 2u);
        check("forged hello ignored", welcomes == 0 && joinedPeer && hosted == RECOMP_NETPLAY_LOBBY_OK &&
            host.peer_port == htons(47642u));
        recomp_netplay_close(&forger);
        recomp_netplay_close(&host);
        recomp_netplay_close(&joiner);
    }

    // A packet from the matchmaker's address that does not echo the socket's nonce is forged, and ignored.
    {
        RecompNetplay server, room;
        const bool listening[2] = {recomp_netplay_listen(&server, 47644u), recomp_netplay_listen(&room, 47645u)};
        const bool opened = listening[0] && listening[1] && recomp_netplay_match_server(&room, "127.0.0.1", 47644u);
        std::uint8_t wrong[RECOMP_NETPLAY_TOKEN_BYTES];
        std::memcpy(wrong, room.match.nonce, sizeof wrong);
        wrong[0] ^= 1u;
        sendFrom(server, 47645u, peerPacket(47630u, wrong));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recomp_netplay_match_poll(&room);
        const bool ignored = !room.match.met;
        sendFrom(server, 47645u, peerPacket(47630u, room.match.nonce));
        for (int i = 0; i < 50 && !room.match.met; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            recomp_netplay_match_poll(&room);
        }
        check("forged matchmaker packet ignored", opened && ignored && room.match.met);

        // Relayed packets are not from a peer who proved its own address: the server paired someone else since.
        std::vector<std::uint8_t> wrapped = {'D', '2', 'M', 'M', 2u, 9u};
        wrapped.insert(wrapped.end(), room.match.nonce, room.match.nonce + RECOMP_NETPLAY_TOKEN_BYTES);
        wrapped.resize(wrapped.size() + 4u);
        room.match.direct = true;
        sendFrom(server, 47645u, wrapped);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recomp_netplay_match_poll(&room);
        check("relay ignored beside a direct peer", !room.match.relay);
        room.match.direct = false;

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
        sendFrom(server, 47645u, cookie);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recomp_netplay_match_poll(&room);
        const std::vector<std::uint8_t> again = heardHost();
        check("cookie sent again", !first.empty() && !again.empty() &&
            std::all_of(first.begin() + 14, first.begin() + 22, [](std::uint8_t b) { return b == 0u; }) &&
            std::equal(cookie.begin() + 14, cookie.end(), again.begin() + 14));

        // A datagram longer than any packet does not end the pass: the one behind it is read too.
        room.match.met = false;
        sendFrom(server, 47645u, std::vector<std::uint8_t>(1400u));
        sendFrom(server, 47645u, peerPacket(47631u, room.match.nonce));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        recomp_netplay_match_poll(&room);
        check("oversized datagram skipped", room.match.met);
        recomp_netplay_close(&server);
        recomp_netplay_close(&room);
    }

    // Matched rooms punch through to each other, or meet through the relay without
    // endpoints, or after 5 s of punching at endpoints that never answer.
    bool relayed = true;
    // Each waits only until both rooms meet; punching must win inside the 5 s before the relay takes over.
    check("matched rooms punch through", matchRooms(47621u, 47620u, 4500, relayed) && !relayed);
    // IPv4 endpoints nobody listens on; only the IPv6 ones reach, so the rooms meet over ::1 and do not relay.
    check("matched rooms punch over IPv6 when IPv4 cannot", matchRooms(47629u, 47628u, 4500, relayed, 47621u, 47620u) && !relayed);
    check("matched rooms relay without endpoints", matchRooms(0u, 0u, 5000, relayed) && relayed);
    check("matched rooms relay when punching fails", matchRooms(47629u, 47628u, 10000, relayed) && relayed);
    check("a relay fallback wins over a kept IPv6 path", relayOverridesIpv6());
    estimatePings();
    serveBounds();

    if (failures != 0) return 1;
    std::puts("PASS netplay lobby joins one build, swaps profiles, refuses another and runs a room");
    return 0;
}
