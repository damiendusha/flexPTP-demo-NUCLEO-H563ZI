# Running a PTP Master on the Host

How to set up a Linux host as a PTP grandmaster for this demo, using `linuxptp`.

> **Target status: locked and disciplining the clock.** Verified against `ptp4l` on a
> directly-connected host with the **`default`** profile: 190 of 191 samples applying a
> correction, offset holding at a mean of 519 ns with an 83 µs peak. The residual is the
> host's software-timestamping floor, not the board — see
> [Accuracy expectations](#accuracy-expectations).

## Two routes

The board ships with the `gPTP` profile loaded at startup (`FLEXPTP_INITIAL_PROFILE` in
[`Src/main.c`](../Src/main.c)), but it will run either profile. Pick one and match both ends:

| | **`default`** — plain IEEE 1588 | **`gPTP`** — 802.1AS |
|---|---|---|
| Transport | UDP/IPv4, multicast 224.0.1.129 | Raw Layer 2, ethertype 0x88F7 |
| Delay mechanism | E2E | P2P |
| Host command | plain `ptp4l`, no config file | `ptp4l -f /etc/linuxptp/gPTP.cfg` |
| Board command | **`ptp profile preset default`** | loaded at boot, nothing to do |
| Status | **proven on this port** | not yet exercised end to end |

The `default` route is the one below in [Command](#command); the gPTP route is
[further down](#alternative-the-gptp-profile). They do not interoperate in any combination —
a mismatch is silent on both sides.

> **A reflash resets the board to `gPTP`.** The profile is not persisted, so
> `ptp profile preset default` must be re-issued after every flash. Symptom if you forget:
> the board reports `LISTENING -> PRE_MASTER -> MASTER` because it never hears an Announce
> it recognises, and the host sees nothing at all.

## Prerequisites

```bash
sudo apt install linuxptp        # provides ptp4l, pmc, phc2sys
ip -br addr                      # find the interface facing the board
ethtool -T <iface> | grep -i "PTP Hardware Clock"
```

If that last command reports `PTP Hardware Clock: none`, your NIC has no hardware
timestamping and you must add `-S` (software timestamping) below.

### On a direct cable

The `default` route runs over UDP, so the board needs an address. A switch is worth avoiding
outright — one silently discarded every Delay_Resp here (F42) — but a direct cable means
serving DHCP yourself. Give the NIC an address and run `dnsmasq` on that interface alone:

```bash
sudo ip addr add 192.168.2.1/24 dev enp5s0 && sudo ip link set enp5s0 up
sudo dnsmasq -d -i enp5s0 --bind-interfaces --dhcp-range=192.168.2.100,192.168.2.200,1h
```

The lease it logs confirms two things at once: the link works, and the MAC is the one
derived from the device UID (`56:00:1e:...` here — see F51). The board prints its address as
`IP-address: …` once DHCP completes; nothing PTP will work before that line appears.

## Command

For the **`default`** profile there is no config file and nothing to override — every
setting that matters is already `ptp4l`'s default:

```bash
sudo ptp4l -i enp5s0 -S -m --serverOnly=1 --priority1=100
```

and on the target, over the ST-Link VCP:

```
flexptp
ptp profile preset default
ptp log def on
```

The board should go `LISTENING -> UNCALIBRATED -> SLAVE` within a couple of seconds and
begin printing one line per Sync:

```
T1 [s] | T1 [ns] | T4 [s] | T4 [ns] | Dt [s] | Dt [ns] | Dt [tick] | addend | corr_ppb | mpd_ns | sync_period_ns
1786314408 145267096 1786314407 144... 0  -10692  -2138  3435821352   658.7748  50702  1000140100
```

Watch two columns. `Dt [ns]` is the offset and should collapse toward zero. **`corr_ppb` is
the one that matters**: if it is `0.0000` on every line while the offset drifts, the servo is
not running — the clock is free-running and PTP is doing nothing, however healthy the state
machine looks. That was [F59](port_review.md#f59--one-duplicated-sync-cycle-latches-the-servo-into-nan-for-ever-resolved).

## Alternative: the gPTP profile

The host **must** match these:

| Parameter | Value | Why it matters |
|---|---|---|
| Transport | **Layer 2** (`PTP_TP_802_3`), ethertype 0x88F7 | Not UDP/IPv4. An L4 master is invisible to the board. |
| Destination MAC | **`01:80:C2:00:00:0E`** | 802.1AS uses this, *not* the `01:1B:19:00:00:00` of default L2 PTP. |
| `transportSpecific` | **`0x1`** | 802.1AS sets the majorSdoId to 1; frames with 0 are discarded. |
| Delay mechanism | **P2P** (peer delay) | The board is `PTP_DM_P2P`. E2E will not interoperate. |
| `logSyncInterval` | `-3` (8 Sync/s) | Matches the board's preset. |
| Board `priority1` | 128 | The host must be **numerically lower** to win BMCA. |

The first three are the ones that cause silent failure: everything looks healthy on the
host, and the board simply never sees a packet.

> **The config must live in `/etc/linuxptp/`.** Ubuntu ships an AppArmor profile at
> `/etc/apparmor.d/usr.sbin.ptp4l` confining ptp4l's config reads to
> `@{etc_ro}/linuxptp/** r`. Pointing `-f` at the shipped copy under
> `/usr/share/doc/linuxptp/configs/` fails with **"Permission denied" even as root** —
> which looks like a file-permission problem but is not: the file is world-readable and
> every directory on the path is traversable.

`linuxptp` ships a gPTP profile that already matches the board on the four settings that
matter most — transport, destination MAC, `transportSpecific` and sync interval. Copy it
into the directory AppArmor allows, then supply the two overrides it lacks:

```bash
sudo install -D -m644 /usr/share/doc/linuxptp/configs/gPTP.cfg /etc/linuxptp/gPTP.cfg
```

```bash
sudo ptp4l -f /etc/linuxptp/gPTP.cfg -i enp7s0 -S -m --serverOnly=1 --priority1=100
```

Replace `enp7s0` with your interface. The four flags, and why each is there:

| Flag | Why |
|---|---|
| `-S` | Software timestamping. Required **only** if your NIC has no PHC — check with `ethtool -T <iface>`. Drop it if it has one; accuracy improves by orders of magnitude. |
| `-m` | Log to stdout so you can watch it. |
| `--serverOnly=1` | Never become a slave. This is the linuxptp **4.x** spelling; 3.x and earlier use `--masterOnly=1`. |
| `--priority1=100` | **Not optional.** The stock `gPTP.cfg` sets `priority1 248`, which is *worse* than the board's 128 — without this, BMCA elects the board as grandmaster and neither side ever syncs. |

Re-copy the file after a `linuxptp` package upgrade if you want to track upstream changes.

<details>
<summary>Alternative: a pre-baked config in this repository</summary>

`tools/linuxptp/flexptp-gm.cfg` is the same stock profile with the two overrides already
folded in, so the command line stays short and the `priority1` override cannot be
forgotten:

```bash
sudo install -D -m644 tools/linuxptp/flexptp-gm.cfg /etc/linuxptp/flexptp-gm.cfg
sudo ptp4l -f /etc/linuxptp/flexptp-gm.cfg -i enp7s0 -S -m
```

Being a copy, it will not pick up upstream changes to `gPTP.cfg`. The stock-file route
above is the one to prefer.

</details>

## Verifying

On the host, in a second terminal:

```bash
sudo pmc -u -b 0 'GET PORT_DATA_SET'
sudo pmc -u -b 0 'GET TIME_STATUS_NP'
sudo pmc -u -b 0 'GET PARENT_DATA_SET'    # who won BMCA
```

For the gPTP route add `-f /etc/linuxptp/gPTP.cfg` to each.

On the target, over the ST-Link VCP (`/dev/ttyACM0`, 115200 8N1):

```
flexptp                          # start the stack if it is not already running
ptp profile preset default       # re-issue after every flash
ptp log def on                   # watch the offset converge
ptp info                         # show the current profile and master
```

A healthy slave shows the offset shrinking toward zero and the state reaching
`SLAVE`/`LOCKED`. `PTP_ACCURACY_LIMIT_NS` (default 100 ns) is the threshold flexPTP uses to
declare LOCKED.

## Accuracy expectations

With **software timestamping** on the host expect microsecond-class results — enough to
prove the link works, not enough to characterise it. flexPTP is capable of tens of
nanoseconds, but only against a hardware-timestamping master.

Two further limits are inherent to this board and unrelated to the host:

- The MCU clock is sourced from the ST-Link's MCO at 8 MHz, which the project README
  describes as noisy. The X3 crystal is the better reference — but note it is **already
  fitted on MB1404 and is 25 MHz**, selected with SB3/SB4 ON and SB49 OFF, and using it
  requires `HSE_VALUE` = 25000000 with a re-derived `PLLM`. The firmware's welcome message
  still repeats the H743 project's "solder an 8 MHz oscillator" advice, which is wrong for
  this board (finding F55).
- Timestamp granularity is 5 ns, set by the PTP subsecond increment and not improvable on
  this part — see [ptp_clock.md](ptp_clock.md).

## If nothing happens

Symptoms are usually silence rather than errors: PTP has no handshake, so a mismatch looks
identical to an absent peer.

| Symptom | Check |
|---|---|
| Board reports `LISTENING -> PRE_MASTER -> MASTER` | **Profile mismatch** — it heard no Announce it recognised, so it elected itself. Almost always a `default`/`gPTP` mix-up, and the usual cause is a reflash reverting the board to `gPTP`. Re-issue `ptp profile preset default`. |
| Nothing at all on the board | Are frames leaving the host? `sudo tcpdump -i <iface> -e ether proto 0x88f7` for gPTP, `sudo tcpdump -i <iface> udp port 319 or udp port 320` for `default` |
| Sync arrives but Delay_Resp never does | **A switch may silently discard PTP Delay_Resp** while forwarding Sync/Announce on the same multicast group. Verified on this setup — it cost days. Test with a direct cable ([F42](port_review.md)). |
| Offset drifts while `corr_ppb` stays `0.0000` | The servo is not running; the clock is free-running ([F59](port_review.md)). `ptp reset` restores it. |
| Board is on a different segment | PTP multicast does not cross routers, and the gPTP profile is below IP entirely |
| Did BMCA pick the wrong grandmaster? | `sudo pmc -u -b 0 'GET PARENT_DATA_SET'`; lower `--priority1` |
| Right destination MAC? (gPTP) | tcpdump should show `01:80:c2:00:00:0e`, not `01:1b:19:...` |
| Is the target's PTP stack running? | `flexptp` at the CLI; look for *"Turning PTP on!"* |
| `-f` fails with "Permission denied" as root | AppArmor: the config must be under `/etc/linuxptp/` |

Firewall rules are irrelevant for the gPTP route — that is raw Ethernet, below IP. For the
`default` route they are not: it is UDP on ports 319 and 320.
