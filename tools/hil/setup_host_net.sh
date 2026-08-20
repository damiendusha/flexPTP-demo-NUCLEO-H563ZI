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
#   sudo tools/hil/setup_host_net.sh                          # set up
#   sudo tools/hil/setup_host_net.sh --stop                   # tear down
#   sudo tools/hil/setup_host_net.sh --with-ptp4l             # set up + PTP master, 'default' profile
#   sudo tools/hil/setup_host_net.sh --gptp                   # set up + PTP master, 'gPTP' profile
#   sudo tools/hil/setup_host_net.sh --with-ptp4l --software  # force software timestamping
#
# Anything after -- is passed to ptp4l verbatim:
#
#   sudo tools/hil/setup_host_net.sh --gptp -- --neighborPropDelayThresh 100000
#
# ptp4l runs in the foreground; Ctrl-C stops it and leaves the DHCP setup in place.
#
# See projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/host_ptp_setup.md for
# the full picture, and docs/ptp_profiles.md alongside it for the profile trade-offs.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IFACE="${HIL_IFACE:-enp5s0}"
HOST_ADDR="${HIL_HOST_ADDR:-192.168.2.1/24}"
DHCP_RANGE="${HIL_DHCP_RANGE:-192.168.2.100,192.168.2.200,1h}"
PIDFILE="/run/hil-dnsmasq-${IFACE}.pid"

# Repo copy of the gPTP grandmaster config, and where AppArmor permits ptp4l to read it from.
GM_CFG_SRC="$HERE/../linuxptp/flexptp-gm.cfg"
GM_CFG="/etc/linuxptp/flexptp-gm.cfg"

die() { echo "error: $*" >&2; exit 1; }

usage() {
    sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
    exit "${1:-0}"
}

WITH_PTP4L=0
PROFILE=default
PTP4L_EXTRA=()
TS_MODE=auto        # auto | hw | sw
DO_STOP=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --stop)         DO_STOP=1 ;;
        --with-ptp4l)   WITH_PTP4L=1 ;;
        # --gptp is meaningless without a master to run, so it implies --with-ptp4l.
        --gptp)         WITH_PTP4L=1; PROFILE=gptp ;;
        --default)      PROFILE=default ;;
        --software|-S)  TS_MODE=sw ;;
        --hardware|-H)  TS_MODE=hw ;;
        --iface)        shift; [[ $# -gt 0 ]] || die "--iface needs an argument"; IFACE="$1" ;;
        -h|--help)      usage 0 ;;
        # Everything after -- goes to ptp4l verbatim, e.g. -- --neighborPropDelayThresh 100000
        --)             shift; PTP4L_EXTRA=("$@"); break ;;
        *)              echo "error: unknown argument: $1" >&2; usage 1 ;;
    esac
    shift
done

# PIDFILE follows --iface.
PIDFILE="/run/hil-dnsmasq-${IFACE}.pid"

[[ $EUID -eq 0 ]] || die "must run as root (use sudo)"

stop() {
    if [[ -f "$PIDFILE" ]]; then
        kill "$(cat "$PIDFILE")" 2>/dev/null || true
        rm -f "$PIDFILE"
        echo "stopped dnsmasq on $IFACE"
    fi
    ip addr del "$HOST_ADDR" dev "$IFACE" 2>/dev/null && echo "removed $HOST_ADDR from $IFACE" || true
}

if [[ $DO_STOP -eq 1 ]]; then
    stop
    exit 0
fi

# Hardware or software timestamping?
#
# ethtool's -T output changed shape in the 6.x series: a NIC that HAS a PHC now reports
# "Hardware timestamp provider index: 0" and no "PTP Hardware Clock:" line at all, while one
# without still reports "PTP Hardware Clock: none". Grepping for that string therefore finds
# nothing in both cases and silently mis-detects a capable NIC. The capability list is the
# part that is stable across both output formats.
timestamp_flag() {
    case "$TS_MODE" in
        hw) echo "-H"; return ;;
        sw) echo "-S"; return ;;
    esac
    if ethtool -T "$IFACE" 2>/dev/null | grep -qw hardware-transmit; then
        echo "-H"
    else
        echo "-S"
    fi
}

ip link show "$IFACE" >/dev/null 2>&1 || die "no such interface: $IFACE (set HIL_IFACE)"
command -v dnsmasq >/dev/null || die "dnsmasq not installed (apt install dnsmasq)"
if [[ $WITH_PTP4L -eq 1 ]]; then
    command -v ptp4l >/dev/null || die "ptp4l not installed (apt install linuxptp)"
    # Without ethtool the auto-detection below cannot tell a NIC with a PHC from one without,
    # and would quietly settle on -S. Demand it, or make the choice explicit.
    [[ "$TS_MODE" != "auto" ]] || command -v ethtool >/dev/null || \
        die "ethtool not installed and needed to detect a PHC (apt install ethtool, or pass --hardware/--software)"
fi

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

[[ $WITH_PTP4L -eq 1 ]] || exit 0

TS_FLAG="$(timestamp_flag)"
if [[ "$TS_FLAG" == "-H" ]]; then
    echo
    echo "== $IFACE has a PTP hardware clock; using hardware timestamping (-H) =="
    # Informational: which /dev/ptpN ptp4l will discipline. sysfs names it in one place
    # regardless of which of the two ethtool output formats is in play.
    phc="$(basename "$(ls -d /sys/class/net/"$IFACE"/device/ptp/ptp* 2>/dev/null | head -1)" 2>/dev/null || true)"
    [[ -n "$phc" ]] && echo "   PHC: /dev/$phc"
    # The PHC free-runs here, which is what the board is measured against. Steering it to
    # system time as well is optional and needs a second daemon:
    #     sudo phc2sys -s CLOCK_REALTIME -c $IFACE -w -m
else
    echo
    if [[ "$TS_MODE" == "sw" ]]; then
        echo "== software timestamping (-S), forced =="
    else
        echo "== $IFACE has no PTP hardware clock; falling back to software timestamping (-S) =="
    fi
    echo "   The offset floor will be tens of microseconds -- that is the host, not the board."
fi

if [[ "$PROFILE" == "gptp" ]]; then
    # AppArmor (/etc/apparmor.d/usr.sbin.ptp4l) confines ptp4l's config reads to
    # "@{etc_ro}/linuxptp/** r", so the repo copy cannot be passed to -f directly: it fails
    # with "Permission denied" even as root. Install it where the profile allows.
    [[ -f "$GM_CFG_SRC" ]] || die "missing $GM_CFG_SRC"
    if ! cmp -s "$GM_CFG_SRC" "$GM_CFG"; then
        echo "== installing $GM_CFG (AppArmor confines -f to /etc/linuxptp/) =="
        install -D -m644 "$GM_CFG_SRC" "$GM_CFG"
    fi

    if [[ "$TS_FLAG" == "-S" ]]; then
        echo
        echo "WARNING: gPTP with software timestamping cannot reach SLAVE. Software timestamps"
        echo "         put the measured peer delay around 8-9 us, ten times over the 800 ns"
        echo "         neighborPropDelayThresh, so the port never becomes asCapable and ptp4l"
        echo "         sends no Announce and no Sync at all. Peer-delay traffic still flows,"
        echo "         which is what makes it confusing. To probe anyway, append:"
        echo "             --neighborPropDelayThresh 100000"
    fi

    echo
    echo "== starting ptp4l on $IFACE in the gPTP profile (Ctrl-C to stop) =="
    echo "   The board does NOT boot into gPTP. At the ST-Link VCP, issue:"
    echo "       ptp profile preset gPTP"
    echo "   It is not persisted -- re-issue it after every reflash."
    echo "   If the log stays on Pdelay_Req with no Announce, the port is not asCapable:"
    echo "   check the measured delay against neighborPropDelayThresh (800 ns) in $GM_CFG."
    # serverOnly, priority1 and the 802.1AS transport settings all come from the config file.
    exec ptp4l -f "$GM_CFG" -i "$IFACE" "$TS_FLAG" -m ${PTP4L_EXTRA[@]+"${PTP4L_EXTRA[@]}"}
else
    echo
    echo "== starting ptp4l on $IFACE in the default profile (Ctrl-C to stop) =="
    echo "   The board boots into 'default', so there is nothing to issue on the target."
    exec ptp4l -i "$IFACE" "$TS_FLAG" -m --serverOnly=1 --priority1=100 ${PTP4L_EXTRA[@]+"${PTP4L_EXTRA[@]}"}
fi
