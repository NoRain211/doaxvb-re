#ifndef DOAXBV_RECOMP_NETPLAY_H
#define DOAXBV_RECOMP_NETPLAY_H

#include "rollback.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UDP transport for a rollback session. Each packet carries every local pad
   the peer has not acknowledged, so a lost packet is covered by the next one
   and there is no resend. */
enum {
    RECOMP_NETPLAY_PAD_BYTES = 18u,
    RECOMP_NETPLAY_HEADER_BYTES = 33u,
    RECOMP_NETPLAY_MAX_PADS = RECOMP_ROLLBACK_HISTORY,
    /* At most the 1,200 bytes the matchmaker relays (doa2u-re's tools/matchmaker.py). */
    RECOMP_NETPLAY_MAX_PACKET =
        RECOMP_NETPLAY_HEADER_BYTES + RECOMP_NETPLAY_MAX_PADS * RECOMP_NETPLAY_PAD_BYTES,
    RECOMP_NETPLAY_SYNC_FRAMES = 32u,
    /* Desync checks a side keeps to compare with the other's newest. */
    RECOMP_NETPLAY_CHECKS = 8u,
    RECOMP_NETPLAY_DIGEST_BYTES = 64u,
    /* The largest profile a host sends; it arrives in 1 KiB chunks. */
    RECOMP_NETPLAY_MAX_PROFILE = 1024u * 1024u,
    /* The longest profile name a room passes on, in UTF-8 bytes. */
    RECOMP_NETPLAY_NAME_BYTES = 32u,
    /* The longest room chat line, in printable ASCII. */
    RECOMP_NETPLAY_CHAT_BYTES = 40u,
};

/* What the lobby agrees on before either runner boots. */
typedef struct RecompNetplayLobby {
    char digest[RECOMP_NETPLAY_DIGEST_BYTES]; /* runner build; both sides must match */
    uint8_t delay;                            /* input delay in frames */
    uint8_t flags;                            /* host options the joiner adopts */
} RecompNetplayLobby;

/* The two players' profiles, which the lobby swaps. */
typedef struct RecompNetplayProfiles {
    const uint8_t *mine;
    uint32_t mine_size;   /* at most RECOMP_NETPLAY_MAX_PROFILE */
    uint8_t *theirs;      /* holds RECOMP_NETPLAY_MAX_PROFILE bytes */
    uint32_t theirs_size; /* set by a successful lobby */
} RecompNetplayProfiles;

typedef enum RecompNetplayLobbyResult {
    RECOMP_NETPLAY_LOBBY_OK,
    RECOMP_NETPLAY_LOBBY_TIMEOUT,
    RECOMP_NETPLAY_LOBBY_MISMATCH, /* the other runner is a different build */
    RECOMP_NETPLAY_LOBBY_SOCKET,
    RECOMP_NETPLAY_LOBBY_CANCELLED, /* by this side, or by the other when peer_left is set */
} RecompNetplayLobbyResult;

enum {
    RECOMP_NETPLAY_LISTED_ROOMS = 12u,
    RECOMP_NETPLAY_TOKEN_BYTES = 8u, /* a socket's nonce and the matchmaker's cookie */
};

/* One open room from the matchmaker's list. */
typedef struct RecompNetplayListedRoom {
    uint32_t id;
    uint32_t age_s;
    uint8_t delay;
    char name[RECOMP_NETPLAY_NAME_BYTES + 1u];
    /* An estimate: this socket's LIST to ROOMS round trip plus the host's
       HOST to HOSTED, which the list carries. Not pinged when the host's is
       unknown; the list never says where a host is. */
    uint32_t ping_ms;
    bool pinged;
} RecompNetplayListedRoom;

/* What a socket heard from the matchmaker (doa2u-re's tools/matchmaker.py), which the
   socket's receives record on their way, and how it reaches its peer. */
typedef struct RecompNetplayMatch {
    uint32_t server_address; /* network order; port 0 means none */
    uint16_t server_port;
    uint32_t room_id; /* this host's listing, once the server answered */
    uint64_t host_sent_ms, list_sent_ms; /* when the last HOST or LIST went out; 0 once answered */
    uint16_t host_rtt_ms; /* HOST to HOSTED, said in the next HOST; 0 unknown */
    RecompNetplayListedRoom rooms[RECOMP_NETPLAY_LISTED_ROOMS];
    uint32_t room_count;
    bool listed;  /* a list arrived; the caller clears it */
    bool gone;    /* the room asked for is no longer open; the caller clears it */
    bool met;     /* the server paired this socket with a player; the caller clears it */
    /* The paired player's public and LAN endpoints, network order, port 0
       when unknown. Room ticks send to both until one answers. */
    uint32_t candidate_address[2];
    uint16_t candidate_port[2];
    bool punching;
    uint64_t punch_ms;
    bool relay; /* packets to the peer go through the server, as do its answers */
    /* Random for each socket; the server's packets echo it, so forged ones are dropped. A room's
       socket hands it to the session's runner, since the server relays a pair with it. */
    uint8_t nonce[RECOMP_NETPLAY_TOKEN_BYTES];
    /* The server's proof of this socket's address, zeros until it sends one. */
    uint8_t cookie[RECOMP_NETPLAY_TOKEN_BYTES];
    /* The last HOST, JOIN or LEAVE, sent once more when a new cookie answers it. */
    uint8_t asked[160];
    size_t asked_size;
    uint64_t asked_ms;
} RecompNetplayMatch;

typedef struct RecompNetplay {
    uintptr_t socket;
    uint32_t peer_address; /* IPv4, network order */
    uint16_t peer_port;    /* network order */
    bool peer_seen;
    uint32_t peer_ack;     /* local pads the peer holds */
    uint32_t peer_frame;   /* the peer's next frame when it last sent */
    int32_t peer_advantage;
    /* One advantage per frame on each side. Sampling the peer once per local
       frame would count its stalls, when it sends nothing new, too often. */
    int32_t local_samples[RECOMP_NETPLAY_SYNC_FRAMES];
    int32_t peer_samples[RECOMP_NETPLAY_SYNC_FRAMES];
    uint32_t local_count;
    uint32_t peer_count;
    uint32_t cooldown;
    /* Round trip: from a local pad's first send until the peer says it holds it. */
    uint64_t pad_sent_ms[RECOMP_NETPLAY_MAX_PADS];
    uint32_t pads_stamped;
    uint32_t rtt_ms; /* smoothed over the last few samples */
    bool rtt_known;
    bool peer_left; /* the other runner said goodbye */
    /* Desync checks: hashes of guest RAM at frames both sides' pads are
       known for. Each packet carries this side's newest; frame 0 is none. */
    uint32_t check_frames[RECOMP_NETPLAY_CHECKS];
    uint64_t check_hashes[RECOMP_NETPLAY_CHECKS];
    uint32_t check_count;
    uint32_t peer_check_frame;
    uint64_t peer_check_hash;
    RecompNetplayMatch match;
} RecompNetplay;

/* Binds local_port on all interfaces; packets from any other address are
   dropped. shortcut: IPv4 only; add AF_INET6 when the lobby needs it. */
bool recomp_netplay_open(RecompNetplay *net, uint16_t local_port,
    const char *peer_host, uint16_t peer_port);
/* Binds port on all interfaces with no peer yet, for a room's host. */
bool recomp_netplay_listen(RecompNetplay *net, uint16_t port);
/* The port the socket is bound to, host order; 0 when unknown. */
uint16_t recomp_netplay_local_port(const RecompNetplay *net);

/* The room two players wait in, from the Versus menu, before the runners
   restart into a session. Each side says its build, name and readiness ten
   times a second; the host also says when to start. */
typedef struct RecompNetplayRoom {
    bool hosting;  /* set before the first tick; a host takes the first player who talks */
    uint64_t id;   /* this install's player ID, set before the first tick */
    /* Set before the first tick, or null: true for a player ID this player
       blocked. Their messages are dropped and set refused; a side that was
       calling or punching to them stops. */
    bool (*blocked)(uint64_t id);
    bool refused;  /* a blocked player spoke; the caller clears it */
    bool joined;   /* the other player spoke in the last 3 s */
    bool mismatch; /* the other player runs another build */
    bool ready;
    bool start;    /* from the host: restart into the session now */
    bool pinged;
    uint32_t ping_ms;
    char name[RECOMP_NETPLAY_NAME_BYTES + 1u];
    uint64_t peer_id; /* the other player's install ID */
    /* The other player's newest chat line, already checked; heard_new is set
       when one arrives, for the caller to clear. */
    char heard[RECOMP_NETPLAY_CHAT_BYTES + 1u];
    bool heard_new;
    /* Kept between ticks. */
    uint64_t heard_ms, sent_ms, echo_at_ms;
    uint32_t echo;
    char say[RECOMP_NETPLAY_CHAT_BYTES + 1u];
    uint32_t say_id, heard_id;
    uint64_t said_ms;
    /* A host's peer taken from an unasked room packet, whose address may be forged, gets one
       packet per packet it sends until it echoes one of this side's clocks. */
    bool doubted, owed;
} RecompNetplayRoom;

/* Reads what the other player said and, every 100 ms or every call while
   start is set, says this side's state. digest holds
   RECOMP_NETPLAY_DIGEST_BYTES bytes; name is UTF-8 and cut to
   RECOMP_NETPLAY_NAME_BYTES. A host not starting answers a joiner still
   calling its last match with a goodbye. */
void recomp_netplay_room_tick(RecompNetplay *net, RecompNetplayRoom *room, const char *digest,
    const char *name, bool ready, bool start);
/* Sends a chat line, cut to RECOMP_NETPLAY_CHAT_BYTES, with the next ticks:
   each tick for 2 s repeats it, and the other side shows it once. The other
   side drops a line that is not printable ASCII or that
   recomp_netplay_name_banned refuses. */
void recomp_netplay_room_say(RecompNetplayRoom *room, const char *text);
/* Forgets the other player as if they went quiet; a host stops answering
   them and lets another player in. */
void recomp_netplay_room_drop(RecompNetplay *net, RecompNetplayRoom *room);

/* Matchmaking, all from the room's own socket so the server sees the
   endpoint the room uses. False when host does not resolve. */
bool recomp_netplay_match_server(RecompNetplay *net, const char *host, uint16_t port);
/* A host's listing and heartbeat; say it every 5 s while the room is up. */
void recomp_netplay_match_host(RecompNetplay *net, const char *digest, uint8_t delay, bool open,
    const char *name);
void recomp_netplay_match_list(RecompNetplay *net, const char *digest);
/* Asks to join a listed room; again until met or gone. */
void recomp_netplay_match_join(RecompNetplay *net, uint32_t room_id, const char *digest, const char *name);
void recomp_netplay_match_leave(RecompNetplay *net);
/* Drains the socket outside a room, keeping only the matchmaker's answers. */
void recomp_netplay_match_poll(RecompNetplay *net);
/* The input delay to suggest for a round trip: none up to 20 ms, then a
   frame per 33 ms rounded up, at most RECOMP_ROLLBACK_WINDOW. */
uint32_t recomp_netplay_suggested_delay(uint32_t rtt_ms);
/* Waits on a socket from recomp_netplay_listen, or a room's, for a joiner's
   hello, answering each with the host's
   digest and settings. The first joiner becomes the peer; one with another
   digest ends the wait with MISMATCH. A room's socket, which already has a
   peer, hears only that peer. Returns once both sides hold both profiles.
   cancelled, when not null, is polled every 20 ms; either side cancelling
   sends the other a goodbye, which ends its wait too. */
RecompNetplayLobbyResult recomp_netplay_host(RecompNetplay *net, const RecompNetplayLobby *lobby,
    RecompNetplayProfiles *profiles, uint32_t timeout_ms, bool (*cancelled)(void));
/* Says hello to the peer of a socket from recomp_netplay_open, or a room's,
   until it answers, copies the host's delay and flags into lobby, then swaps
   profiles as the host does. A delay over RECOMP_ROLLBACK_WINDOW is a MISMATCH. */
RecompNetplayLobbyResult recomp_netplay_join(RecompNetplay *net, RecompNetplayLobby *lobby,
    RecompNetplayProfiles *profiles, uint32_t timeout_ms, bool (*cancelled)(void));
void recomp_netplay_close(RecompNetplay *net);
/* The local IPv4 address of the interface that reaches the internet, as
   text; it also starts Winsock on Windows. */
bool recomp_netplay_route_address(char *text, size_t size);
/* Asks the router over UPnP to forward UDP port to this computer, and
   prints the outcome and the address the other player joins, which also
   goes to address as host:port (empty when the router's own address is
   private). False when no router answered or it refused; the port may
   still be reachable. Takes seconds. */
bool recomp_netplay_open_router_port(uint16_t port, char *address, size_t size);
/* Removes that forward; nothing when none was made. */
void recomp_netplay_close_router_port(void);
/* True for a profile name that is not allowed online, or one too long to
   check. Takes UTF-8. */
bool recomp_netplay_name_banned(const char *name);
void recomp_netplay_send(RecompNetplay *net, const RecompRollbackSession *session,
    int32_t advantage);
/* Tells the peer this runner is quitting, a few times since UDP may drop one. */
void recomp_netplay_send_goodbye(const RecompNetplay *net);
/* Drains the socket into the session. */
void recomp_netplay_receive(RecompNetplay *net, RecompRollbackSession *session);
/* Frames to wait now so neither side runs ahead: half the difference of the
   two averaged advantages, which cancels the latency both of them include.
   Call once per new frame with the advantage that frame sent. */
uint32_t recomp_netplay_wait_frames(RecompNetplay *net, int32_t advantage);
/* Keeps this side's hash of a confirmed frame for its packets to carry. */
void recomp_netplay_add_check(RecompNetplay *net, uint32_t frame, uint64_t hash);
/* This side's hash for the frame of the other side's newest check (peer_check_frame);
   false when this side has not hashed that frame or no longer keeps it. */
bool recomp_netplay_own_check(const RecompNetplay *net, uint64_t *mine);

size_t recomp_netplay_encode(const RecompNetplay *net, const RecompRollbackSession *session,
    int32_t advantage, uint8_t *packet);
/* False for a malformed packet, which changes nothing. A goodbye sets peer_left. */
bool recomp_netplay_decode(RecompNetplay *net, RecompRollbackSession *session,
    const uint8_t *packet, size_t size);

#ifdef __cplusplus
}
#endif

#endif
