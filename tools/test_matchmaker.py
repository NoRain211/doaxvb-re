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
HOST_AT6 = ("2001:db8::7", 40001)  # the same players over IPv6: one socket, one nonce, two endpoints
JOINER_AT6 = ("2001:db8:1::9", 40002)
NONCE_OF = {HOST_AT6: HOST_AT, JOINER_AT6: JOINER_AT}
NO_TOKENS = bytes(2 * mm.TOKEN)  # MatchmakerTest.send fills in the sender's nonce and cookie


def nonce(sender):
    """A stand-in for the random nonce of the client socket at sender."""
    return mm.endpoint(NONCE_OF.get(sender, sender)) + b"\0\0"


def hosting(name=b"KASUMI", is_open=1, build=BUILD, lan=LAN, rtt=0):
    return mm.header(mm.HOST) + NO_TOKENS + mm.HOST_BODY.pack(build, 2, is_open, len(name), name, lan, rtt)


def listing(build=BUILD, size=mm.LIST_BYTES):
    return (mm.header(mm.LIST) + bytes(mm.TOKEN) + build).ljust(size, b"\0")


def joining(room_id, name=b"AYANE", build=BUILD, lan=LAN):
    return mm.header(mm.JOIN) + NO_TOKENS + mm.JOIN_BODY.pack(room_id, build, len(name), name, lan)


def saying_v6():
    return mm.header(mm.ADDR6) + NO_TOKENS


def relaying(game=b"pads"):
    return mm.header(mm.RELAY) + NO_TOKENS + game


def relayed(to, game=b"pads"):
    return mm.header(mm.RELAY) + nonce(to) + game


def rooms(answer):
    count = answer[6 + mm.TOKEN]
    assert len(answer) == 7 + mm.TOKEN + count * mm.ENTRY.size
    return [mm.ENTRY.unpack_from(answer, 7 + mm.TOKEN + i * mm.ENTRY.size) for i in range(count)]


class MatchmakerTest(unittest.TestCase):
    def setUp(self):
        self.ids = iter(range(1, 1000))
        self.server = mm.Matchmaker(lambda: next(self.ids))
        self.sent = []

    def send(self, data, sender, now, cookie=None):
        """Sends data from sender with its nonce and, unless one is given, the server's current cookie."""
        if len(data) >= mm.ASKED and data[:4] == mm.MAGIC:
            if cookie is None:
                cookie = self.server.cookie(sender, int(now // mm.COOKIE_SECONDS))
            data = data[:6] + nonce(sender) + (data[14:mm.ASKED] if data[5] == mm.LIST else cookie) + data[mm.ASKED:]
        answers = self.server.handle(data, sender, now)
        for packet, to in answers:
            # No answer outgrows its ask, so a forged sender gains nothing.
            self.assertLessEqual(len(packet), len(data))
            # Each carries its receiver's nonce, so a forged one from elsewhere is ignored.
            self.assertEqual(packet[6:6 + mm.TOKEN], nonce(to))
        return answers

    def host(self, now=0.0, sender=HOST_AT, **fields):
        [(answer, to)] = self.send(hosting(**fields), sender, now)
        self.assertEqual((to, answer[5]), (sender, mm.HOSTED))
        return struct.unpack_from(">I", answer, 6 + mm.TOKEN)[0], answer[10 + mm.TOKEN:]

    def test_host_learns_its_room_and_public_endpoint(self):
        room_id, public = self.host()
        self.assertEqual(public, mm.endpoint(HOST_AT))
        self.assertEqual(self.host(now=5.0)[0], room_id)  # a heartbeat keeps the room

    def test_list_shows_open_rooms_of_the_same_build_newest_first(self):
        first, _ = self.host(now=0.0)
        second, _ = self.host(now=1.0, sender=("203.0.113.8", 47701), name=b"HAYATE")
        self.host(now=1.0, sender=("203.0.113.9", 47701), build=OTHER_BUILD)
        self.host(now=1.0, sender=("203.0.113.10", 47701), is_open=0)
        [(answer, _)] = self.send(listing(), JOINER_AT, 3.0)
        listed = rooms(answer)
        self.assertEqual([(entry[0], entry[1], entry[4][:entry[3]]) for entry in listed],
                         [(second, 2, b"HAYATE"), (first, 3, b"KASUMI")])
        self.assertEqual(self.send(listing(size=100), JOINER_AT, 3.0), [])

    def test_a_list_carries_each_hosts_round_trip_and_never_its_address(self):
        first, _ = self.host(now=0.0, rtt=45)
        second, _ = self.host(now=1.0, sender=("203.0.113.8", 47702), name=b"HAYATE")
        [(answer, _)] = self.send(listing(), JOINER_AT, 3.0)
        self.assertEqual([(entry[0], entry[4][:entry[3]], entry[5]) for entry in rooms(answer)],
                         [(second, b"HAYATE", 0), (first, b"KASUMI", 45)])
        for host in (HOST_AT[0], "203.0.113.8"):
            self.assertNotIn(socket.inet_aton(host), answer)
        self.host(now=5.0, rtt=60)  # the latest heartbeat's round trip
        self.assertEqual(rooms(self.send(listing(), JOINER_AT, 6.0)[0][0])[1][5], 60)

    def test_a_full_list_stays_within_its_ask(self):
        for i in range(13):  # one more than a list holds
            newest, _ = self.host(now=i * 0.01, sender=(f"203.0.113.{i + 1}", 47701))
        listed = rooms(self.send(listing(), JOINER_AT, 1.0)[0][0])
        self.assertEqual(len(listed), (mm.LIST_BYTES - 7 - mm.TOKEN) // mm.ENTRY.size)
        self.assertEqual(listed[0][0], newest)  # the room left out is the oldest

    def test_join_sends_each_side_the_others_endpoints(self):
        room_id, _ = self.host()
        answers = dict((to, packet) for packet, to in self.send(joining(room_id), JOINER_AT, 1.0))
        to_host, to_joiner = (packet[:6] + packet[6 + mm.TOKEN:] for packet in (answers[HOST_AT], answers[JOINER_AT]))
        self.assertEqual(to_host[:6], mm.header(mm.PEER))
        # Behind different routers, neither learns the other's LAN address.
        self.assertEqual(to_host[6:18], mm.endpoint(JOINER_AT) + bytes(6))
        self.assertEqual(to_host[19:19 + to_host[18]], b"AYANE")
        self.assertEqual(to_joiner[6:18], mm.endpoint(HOST_AT) + bytes(6))
        self.assertEqual(to_joiner[19:19 + to_joiner[18]], b"KASUMI")
        # Claimed while the two connect; a heartbeat still saying open lists it again later.
        self.assertEqual(rooms(self.send(listing(), ("192.0.2.1", 1), 2.0)[0][0]), [])
        self.host(now=12.0)
        self.assertEqual(len(rooms(self.send(listing(), ("192.0.2.1", 1), 12.0)[0][0])), 1)

    def test_two_players_with_ipv6_are_handed_each_others_endpoints_after_the_ipv4_ones(self):
        self.send(saying_v6(), HOST_AT6, 0.0)
        room_id, _ = self.host(now=0.1)
        self.send(saying_v6(), JOINER_AT6, 0.2)
        answers = dict((to, packet) for packet, to in self.send(joining(room_id), JOINER_AT, 1.0))
        self.assertEqual(answers[HOST_AT][-18:], mm.endpoint(JOINER_AT6))
        self.assertEqual(answers[JOINER_AT][-18:], mm.endpoint(HOST_AT6))
        # The IPv4 part is as without IPv6.
        self.assertEqual(answers[HOST_AT][6 + mm.TOKEN:12 + mm.TOKEN], mm.endpoint(JOINER_AT))

    def test_one_player_without_ipv6_leaves_the_short_answer_for_both(self):
        self.send(saying_v6(), HOST_AT6, 0.0)
        room_id, _ = self.host(now=0.1)
        for packet, _ in self.send(joining(room_id), JOINER_AT, 1.0):
            self.assertEqual(len(packet), 6 + mm.TOKEN + 6 + 6 + 1 + mm.NAME)

    def test_an_ipv6_endpoint_is_stored_only_with_a_cookie_and_expires(self):
        self.send(saying_v6(), HOST_AT6, 0.0, cookie=bytes(mm.TOKEN))  # forged: nothing stored
        self.assertEqual(self.server.v6, {})
        self.send(saying_v6(), HOST_AT6, 0.0)
        self.assertEqual(len(self.server.v6), 1)
        self.send(listing(), JOINER_AT, mm.ADDR6_SECONDS + 5.0)
        self.assertEqual(self.server.v6, {})

    def test_a_full_ipv6_table_still_refreshes_what_it_holds(self):
        self.send(saying_v6(), HOST_AT6, 0.0)
        self.server.v6.update((bytes(2) + i.to_bytes(6, "big"), (JOINER_AT6, 0.0)) for i in range(2 * mm.MAX_ROOMS))
        self.send(saying_v6(), HOST_AT6, 15.0)
        self.assertEqual(self.server.v6[nonce(HOST_AT)][1], 15.0)

    def test_ipv6_senders_may_only_list_and_say_their_address(self):
        self.assertEqual(self.send(hosting(), HOST_AT6, 0.0), [])
        self.assertEqual(self.server.rooms, {})
        [(answer, to)] = self.send(listing(), JOINER_AT6, 0.0)
        self.assertEqual((answer[5], to), (mm.ROOMS, JOINER_AT6))

    def test_join_refuses_a_missing_claimed_or_other_build_room(self):
        room_id, _ = self.host()
        for packet in (joining(room_id + 1), joining(room_id, build=OTHER_BUILD)):
            [(answer, to)] = self.send(packet, JOINER_AT, 1.0)
            self.assertEqual((to, answer[5]), (JOINER_AT, mm.GONE))
        self.send(joining(room_id), JOINER_AT, 1.0)
        self.assertEqual(self.send(joining(room_id), ("192.0.2.1", 1), 2.0)[0][0][5], mm.GONE)

    def test_only_private_lan_endpoints_with_a_port_are_passed_on_behind_one_router(self):
        same_router = (HOST_AT[0], 51234)
        for lan, kept in ((LAN, True), (socket.inet_aton("10.1.2.3") + b"\x01\x02", True),
                          (socket.inet_aton("169.254.9.9") + b"\x01\x02", True),
                          (socket.inet_aton("8.8.8.8") + b"\x00\x35", False),
                          (socket.inet_aton("127.0.0.1") + b"\x01\x02", False),
                          (socket.inet_aton("192.168.1.20") + bytes(2), False)):
            self.setUp()
            room_id, _ = self.host(lan=lan)
            answers = dict((to, packet) for packet, to in self.send(joining(room_id, lan=lan), same_router, 1.0))
            self.assertEqual(len(answers), 2)
            for packet in answers.values():
                self.assertEqual(packet[12 + mm.TOKEN:18 + mm.TOKEN], lan if kept else bytes(6))

    def test_one_address_holds_one_claim(self):
        first, _ = self.host()
        second, _ = self.host(sender=("203.0.113.8", 47701))
        self.send(joining(first), JOINER_AT, 1.0)
        self.send(joining(second), (JOINER_AT[0], 51235), 1.0)
        listed = [entry[0] for entry in rooms(self.send(listing(), ("192.0.2.1", 1), 2.0)[0][0])]
        self.assertEqual(listed, [first])

    def test_joins_are_rate_limited_per_address(self):
        room_id, _ = self.host()
        answered = sum(bool(self.send(joining(room_id), (JOINER_AT[0], 50000 + i), 1.0))
                       for i in range(mm.JOINS_PER_SECOND + 5))
        self.assertEqual(answered, mm.JOINS_PER_SECOND)
        self.assertTrue(self.send(joining(room_id), JOINER_AT, 2.5))  # a new second

    def test_a_repeated_join_answers_again(self):
        room_id, _ = self.host()
        first = self.send(joining(room_id), JOINER_AT, 1.0)
        self.assertEqual(self.send(joining(room_id), JOINER_AT, 1.5), first)

    def test_a_restarted_server_pairs_a_room_and_its_joiner_again(self):
        room_id, _ = self.host()
        self.send(joining(room_id), JOINER_AT, 1.0)
        self.server = mm.Matchmaker()  # everything is forgotten
        self.assertEqual(self.send(relaying(), JOINER_AT, 2.0), [])
        self.assertEqual(self.send(joining(room_id), JOINER_AT, 2.0)[0][0][5], mm.GONE)  # until the host's heartbeat
        self.assertEqual(self.host(now=3.0)[0], room_id)
        answers = self.send(joining(room_id), JOINER_AT, 4.0)  # both sides hear PEER again
        self.assertEqual(sorted(to for _, to in answers), sorted([HOST_AT, JOINER_AT]))
        self.assertTrue(all(packet[5] == mm.PEER for packet, _ in answers))
        self.assertEqual(self.send(relaying(), JOINER_AT, 5.0), [(relayed(HOST_AT), HOST_AT)])

    def test_relay_only_hides_endpoints(self):
        self.server.relay_only = True
        room_id, _ = self.host()
        for packet, _ in self.send(joining(room_id), JOINER_AT, 1.0):
            self.assertEqual(packet[6 + mm.TOKEN:18 + mm.TOKEN], bytes(12))
        self.assertEqual(self.send(relaying(), JOINER_AT, 2.0), [(relayed(HOST_AT), HOST_AT)])

    def test_relay_forwards_only_within_a_pair_and_caps_the_rate(self):
        room_id, _ = self.host()
        self.send(joining(room_id), JOINER_AT, 1.0)
        self.assertEqual(self.send(relaying(), JOINER_AT, 2.0), [(relayed(HOST_AT), HOST_AT)])
        self.assertEqual(self.send(relaying(), HOST_AT, 2.0), [(relayed(JOINER_AT), JOINER_AT)])
        self.assertEqual(self.send(relaying(), ("192.0.2.1", 1), 2.0), [])
        forwarded = sum(len(self.send(relaying(), JOINER_AT, 3.5)) for _ in range(mm.RELAY_PER_SECOND + 10))
        self.assertEqual(forwarded, mm.RELAY_PER_SECOND)

    def test_one_address_relays_and_pairs_only_so_much(self):
        hosts = [(f"203.0.113.{i + 1}", 47701) for i in range(mm.PAIRS_PER_ADDRESS + 1)]
        ids = [self.host(sender=host)[0] for host in hosts]
        joiners = [(JOINER_AT[0], 50000 + i) for i in range(len(hosts))]
        for i, joiner in enumerate(joiners):  # under the JOIN rate, and each new claim lets go of the last
            kind = self.send(joining(ids[i]), joiner, 1.0 + i)[0][0][5]
            self.assertEqual(kind, mm.PEER if i < mm.PAIRS_PER_ADDRESS else mm.GONE)
        # Each pair sends past its own cap; what that cap drops is not counted against the address.
        forwarded = [sum(len(self.send(relaying(), joiner, 30.5)) for _ in range(mm.RELAY_PER_SECOND + 10))
                     for joiner in joiners[:3]]
        self.assertEqual(forwarded, [mm.RELAY_PER_SECOND, mm.RELAY_PER_SECOND, 0])
        self.send(listing(), JOINER_AT, 30.5 + mm.PAIR_SECONDS + 1.0)  # a sweep
        self.assertEqual(self.server.pairs, {})

    def test_a_sender_without_a_cookie_gets_one_and_nothing_is_stored(self):
        room_id, _ = self.host()
        forged = ("192.0.2.1", 1)
        stale = self.server.cookie(forged, -2)  # two minutes old
        others = self.server.cookie(HOST_AT, 0)  # the right epoch, but another endpoint's
        for packet, sender in ((hosting(), forged), (joining(room_id), forged), (joining(room_id), JOINER_AT),
                               (mm.header(mm.LEAVE) + NO_TOKENS, HOST_AT), (relaying(), JOINER_AT)):
            for cookie in (bytes(mm.TOKEN), stale, others if sender != HOST_AT else stale):
                [(answer, to)] = self.send(packet, sender, 1.0, cookie=cookie)
                self.assertEqual((to, answer[:6], answer[6 + mm.TOKEN:]),
                                 (sender, mm.header(mm.COOKIE), self.server.cookie(sender, 0)))
        self.assertEqual(list(self.server.rooms), [HOST_AT])
        self.assertEqual((self.server.pairs, self.server.claims, self.server.join_counts), ({}, {}, {}))
        # The cookie it was given lets it in, through the next minute too, with a new cookie then.
        room_id, _ = self.host(now=55.0)  # a new room; the first expired
        [(answer, _)] = self.send(joining(room_id), JOINER_AT, 55.0, cookie=bytes(mm.TOKEN))
        answers = self.send(joining(room_id), JOINER_AT, 61.0, cookie=answer[6 + mm.TOKEN:])
        self.assertEqual(sorted(packet[5] for packet, _ in answers), [mm.PEER, mm.PEER, mm.COOKIE])

    def test_rooms_expire_leave_and_are_capped_per_address(self):
        self.host()
        self.send(mm.header(mm.LEAVE) + NO_TOKENS, HOST_AT, 1.0)
        self.assertEqual(self.server.rooms, {})
        self.host(now=2.0)
        self.send(listing(), JOINER_AT, 2.0 + mm.ROOM_SECONDS + 1.0)
        self.assertEqual(self.server.rooms, {})
        for port in range(mm.ROOMS_PER_ADDRESS):
            self.host(now=60.0, sender=(HOST_AT[0], 50000 + port))
        self.assertEqual(self.send(hosting(), (HOST_AT[0], 60000), 60.0), [])

    def test_an_address_opens_few_rooms_a_minute(self):
        # Leaving and hosting again from new ports would keep one address's rooms at the top of the list.
        for port in range(mm.NEW_ROOMS_PER_MINUTE):
            self.host(now=1.0, sender=(HOST_AT[0], 50000 + port))
            self.send(mm.header(mm.LEAVE) + NO_TOKENS, (HOST_AT[0], 50000 + port), 1.0)
        self.assertEqual(self.send(hosting(), (HOST_AT[0], 60000), 2.0), [])
        self.host(now=2.0, sender=JOINER_AT)  # another address is not held back
        self.host(now=61.0, sender=(HOST_AT[0], 60000))

    def test_garbage_gets_no_answer(self):
        for packet in (b"", b"D2MM", mm.header(mm.HOST) + NO_TOKENS + b"short", mm.header(mm.LEAVE) + b"short",
                       b"XXXX" + bytes((mm.VERSION, mm.LIST)) + bytes(506), b"D2MM\x01\x03" + bytes(506)):
            self.assertEqual(self.send(packet, JOINER_AT, 0.0), [])

    def test_status_counts_without_addresses_and_restarts(self):
        room_id, _ = self.host()
        self.send(joining(room_id), JOINER_AT, 1.0)
        self.send(joining(room_id), JOINER_AT, 1.5)  # a repeat is the same join
        for _ in range(mm.RELAY_PER_SECOND + 2):
            self.send(relaying(), JOINER_AT, 2.0)
        line = self.server.status(3.0)
        self.assertEqual(line, "matchmaker: rooms 1, open 1, pairs 1, joins 1, relayed 250 (4 KiB), over the relay cap 2")
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
