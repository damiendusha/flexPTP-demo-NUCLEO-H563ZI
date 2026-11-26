"""Malformed and foreign PTP traffic at a board slaved to the host.

The default profile (UDP/IPv4, E2E, domain 0) with the host's ptp4l as master. Each traffic class
is sent to 224.0.1.129 for WINDOW seconds at RATE packets/s while the board logs its Sync cycles,
and is judged against a quiet reference window. A slave must ignore every class here:

    trunc1     1-byte datagram to 319, first byte = Sync         shorter than a PTP header
    trunc33    foreign two-step Sync cut to 33 bytes              shorter than a PTP header
    badver     foreign two-step Sync with versionPTP 1            not a PTPv2 message
    badtype    messageType 0x5 (reserved)                         not a message at all
    garbage    1-200 random bytes to 319 and 320 alternately
    oversize   foreign two-step Sync padded to 1400 bytes         well-formed, not from the master
    domain1    foreign two-step Sync in domain 1                  not this domain
    foreign    well-formed foreign two-step Sync, domain 0        not from the selected master
    announce   foreign Announce, priority1 255 / class 248        worse than the host: BMCA keeps it

flexPTP RC1.0-26 ignores only badtype, garbage, domain1 and announce. The first four classes stop
synchronisation outright (0 of 30 cycles) because nothing compares the received length or
versionPTP with the header (finding U12), and the foreign Syncs because the slave takes Sync and
Follow_Up from any sender (U13). //patches/flexptp carries both fixes; this is their regression
test, and with them every class must leave the Sync rate alone.

The run is also a survival test, asserted for every class: the board keeps the host as master,
keeps its stack up, never resets, and resynchronises afterwards.

Multicast loopback is off, so the host's own ptp4l never sees this traffic.
"""

from __future__ import annotations

import random
import socket
import struct
import time

GROUP, EVENT, GENERAL = "224.0.1.129", 319, 320
FOREIGN = bytes.fromhex("020000fffe000001")   # locally administered, not on the link
WINDOW, RATE = 30.0, 10


def hdr(mtype: int, length: int, seq: int, ident: bytes = FOREIGN, domain: int = 0,
        two_step: bool = True, version: int = 2, control: int = 0, logint: int = 0) -> bytes:
    """A 34-byte PTPv2 common header (IEEE 1588-2019 Table 35)."""
    flags = 0x0200 if two_step else 0
    return struct.pack("!BBHBBHq4s8sHHBb", mtype & 0x0F, version, length, domain, 0, flags, 0,
                       b"\0" * 4, ident, 1, seq & 0xFFFF, control, logint)


def ts(sec: int = 0, ns: int = 0) -> bytes:
    """A 10-byte PTP Timestamp: 48-bit seconds, 32-bit nanoseconds."""
    return struct.pack("!HII", (sec >> 32) & 0xFFFF, sec & 0xFFFFFFFF, ns)


def sync(seq: int, **kw) -> bytes:
    return hdr(0x0, 44, seq, **kw) + ts()


def announce_worse(seq: int) -> bytes:
    body = ts() + struct.pack("!hBBBBHB8sHB", 37, 0, 255, 248, 0xFE, 0xFFFF, 255, FOREIGN, 0, 0xA0)
    return hdr(0xB, 64, seq, two_step=False, control=5, logint=1) + body


_rnd = random.Random(1588)

# name -> (port or None for alternating, packet(seq), description)
CLASSES = {
    "trunc1":   (EVENT, lambda s: b"\x00", "1-byte datagram, first byte = Sync"),
    "trunc33":  (EVENT, lambda s: sync(s)[:33], "foreign two-step Sync cut to 33 B (header is 34)"),
    "badver":   (EVENT, lambda s: sync(s, version=1), "foreign two-step Sync, versionPTP 1"),
    "badtype":  (EVENT, lambda s: hdr(0x5, 44, s) + ts(), "messageType 0x5 (reserved)"),
    "garbage":  (None, lambda s: bytes(_rnd.getrandbits(8) for _ in range(_rnd.randint(1, 200))),
                 "random 1-200 B to 319 and 320 alternately"),
    "oversize": (EVENT, lambda s: sync(s) + bytes(1356), "foreign two-step Sync padded to 1400 B"),
    "domain1":  (EVENT, lambda s: sync(s, domain=1), "foreign two-step Sync, domain 1"),
    "foreign":  (EVENT, lambda s: sync(s), "well-formed foreign two-step Sync, domain 0"),
    "announce": (GENERAL, announce_worse, "well-formed foreign Announce, priority1 255 / class 248"),
}


def sender(host_ip: str) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(host_ip))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 0)
    return sock


def window(board, sock, sync_lines, name: "str | None" = None, seconds: float = WINDOW,
           rate: int = RATE) -> "list[str]":
    """Log Sync cycles for `seconds`, sending class `name` (or nothing) at `rate` meanwhile."""
    port, make, _ = CLASSES[name] if name else (None, None, None)
    board.cmd("ptp log def on")
    t_end, seq, buf, next_tx = time.time() + seconds, 0, "", time.time()
    while time.time() < t_end:
        if make and time.time() >= next_tx:
            p = port if port else (EVENT if seq % 2 == 0 else GENERAL)
            sock.sendto(make(seq), (GROUP, p))
            seq += 1
            next_tx += 1.0 / rate
        buf += board.read_for(0.05)
    board.cmd("ptp log def off")
    return sync_lines(buf)
