# Running a PTP Master on the Host

How to set up a Linux host as a PTP grandmaster for this demo, using `linuxptp`.

> **Target status:** the PTP timestamp clock runs (F53 resolved — `PLL1QEN` had never been
> enabled, so `pll1_q_ck` was absent). The board should now respond to Sync messages.
> End-to-end lock has not yet been demonstrated against a grandmaster.

## What the target expects

The application loads the `gPTP` profile at startup (`FLEXPTP_INITIAL_PROFILE` in
`Src/main.c`), so the host **must** match these:

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

## Prerequisites

```bash
sudo apt install linuxptp        # provides ptp4l, pmc, phc2sys
ip -br addr                      # find the interface facing the board
ethtool -T <iface> | grep -i "PTP Hardware Clock"
```

If that last command reports `PTP Hardware Clock: none`, your NIC has no hardware
timestamping and you must add `-S` (software timestamping) below.

## Command

> **The config must live in `/etc/linuxptp/`.** Ubuntu ships an AppArmor profile at
> `/etc/apparmor.d/usr.sbin.ptp4l` that confines ptp4l's config reads to
> `@{etc_ro}/linuxptp/** r`. Pointing `-f` at the shipped copy under
> `/usr/share/doc/linuxptp/configs/` fails with **"Permission denied" even as root**, which
> looks like a file-permission problem but is not — the file is world-readable and every
> directory on the path is traversable.

This repository carries a ready-made config with the overrides already folded in:

```bash
sudo install -D -m644 tools/linuxptp/flexptp-gm.cfg /etc/linuxptp/flexptp-gm.cfg
sudo ptp4l -f /etc/linuxptp/flexptp-gm.cfg -i enp7s0 -S -m
```

Replace `enp7s0` with your interface, and drop `-S` if your NIC has a PTP hardware clock.

If you would rather use linuxptp's stock file, copy it into the allowed directory and
supply the overrides on the command line:

```bash
sudo install -D -m644 /usr/share/doc/linuxptp/configs/gPTP.cfg /etc/linuxptp/gPTP.cfg
sudo ptp4l -f /etc/linuxptp/gPTP.cfg -i enp7s0 -S -m --serverOnly=1 --priority1=100
```

The flags that are not in the stock config file:

- **`-S`** — software timestamping. Required only if your NIC has no PHC. Drop it if it
  does, for far better accuracy.
- **`-m`** — log to stdout so you can watch it.
- **`--serverOnly=1`** — never become a slave. This is the linuxptp **4.x** spelling; on
  3.x and earlier it is `--masterOnly=1`.
- **`--priority1=100`** — **not optional.** The shipped `gPTP.cfg` sets `priority1 248`,
  which is *worse* than the board's 128, so without this BMCA elects the board as
  grandmaster and neither side ever syncs.

## Verifying

On the host, in a second terminal:

```bash
sudo pmc -u -b 0 -f /etc/linuxptp/flexptp-gm.cfg 'GET PORT_DATA_SET'
sudo pmc -u -b 0 -f /etc/linuxptp/flexptp-gm.cfg 'GET TIME_STATUS_NP'
```

On the target, over the ST-Link VCP (`/dev/ttyACM0`, 115200 8N1):

```
flexptp        # start the stack if it is not already running
ptp log        # watch the offset converge
ptp info       # show the current profile and master
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

Symptoms are usually silence rather than errors, because L2 PTP has no handshake.

| Check | Command |
|---|---|
| Are frames leaving the host? | `sudo tcpdump -i <iface> -e ether proto 0x88f7` |
| Is the board on the same physical segment? | PTP is L2 — it does not cross routers |
| Did BMCA pick the wrong grandmaster? | `pmc ... 'GET PARENT_DATA_SET'`; lower `--priority1` |
| Right destination MAC? | tcpdump should show `01:80:c2:00:00:0e`, not `01:1b:19:...` |
| Is the target's PTP stack running? | `flexptp` at the CLI; look for *"Turning PTP on!"* |
| `-f` fails with "Permission denied" as root | AppArmor: the config must be under `/etc/linuxptp/` |

Note that firewall rules are irrelevant here: this is raw Ethernet, below IP.
