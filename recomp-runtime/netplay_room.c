/* The room: what two players say to each other before a match, and how a
   host decides who its other player is. */
#include "netplay_wire.h"

#include <stdio.h>
#include <string.h>

/* What the clocks a host sends an address it was not given are offset by. Every player this host
   answers sees its clock, so without this a clock shown to one address could be echoed from a forged one.
   shortcut: a keyed mixer, not a MAC; replace it if a seat ever guards more than the room. */
static uint32_t clock_salt(const RecompNetplay *net, const struct sockaddr_in *to)
{
    uint64_t mixed = net->match.clock_key ^ ((uint64_t)to->sin_addr.s_addr << 16 | to->sin_port);
    mixed = (mixed ^ mixed >> 30) * 0xbf58476d1ce4e5b9u; /* splitmix64's finalizer */
    mixed = (mixed ^ mixed >> 27) * 0x94d049bb133111ebu;
    return (uint32_t)((mixed ^ mixed >> 31) >> 32);
}

/* How long ago this side sent the clock a packet echoes. Signed: the clock is forced odd, so it
   can be 1 ms ahead. */
static int32_t clock_age(uint64_t now, uint32_t salt, uint32_t echo)
{
    return (int32_t)((uint32_t)now + salt - echo);
}

/* Fills a room packet but for its chat line and returns where that goes.
   Layout: magic, version, kind, digest, flags, name length, name, the
   sender's clock, the last clock it heard and how long ago, for the ping,
   a chat line's id, length and text, then the sender's install ID. */
static uint8_t *room_packet(uint8_t *packet, const RecompNetplayRoom *room, const char *digest, const char *name,
    bool ready, bool start, uint32_t clock, uint32_t echo, uint64_t age)
{
    memset(packet, 0, ROOM_BYTES);
    put32(packet, LOBBY_MAGIC);
    packet[4] = LOBBY_VERSION;
    packet[5] = LOBBY_ROOM;
    memcpy(packet + 6u, digest, RECOMP_NETPLAY_DIGEST_BYTES);
    uint8_t *at = packet + 6u + RECOMP_NETPLAY_DIGEST_BYTES;
    at[0] = (uint8_t)((ready ? ROOM_READY : 0u) | (start ? ROOM_START : 0u));
    const size_t length = strlen(name);
    at[1] = (uint8_t)(length < RECOMP_NETPLAY_NAME_BYTES ? length : RECOMP_NETPLAY_NAME_BYTES);
    memcpy(at + 2u, name, at[1]);
    at = packet + ROOM_CLOCKS;
    put32(at, clock | 1u); /* never 0, which means no clock heard */
    put32(at + 4u, echo);
    put16(at + 8u, (uint16_t)(echo == 0u ? 0u : age > 65535u ? 65535u : age));
    put32(packet + ROOM_BYTES - 8u, (uint32_t)room->id);
    put32(packet + ROOM_BYTES - 4u, (uint32_t)(room->id >> 32));
    return at + 10u;
}

/* An empty host hears anyone, at an address that may be forged. It takes a caller only when
   they echo a clock sent to that address in the last 3 s, and answers any other one packet
   for one. Nothing is kept until then, so forged packets neither hold the room nor keep a
   player out. True for a caller to take, whose clocks room->salt then offsets. */
static bool room_caller_proved(const RecompNetplay *net, RecompNetplayRoom *room, const struct sockaddr_in *from,
    const uint8_t *packet, uint64_t now, const char *digest, const char *name)
{
    const uint32_t salt = clock_salt(net, from), echo = get32(packet + ROOM_CLOCKS + 4u);
    const int32_t age = clock_age(now, salt, echo);
    if (echo != 0u && age >= -1 && age <= (int32_t)ROOM_SILENCE_MS) {
        room->salt = salt;
        return true;
    }
    uint8_t answer[ROOM_BYTES];
    room_packet(answer, room, digest, name, false, false, (uint32_t)now + salt, get32(packet + ROOM_CLOCKS), 0u);
    netplay_send_raw(net, from->sin_addr.s_addr, from->sin_port, answer, ROOM_BYTES);
    return false;
}

void recomp_netplay_room_tick(RecompNetplay *net, RecompNetplayRoom *room, const char *digest,
    const char *name, bool ready, bool start)
{
    const uint64_t now = netplay_now_ms();
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u];
    struct sockaddr_in from;
    int size;
    while ((size = netplay_receive_from(net, packet, sizeof packet, &from)) >= 0) {
        if (room->hosting && !start && size == LOBBY_BYTES && get32(packet) == LOBBY_MAGIC &&
            packet[5] == LOBBY_HELLO) {
            /* A joiner calling a match this host cancelled: send it back to its room. */
            uint8_t goodbye[4];
            put32(goodbye, GOODBYE_MAGIC);
            netplay_send_to(net, from.sin_addr.s_addr, from.sin_port, goodbye, sizeof goodbye);
            continue;
        }
        if (size == 4 && get32(packet) == ROOM_CLOSED_MAGIC) {
            /* A session's goodbye can linger into the room, so leaving has its own packet. */
            if (net->peer_port != 0u && netplay_from_peer(net, &from)) {
                fprintf(stderr, "recomp netplay: the other player left the room\n");
                recomp_netplay_room_drop(net, room);
                net->peer_port = 0u; /* a joiner stops calling the closed room */
                room->left = true;
            }
            continue;
        }
        if (size != ROOM_BYTES || get32(packet) != LOBBY_MAGIC || packet[4] != LOBBY_VERSION ||
            packet[5] != LOBBY_ROOM)
            continue;
        RecompNetplayMatch *m = &net->match;
        bool candidate = false;
        for (unsigned i = 0u; i < 2u; ++i)
            candidate = candidate || (m->punching && m->candidate_port[i] == from.sin_port &&
                                         m->candidate_address[i] == from.sin_addr.s_addr);
        const uint8_t *sender = packet + ROOM_BYTES - 8u;
        if (room->blocked != NULL && room->blocked((uint64_t)get32(sender + 4u) << 32 | get32(sender))) {
            /* Say nothing back. Relay stays on, so the server's relayed packets end here quietly. */
            /* Only this room's own peer, a candidate or, for an empty host, the would-be peer. */
            if (netplay_from_peer(net, &from) || candidate || (room->hosting && !room->joined)) room->refused = true;
            if (netplay_from_peer(net, &from) || candidate) {
                net->peer_port = 0u;
                m->punching = false;
            }
            continue;
        }
        if (!netplay_from_peer(net, &from)) {
            /* A joiner takes either endpoint the matchmaker gave. A held seat waits for its last joiner. */
            if (room->joined || room->held_until_ms != 0u || (!room->hosting && !candidate)) continue;
            if (!candidate && !room_caller_proved(net, room, &from, packet, now, digest, name)) continue;
            net->peer_address = from.sin_addr.s_addr;
            net->peer_port = from.sin_port;
            room->pinged = false;
            m->direct = !candidate; /* a caller who proved its own address */
            m->relay = false; /* heard directly */
        }
        if (m->punching) { /* relayed packets end punching first, so this one came directly */
            m->punching = false;
            fprintf(stderr, "recomp netplay: reached the other player directly\n");
        }
        const uint8_t *at = packet + 6u + RECOMP_NETPLAY_DIGEST_BYTES;
        const uint8_t *clocks = packet + ROOM_CLOCKS;
        room->echo = get32(clocks);
        room->echo_at_ms = now;
        const uint32_t echo = get32(clocks + 4u), age = get16(clocks + 8u);
        room->joined = true;
        m->busy = true;
        room->heard_ms = now;
        room->mismatch = memcmp(packet + 6u, digest, RECOMP_NETPLAY_DIGEST_BYTES) != 0;
        room->ready = (at[0] & ROOM_READY) != 0u;
        room->start = (at[0] & ROOM_START) != 0u;
        const size_t length = at[1] < RECOMP_NETPLAY_NAME_BYTES ? at[1] : RECOMP_NETPLAY_NAME_BYTES;
        memcpy(room->name, at + 2u, length);
        room->name[length] = '\0';
        room->peer_id = (uint64_t)get32(sender + 4u) << 32 | get32(sender);
        if (echo != 0u) {
            const int64_t ping = (int64_t)clock_age(now, room->salt, echo) - (int64_t)age;
            room->ping_ms = ping > 0 ? (uint32_t)ping : 0u;
            room->pinged = true;
        }
        at = clocks + 10u;
        const uint32_t id = get32(at);
        const size_t said = at[4];
        /* One line a second; a line held back here comes again in a later repeat. */
        if (id != room->heard_id && said != 0u && said <= RECOMP_NETPLAY_CHAT_BYTES &&
            now - room->heard_line_ms >= 1000u) {
            room->heard_id = id;
            room->heard_line_ms = now;
            char line[RECOMP_NETPLAY_CHAT_BYTES + 1u];
            bool printable = true;
            for (size_t i = 0u; i < said; ++i) printable = printable && at[5u + i] >= 0x20u && at[5u + i] < 0x7fu;
            memcpy(line, at + 5u, said);
            line[said] = '\0';
            if (printable && !recomp_netplay_name_banned(line)) {
                memcpy(room->heard, line, said + 1u);
                room->heard_new = true;
            }
        }
    }
    const bool quiet = room->joined && now - room->heard_ms > ROOM_SILENCE_MS;
    const bool stayed_away = !room->joined && room->held_until_ms != 0u && now > room->held_until_ms;
    if (quiet || stayed_away) recomp_netplay_room_drop(net, room);
    /* punch_ms can be newer than now when this tick read the match itself. */
    if (net->match.punching && now > net->match.punch_ms + PUNCH_MS) netplay_relay_on(net, "no direct path after 5 s");
    if (net->peer_port == 0u || (!start && now - room->sent_ms < 100u)) return;
    room->sent_ms = now;
    uint8_t *at = room_packet(packet, room, digest, name, ready, start, (uint32_t)now + room->salt, room->echo,
        now - room->echo_at_ms);
    if (now - room->said_ms < 2000u) {
        put32(at, room->say_id);
        at[4] = (uint8_t)strlen(room->say);
        memcpy(at + 5u, room->say, at[4]);
    }
    const RecompNetplayMatch *m = &net->match;
    if (!m->punching) {
        netplay_send_to(net, net->peer_address, net->peer_port, packet, ROOM_BYTES);
        return;
    }
    /* Sending first opens this side's router for the other's answer. */
    for (unsigned i = 0u; i < 2u; ++i)
        if (m->candidate_port[i] != 0u) netplay_send_raw(net, m->candidate_address[i], m->candidate_port[i], packet, ROOM_BYTES);
    netplay_punch6(net, packet, ROOM_BYTES);
}

void recomp_netplay_room_say(RecompNetplayRoom *room, const char *text)
{
    const uint64_t now = netplay_now_ms();
    const size_t length = strlen(text);
    const size_t kept = length < RECOMP_NETPLAY_CHAT_BYTES ? length : RECOMP_NETPLAY_CHAT_BYTES;
    memcpy(room->say, text, kept);
    room->say[kept] = '\0';
    /* The clock makes each line's id differ from those of earlier rooms, too. */
    const uint32_t id = (uint32_t)now | 1u;
    room->say_id = id == room->say_id ? id + 2u : id;
    room->said_ms = now;
    room->sent_ms = 0u; /* on the next tick */
}

void recomp_netplay_room_drop(RecompNetplay *net, RecompNetplayRoom *room)
{
    room->joined = room->ready = room->start = room->pinged = room->mismatch = false;
    room->echo = 0u;
    room->salt = 0u;
    room->held_until_ms = 0u;
    net->match.busy = false;
    net->match.direct = false; /* direct6 stays, as relay does for a joiner: IPv6 may be the only path */
    if (room->hosting) { /* let another player in */
        net->peer_port = 0u;
        net->match.relay = false;
    }
}

void recomp_netplay_room_hold(RecompNetplay *net, RecompNetplayRoom *room)
{
    room->held_until_ms = netplay_now_ms() + ROOM_RETURN_MS;
    net->match.busy = true;
}

void recomp_netplay_room_leave(const RecompNetplay *net)
{
    if (net->peer_port == 0u) return;
    uint8_t packet[4];
    put32(packet, ROOM_CLOSED_MAGIC);
    for (int i = 0; i < 3; ++i) netplay_send_to(net, net->peer_address, net->peer_port, packet, sizeof packet);
}
