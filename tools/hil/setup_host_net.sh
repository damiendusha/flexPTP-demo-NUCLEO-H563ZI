#!/usr/bin/env bash
# Host-side network setup for hardware-in-the-loop testing over a DIRECT CABLE.
#
# The board runs a DHCP client, so on a direct cable the host has to serve DHCP itself.
# This script gives the board-facing NIC a static address and starts dnsmasq bound to that
# one interface -- it cannot answer DHCP on your main LAN.
#
# Everything here needs root. It is a separate script, rather than something the Python
# harness shells out to, precisely so that the privileged part is auditable in one place
# and run deliberately.
#
#   sudo tools/hil/setup_host_net.sh              # set up
#   sudo tools/hil/setup_host_net.sh --stop       # tear down
#   sudo tools/hil/setup_host_net.sh --with-ptp4l # set up and run a PTP master (foreground)
#
# See docs/host_ptp_setup.md for the full picture, including the gPTP alternative.

set -euo pipefail

IFACE="${HIL_IFACE:-enp5s0}"
HOST_ADDR="${HIL_HOST_ADDR:-192.168.2.1/24}"
DHCP_RANGE="${HIL_DHCP_RANGE:-192.168.2.100,192.168.2.200,1h}"
PIDFILE="/run/hil-dnsmasq-${IFACE}.pid"

die() { echo "error: $*" >&2; exit 1; }
[[ $EUID -eq 0 ]] || die "must run as root (use sudo)"

stop() {
    if [[ -f "$PIDFILE" ]]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null || true
        rm -f "$PIDFILE"
        echo "stopped dnsmasq on $IFACE"
    fi
    ip addr del "$HOST_ADDR" dev "$IFACE" 2>/dev/null && echo "removed $HOST_ADDR from $IFACE" || true
}

if [[ "${1:-}" == "--stop" ]]; then
    stop
    exit 0
fi

ip link show "$IFACE" >/dev/null 2>&1 || die "no such interface: $IFACE (set HIL_IFACE)"
command -v dnsmasq >/dev/null || die "dnsmasq not installed (apt install dnsmasq)"

# Refuse to touch an interface that carries a default route -- that is the main LAN, and
# serving DHCP onto it would be antisocial at best.
if ip route show default | grep -q "dev $IFACE"; then
    die "$IFACE carries the default route; refusing. Set HIL_IFACE to the board-facing NIC."
fi

echo "== configuring $IFACE =="
ip addr replace "$HOST_ADDR" dev "$IFACE"
ip link set "$IFACE" up

if ! ip -4 addr show "$IFACE" | grep -q "${HOST_ADDR%/*}"; then
    die "failed to set $HOST_ADDR on $IFACE"
fi
echo "   $IFACE has ${HOST_ADDR}"

echo "== starting dnsmasq (DHCP only, bound to $IFACE) =="
stop_existing=$(pgrep -f "dnsmasq.*$IFACE" || true)
[[ -n "$stop_existing" ]] && { echo "   killing existing dnsmasq: $stop_existing"; kill $stop_existing || true; sleep 0.5; }

dnsmasq \
    --interface="$IFACE" \
    --bind-interfaces \
    --except-interface=lo \
    --no-resolv \
    --port=0 \
    --dhcp-range="$DHCP_RANGE" \
    --pid-file="$PIDFILE" \
    --log-dhcp

sleep 0.5
[[ -f "$PIDFILE" ]] || die "dnsmasq did not start"
echo "   dnsmasq pid $(cat "$PIDFILE"), range $DHCP_RANGE"

echo
echo "Ready. The board should print 'IP-address: 192.168.2.x' within ~20 s of a reset."
echo "Run the harness with:  tools/hil/run.py --stage S0 --iface $IFACE"

if [[ "${1:-}" == "--with-ptp4l" ]]; then
    echo
    # -S is not optional on a NIC without a PTP hardware clock; check yours with
    #    ethtool -T <iface> | grep 'PTP Hardware Clock'
    # Software timestamping puts the floor in the tens of microseconds -- that is the host,
    # not the board. See docs/host_ptp_setup.md, "Accuracy expectations".
    echo "== starting ptp4l on $IFACE (Ctrl-C to stop) =="
    exec ptp4l -i "$IFACE" -S -m --serverOnly=1 --priority1=100
fi
