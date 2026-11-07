#ifndef DOAXBV_NETPLAY_INTERNAL_H
#define DOAXBV_NETPLAY_INTERNAL_H

/* Shared by netplay.c (session packets, sockets, lobby) and netplay_room.c
   (matchmaker, room, probes and the relay-aware send and receive). */

#include "netplay.h"

#include <time.h>

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
    PROBE_MAGIC = 0x49503244u, /* "D2PI": a listed room's id and the asker's clock */
    ECHO_MAGIC = 0x4f503244u,  /* "D2PO": the same probe sent back */
    PROBE_BYTES = 12u,
    LOBBY_MAGIC = 0x424c3244u, /* "D2LB" */
    LOBBY_VERSION = 5u,
    LOBBY_HELLO = 1u,
    LOBBY_WELCOME = 2u,
    LOBBY_REQUEST = 3u, /* send your chunks from this index; past your last, I hold them all */
    LOBBY_CHUNK = 4u,   /* one chunk of the sender's profile */
    LOBBY_ROOM = 5u,    /* a room member's state */
    LOBBY_BYTES = 6u + RECOMP_NETPLAY_DIGEST_BYTES + 6u,
    ROOM_BYTES = 6u + RECOMP_NETPLAY_DIGEST_BYTES + 2u + RECOMP_NETPLAY_NAME_BYTES + 10u + 5u +
        RECOMP_NETPLAY_CHAT_BYTES + 8u,
    ROOM_READY = 1u,
    ROOM_START = 2u,
    ROOM_SILENCE_MS = 3000u,
    PART_HEADER = 10u,
    CHUNK_BYTES = 1024u,
    CHUNKS_PER_REQUEST = 16u,
    MAX_CHUNKS = RECOMP_NETPLAY_MAX_PROFILE / CHUNK_BYTES,
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

static inline bool from_peer(const RecompNetplay *net, const struct sockaddr_in *from)
{
    return from->sin_addr.s_addr == net->peer_address && from->sin_port == net->peer_port;
}

static inline uint64_t now_ms(void)
{
    /* Monotonic, so a wall-clock correction cannot end a wait or skew a round trip. */
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (uint64_t)(count.QuadPart / frequency.QuadPart * 1000 +
        count.QuadPart % frequency.QuadPart * 1000 / frequency.QuadPart);
#else
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

bool netplay_resolve(const char *host, uint32_t *address);
void netplay_send_to(const RecompNetplay *net, uint32_t address, uint16_t port,
    const uint8_t *packet, size_t size);
int netplay_receive_from(RecompNetplay *net, uint8_t *packet, size_t capacity,
    struct sockaddr_in *from);

#endif
