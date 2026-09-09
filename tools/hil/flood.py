#!/usr/bin/env python3
"""Saturate the board's receive path, from the host, without root.

WHAT THIS IS FOR

flexPTP's RTOS bindings used to enqueue received and transmitted messages with an unbounded
wait. On the lwIP firmware that is a deadlock, not a stall: lwIP calls `ptp_receive_cb()` from
`tcpip_thread` with `LOCK_TCPIP_CORE` held, so blocking there puts the thread that owns the
core lock to sleep while it still holds it, and the wait is circular --

    tcpip_thread holds the core lock and blocks on sRxPacketFIFO being full;
    sRxPacketFIFO is drained only by task_ptp();
    task_ptp(), to send anything, calls ptp_transmit() -> LOCK_TCPIP_CORE.

Nothing times out. ARP, ICMP and every UDP sender in the application stop for ever, and only a
reset recovers it. Upstream flexPTP fixed this in `e873105` by making every enqueue
non-blocking; this floods the link hard enough and long enough to show that the fix holds.

The queues have to FILL before any of it matters, which is why the default is 120 s: a 30 s
flood survives even the broken firmware, so a short run proves nothing (see `e873105`'s commit
message -- 120 s reproduced the hang 3 of 3).

WHAT IT SENDS

Three streams, because they land in different places:

  ptp-event    224.0.1.129:319   the timestamped path -- straight into flexPTP's rx FIFO
  ptp-general  224.0.1.129:320   the same FIFO by the other port
  unicast      <board>:<port>    the stack's own receive path, past flexPTP

The payloads are well-formed enough in their first byte to be classified as PTP messages and
nothing more; the point is queue pressure, not protocol confusion.

SAFETY

Multicast is pinned to the board-facing interface with IP_MULTICAST_IF and TTL 1, and the
sockets are bound to that interface's address, so this cannot leak onto another network. The
script refuses to run on an interface that carries the default route -- the same rule
setup_host_net.sh applies.

    tools/hil/flood.py --board 192.168.2.191 --seconds 120
    tools/hil/flood.py --board 192.168.2.191 --seconds 30 --pps 20000
"""

from __future__ import annotations

import argparse
import ipaddress
import socket
import struct
import subprocess
import sys
import threading
import time

PTP_PRIMARY_GROUP = "224.0.1.129"
PTP_PEER_DELAY_GROUP = "224.0.0.107"
PTP_PORT_EVENT = 319
PTP_PORT_GENERAL = 320


def host_addr_of(iface: str) -> str:
    """The interface's IPv4 address, or raise."""
    out = subprocess.run(
        ["ip", "-4", "-o", "addr", "show", "dev", iface],
        capture_output=True, text=True, check=True,
    ).stdout
    for field in out.split():
        if "/" in field:
            try:
                return str(ipaddress.ip_interface(field).ip)
            except ValueError:
                continue
    raise SystemExit(f"error: {iface} has no IPv4 address (run tools/hil/setup_host_net.sh)")


def carries_default_route(iface: str) -> bool:
    out = subprocess.run(["ip", "route", "show", "default"],
                         capture_output=True, text=True, check=True).stdout
    return any(f"dev {iface} " in f"{line} " for line in out.splitlines())


def make_socket(host_ip: str, multicast: bool) -> socket.socket:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
    s.bind((host_ip, 0))
    if multicast:
        # Pin the egress interface explicitly. Without this the kernel picks by route, which
        # on a bench with a real LAN present is the wrong one.
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                     socket.inet_aton(host_ip))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, struct.pack("b", 1))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, struct.pack("b", 0))
    return s


class Stream(threading.Thread):
    """One sender. Counts what it managed to put on the wire."""

    def __init__(self, name: str, host_ip: str, dest: tuple[str, int],
                 payload: bytes, deadline: float, pps: int | None):
        super().__init__(daemon=True, name=name)
        self.stream_name = name
        self.dest = dest
        self.payload = payload
        self.deadline = deadline
        self.interval = (1.0 / pps) if pps else 0.0
        self.sock = make_socket(host_ip, multicast=ipaddress.ip_address(dest[0]).is_multicast)
        self.sent = 0
        self.errors = 0

    def run(self) -> None:
        next_send = time.monotonic()
        while time.monotonic() < self.deadline:
            try:
                self.sock.sendto(self.payload, self.dest)
                self.sent += 1
            except OSError:
                # ENOBUFS on a saturated qdisc is the expected way to be told "too fast".
                self.errors += 1
                time.sleep(0.001)
            if self.interval:
                next_send += self.interval
                delay = next_send - time.monotonic()
                if delay > 0:
                    time.sleep(delay)
        self.sock.close()


def ptp_payload(message_type: int, size: int) -> bytes:
    """A frame whose first byte classifies as a PTP messageType, padded to `size`.

    Deliberately not a valid PTP message beyond that: the firmware should drop it on the
    domain and length checks, and what is being measured is the pressure it applies to the
    receive queue on the way to being dropped.
    """
    head = bytes([message_type & 0x0F, 0x02, 0x00, size & 0xFF])
    return head + b"\xA5" * max(0, size - len(head))


def flood(iface: str, board_ip: str | None, seconds: float,
          pps: int | None, size: int, quiet: bool = False) -> dict:
    if carries_default_route(iface):
        raise SystemExit(f"error: refusing to flood {iface} -- it carries the default route")

    host_ip = host_addr_of(iface)
    deadline = time.monotonic() + seconds

    # Per-stream rate cap, so --pps means the same thing regardless of stream count.
    streams: list[Stream] = []
    specs = [
        ("ptp-event", (PTP_PRIMARY_GROUP, PTP_PORT_EVENT), ptp_payload(0x00, size)),
        ("ptp-general", (PTP_PRIMARY_GROUP, PTP_PORT_GENERAL), ptp_payload(0x08, size)),
        ("pdelay-event", (PTP_PEER_DELAY_GROUP, PTP_PORT_EVENT), ptp_payload(0x02, size)),
    ]
    if board_ip:
        specs.append(("unicast", (board_ip, 5000), b"\xA5" * size))

    per_stream_pps = (pps // len(specs)) if pps else None
    for name, dest, payload in specs:
        streams.append(Stream(name, host_ip, dest, payload, deadline, per_stream_pps))

    started = time.monotonic()
    for s in streams:
        s.start()
    for s in streams:
        s.join()
    elapsed = time.monotonic() - started

    total = sum(s.sent for s in streams)
    result = {
        "seconds": round(elapsed, 2),
        "packets": total,
        "pps": round(total / elapsed) if elapsed else 0,
        "bytes": total * size,
        "mbit_s": round(total * size * 8 / elapsed / 1e6, 1) if elapsed else 0.0,
        "errors": sum(s.errors for s in streams),
        "streams": {s.stream_name: s.sent for s in streams},
    }
    if not quiet:
        print(f"flooded {iface} for {result['seconds']} s: {result['packets']} packets, "
              f"{result['pps']} pps, {result['mbit_s']} Mbit/s, {result['errors']} send errors")
        for name, sent in result["streams"].items():
            print(f"  {name:<14} {sent}")
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iface", default="enp5s0", help="board-facing interface")
    ap.add_argument("--board", help="board IPv4 address, for the unicast stream")
    ap.add_argument("--seconds", type=float, default=120.0,
                    help="flood duration (default 120 -- see the module docstring)")
    ap.add_argument("--pps", type=int, default=None,
                    help="total packets per second across all streams (default: unlimited)")
    ap.add_argument("--size", type=int, default=128, help="payload bytes per packet")
    args = ap.parse_args()

    flood(args.iface, args.board, args.seconds, args.pps, args.size)
    return 0


if __name__ == "__main__":
    sys.exit(main())
