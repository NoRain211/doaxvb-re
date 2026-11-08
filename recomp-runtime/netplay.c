#include "netplay_internal.h"

#include <string.h>

static void put_pad(uint8_t *out, const RecompInputGamepad *pad)
{
    put16(out, pad->buttons);
    memcpy(out + 2, pad->analog_buttons, RECOMP_INPUT_ANALOG_BUTTON_COUNT);
    put16(out + 10, (uint16_t)pad->thumb_lx);
    put16(out + 12, (uint16_t)pad->thumb_ly);
    put16(out + 14, (uint16_t)pad->thumb_rx);
    put16(out + 16, (uint16_t)pad->thumb_ry);
}

static void get_pad(const uint8_t *in, RecompInputGamepad *pad)
{
    memset(pad, 0, sizeof *pad);
    pad->buttons = get16(in);
    memcpy(pad->analog_buttons, in + 2, RECOMP_INPUT_ANALOG_BUTTON_COUNT);
    pad->thumb_lx = (int16_t)get16(in + 10);
    pad->thumb_ly = (int16_t)get16(in + 12);
    pad->thumb_rx = (int16_t)get16(in + 14);
    pad->thumb_ry = (int16_t)get16(in + 16);
}

/* Layout: magic, sender's next frame, pads it holds from us, its advantage,
   first pad's frame, pad count, its newest desync check's frame and hash,
   pads. */
size_t recomp_netplay_encode(const RecompNetplay *net, const RecompRollbackSession *s,
    int32_t advantage, uint8_t *packet)
{
    uint32_t first = net->peer_ack;
    if (s->local_count > RECOMP_ROLLBACK_HISTORY && first < s->local_count - RECOMP_ROLLBACK_HISTORY)
        first = s->local_count - RECOMP_ROLLBACK_HISTORY;
    if (first > s->local_count) first = s->local_count;
    uint32_t count = s->local_count - first;
    if (count > RECOMP_NETPLAY_MAX_PADS) count = RECOMP_NETPLAY_MAX_PADS;
    put32(packet, MAGIC);
    put32(packet + 4, s->frame);
    put32(packet + 8, s->remote_count);
    put32(packet + 12, (uint32_t)advantage);
    put32(packet + 16, first);
    packet[20] = (uint8_t)count;
    const uint32_t newest = (net->check_count + RECOMP_NETPLAY_CHECKS - 1u) % RECOMP_NETPLAY_CHECKS;
    const bool checked = net->check_count != 0u;
    put32(packet + 21, checked ? net->check_frames[newest] : 0u);
    put32(packet + 25, checked ? (uint32_t)net->check_hashes[newest] : 0u);
    put32(packet + 29, checked ? (uint32_t)(net->check_hashes[newest] >> 32) : 0u);
    for (uint32_t i = 0u; i < count; ++i)
        put_pad(packet + RECOMP_NETPLAY_HEADER_BYTES + i * RECOMP_NETPLAY_PAD_BYTES,
            &s->local[(first + i) % RECOMP_ROLLBACK_HISTORY]);
    return RECOMP_NETPLAY_HEADER_BYTES + count * RECOMP_NETPLAY_PAD_BYTES;
}

bool recomp_netplay_decode(RecompNetplay *net, RecompRollbackSession *s,
    const uint8_t *packet, size_t size)
{
    if (size == 4u && get32(packet) == GOODBYE_MAGIC) {
        net->peer_left = true;
        return true;
    }
    if (size < RECOMP_NETPLAY_HEADER_BYTES || get32(packet) != MAGIC) return false;
    const uint32_t count = packet[20];
    if (count > RECOMP_NETPLAY_MAX_PADS ||
        size != RECOMP_NETPLAY_HEADER_BYTES + count * RECOMP_NETPLAY_PAD_BYTES)
        return false;
    const uint32_t ack = get32(packet + 8);
    if (ack > s->local_count) return false;
    const uint32_t first = get32(packet + 16);
    const uint32_t frame = get32(packet + 4);
    const int32_t advantage = (int32_t)get32(packet + 12);
    if (!net->peer_seen || frame > net->peer_frame)
        net->peer_samples[net->peer_count++ % RECOMP_NETPLAY_SYNC_FRAMES] = advantage;
    /* Packets can arrive out of order; only newer state moves forward. */
    if (!net->peer_seen || frame >= net->peer_frame) {
        net->peer_frame = frame;
        net->peer_advantage = advantage;
    }
    if (ack > net->peer_ack) {
        /* The newest pad the peer holds was sent once, when it was stamped. */
        if (ack <= net->pads_stamped && ack + RECOMP_NETPLAY_MAX_PADS > net->pads_stamped) {
            const uint64_t sample = now_ms() - net->pad_sent_ms[(ack - 1u) % RECOMP_NETPLAY_MAX_PADS];
            net->rtt_ms = net->rtt_known ? (uint32_t)((net->rtt_ms * 7u + sample) / 8u) : (uint32_t)sample;
            net->rtt_known = true;
        }
        net->peer_ack = ack;
    }
    net->peer_seen = true;
    const uint32_t check_frame = get32(packet + 21);
    if (check_frame > net->peer_check_frame) {
        net->peer_check_frame = check_frame;
        net->peer_check_hash = get32(packet + 25) | (uint64_t)get32(packet + 29) << 32;
    }
    for (uint32_t i = 0u; i < count; ++i) {
        const uint32_t frame = first + i;
        if (frame < s->remote_count) continue;
        if (frame > s->remote_count) break;
        RecompInputGamepad pad;
        get_pad(packet + RECOMP_NETPLAY_HEADER_BYTES + i * RECOMP_NETPLAY_PAD_BYTES, &pad);
        if (!recomp_rollback_add_remote(s, &pad)) break;
    }
    return true;
}

/* Layout: magic, version, kind, digest, delay, flags, the sender's profile size. */
static size_t encode_lobby(uint8_t kind, const RecompNetplayLobby *lobby, uint32_t profile_size,
    uint8_t *out)
{
    put32(out, LOBBY_MAGIC);
    out[4] = LOBBY_VERSION;
    out[5] = kind;
    memcpy(out + 6, lobby->digest, RECOMP_NETPLAY_DIGEST_BYTES);
    out[6u + RECOMP_NETPLAY_DIGEST_BYTES] = lobby->delay;
    out[7u + RECOMP_NETPLAY_DIGEST_BYTES] = lobby->flags;
    put32(out + 8u + RECOMP_NETPLAY_DIGEST_BYTES, profile_size);
    return LOBBY_BYTES;
}

/* The packet's kind, or 0 when it is not a lobby packet of this version. */
static uint8_t decode_lobby(const uint8_t *in, size_t size, RecompNetplayLobby *lobby,
    uint32_t *profile_size)
{
    if (size != LOBBY_BYTES || get32(in) != LOBBY_MAGIC || in[4] != LOBBY_VERSION) return 0u;
    memcpy(lobby->digest, in + 6, RECOMP_NETPLAY_DIGEST_BYTES);
    lobby->delay = in[6u + RECOMP_NETPLAY_DIGEST_BYTES];
    lobby->flags = in[7u + RECOMP_NETPLAY_DIGEST_BYTES];
    *profile_size = get32(in + 8u + RECOMP_NETPLAY_DIGEST_BYTES);
    return in[5];
}

/* A request or chunk: magic, version, kind, chunk index, then a chunk's bytes. */
static size_t encode_part(uint8_t kind, uint32_t index, uint8_t *out)
{
    put32(out, LOBBY_MAGIC);
    out[4] = LOBBY_VERSION;
    out[5] = kind;
    put32(out + 6, index);
    return PART_HEADER;
}

static bool decode_part(const uint8_t *in, size_t size, uint8_t kind, uint32_t *index)
{
    if (size < PART_HEADER || get32(in) != LOBBY_MAGIC || in[4] != LOBBY_VERSION || in[5] != kind)
        return false;
    *index = get32(in + 6);
    return true;
}

static uint32_t chunk_count(uint32_t size) { return (size + CHUNK_BYTES - 1u) / CHUNK_BYTES; }

static uint32_t chunk_size(uint32_t size, uint32_t index)
{
    const uint32_t left = size - index * CHUNK_BYTES;
    return left < CHUNK_BYTES ? left : CHUNK_BYTES;
}

static bool open_socket(RecompNetplay *net, uint16_t local_port)
{
    memset(net, 0, sizeof *net);
    net->socket = (uintptr_t)INVALID_SOCKET;
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
#endif
    const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET) return false;
    struct sockaddr_in local;
    memset(&local, 0, sizeof local);
    local.sin_family = AF_INET;
    local.sin_port = htons(local_port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
#ifdef _WIN32
    u_long nonblocking = 1u;
    const bool configured = ioctlsocket(handle, FIONBIO, &nonblocking) == 0;
#else
    const bool configured = fcntl(handle, F_SETFL, fcntl(handle, F_GETFL) | O_NONBLOCK) == 0;
#endif
    if (!configured || bind(handle, (const struct sockaddr *)&local, sizeof local) != 0) {
        closesocket(handle);
        return false;
    }
    net->socket = (uintptr_t)handle;
    return true;
}

bool netplay_resolve(const char *host, uint32_t *address)
{
    struct addrinfo hints, *found = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &found) != 0 || found == NULL) return false;
    *address = ((const struct sockaddr_in *)(const void *)found->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(found);
    return true;
}

bool recomp_netplay_open(RecompNetplay *net, uint16_t local_port,
    const char *peer_host, uint16_t peer_port)
{
    if (!open_socket(net, local_port)) return false;
    if (!netplay_resolve(peer_host, &net->peer_address)) {
        recomp_netplay_close(net);
        return false;
    }
    net->peer_port = htons(peer_port);
    return true;
}

void recomp_netplay_close(RecompNetplay *net)
{
    if ((SOCKET)net->socket != INVALID_SOCKET) closesocket((SOCKET)net->socket);
    net->socket = (uintptr_t)INVALID_SOCKET;
}

bool recomp_netplay_listen(RecompNetplay *net, uint16_t port)
{
    return open_socket(net, port);
}

bool recomp_netplay_route_address(char *text, size_t size)
{
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
#endif
    const SOCKET handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle == INVALID_SOCKET) return false;
    /* Connecting a UDP socket sends nothing; it only picks the route. */
    struct sockaddr_in remote, local;
    memset(&remote, 0, sizeof remote);
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    remote.sin_addr.s_addr = htonl(0x08080808u);
    SocketLength length = sizeof local;
    const bool found = connect(handle, (const struct sockaddr *)&remote, sizeof remote) == 0 &&
        getsockname(handle, (struct sockaddr *)&local, &length) == 0 &&
        inet_ntop(AF_INET, &local.sin_addr, text, (SocketLength)size) != NULL;
    closesocket(handle);
    return found;
}
void recomp_netplay_send(RecompNetplay *net, const RecompRollbackSession *s, int32_t advantage)
{
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET];
    const uint64_t now = now_ms();
    for (; net->pads_stamped < s->local_count; ++net->pads_stamped)
        net->pad_sent_ms[net->pads_stamped % RECOMP_NETPLAY_MAX_PADS] = now;
    netplay_send_to(net, net->peer_address, net->peer_port, packet,
        recomp_netplay_encode(net, s, advantage, packet));
}

void recomp_netplay_send_goodbye(const RecompNetplay *net)
{
    uint8_t packet[4];
    put32(packet, GOODBYE_MAGIC);
    for (int i = 0; i < 3; ++i) netplay_send_to(net, net->peer_address, net->peer_port, packet, sizeof packet);
}

uint32_t recomp_netplay_suggested_delay(uint32_t rtt_ms)
{
    if (rtt_ms <= 20u) return 0u;
    const uint32_t frames = rtt_ms / 33u + (rtt_ms % 33u != 0u);
    return frames < RECOMP_ROLLBACK_WINDOW ? frames : RECOMP_ROLLBACK_WINDOW;
}

static void wait_readable(const RecompNetplay *net, unsigned milliseconds)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET((SOCKET)net->socket, &readable);
    struct timeval timeout = {0, (long)milliseconds * 1000L};
    (void)select((int)net->socket + 1, &readable, NULL, NULL, &timeout);
}

/* One direction each way: this side serves its profile in chunks on request
   and fetches the peer's the same way. A request past the last chunk says
   the asker holds them all; a session packet says the same, if every one of
   those was lost. */
typedef struct Transfer {
    RecompNetplayProfiles *profiles;
    uint32_t chunks, missing, asked; /* of the peer's profile */
    uint64_t next_ask, next_serve;
    uint64_t served; /* bytes of this side's profile sent */
    uint32_t served_end; /* past the last chunk sent */
    bool known, peer_done;
    bool have[MAX_CHUNKS];
} Transfer;

static void transfer_start(Transfer *t, uint32_t size)
{
    t->profiles->theirs_size = size;
    t->chunks = chunk_count(size);
    t->known = true;
    t->next_ask = 0u;
}

static void transfer_packet(RecompNetplay *net, Transfer *t, const uint8_t *packet, size_t size)
{
    const RecompNetplayProfiles *p = t->profiles;
    uint32_t index;
    if (decode_part(packet, size, LOBBY_REQUEST, &index)) {
        const uint32_t mine = chunk_count(p->mine_size);
        if (index >= mine) {
            t->peer_done = true;
            return;
        }
        /* A 10-byte request draws up to 16 KB. The next window goes out at
           once, as an honest peer moves on; an earlier one, a retransmit or
           a replay, waits 50 ms, and eight copies end it, far more than a
           lossy link needs. */
        const uint64_t now = now_ms();
        if (t->served >= 8ull * p->mine_size || (index < t->served_end && now < t->next_serve)) return;
        t->next_serve = now + 50u;
        const uint32_t end = index + CHUNKS_PER_REQUEST < mine ? index + CHUNKS_PER_REQUEST : mine;
        if (end > t->served_end) t->served_end = end; /* only forward, so a replayed window stays a replay */
        uint8_t chunk[PART_HEADER + CHUNK_BYTES];
        for (uint32_t i = index; i < mine && i < index + CHUNKS_PER_REQUEST; ++i) {
            const uint32_t bytes = chunk_size(p->mine_size, i);
            memcpy(chunk + encode_part(LOBBY_CHUNK, i, chunk), p->mine + i * CHUNK_BYTES, bytes);
            netplay_send_to(net, net->peer_address, net->peer_port, chunk, PART_HEADER + bytes);
            t->served += bytes;
        }
    } else if (t->known && decode_part(packet, size, LOBBY_CHUNK, &index) && index < t->chunks &&
        size == PART_HEADER + chunk_size(p->theirs_size, index)) {
        memcpy(p->theirs + index * CHUNK_BYTES, packet + PART_HEADER, size - PART_HEADER);
        t->have[index] = true;
    } else if (size >= 4u && get32(packet) == MAGIC) {
        t->peer_done = true;
    }
}

/* Asks for the first missing chunk once a burst has arrived or after a
   loss, and repeats done until the peer has finished too. True once both
   sides hold both profiles, after saying done once more in case the peer
   finished before hearing it. */
static bool transfer_tick(RecompNetplay *net, Transfer *t, uint64_t now)
{
    if (!t->known) return false;
    while (t->missing < t->chunks && t->have[t->missing]) ++t->missing;
    const bool done = t->missing == t->chunks && t->peer_done;
    if (done || now >= t->next_ask || (t->missing < t->chunks && t->missing >= t->asked + CHUNKS_PER_REQUEST)) {
        uint8_t request[PART_HEADER];
        netplay_send_to(net, net->peer_address, net->peer_port, request,
            encode_part(LOBBY_REQUEST, t->missing, request));
        t->asked = t->missing;
        t->next_ask = now + 100u;
    }
    return done;
}

static bool is_goodbye(const uint8_t *packet, int size)
{
    return size == 4 && get32(packet) == GOODBYE_MAGIC;
}

RecompNetplayLobbyResult recomp_netplay_host(RecompNetplay *net, const RecompNetplayLobby *lobby,
    RecompNetplayProfiles *profiles, uint32_t timeout_ms, bool (*cancelled)(void))
{
    const uint64_t deadline = now_ms() + timeout_ms;
    Transfer t;
    memset(&t, 0, sizeof t);
    t.profiles = profiles;
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u], welcome[LOBBY_BYTES];
    const size_t welcome_size = encode_lobby(LOBBY_WELCOME, lobby, profiles->mine_size, welcome);
    for (;;) {
        struct sockaddr_in from;
        int size;
        while ((size = netplay_receive_from(net, packet, sizeof packet, &from)) >= 0) {
            if (t.known && !from_peer(net, &from)) continue;
            /* A room's host already knows its joiner, whose goodbye can beat a lost hello. */
            if (is_goodbye(packet, size) && (t.known || (net->peer_port != 0u && from_peer(net, &from)))) {
                net->peer_left = true;
                return RECOMP_NETPLAY_LOBBY_CANCELLED;
            }
            RecompNetplayLobby hello;
            uint32_t theirs;
            if (decode_lobby(packet, (size_t)size, &hello, &theirs) == LOBBY_HELLO) {
                if (t.known) {
                    netplay_send_to(net, from.sin_addr.s_addr, from.sin_port, welcome, welcome_size);
                    continue;
                }
                if (theirs > RECOMP_NETPLAY_MAX_PROFILE) continue;
                netplay_send_to(net, from.sin_addr.s_addr, from.sin_port, welcome, welcome_size);
                if (memcmp(hello.digest, lobby->digest, RECOMP_NETPLAY_DIGEST_BYTES) != 0)
                    return RECOMP_NETPLAY_LOBBY_MISMATCH;
                net->peer_address = from.sin_addr.s_addr;
                net->peer_port = from.sin_port;
                transfer_start(&t, theirs);
            } else if (t.known) {
                transfer_packet(net, &t, packet, (size_t)size);
            }
        }
        const uint64_t now = now_ms();
        if (transfer_tick(net, &t, now)) return RECOMP_NETPLAY_LOBBY_OK;
        if (now >= deadline) return RECOMP_NETPLAY_LOBBY_TIMEOUT;
        if (cancelled != NULL && cancelled()) {
            if (t.known) recomp_netplay_send_goodbye(net);
            return RECOMP_NETPLAY_LOBBY_CANCELLED;
        }
        wait_readable(net, 20u);
    }
}

RecompNetplayLobbyResult recomp_netplay_join(RecompNetplay *net, RecompNetplayLobby *lobby,
    RecompNetplayProfiles *profiles, uint32_t timeout_ms, bool (*cancelled)(void))
{
    const uint64_t deadline = now_ms() + timeout_ms;
    Transfer t;
    memset(&t, 0, sizeof t);
    t.profiles = profiles;
    uint64_t next_hello = 0u;
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u], hello[LOBBY_BYTES];
    const size_t hello_size = encode_lobby(LOBBY_HELLO, lobby, profiles->mine_size, hello);
    for (;;) {
        struct sockaddr_in from;
        int size;
        while ((size = netplay_receive_from(net, packet, sizeof packet, &from)) >= 0) {
            if (!from_peer(net, &from)) continue;
            if (is_goodbye(packet, size)) {
                net->peer_left = true;
                return RECOMP_NETPLAY_LOBBY_CANCELLED;
            }
            RecompNetplayLobby welcome;
            uint32_t theirs;
            if (!t.known && decode_lobby(packet, (size_t)size, &welcome, &theirs) == LOBBY_WELCOME) {
                if (memcmp(welcome.digest, lobby->digest, RECOMP_NETPLAY_DIGEST_BYTES) != 0 ||
                    welcome.delay > RECOMP_ROLLBACK_WINDOW)
                    return RECOMP_NETPLAY_LOBBY_MISMATCH;
                if (theirs > RECOMP_NETPLAY_MAX_PROFILE) continue;
                lobby->delay = welcome.delay;
                lobby->flags = welcome.flags;
                transfer_start(&t, theirs);
            } else if (t.known) {
                transfer_packet(net, &t, packet, (size_t)size);
            }
        }
        const uint64_t now = now_ms();
        if (!t.known && now >= next_hello) {
            netplay_send_to(net, net->peer_address, net->peer_port, hello, hello_size);
            next_hello = now + 100u;
        }
        if (transfer_tick(net, &t, now)) return RECOMP_NETPLAY_LOBBY_OK;
        if (now >= deadline) return RECOMP_NETPLAY_LOBBY_TIMEOUT;
        if (cancelled != NULL && cancelled()) {
            recomp_netplay_send_goodbye(net);
            return RECOMP_NETPLAY_LOBBY_CANCELLED;
        }
        wait_readable(net, 20u);
    }
}

void recomp_netplay_receive(RecompNetplay *net, RecompRollbackSession *s)
{
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET + 1u];
    struct sockaddr_in from;
    int size;
    while ((size = netplay_receive_from(net, packet, sizeof packet, &from)) >= 0)
        if (from_peer(net, &from)) (void)recomp_netplay_decode(net, s, packet, (size_t)size);
}

uint32_t recomp_netplay_wait_frames(RecompNetplay *net, int32_t advantage)
{
    enum { N = RECOMP_NETPLAY_SYNC_FRAMES };
    net->local_samples[net->local_count++ % N] = advantage;
    if (net->cooldown != 0u) {
        --net->cooldown;
        return 0u;
    }
    if (net->local_count < N || net->peer_count < N) return 0u;
    int32_t difference = 0;
    for (uint32_t i = 0u; i < N; ++i) difference += net->local_samples[i] - net->peer_samples[i];
    if (difference <= 0) return 0u;
    /* Half the averaged difference, rounded to the nearest frame. */
    const int32_t wait = (difference + (int32_t)N) / (int32_t)(2u * N);
    if (wait <= 0) return 0u;
    /* The peer needs a round trip to report the new advantage. */
    net->cooldown = RECOMP_NETPLAY_SYNC_FRAMES;
    return wait > (int32_t)RECOMP_ROLLBACK_WINDOW ? RECOMP_ROLLBACK_WINDOW : (uint32_t)wait;
}

void recomp_netplay_add_check(RecompNetplay *net, uint32_t frame, uint64_t hash)
{
    net->check_frames[net->check_count % RECOMP_NETPLAY_CHECKS] = frame;
    net->check_hashes[net->check_count % RECOMP_NETPLAY_CHECKS] = hash;
    ++net->check_count;
}

bool recomp_netplay_own_check(const RecompNetplay *net, uint64_t *mine)
{
    for (uint32_t i = 0u; net->peer_check_frame != 0u && i < RECOMP_NETPLAY_CHECKS; ++i) {
        if (net->check_frames[i] != net->peer_check_frame) continue;
        *mine = net->check_hashes[i];
        return true;
    }
    return false;
}

