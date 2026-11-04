# SPDX-License-Identifier: GPL-3.0-or-later
import socket
import struct
import threading
import unittest

import matchmaker as mm

HOST_AT = ("203.0.113.7", 47701)
JOINER_AT = ("198.51.100.9", 51234)
BUILD = b"a" * mm.DIGEST
OTHER_BUILD = b"b" * mm.DIGEST
LAN = socket.inet_aton("192.168.1.20") + struct.pack(">H", 47701)


def hosting(name=b"KASUMI", is_open=1, build=BUILD):
    return mm.header(mm.HOST) + mm.HOST_BODY.pack(build, 2, is_open, len(name), name, LAN)


def listing(build=BUILD, size=mm.LIST_BYTES, endpoints=False):
    flag = bytes((mm.WITH_ENDPOINTS,)) if endpoints else b""
    return (mm.header(mm.LIST) + build + flag).ljust(size, b"\0")


def joining(room_id, name=b"AYANE", build=BUILD):
    return mm.header(mm.JOIN) + mm.JOIN_BODY.pack(room_id, build, len(name), name, LAN)


def rooms(answer, entry=mm.ENTRY):
    count = answer[6]
    assert len(answer) == 7 + count * entry.size
    return [entry.unpack_from(answer, 7 + i * entry.size) for i in range(count)]


class MatchmakerTest(unittest.TestCase):
    def setUp(self):
        self.ids = iter(range(1, 1000))
        self.server = mm.Matchmaker(lambda: next(self.ids))
        self.sent = []

    def send(self, data, sender, now):
        answers = self.server.handle(data, sender, now)
        # No answer outgrows its ask, so a forged sender gains nothing.
        for packet, _ in answers:
            self.assertLessEqual(len(packet), len(data))
        return answers

    def host(self, now=0.0, sender=HOST_AT, **fields):
        [(answer, to)] = self.send(hosting(**fields), sender, now)
        self.assertEqual((to, answer[5]), (sender, mm.HOSTED))
        return struct.unpack_from(">I", answer, 6)[0], answer[10:]

    def test_host_learns_its_room_and_public_endpoint(self):
        room_id, public = self.host()
        self.assertEqual(public, mm.endpoint(HOST_AT))
        self.assertEqual(self.host(now=5.0)[0], room_id)  # a heartbeat keeps the room

    def test_list_shows_open_rooms_of_the_same_build_oldest_first(self):
        first, _ = self.host(now=0.0)
        self.host(now=1.0, sender=("203.0.113.8", 47701), name=b"HAYATE")
        self.host(now=1.0, sender=("203.0.113.9", 47701), build=OTHER_BUILD)
        self.host(now=1.0, sender=("203.0.113.10", 47701), is_open=0)
        [(answer, _)] = self.send(listing(), JOINER_AT, 3.0)
        listed = rooms(answer)
        self.assertEqual([(entry[0], entry[1], entry[4][:entry[3]]) for entry in listed],
                         [(first, 3, b"KASUMI"), (first + 1, 2, b"HAYATE")])
        self.assertEqual(self.send(listing(size=100), JOINER_AT, 3.0), [])

    def test_a_list_that_asks_carries_each_hosts_public_endpoint(self):
        first, _ = self.host(now=0.0)
        self.host(now=1.0, sender=("203.0.113.8", 47702), name=b"HAYATE")
        [(answer, _)] = self.send(listing(endpoints=True), JOINER_AT, 3.0)
        self.assertEqual([(entry[0], entry[4][:entry[3]], entry[5]) for entry in rooms(answer, mm.ENTRY_AT)],
                         [(first, b"KASUMI", mm.endpoint(HOST_AT)),
                          (first + 1, b"HAYATE", mm.endpoint(("203.0.113.8", 47702)))])

    def test_a_full_list_stays_within_its_ask_in_either_form(self):
        for i in range(12):
            self.host(now=0.0, sender=(f"203.0.113.{i + 1}", 47701))
        self.assertEqual(len(rooms(self.send(listing(), JOINER_AT, 1.0)[0][0])), 12)
        self.assertEqual(len(rooms(self.send(listing(endpoints=True), JOINER_AT, 1.0)[0][0], mm.ENTRY_AT)),
                         (mm.LIST_BYTES - 7) // mm.ENTRY_AT.size)

    def test_join_sends_each_side_the_others_endpoints(self):
        room_id, _ = self.host()
        answers = dict((to, packet) for packet, to in self.send(joining(room_id), JOINER_AT, 1.0))
        to_host, to_joiner = answers[HOST_AT], answers[JOINER_AT]
        self.assertEqual(to_host[:6], mm.header(mm.PEER))
        self.assertEqual(to_host[6:18], mm.endpoint(JOINER_AT) + LAN)
        self.assertEqual(to_host[19:19 + to_host[18]], b"AYANE")
        self.assertEqual(to_joiner[6:18], mm.endpoint(HOST_AT) + LAN)
        self.assertEqual(to_joiner[19:19 + to_joiner[18]], b"KASUMI")
        # Claimed while the two connect; a heartbeat still saying open lists it again later.
        self.assertEqual(rooms(self.send(listing(), ("192.0.2.1", 1), 2.0)[0][0]), [])
        self.host(now=12.0)
        self.assertEqual(len(rooms(self.send(listing(), ("192.0.2.1", 1), 12.0)[0][0])), 1)

    def test_join_refuses_a_missing_claimed_or_other_build_room(self):
        room_id, _ = self.host()
        for packet in (joining(room_id + 1), joining(room_id, build=OTHER_BUILD)):
            [(answer, to)] = self.send(packet, JOINER_AT, 1.0)
            self.assertEqual((to, answer[5]), (JOINER_AT, mm.GONE))
        self.send(joining(room_id), JOINER_AT, 1.0)
        self.assertEqual(self.send(joining(room_id), ("192.0.2.1", 1), 2.0)[0][0][5], mm.GONE)

    def test_a_repeated_join_answers_again(self):
        room_id, _ = self.host()
        first = self.send(joining(room_id), JOINER_AT, 1.0)
        self.assertEqual(self.send(joining(room_id), JOINER_AT, 1.5), first)

    def test_relay_only_hides_endpoints(self):
        self.server.relay_only = True
        room_id, _ = self.host()
        [(answer, _)] = self.send(listing(endpoints=True), JOINER_AT, 0.5)
        self.assertEqual(rooms(answer, mm.ENTRY_AT)[0][5], bytes(6))
        for packet, _ in self.send(joining(room_id), JOINER_AT, 1.0):
            self.assertEqual(packet[6:18], bytes(12))
        game = mm.header(mm.RELAY) + b"pads"
        self.assertEqual(self.send(game, JOINER_AT, 2.0), [(game, HOST_AT)])

    def test_relay_forwards_only_within_a_pair_and_caps_the_rate(self):
        room_id, _ = self.host()
        self.send(joining(room_id), JOINER_AT, 1.0)
        game = mm.header(mm.RELAY) + b"pads"
        self.assertEqual(self.send(game, JOINER_AT, 2.0), [(game, HOST_AT)])
        self.assertEqual(self.send(game, HOST_AT, 2.0), [(game, JOINER_AT)])
        self.assertEqual(self.send(game, ("192.0.2.1", 1), 2.0), [])
        forwarded = sum(len(self.send(game, JOINER_AT, 3.5)) for _ in range(mm.RELAY_PER_SECOND + 10))
        self.assertEqual(forwarded, mm.RELAY_PER_SECOND)

    def test_rooms_expire_leave_and_are_capped_per_address(self):
        self.host()
        self.send(mm.header(mm.LEAVE), HOST_AT, 1.0)
        self.assertEqual(self.server.rooms, {})
        self.host(now=2.0)
        self.send(listing(), JOINER_AT, 2.0 + mm.ROOM_SECONDS + 1.0)
        self.assertEqual(self.server.rooms, {})
        for port in range(mm.ROOMS_PER_ADDRESS):
            self.host(now=30.0, sender=(HOST_AT[0], 50000 + port))
        self.assertEqual(self.send(hosting(), (HOST_AT[0], 60000), 30.0), [])

    def test_garbage_gets_no_answer(self):
        for packet in (b"", b"D2MM", mm.header(mm.HOST) + b"short", b"XXXX" + bytes((1, mm.LIST)) + bytes(506)):
            self.assertEqual(self.send(packet, JOINER_AT, 0.0), [])

    def test_status_counts_without_addresses_and_restarts(self):
        room_id, _ = self.host()
        self.send(joining(room_id), JOINER_AT, 1.0)
        self.send(joining(room_id), JOINER_AT, 1.5)  # a repeat is the same join
        game = mm.header(mm.RELAY) + b"pads"
        for _ in range(mm.RELAY_PER_SECOND + 2):
            self.send(game, JOINER_AT, 2.0)
        line = self.server.status(3.0)
        self.assertEqual(line, "matchmaker: rooms 1, open 1, pairs 1, joins 1, relayed 250 (2 KiB), over the relay cap 2")
        self.assertNotIn(HOST_AT[0], line)
        self.assertNotIn(JOINER_AT[0], line)
        self.assertTrue(self.server.status(4.0).endswith("joins 0, relayed 0 (0 KiB), over the relay cap 0"))

    def test_probe_hears_a_matchmaker_and_nothing_else(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as server:
            server.bind(("127.0.0.1", 0))

            def answer():
                data, sender = server.recvfrom(2048)
                for packet, to in mm.Matchmaker().handle(data, sender, 0.0):
                    server.sendto(packet, to)

            threading.Thread(target=answer, daemon=True).start()
            self.assertIsNotNone(mm.probe("127.0.0.1", server.getsockname()[1]))
            silent = server.getsockname()[1]  # nothing answers a second ask
            self.assertIsNone(mm.probe("127.0.0.1", silent, timeout=0.3))


if __name__ == "__main__":
    unittest.main()
