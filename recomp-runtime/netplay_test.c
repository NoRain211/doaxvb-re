#include "netplay.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000u)
#endif

static int failures;

static void check(const char *name, bool passed)
{
    if (!passed) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static RecompInputGamepad pad(uint16_t buttons, int16_t thumb)
{
    RecompInputGamepad value;
    memset(&value, 0, sizeof value);
    value.buttons = buttons;
    value.analog_buttons[3] = 0xffu;
    value.thumb_ly = thumb;
    return value;
}

int main(void)
{
    static RecompRollbackSession a, b;
    static RecompNetplay net_a, net_b;
    uint8_t packet[RECOMP_NETPLAY_MAX_PACKET];

    recomp_rollback_init(&a, 0u);
    recomp_rollback_init(&b, 0u);
    for (uint16_t i = 0u; i < 5u; ++i) {
        const RecompInputGamepad value = pad(i, (int16_t)(-1000 * i));
        recomp_rollback_add_local(&a, &value);
    }
    size_t size = recomp_netplay_encode(&net_a, &a, -3, packet);
    check("encode size", size == RECOMP_NETPLAY_HEADER_BYTES + 5u * RECOMP_NETPLAY_PAD_BYTES);
    check("truncated rejected", !recomp_netplay_decode(&net_b, &b, packet, size - 1u));
    packet[0] ^= 1u;
    check("bad magic rejected", !recomp_netplay_decode(&net_b, &b, packet, size));
    packet[0] ^= 1u;
    check("decode", recomp_netplay_decode(&net_b, &b, packet, size));
    check("pads arrive", b.remote_count == 5u && b.remote[4].buttons == 4u &&
        b.remote[4].thumb_ly == -4000 && b.remote[4].analog_buttons[3] == 0xffu);
    check("advantage arrives", net_b.peer_advantage == -3 && net_b.peer_seen);
    check("duplicate ignored", recomp_netplay_decode(&net_b, &b, packet, size) && b.remote_count == 5u);

    // The ack trims what the next packet repeats.
    size = recomp_netplay_encode(&net_b, &b, 0, packet);
    check("ack decode", recomp_netplay_decode(&net_a, &a, packet, size) && net_a.peer_ack == 5u);
    check("acked pads not resent",
        recomp_netplay_encode(&net_a, &a, 0, packet) == RECOMP_NETPLAY_HEADER_BYTES);
    check("ack beyond our pads rejected", (packet[8] = 9u, !recomp_netplay_decode(&net_b, &b, packet,
        RECOMP_NETPLAY_HEADER_BYTES)));

    // Desync checks: each packet carries the newest; a side compares the frames it also hashed.
    memset(&net_a, 0, sizeof net_a);
    memset(&net_b, 0, sizeof net_b);
    uint64_t mine = 0u;
    size = recomp_netplay_encode(&net_a, &a, 0, packet);
    check("no check yet", recomp_netplay_decode(&net_b, &b, packet, size) && net_b.peer_check_frame == 0u &&
        !recomp_netplay_own_check(&net_b, &mine));
    for (uint32_t frame = 30u; frame <= 300u; frame += 30u)
        recomp_netplay_add_check(&net_a, frame, 0x0123456789abcdefull * frame);
    size = recomp_netplay_encode(&net_a, &a, 0, packet);
    check("check decode", recomp_netplay_decode(&net_b, &b, packet, size) && net_b.peer_check_frame == 300u &&
        net_b.peer_check_hash == 0x0123456789abcdefull * 300u);
    check("peer ahead waits", !recomp_netplay_own_check(&net_b, &mine));
    recomp_netplay_add_check(&net_b, 270u, 0x0123456789abcdefull * 270u);
    recomp_netplay_add_check(&net_b, 300u, 0x0123456789abcdefull * 300u);
    check("same hash", recomp_netplay_own_check(&net_b, &mine) && mine == net_b.peer_check_hash);
    check("older check ignored", (net_a.check_count = 9u, size = recomp_netplay_encode(&net_a, &a, 0, packet),
        recomp_netplay_decode(&net_b, &b, packet, size) && net_b.peer_check_frame == 300u));
    net_b.check_hashes[1] ^= 1u << 20;
    check("desync found", recomp_netplay_own_check(&net_b, &mine) && mine != net_b.peer_check_hash);
    // A check that has left the ring compares with nothing.
    for (uint32_t frame = 330u; frame <= 540u; frame += 30u) recomp_netplay_add_check(&net_b, frame, 0u);
    check("forgotten check", !recomp_netplay_own_check(&net_b, &mine));

    // Frame wait: a side 4 frames ahead waits 2, then backs off.
    memset(&net_a, 0, sizeof net_a);
    for (unsigned i = 0u; i < RECOMP_NETPLAY_SYNC_FRAMES; ++i) net_a.peer_samples[i] = -2;
    net_a.peer_count = RECOMP_NETPLAY_SYNC_FRAMES;
    uint32_t waited = 0u;
    for (unsigned i = 0u; i < RECOMP_NETPLAY_SYNC_FRAMES; ++i) waited += recomp_netplay_wait_frames(&net_a, 2);
    check("ahead side waits", waited == 2u);
    check("cooldown", recomp_netplay_wait_frames(&net_a, 2) == 0u);
    memset(&net_b, 0, sizeof net_b);
    for (unsigned i = 0u; i < RECOMP_NETPLAY_SYNC_FRAMES; ++i) net_b.peer_samples[i] = 2;
    net_b.peer_count = RECOMP_NETPLAY_SYNC_FRAMES;
    waited = 0u;
    for (unsigned i = 0u; i < 2u * RECOMP_NETPLAY_SYNC_FRAMES; ++i) waited += recomp_netplay_wait_frames(&net_b, -2);
    check("behind side never waits", waited == 0u);

    // A peer frame is sampled once, however many packets repeat it.
    recomp_rollback_init(&a, 0u);
    memset(&net_a, 0, sizeof net_a);
    a.frame = 3u;
    size = recomp_netplay_encode(&net_a, &a, 6, packet);
    memset(&net_b, 0, sizeof net_b);
    recomp_rollback_init(&b, 0u);
    for (int i = 0; i < 5; ++i) recomp_netplay_decode(&net_b, &b, packet, size);
    check("one sample per peer frame", net_b.peer_count == 1u && net_b.peer_samples[0] == 6);

    // Real sockets on loopback, on ports the system picks: pads cross both ways.
    recomp_rollback_init(&a, 0u);
    recomp_rollback_init(&b, 0u);
    check("open a", recomp_netplay_open(&net_a, 0u, "127.0.0.1", 0u));
    check("open b", recomp_netplay_open(&net_b, 0u, "127.0.0.1", recomp_netplay_local_port(&net_a)));
    const uint16_t port_b = recomp_netplay_local_port(&net_b);
    const uint8_t port_b_order[2] = {(uint8_t)(port_b >> 8), (uint8_t)port_b}; // network order
    memcpy(&net_a.peer_port, port_b_order, sizeof port_b_order);
    const RecompInputGamepad seven = pad(7u, 0), nine = pad(9u, 0);
    recomp_rollback_add_local(&a, &seven);
    recomp_rollback_add_local(&b, &nine);
    for (unsigned tries = 0u; tries < 200u && (a.remote_count == 0u || b.remote_count == 0u); ++tries) {
        recomp_netplay_send(&net_a, &a, 0);
        recomp_netplay_send(&net_b, &b, 0);
        sleep_ms(5u);
        recomp_netplay_receive(&net_a, &a);
        recomp_netplay_receive(&net_b, &b);
    }
    check("socket pads", a.remote_count == 1u && a.remote[0].buttons == 9u &&
        b.remote_count == 1u && b.remote[0].buttons == 7u);
    // Each side has now heard the other hold its pad, which times the round trip.
    for (int i = 0; i < 50 && !(net_a.rtt_known && net_b.rtt_known); ++i) {
        recomp_netplay_send(&net_a, &a, 0);
        recomp_netplay_send(&net_b, &b, 0);
        sleep_ms(5u);
        recomp_netplay_receive(&net_a, &a);
        recomp_netplay_receive(&net_b, &b);
    }
    check("round trip", net_a.rtt_known && net_b.rtt_known && net_a.rtt_ms < 1000u && net_b.rtt_ms < 1000u);
    check("no goodbye yet", !net_a.peer_left && !net_b.peer_left);
    recomp_netplay_send_goodbye(&net_b);
    for (int i = 0; i < 50 && !net_a.peer_left; ++i) {
        sleep_ms(5u);
        recomp_netplay_receive(&net_a, &a);
    }
    check("goodbye arrives", net_a.peer_left && a.remote_count == 1u);
    recomp_netplay_close(&net_a);
    recomp_netplay_close(&net_b);

    static const char *const allowed[] = {"PLAYER1", "Ayane", "Raccoon", "Tycoon", "Spice", "Nigeria",
        "Homer", "Pakistan", "Hayabusa 2", "Nazir", "Maryann", "Fascia", "Heilman", "Kasumi1988",
        "Shelby88", ""};
    for (size_t i = 0u; i < sizeof allowed / sizeof *allowed; ++i)
        check(allowed[i], !recomp_netplay_name_banned(allowed[i]));
    static const char *const banned[] = {"N1GG3R", "xX_n i g g a_Xx", "f4g", "FAGS", "FAG123", "Tr4nny",
        "c00n", "Dyke_99", "niiiggggerrr", "\xc3\xa9" "faggot", "1488", "14_88", "Kasumi14 88", "H1TL3R",
        "Sieg Heil", "N4ZI", "Nazis", "xXneonaziXx", "Fascist", "facist", "WhitePower", "kkk_lord",
        "Aryan", "HH88", "14 W0RDS", "F4G123"};
    for (size_t i = 0u; i < sizeof banned / sizeof *banned; ++i)
        check(banned[i], recomp_netplay_name_banned(banned[i]));
    char longName[300];
    memset(longName, 'a', sizeof longName - 1u);
    longName[sizeof longName - 1u] = 0;
    check("too long to check", recomp_netplay_name_banned(longName));

    static const uint32_t rtts[] = {0u, 20u, 21u, 33u, 34u, 66u, 100u, 264u, 1000u, 0xffffffffu};
    static const uint32_t delays[] = {0u, 0u, 1u, 1u, 2u, 2u, 4u, 8u, 8u, 8u};
    for (size_t i = 0u; i < sizeof rtts / sizeof *rtts; ++i)
        check("suggested delay", recomp_netplay_suggested_delay(rtts[i]) == delays[i]);

    if (failures != 0) return 1;
    puts("PASS netplay carries pads, acks, advantage and goodbyes over UDP and screens names");
    return 0;
}
