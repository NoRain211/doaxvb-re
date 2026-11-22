#ifndef DOAXBV_RECOMP_NETPLAY_WIRE_H
#define DOAXBV_RECOMP_NETPLAY_WIRE_H

/* What netplay.c and netplay_room.c share: the socket's platform types, the
   packets' constants and byte order, and the transport under both. Callers
   use netplay.h. */
#include "netplay.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int SocketLength;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET;
typedef socklen_t SocketLength;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

enum {
    MAGIC = 0x42523244u,       /* "D2RB" */
    GOODBYE_MAGIC = 0x59423244u, /* "D2BY", the whole packet */
    ROOM_CLOSED_MAGIC = 0x43523244u, /* "D2RC", the whole packet: the sender left the room */
    LOBBY_MAGIC = 0x424c3244u, /* "D2LB" */
    LOBBY_VERSION = 5u,
    LOBBY_HELLO = 1u,
    LOBBY_WELCOME = 2u,
    LOBBY_REQUEST = 3u, /* send your chunks from this index; past your last, I hold them all */
    LOBBY_CHUNK = 4u,   /* one chunk of the sender's profile */
    LOBBY_ROOM = 5u,    /* a room member's state */
    LOBBY_BYTES = 6u + RECOMP_NETPLAY_DIGEST_BYTES + 6u,
    ROOM_CLOCKS = 6u + RECOMP_NETPLAY_DIGEST_BYTES + 2u + RECOMP_NETPLAY_NAME_BYTES, /* where a room packet's clocks are */
    ROOM_BYTES = ROOM_CLOCKS + 10u + 5u + RECOMP_NETPLAY_CHAT_BYTES + 8u,
    ROOM_READY = 1u,
    ROOM_START = 2u,
    ROOM_SILENCE_MS = 3000u,
    ROOM_RETURN_MS = 10000u, /* a host back from a session keeps its seat this long for the last joiner */
    PART_HEADER = 10u,
    CHUNK_BYTES = 1024u,
    CHUNKS_PER_REQUEST = 16u,
    MAX_CHUNKS = RECOMP_NETPLAY_MAX_PROFILE / CHUNK_BYTES,
    PUNCH_MS = 5000u, /* how long a room tries the endpoints the matchmaker gave before it relays */
};

static inline void put32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0u; i < 4u; ++i) out[i] = (uint8_t)(value >> (8u * i));
}

static inline uint32_t get32(const uint8_t *in)
{
    return (uint32_t)in[0] | (uint32_t)in[1] << 8 | (uint32_t)in[2] << 16 | (uint32_t)in[3] << 24;
}

static inline void put16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static inline uint16_t get16(const uint8_t *in)
{
    return (uint16_t)(in[0] | in[1] << 8);
}

/* Monotonic milliseconds. */
uint64_t netplay_now_ms(void);
void netplay_send_raw(const RecompNetplay *net, uint32_t address, uint16_t port,
    const uint8_t *packet, size_t size);
/* As netplay_send_raw, but through the matchmaker when the socket relays to this peer. */
void netplay_send_to(const RecompNetplay *net, uint32_t address, uint16_t port,
    const uint8_t *packet, size_t size);
/* The next queued packet's size, or -1 when none is left; a relayed packet comes out unwrapped, from the peer. */
int netplay_receive_from(RecompNetplay *net, uint8_t *packet, size_t capacity,
    struct sockaddr_in *from);
/* A punching room's packet to the peer's IPv6 endpoint too, when the matchmaker gave one. */
void netplay_punch6(const RecompNetplay *net, const uint8_t *packet, size_t size);
bool netplay_from_peer(const RecompNetplay *net, const struct sockaddr_in *from);
void netplay_relay_on(RecompNetplay *net, const char *why);

#endif
