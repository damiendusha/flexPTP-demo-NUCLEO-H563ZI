#!/usr/bin/env bash
#
# One-time, privileged install of the HIL bench's host side, so that afterwards the bench user
# -- or an agent running as that user -- can run the PTP master and DHCP WITHOUT sudo.
#
#   sudo tools/hil/privileged/install.sh              # install for $SUDO_USER on enp5s0
#   sudo HIL_IFACE=eth1 tools/hil/privileged/install.sh
#   sudo tools/hil/privileged/install.sh --uninstall
#
# Then, as the normal user, no sudo:
#
#   systemctl start flexptp-hil-dhcp            # DHCP for the board
#   systemctl start flexptp-ptp4l-default       # grandmaster, default profile (UDP/IPv4, E2E)
#   systemctl start flexptp-ptp4l-gptp          # grandmaster, gPTP (802.1AS)
#   systemctl start flexptp-ptp4l-slave         # ptp4l as SLAVE, to test the board as master
#   systemctl stop  flexptp-ptp4l-gptp
#   journalctl -u flexptp-ptp4l-gptp -f         # works because the user is in group adm
#
# WHAT IT GRANTS, exactly:
#
#   - start / stop / restart of the four units above, for ONE user. Nothing else -- not
#     enable, not edit, not any other unit. See 50-flexptp-hil.rules.
#   - Every unit runs a fixed command line on a fixed interface, from root-owned files in
#     /etc. The repository is read once, here, and never again: a root service that executed a
#     user-writable file would be a privilege escalation, so none does.
#
# WHAT IT CHANGES on the host:
#
#   - a NetworkManager profile "flexptp-hil" that owns the interface with a static address.
#     This is also the fix for the address that kept vanishing: NM's auto-created "Wired
#     connection 1" claimed the interface and ran a DHCP client that never got a lease, and
#     each retry flushed the address setup_host_net.sh had added by hand. The new profile
#     outranks it on this interface only (autoconnect-priority); it is NOT modified.
#   - /etc/linuxptp/flexptp-gm.cfg (where AppArmor lets ptp4l read config)
#   - /etc/systemd/system/flexptp-*.service, /etc/polkit-1/rules.d/50-flexptp-hil.rules
#
# None of the units is enabled at boot: they run when started and not otherwise.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_CFG="$HERE/../../linuxptp/flexptp-gm.cfg"

IFACE="${HIL_IFACE:-enp5s0}"
HOST_ADDR="${HIL_HOST_ADDR:-192.168.2.1/24}"
DHCP_RANGE="${HIL_DHCP_RANGE:-192.168.2.100,192.168.2.200,1h}"
BENCH_USER="${HIL_USER:-${SUDO_USER:-}}"
NM_PROFILE="flexptp-hil"

UNITS=(flexptp-hil-dhcp flexptp-ptp4l-default flexptp-ptp4l-gptp flexptp-ptp4l-slave)
RULE=/etc/polkit-1/rules.d/50-flexptp-hil.rules
GM_CFG=/etc/linuxptp/flexptp-gm.cfg

die() { echo "error: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "must run as root: sudo $0"

if [[ "${1:-}" == "--uninstall" ]]; then
    for u in "${UNITS[@]}"; do
        systemctl stop "$u" 2>/dev/null || true
        rm -f "/etc/systemd/system/$u.service"
    done
    systemctl daemon-reload
    rm -f "$RULE"
    nmcli -t -f NAME con show | grep -qx "$NM_PROFILE" && nmcli con delete "$NM_PROFILE" >/dev/null
    echo "uninstalled (left $GM_CFG in place; remove it by hand if wanted)"
    exit 0
fi

[[ -n "$BENCH_USER" && "$BENCH_USER" != root ]] || die "run via sudo from the bench user, or set HIL_USER"
id "$BENCH_USER" >/dev/null 2>&1 || die "no such user: $BENCH_USER"
ip link show "$IFACE" >/dev/null 2>&1 || die "no such interface: $IFACE (set HIL_IFACE)"
if ip route show default | grep -q "dev $IFACE "; then
    die "$IFACE carries the default route; refusing. Set HIL_IFACE to the board-facing NIC."
fi
for b in /usr/sbin/ptp4l /usr/sbin/dnsmasq nmcli; do
    command -v "$b" >/dev/null || die "missing $b (apt install linuxptp dnsmasq network-manager)"
done
[[ -f "$REPO_CFG" ]] || die "missing $REPO_CFG"

echo "== installing for user '$BENCH_USER' on $IFACE =="

# --- 1. NetworkManager owns the address, so nothing flushes it ------------------------------
if nmcli -t -f NAME con show | grep -qx "$NM_PROFILE"; then
    nmcli con mod "$NM_PROFILE" connection.interface-name "$IFACE" ipv4.addresses "$HOST_ADDR"
else
    nmcli con add type ethernet ifname "$IFACE" con-name "$NM_PROFILE" \
        ipv4.method manual ipv4.addresses "$HOST_ADDR" ipv4.never-default yes \
        ipv6.method disabled \
        connection.autoconnect yes connection.autoconnect-priority 100 >/dev/null
fi
nmcli con up "$NM_PROFILE" >/dev/null
echo "   NetworkManager profile '$NM_PROFILE': $IFACE = $HOST_ADDR"

# --- 2. The gPTP config, where AppArmor lets ptp4l read it ---------------------------------
install -D -m644 -o root -g root "$REPO_CFG" "$GM_CFG"
echo "   $GM_CFG"

# --- 3. The units: fixed arguments, root-owned, not enabled at boot -----------------------
subst() {
    sed -e "s|@IFACE@|$IFACE|g" -e "s|@HOST_ADDR@|$HOST_ADDR|g" \
        -e "s|@DHCP_RANGE@|$DHCP_RANGE|g" -e "s|@USER@|$BENCH_USER|g" "$1"
}
for u in "${UNITS[@]}"; do
    subst "$HERE/$u.service.in" > "/etc/systemd/system/$u.service"
    chmod 644 "/etc/systemd/system/$u.service"
done
systemctl daemon-reload
echo "   ${UNITS[*]}"

# --- 4. polkit: this user may start/stop/restart exactly those units ---------------------
install -d -m755 /etc/polkit-1/rules.d
subst "$HERE/50-flexptp-hil.rules.in" > "$RULE"
chmod 644 "$RULE"
echo "   $RULE"

cat <<EOF

Done. As $BENCH_USER, without sudo:

  systemctl start flexptp-hil-dhcp
  systemctl start flexptp-ptp4l-default     # or -gptp, or -slave
  journalctl -u flexptp-ptp4l-default -f

Undo with: sudo $0 --uninstall
EOF
