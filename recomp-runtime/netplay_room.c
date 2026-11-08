#include "netplay_internal.h"

#include <stdio.h>
#include <string.h>


/* The matchmaker's packets (doa2u-re's tools/matchmaker.py): "D2MM", version, kind,
   then a body whose numbers are big endian. */
static const uint8_t MATCH_MAGIC[4] = {'D', '2', 'M', 'M'};
enum {
    MATCH_VERSION = 1u,
    MATCH_HOST = 1u, MATCH_HOSTED, MATCH_LIST, MATCH_ROOMS, MATCH_JOIN, MATCH_PEER, MATCH_GONE, MATCH_LEAVE,
    MATCH_RELAY,
    MATCH_HEADER = 6u,
    MATCH_LIST_BYTES = 512u, /* a list request is padded, so its answer is never larger */
    MATCH_WITH_ENDPOINTS = 1u, /* after a list request's digest: send each host's public endpoint */
    MATCH_ENTRY = 40u,
    MATCH_ENTRY_AT = MATCH_ENTRY + 6u,
    MATCH_PEER_BYTES = MATCH_HEADER + 6u + 6u + 1u + RECOMP_NETPLAY_NAME_BYTES,
    PUNCH_MS = 5000u,
};

static void send_raw(const RecompNetplay *net, uint32_t address, uint16_t port,
    const uint8_t *packet, size_t size)
{
    struct sockaddr_in peer;
    memset(&peer, 0, sizeof peer);
    peer.sin_family = AF_INET;
    peer.sin_port = port;
    peer.sin_addr.s_addr = address;
    /* A full send buffer drops this packet; the next one repeats its pads. */
    (void)sendto((SOCKET)net->socket, (const char *)packet, (int)size, 0,
        (const struct sockaddr *)&peer, sizeof peer);
}

static void match_header(uint8_t *out, uint8_t kind)
{
    memcpy(out, MATCH_MAGIC, sizeof MATCH_MAGIC);
    out[4] = MATCH_VERSION;
    out[5] = kind;
}

/* A relaying socket reaches its peer through the matchmaker, which knows
   where to forward. */
void netplay_send_to(const RecompNetplay *net, uint32_t address, uint16_t port,
    const uint8_t *packet, size_t size)
{
    uint8_t wrapped[MATCH_HEADER + RECOMP_NETPLAY_MAX_PACKET];
    if (net->match.relay && address == net->peer_address && port == net->peer_port &&
        size <= RECOMP_NETPLAY_MAX_PACKET) {
        match_header(wrapped, MATCH_RELAY);
        memcpy(wrapped + MATCH_HEADER, packet, size);
        send_raw(net, net->match.server_address, net->match.server_port, wrapped, MATCH_HEADER + size);
        return;
    }
    send_raw(net, address, port, packet, size);
}

static uint32_t get32be(const uint8_t *in)
{
    return (uint32_t)in[0] << 24 | (uint32_t)in[1] << 16 | (uint32_t)in[2] << 8 | in[3];
}

static void relay_on(RecompNetplay *net, const char *why)
{
    RecompNetplayMatch *m = &net->match;
    if (m->relay) return;
    m->relay = true;
    m->punching = false;
    /* Without an endpoint the server's own stands for the peer; wrapped packets ignore it. */
    if (net->peer_port == 0u) {
        net->peer_address = m->server_address;
        net->peer_port = m->server_port;
    }
    fprintf(stderr, "recomp netplay: %s; relaying through the matchmaker\n", why);
}

static void match_answer(RecompNetplay *net, const uint8_t *packet, size_t size)
{
    RecompNetplayMatch *m = &net->match;
    if (packet[5] == MATCH_HOSTED && size == MATCH_HEADER + 10u) {
        const uint32_t id = get32be(packet + MATCH_HEADER);
        if (id != m->room_id) fprintf(stderr, "recomp netplay: listed on the matchmaker as room %u\n", id);
        m->room_id = id;
    } else if (packet[5] == MATCH_ROOMS && size >= MATCH_HEADER + 1u && packet[MATCH_HEADER] <= RECOMP_NETPLAY_LISTED_ROOMS) {
        const uint32_t count = packet[MATCH_HEADER];
        /* An older server lists no endpoints. */
        const size_t entry = size == MATCH_HEADER + 1u + count * MATCH_ENTRY_AT ? MATCH_ENTRY_AT : MATCH_ENTRY;
        if (size != MATCH_HEADER + 1u + count * entry) return;
        RecompNetplayListedRoom before[RECOMP_NETPLAY_LISTED_ROOMS];
        const uint32_t before_count = m->room_count;
        memcpy(before, m->rooms, sizeof before);
        m->room_count = count;
        for (uint32_t i = 0u; i < m->room_count; ++i) {
            const uint8_t *at = packet + MATCH_HEADER + 1u + i * entry;
            RecompNetplayListedRoom *room = &m->rooms[i];
            const size_t length = at[7] < RECOMP_NETPLAY_NAME_BYTES ? at[7] : RECOMP_NETPLAY_NAME_BYTES;
            memset(room, 0, sizeof *room);
            room->id = get32be(at);
            room->age_s = (uint32_t)at[4] << 8 | at[5];
            room->delay = at[6];
            memcpy(room->name, at + 8u, length);
            room->name[length] = '\0';
            if (entry == MATCH_ENTRY_AT) {
                memcpy(&room->address, at + MATCH_ENTRY, 4u);
                memcpy(&room->port, at + MATCH_ENTRY + 4u, 2u);
            }
            /* A room listed again keeps the ping it had. */
            for (uint32_t j = 0u; j < before_count; ++j)
                if (before[j].id == room->id && before[j].address == room->address && before[j].port == room->port) {
                    room->ping_ms = before[j].ping_ms;
                    room->pinged = before[j].pinged;
                }
        }
        m->listed = true;
    } else if (packet[5] == MATCH_GONE) {
        m->gone = true;
    } else if (packet[5] == MATCH_PEER && size == MATCH_PEER_BYTES) {
        if (m->busy) return;
        uint32_t address[2];
        uint16_t port[2];
        for (unsigned i = 0u; i < 2u; ++i) {
            memcpy(&address[i], packet + MATCH_HEADER + 6u * i, 4u);
            memcpy(&port[i], packet + MATCH_HEADER + 6u * i + 4u, 2u);
        }
        /* The server says it again for a repeated join; the pair is already met. */
        if (m->met && memcmp(address, m->candidate_address, sizeof address) == 0 &&
            memcmp(port, m->candidate_port, sizeof port) == 0)
            return;
        memcpy(m->candidate_address, address, sizeof address);
        memcpy(m->candidate_port, port, sizeof port);
        m->met = true;
        m->relay = false;
        const unsigned first = port[0] != 0u ? 0u : 1u;
        if (port[first] == 0u) {
            net->peer_port = 0u;
            relay_on(net, "matched without endpoints");
            return;
        }
        net->peer_address = address[first];
        net->peer_port = port[first];
        m->punching = true;
        m->punch_ms = now_ms();
        char text[2][INET_ADDRSTRLEN];
        for (unsigned i = 0u; i < 2u; ++i)
            if (inet_ntop(AF_INET, &address[i], text[i], sizeof text[i]) == NULL) text[i][0] = '\0';
        fprintf(stderr, "recomp netplay: matched; punching to %s:%u and %s:%u\n", text[0], ntohs(port[0]), text[1],
            ntohs(port[1]));
    }
}

/* The next queued packet's size, or -1 when none is left. The matchmaker's
   answers are recorded in net->match on the way, and a relayed packet comes
   out unwrapped, from the peer. */
int netplay_receive_from(RecompNetplay *net, uint8_t *packet, size_t capacity,
    struct sockaddr_in *from)
{
    uint8_t raw[MATCH_HEADER + RECOMP_NETPLAY_MAX_PACKET + 1u];
    for (;;) {
        SocketLength from_size = sizeof *from;
        const int size = (int)recvfrom((SOCKET)net->socket, (char *)raw, (int)sizeof raw, 0,
            (struct sockaddr *)from, &from_size);
        /* Windows reports an ICMP port-unreachable from an earlier send as a
           failed receive; it is not fatal before the peer has bound its port. */
        if (size < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAECONNRESET) continue;
#endif
            return -1;
        }
        if (from->sin_family != AF_INET) continue;
        const uint8_t *body = raw;
        size_t length = (size_t)size;
        RecompNetplayMatch *m = &net->match;
        if (m->server_port != 0u && from->sin_addr.s_addr == m->server_address && from->sin_port == m->server_port) {
            if (length < MATCH_HEADER || memcmp(raw, MATCH_MAGIC, sizeof MATCH_MAGIC) != 0 || raw[4] != MATCH_VERSION)
                continue;
            if (raw[5] != MATCH_RELAY) {
                match_answer(net, raw, length);
                continue;
            }
            /* The other side gave up on a direct path, so this one follows. */
            relay_on(net, "the other player relays");
            from->sin_addr.s_addr = net->peer_address;
            from->sin_port = net->peer_port;
            body += MATCH_HEADER;
            length -= MATCH_HEADER;
        }
        if (length > capacity) length = capacity;
        memcpy(packet, body, length);
        return (int)length;
    }
}

/* Layout: magic, version, kind, digest, flags, name length, name, the
   sender's clock, the last clock it heard and how long ago, for the ping,
   a chat line's id, length and text, then the sender's install ID. */
void recomp_netplay_room_tick(RecompNetplay *net, RecompNetplayRoom *room, const char *digest,
    const char *name, bool ready, bool start)
{
    const uint64_t now = now_ms();
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
        if (size == PROBE_BYTES && get32(packet) == PROBE_MAGIC) {
            /* A FIND MATCH ping. Only a listed room answers, so a private one stays quiet. */
            if (room->hosting && net->match.room_id != 0u && get32(packet + 4) == net->match.room_id) {
                put32(packet, ECHO_MAGIC);
                send_raw(net, from.sin_addr.s_addr, from.sin_port, packet, PROBE_BYTES);
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
            if (from_peer(net, &from) || candidate || (room->hosting && !room->joined)) room->refused = true;
            if (from_peer(net, &from) || candidate) {
                net->peer_port = 0u;
                m->punching = false;
            }
            continue;
        }
        if (!from_peer(net, &from)) {
            /* A host takes the first player who talks; a joiner either endpoint the matchmaker gave. */
            if (room->joined || (!room->hosting && !candidate)) continue;
            net->peer_address = from.sin_addr.s_addr;
            net->peer_port = from.sin_port;
            room->pinged = false;
            m->relay = false; /* heard directly */
        }
        if (m->punching) { /* relayed packets end punching first, so this one came directly */
            m->punching = false;
            char text[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &from.sin_addr, text, sizeof text) == NULL) text[0] = '\0';
            fprintf(stderr, "recomp netplay: reached the other player directly at %s:%u\n", text, ntohs(from.sin_port));
        }
        const uint8_t *at = packet + 6u + RECOMP_NETPLAY_DIGEST_BYTES;
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
        at += 2u + RECOMP_NETPLAY_NAME_BYTES;
        room->echo = get32(at);
        room->echo_at_ms = now;
        const uint32_t echo = get32(at + 4u), age = get16(at + 8u);
        if (echo != 0u) {
            /* Signed: the echoed clock is forced odd, so it can be 1 ms ahead. */
            const int64_t ping = (int64_t)(int32_t)((uint32_t)now - echo) - (int64_t)age;
            room->ping_ms = ping > 0 ? (uint32_t)ping : 0u;
            room->pinged = true;
        }
        at += 10u;
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
    if (room->joined && now - room->heard_ms > ROOM_SILENCE_MS) recomp_netplay_room_drop(net, room);
    /* punch_ms can be newer than now when this tick read the match itself. */
    if (net->match.punching && now > net->match.punch_ms + PUNCH_MS) relay_on(net, "no direct path after 5 s");
    if (net->peer_port == 0u || (!start && now - room->sent_ms < 100u)) return;
    room->sent_ms = now;
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
    at += 2u + RECOMP_NETPLAY_NAME_BYTES;
    put32(at, (uint32_t)now | 1u); /* never 0, which means no clock heard */
    put32(at + 4u, room->echo);
    const uint64_t age = now - room->echo_at_ms;
    put16(at + 8u, (uint16_t)(room->echo == 0u ? 0u : age > 65535u ? 65535u : age));
    at += 10u;
    if (now - room->said_ms < 2000u) {
        put32(at, room->say_id);
        at[4] = (uint8_t)strlen(room->say);
        memcpy(at + 5u, room->say, at[4]);
    }
    put32(packet + ROOM_BYTES - 8u, (uint32_t)room->id);
    put32(packet + ROOM_BYTES - 4u, (uint32_t)(room->id >> 32));
    const RecompNetplayMatch *m = &net->match;
    if (!m->punching) {
        netplay_send_to(net, net->peer_address, net->peer_port, packet, ROOM_BYTES);
        return;
    }
    /* Sending first opens this side's router for the other's answer. */
    for (unsigned i = 0u; i < 2u; ++i)
        if (m->candidate_port[i] != 0u) send_raw(net, m->candidate_address[i], m->candidate_port[i], packet, ROOM_BYTES);
}

void recomp_netplay_room_say(RecompNetplayRoom *room, const char *text)
{
    const uint64_t now = now_ms();
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
    net->match.busy = false;
    if (room->hosting) { /* let another player in */
        net->peer_port = 0u;
        net->match.relay = false;
    }
}

bool recomp_netplay_match_server(RecompNetplay *net, const char *host, uint16_t port)
{
    if (port == 0u || !netplay_resolve(host, &net->match.server_address)) return false;
    net->match.server_port = htons(port);
    return true;
}

static void send_match(const RecompNetplay *net, const uint8_t *packet, size_t size)
{
    if (net->match.server_port != 0u)
        send_raw(net, net->match.server_address, net->match.server_port, packet, size);
}

/* Name length and name, zero padded. */
static uint8_t *put_name(uint8_t *out, const char *name)
{
    const size_t length = strlen(name);
    out[0] = (uint8_t)(length < RECOMP_NETPLAY_NAME_BYTES ? length : RECOMP_NETPLAY_NAME_BYTES);
    memset(out + 1u, 0, RECOMP_NETPLAY_NAME_BYTES);
    memcpy(out + 1u, name, out[0]);
    return out + 1u + RECOMP_NETPLAY_NAME_BYTES;
}

/* This computer's LAN address and the socket's port, for players behind the same router. */
static void put_lan(const RecompNetplay *net, uint8_t *out)
{
    memset(out, 0, 6u);
    char text[INET_ADDRSTRLEN];
    const uint16_t port = htons(recomp_netplay_local_port(net));
    if (!recomp_netplay_route_address(text, sizeof text) || port == 0u || inet_pton(AF_INET, text, out) != 1)
        return;
    memcpy(out + 4u, &port, 2u);
}

uint16_t recomp_netplay_local_port(const RecompNetplay *net)
{
    struct sockaddr_in local;
    SocketLength length = sizeof local;
    if (getsockname((SOCKET)net->socket, (struct sockaddr *)&local, &length) != 0) return 0u;
    return ntohs(local.sin_port);
}

void recomp_netplay_match_host(RecompNetplay *net, const char *digest, uint8_t delay, bool open,
    const char *name)
{
    uint8_t packet[MATCH_HEADER + RECOMP_NETPLAY_DIGEST_BYTES + 2u + 1u + RECOMP_NETPLAY_NAME_BYTES + 6u];
    match_header(packet, MATCH_HOST);
    memcpy(packet + MATCH_HEADER, digest, RECOMP_NETPLAY_DIGEST_BYTES);
    uint8_t *at = packet + MATCH_HEADER + RECOMP_NETPLAY_DIGEST_BYTES;
    at[0] = delay;
    at[1] = open ? 1u : 0u;
    put_lan(net, put_name(at + 2u, name));
    send_match(net, packet, sizeof packet);
}

void recomp_netplay_match_list(RecompNetplay *net, const char *digest)
{
    uint8_t packet[MATCH_LIST_BYTES];
    memset(packet, 0, sizeof packet);
    match_header(packet, MATCH_LIST);
    memcpy(packet + MATCH_HEADER, digest, RECOMP_NETPLAY_DIGEST_BYTES);
    packet[MATCH_HEADER + RECOMP_NETPLAY_DIGEST_BYTES] = MATCH_WITH_ENDPOINTS;
    send_match(net, packet, sizeof packet);
}

void recomp_netplay_match_probe(RecompNetplay *net)
{
    uint8_t packet[PROBE_BYTES];
    put32(packet, PROBE_MAGIC);
    put32(packet + 8, (uint32_t)now_ms());
    const RecompNetplayMatch *m = &net->match;
    for (uint32_t i = 0u; i < m->room_count; ++i) {
        if (m->rooms[i].port == 0u) continue;
        put32(packet + 4, m->rooms[i].id);
        send_raw(net, m->rooms[i].address, m->rooms[i].port, packet, sizeof packet);
    }
}

void recomp_netplay_match_join(RecompNetplay *net, uint32_t room_id, const char *digest, const char *name)
{
    uint8_t packet[MATCH_HEADER + 4u + RECOMP_NETPLAY_DIGEST_BYTES + 1u + RECOMP_NETPLAY_NAME_BYTES + 6u];
    match_header(packet, MATCH_JOIN);
    for (unsigned i = 0u; i < 4u; ++i) packet[MATCH_HEADER + i] = (uint8_t)(room_id >> (24u - 8u * i));
    memcpy(packet + MATCH_HEADER + 4u, digest, RECOMP_NETPLAY_DIGEST_BYTES);
    put_lan(net, put_name(packet + MATCH_HEADER + 4u + RECOMP_NETPLAY_DIGEST_BYTES, name));
    send_match(net, packet, sizeof packet);
}

void recomp_netplay_match_leave(RecompNetplay *net)
{
    uint8_t packet[MATCH_HEADER];
    match_header(packet, MATCH_LEAVE);
    send_match(net, packet, sizeof packet);
    net->match.room_id = 0u;
}

void recomp_netplay_match_poll(RecompNetplay *net)
{
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u];
    struct sockaddr_in from;
    int size;
    while ((size = netplay_receive_from(net, packet, sizeof packet, &from)) >= 0) {
        if (size != PROBE_BYTES || get32(packet) != ECHO_MAGIC) continue;
        const uint32_t id = get32(packet + 4), sample = (uint32_t)now_ms() - get32(packet + 8);
        if (sample > 10000u) continue; /* not a clock this runner sent */
        RecompNetplayMatch *m = &net->match;
        for (uint32_t i = 0u; i < m->room_count; ++i) {
            RecompNetplayListedRoom *room = &m->rooms[i];
            if (room->id != id || room->port != from.sin_port || room->address != from.sin_addr.s_addr) continue;
            room->ping_ms = room->pinged ? (room->ping_ms * 3u + sample) / 4u : sample;
            room->pinged = true;
        }
    }
}
