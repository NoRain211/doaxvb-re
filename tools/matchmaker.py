#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""UDP matchmaker for netplay rooms.

Lists open rooms, pairs a joiner with a host so both can punch through their
routers, and relays a pair's packets when punching fails. One UDP port, no
accounts, everything in memory. docs/rollback-netcode.md says where it runs;
doa2u-re's docs/matchmaking.md at 0cf84f4 describes the packets.
"""
import argparse
import asyncio
import hashlib
import hmac
import ipaddress
import os
import socket
import struct
import sys
import time

MAGIC = b"D2MM"
VERSION = 2
HOST, HOSTED, LIST, ROOMS, JOIN, PEER, GONE, LEAVE, RELAY, COOKIE, ADDR6 = range(1, 12)
TOKEN = 8  # a client's nonce and the server's cookie
ASKED = 6 + 2 * TOKEN  # where a HOST, JOIN, LEAVE or RELAY body starts, after the nonce and cookie
COOKIE_SECONDS = 60  # a cookie lasts this epoch and the next
DIGEST = 64  # the netplay build's hex SHA-256; rooms only meet the same build
NAME = 32
LIST_BYTES = 512  # a list request is padded to this, so no answer outgrows its ask
ROOM_SECONDS = 20.0  # a host says HOST every 5 s
CLAIM_SECONDS = 10.0  # a joined room stays off the list while the two connect
PAIR_SECONDS = 600.0  # a pair's relay lasts this long after its last packet
MAX_ROOMS = 4096
ROOMS_PER_ADDRESS = 4
NEW_ROOMS_PER_MINUTE = 4  # per address; without it, leaving and hosting again keeps one's rooms the newest listed
JOINS_PER_SECOND = 10  # per address; a joiner repeats JOIN until it hears
LISTS_PER_SECOND = 10  # per address; LIST needs no cookie, and each one sorts every room
RELAY_PER_SECOND = 250  # per direction: 60 frames a second, plus repeats while waiting for the other player
RELAY_PER_ADDRESS = 2 * RELAY_PER_SECOND  # a second, so one address cannot relay through every pair it makes
PAIRS_PER_ADDRESS = 16  # each lasts PAIR_SECONDS; without a cap one address adds 10 a second
MAX_RELAY = 1200
ADDR6_SECONDS = 20.0  # a client says ADDR6 with each HOST, JOIN and LIST
ADDR6_PER_SECOND = 10  # per address

HOST_BODY = struct.Struct(">64sBBB32s6sH")  # digest, delay, open, name length, name, LAN endpoint, round trip
JOIN_BODY = struct.Struct(">I64sB32s6s")  # room id, digest, name length, name, LAN endpoint
ENTRY = struct.Struct(">IHBB32sH")  # room id, age in seconds, delay, name length, name, the host's round trip
LAN_NETWORKS = [ipaddress.ip_network(network) for network in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "169.254.0.0/16")]


def header(kind):
    return MAGIC + bytes((VERSION, kind))


def is_v6(address):
    return ":" in address[0]


def endpoint(address):
    """An IPv4 address and port as 6 bytes, or an IPv6 one as 18, in network order."""
    host = address[0].partition("%")[0]
    packed = socket.inet_pton(socket.AF_INET6, host) if is_v6(address) else socket.inet_aton(host)
    return packed + struct.pack(">H", address[1])


def lan_endpoint(lan):
    """A client's advertised LAN endpoint, or zeros (none) unless it is private or link-local with a port.

    The other side punches to it, so a public one would aim that player's packets at any host."""
    address = ipaddress.IPv4Address(lan[:4])
    if lan[4:] == b"\0\0" or not any(address in network for network in LAN_NETWORKS):
        return bytes(6)
    return lan


def peer(nonce, public, lan, name, v6=None):
    """v6, the other player's proven IPv6 endpoint, is appended only when both players have one."""
    return (header(PEER) + nonce + endpoint(public) + lan + bytes((len(name),)) + name.ljust(NAME, b"\0") +
            (endpoint(v6) if v6 else b""))


def over(counts, address, cap):
    """Counts one more for the address since the last sweep; true once that passes the cap."""
    counts[address] = counts.get(address, 0) + 1
    return counts[address] > cap


class Room:
    def __init__(self, room_id, host, now):
        self.id, self.host, self.born = room_id, host, now
        self.claimed_until, self.claimer = 0.0, None


class Pair:
    def __init__(self, other, nonce, now):
        self.other, self.nonce, self.used = other, nonce, now  # nonce: this end's, which packets to it echo
        self.second, self.count = int(now), 0


class Matchmaker:
    def __init__(self, random_id=lambda: int.from_bytes(os.urandom(4), "big"), relay_only=False):
        self.rooms = {}  # by the host's public endpoint
        self.by_id = {}
        self.pairs = {}  # by endpoint, both ways
        self.claims = {}  # the room each joining address holds; one each, so one address cannot hide the list
        self.join_counts = {}  # JOINs per address since the last sweep
        self.list_counts = {}  # LISTs per address since the last sweep
        self.relay_counts = {}  # RELAYs per address since the last sweep
        self.addr6_counts = {}  # ADDR6s per address since the last sweep
        self.v6 = {}  # a client's IPv6 endpoint by its nonce, proven by a cookie, and when it last said it
        self.room_counts, self.minute = {}, None  # rooms each address opened this minute
        self.random_id = random_id
        self.relay_only = relay_only  # players never learn each other's address
        self.secret = os.urandom(32)  # cookies stop working across a restart; clients get new ones
        self.swept = 0.0
        self.joins = self.relayed = self.relayed_bytes = self.over_cap = 0  # since the last status line

    def handle(self, data, sender, now):
        """Returns the (packet, endpoint) answers to one packet from sender."""
        if len(data) < 6 + TOKEN or data[:4] != MAGIC or data[4] != VERSION:
            return []
        if now - self.swept >= 1.0:
            self.sweep(now)
        kind, nonce, cookie, body = data[5], data[6:6 + TOKEN], data[6 + TOKEN:ASKED], data[ASKED:]
        if kind == LIST and len(data) == LIST_BYTES:
            return self.list(data[6 + TOKEN:6 + TOKEN + DIGEST], sender, nonce, now)
        if is_v6(sender) and kind not in (LIST, ADDR6):
            return []  # everything else is answered over IPv4, so a room's endpoint stays one
        sizes = {HOST: len(body) == HOST_BODY.size, JOIN: len(body) == JOIN_BODY.size, LEAVE: not body,
                 RELAY: len(body) <= MAX_RELAY, ADDR6: not body and is_v6(sender)}
        if len(data) < ASKED or not sizes.get(kind):
            return []
        # A forged sender address never sees its cookie, so nothing it asks for is stored.
        epoch = int(now // COOKIE_SECONDS)
        fresh = self.cookie(sender, epoch)
        refresh = [(header(COOKIE) + nonce + fresh, sender)]
        if hmac.compare_digest(cookie, fresh):
            refresh = []
        elif not hmac.compare_digest(cookie, self.cookie(sender, epoch - 1)):
            return refresh
        if kind == HOST:
            return self.host(body, sender, nonce, now) + refresh
        if kind == JOIN:
            return self.join(body, sender, nonce, now) + refresh
        if kind == LEAVE:
            self.remove(self.rooms.get(sender))
            return refresh
        if kind == ADDR6:
            # shortcut: counted per address, so one /64 can fill the table; cap by prefix if that matters.
            # A full table still refreshes what it holds, so a flood of new nonces cannot expire everyone's.
            if not over(self.addr6_counts, sender[0], ADDR6_PER_SECOND) and (nonce in self.v6 or len(self.v6) < 2 * MAX_ROOMS):
                self.v6[nonce] = (sender, now)
            return refresh
        return self.relay(body, sender, now) + refresh

    def cookie(self, sender, epoch):
        """The first bytes of an HMAC of the sender's endpoint and a minute's epoch, which only the server can make."""
        return hmac.new(self.secret, endpoint(sender) + struct.pack(">q", epoch), hashlib.sha256).digest()[:TOKEN]

    def host(self, body, sender, nonce, now):
        digest, delay, is_open, length, name, lan, rtt = HOST_BODY.unpack(body)
        if length > NAME:
            return []
        room = self.rooms.get(sender)
        if room is None:
            same_address = sum(1 for host in self.rooms if host[0] == sender[0])
            if (len(self.rooms) >= MAX_ROOMS or same_address >= ROOMS_PER_ADDRESS or
                    over(self.room_counts, sender[0], NEW_ROOMS_PER_MINUTE)):
                return []
            # From the endpoint and nonce alone: after a restart the room has its id again, so its joiner can ask again.
            room_id = int.from_bytes(hashlib.sha256(endpoint(sender) + nonce).digest()[:4], "big")
            while room_id == 0 or room_id in self.by_id:
                room_id = self.random_id()
            room = self.rooms[sender] = self.by_id[room_id] = Room(room_id, sender, now)
        room.digest, room.delay, room.open = digest, delay, is_open != 0
        room.name, room.lan, room.nonce, room.seen = name[:length], lan_endpoint(lan), nonce, now
        room.rtt = rtt  # the host's own HOST to HOSTED in ms, 0 unknown; joiners add theirs for an estimate
        return [(header(HOSTED) + nonce + struct.pack(">I", room.id) + endpoint(sender), sender)]

    def list(self, digest, sender, nonce, now):
        if over(self.list_counts, sender[0], LISTS_PER_SECOND):
            return []
        rooms = sorted((room for room in self.rooms.values()
                        if room.digest == digest and room.open and room.claimed_until <= now and room.host != sender),
                       key=lambda room: room.born, reverse=True)  # newest first: a full list would never show a new room
        rooms = rooms[:(LIST_BYTES - 7 - TOKEN) // ENTRY.size]
        answer = header(ROOMS) + nonce + bytes((len(rooms),))
        for room in rooms:
            answer += ENTRY.pack(room.id, min(int(now - room.born), 65535), room.delay, len(room.name), room.name,
                                 room.rtt)
        return [(answer, sender)]

    def join(self, body, sender, nonce, now):
        room_id, digest, length, name, lan = JOIN_BODY.unpack(body)
        if over(self.join_counts, sender[0], JOINS_PER_SECOND):
            return []
        room = self.by_id.get(room_id)
        # A joiner asks again until it hears; its own claim answers again.
        claimed = room is not None and room.claimed_until > now and room.claimer != sender
        # The pair count scans every pair, so it runs only for a join that would otherwise pass.
        if (length > NAME or room is None or room.digest != digest or not room.open or claimed or
                sender not in self.pairs and sum(1 for end in self.pairs if end[0] == sender[0]) >= PAIRS_PER_ADDRESS):
            return [(header(GONE) + nonce + struct.pack(">I", room_id), sender)]
        if room.claimer != sender or room.claimed_until <= now:
            held = self.claims.get(sender[0])
            if held is not None and held is not room and held.claimer is not None and held.claimer[0] == sender[0]:
                held.claimed_until, held.claimer = 0.0, None  # back on the list
            self.claims[sender[0]] = room
            room.claimed_until, room.claimer = now + CLAIM_SECONDS, sender
            self.joins += 1
        self.pairs[sender] = Pair(room.host, nonce, now)
        self.pairs[room.host] = Pair(sender, room.nonce, now)
        if self.relay_only:
            nowhere = ("0.0.0.0", 0)
            return [(peer(room.nonce, nowhere, bytes(6), name[:length]), room.host),
                    (peer(nonce, nowhere, bytes(6), room.name), sender)]
        # LAN endpoints only help two players behind one router; anyone else would learn a private address.
        # shortcut: carrier NAT can put two households on one public address; add a LAN probe if that matters.
        same_router = sender[0] == room.host[0]
        # Both players need an IPv6 endpoint, or the one with it would send to a peer that never answers there.
        theirs, mine = self.v6_of(nonce), self.v6_of(room.nonce)
        both = theirs and mine
        return [(peer(room.nonce, sender, lan_endpoint(lan) if same_router else bytes(6), name[:length],
                      theirs if both else None), room.host),
                (peer(nonce, room.host, room.lan if same_router else bytes(6), room.name, mine if both else None),
                 sender)]

    def v6_of(self, nonce):
        known = self.v6.get(nonce)
        return known[0] if known else None

    def relay(self, body, sender, now):
        pair = self.pairs.get(sender)
        back = self.pairs.get(pair.other) if pair is not None else None
        if back is None or back.other != sender:
            return []
        if int(now) != pair.second:
            pair.second, pair.count = int(now), 0
        pair.count += 1
        # The address is counted only past the pair's own cap, so a pair over it cannot use up the address's share.
        if pair.count > RELAY_PER_SECOND or over(self.relay_counts, sender[0], RELAY_PER_ADDRESS):
            self.over_cap += 1
            return []
        pair.used = back.used = now
        self.relayed += 1
        self.relayed_bytes += 6 + TOKEN + len(body)
        return [(header(RELAY) + back.nonce + body, pair.other)]

    def remove(self, room):
        if room is not None:
            del self.rooms[room.host]
            del self.by_id[room.id]

    def sweep(self, now):
        self.swept = now
        self.join_counts.clear()
        self.list_counts.clear()
        self.relay_counts.clear()
        self.addr6_counts.clear()
        for nonce in [nonce for nonce, (_, seen) in self.v6.items() if now - seen > ADDR6_SECONDS]:
            del self.v6[nonce]
        if int(now // 60) != self.minute:
            self.minute = int(now // 60)
            self.room_counts.clear()
        for room in [room for room in self.rooms.values() if now - room.seen > ROOM_SECONDS]:
            self.remove(room)
        for address in [address for address, room in self.claims.items() if room.claimed_until <= now]:
            del self.claims[address]
        for key in [key for key, pair in self.pairs.items() if now - pair.used > PAIR_SECONDS]:
            del self.pairs[key]

    def status(self, now):
        """One log line of counts, without addresses; the join and relay counts restart after it."""
        self.sweep(now)
        line = (f"matchmaker: rooms {len(self.rooms)}, open {sum(room.open for room in self.rooms.values())}, "
                f"pairs {len(self.pairs) // 2}, joins {self.joins}, relayed {self.relayed} "
                f"({self.relayed_bytes // 1024} KiB), over the relay cap {self.over_cap}")
        self.joins = self.relayed = self.relayed_bytes = self.over_cap = 0
        return line


class Server(asyncio.DatagramProtocol):
    def __init__(self, matchmaker):
        self.matchmaker = matchmaker
        self.loop = asyncio.get_running_loop()

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, sender):
        for packet, target in self.matchmaker.handle(data, sender[:2], self.loop.time()):
            # An IPv6 answer goes to the sender's full 4-tuple; Windows' event loop rejects the short form.
            self.transport.sendto(packet, sender if target == sender[:2] else target)


async def serve(address, port, relay_only, status_seconds, address6="::"):
    loop = asyncio.get_running_loop()
    matchmaker = Matchmaker(relay_only=relay_only)
    await loop.create_datagram_endpoint(lambda: Server(matchmaker), local_addr=(address, port))
    print(f"matchmaker: UDP {address}:{port}{' relay only' if relay_only else ''}", flush=True)
    if address6:
        # Its own socket, v6 only, so the IPv4 socket keeps its endpoints and a v4-only host still starts.
        six = None
        try:
            six = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            six.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
            six.bind((address6, port))
            await loop.create_datagram_endpoint(lambda: Server(matchmaker), sock=six)
            print(f"matchmaker: UDP [{address6}]:{port}", flush=True)
        except OSError as error:
            if six is not None:
                six.close()
            print(f"matchmaker: no IPv6 ({error})", flush=True)
    if status_seconds <= 0:
        await asyncio.Event().wait()
    while True:
        await asyncio.sleep(status_seconds)
        print(matchmaker.status(loop.time()), flush=True)


def probe(host, port, timeout=3.0, family=socket.AF_INET):
    """Asks a matchmaker for its list; returns the answer's round trip in ms, or None without one."""
    with socket.socket(family, socket.SOCK_DGRAM) as ask:
        ask.settimeout(timeout)
        nonce = os.urandom(TOKEN)
        started = time.monotonic()
        try:
            ask.sendto((header(LIST) + nonce).ljust(LIST_BYTES, b"\0"), (host, port))
            answer = ask.recv(2048)
        except OSError:  # a timeout, an unknown name, or Windows reporting a closed port
            return None
        if answer[:6 + TOKEN] != header(ROOMS) + nonce:
            return None
        return (time.monotonic() - started) * 1000.0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--address", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=47703)
    parser.add_argument("--relay-only", action="store_true", help="relay every pair, so no player learns another's address")
    parser.add_argument("--status", type=float, default=60.0, help="seconds between status lines, 0 for none")
    parser.add_argument("--address6", default="::", help="IPv6 address to bind, empty for IPv4 only")
    parser.add_argument("--probe", metavar="HOST:PORT", help="check that a running matchmaker answers, then exit")
    parser.add_argument("--ipv6", action="store_true", help="probe over IPv6; HOST may be an address, so use the last colon for the port")
    arguments = parser.parse_args()
    if arguments.probe:
        host, _, port = arguments.probe.rpartition(":") if arguments.ipv6 else arguments.probe.partition(":")
        took = probe(host.strip("[]"), int(port or 47703), family=socket.AF_INET6 if arguments.ipv6 else socket.AF_INET)
        print(f"{arguments.probe}: " + (f"answered in {took:.0f} ms" if took is not None else "no answer"))
        sys.exit(0 if took is not None else 1)
    asyncio.run(serve(arguments.address, arguments.port, arguments.relay_only, arguments.status, arguments.address6))


if __name__ == "__main__":
    main()
