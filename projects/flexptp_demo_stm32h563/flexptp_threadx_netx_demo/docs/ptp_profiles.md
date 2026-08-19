# PTP Profiles: What to Run, and Why

flexPTP ships three profile presets. They are not interchangeable, and a mismatch between
the board and the master is **silent on both sides** — PTP has no handshake, so a wrong
profile looks exactly like an absent peer. This document covers what each preset actually
sets, the trade-offs behind the two axes that matter (transport and delay mechanism), and
how to configure a Linux host to match.

For the mechanics of running `ptp4l` — installation, DHCP on a direct cable, verification
commands and a symptom table — see [host_ptp_setup.md](host_ptp_setup.md). This document is
about *which* profile to pick.

## The short answer

**Run `default` unless you have a reason not to.** It is the boot preset
([`FLEXPTP_INITIAL_PROFILE`](../main.c)), the one proven on this port, the only one that
survives ordinary switches, and the one a stock `ptp4l` with no config file already matches.

Pick `gPTP` only if the entire path is 802.1AS-capable or a direct cable, and `defp2p` only
on a direct cable or an all-peer-delay fabric.

## The three presets

Defined in [`ptp_profile_presets.c`](../../../../Modules/flexptp/src/flexptp/ptp_profile_presets.c),
fields per `PtpProfile` in
[`ptp_types.h`](../../../../Modules/flexptp/src/flexptp/ptp_types.h):

| | **`default`** | **`gPTP`** | **`defp2p`** |
|---|---|---|---|
| Transport | UDP/IPv4 (L4) | Raw 802.3 (L2) | UDP/IPv4 (L4) |
| Multicast destination | `224.0.1.129`, UDP 319/320 | MAC `01:80:C2:00:00:0E`, ethertype `0x88F7` | `224.0.1.129`, UDP 319/320 |
| `transportSpecific` (majorSdoId) | 0 | 1 | 0 |
| Delay mechanism | **E2E** | **P2P** | **P2P** |
| Sync period | `0` → 1/s | `-3` → 8/s | `0` → 1/s |
| Announce period | `1` → every 2 s | `0` → 1/s | `1` → every 2 s |
| (P)DelayReq period | `0` → 1/s | `0` → 1/s | `0` → 1/s |
| Domain | 0 | 0 | 0 |
| Profile flags | none | `0x1` — Sync only to a compliant P2P peer | none |
| TLV chain | none | `gptp` | none |
| Works through ordinary switches | **yes** | **no** — by design | no |
| Status on this port | **proven** | not exercised end to end | not exercised |

List them on the target with `ptp profile preset`, and print the loaded one with
`ptp profile`.

## Axis 1: transport — L2 or L4

**L4 (UDP/IPv4)** is routable, traverses ordinary switches and routers normally, and is
trivially diagnosable with standard tools:

```bash
sudo tcpdump -i <iface> udp port 319 or udp port 320
```

You pay roughly 28 bytes of UDP/IP header per message, slightly more stack processing on the
timestamp path, and a dependency on the IP stack: the demo acquires its address by DHCP, and
nothing PTP works before the board prints `IP-address: …`.

**L2 (raw 802.3)** is leaner, has no IP dependency and is marginally more deterministic, but
is confined to a single broadcast domain — it cannot cross a router.

The decisive detail is *which* L2 address. Plain IEEE 1588 over L2 uses
`01:1B:19:00:00:00`, which ordinary bridges forward. **802.1AS/gPTP uses
`01:80:C2:00:00:0E`, which is in the IEEE 802.1D reserved range that conformant bridges must
not forward.** gPTP frames therefore die at the first standards-compliant switch. This is
not a bug or a configuration error — it is what the reserved range is for, and it is why
gPTP presumes every hop is a time-aware bridge.

That behaviour was confirmed here by `tcpdump`: the host's `Pdelay_Req` frames leave and
nothing returns, and `ptp4l` never progresses past `Pdelay_Req`, because a gPTP port will
not emit Sync or Announce until the peer-delay exchange proves the link `asCapable`. See
[design_doc.md](design_doc.md).

Note that flexPTP ties transport to the preset, but `ptp transport {ipv4|802.3}` can
override it afterwards — so plain-1588-over-L2 is reachable as `ptp profile preset default`
followed by `ptp transport 802.3`, with `network_transport L2` on the host. That combination
avoids both the reserved-MAC problem and the IGMP-snooping one below, at the cost of
routability. It is untested on this port.

## Axis 2: delay mechanism — E2E or P2P

This is the axis that actually decides whether a switched network works.

**E2E** (`Delay_Req`/`Delay_Resp`) measures the *whole* path between master and slave. Any
latency contributed by intermediate devices is folded into the measured mean path delay.
That is the correct model when the devices in between are not PTP-aware.

**P2P** (peer delay) measures only the link to the *immediate neighbour*, and assumes every
hop is a peer-delay-capable transparent clock that measures its own links and subtracts its
own residence time. Put an ordinary switch in the middle and both assumptions fail: the
switch does not participate, so the measured "link delay" is not a link delay at all, and
its residence time is never corrected for.

So: **E2E for ordinary networks, P2P only when every hop participates.** `defp2p` exists for
the case where you want peer delay without the rest of 802.1AS — a direct cable, or a fabric
of P2P transparent clocks.

## Axis 3: message rates

`gPTP` runs Sync at 8/s against `default`'s 1/s. Faster Sync converges the servo sooner and
tracks oscillator drift more tightly, at the cost of bandwidth and interrupt load. It is not
tied to the profile: adjust with

```
ptp period sync -3
```

The host must agree, or the servo's period estimate is wrong. `ptp period sync matched`
makes the board follow whatever the master advertises, which is the safer option when you
are changing rates.

## Setting up the host

### For `default` (recommended)

Every setting that matters is already `ptp4l`'s default, so there is no config file:

```bash
sudo ptp4l -i <iface> -S -m --serverOnly=1 --priority1=100
```

| Flag | Why |
|---|---|
| `-S` | Software timestamping. Required **only** if the NIC has no PHC — check `ethtool -T <iface>`. Drop it if it has one; accuracy improves by orders of magnitude. |
| `-m` | Log to stdout so you can watch it. |
| `--serverOnly=1` | Never become a slave. This is the linuxptp **4.x** spelling; 3.x uses `--masterOnly=1`. |
| `--priority1=100` | **Not optional.** The board defaults to `priority1` 128, and BMCA picks the numerically *lower* value. Without this the board elects itself grandmaster and neither side ever syncs. |

The board needs nothing beyond `flexptp` to start the stack, since `default` is the boot
preset. Confirm with `ptp profile`.

### For `gPTP`

Needs a config file, and the four settings that fail silently if wrong are transport,
destination MAC, `transportSpecific` and delay mechanism. `linuxptp` ships a matching
profile; copy it to where AppArmor permits `ptp4l` to read it, then override `priority1`:

```bash
sudo install -D -m644 /usr/share/doc/linuxptp/configs/gPTP.cfg /etc/linuxptp/gPTP.cfg
```

```bash
sudo ptp4l -f /etc/linuxptp/gPTP.cfg -i <iface> -S -m --serverOnly=1 --priority1=100
```

`tools/linuxptp/flexptp-gm.cfg` in this repository is the same profile with the overrides
folded in. Full detail, including the AppArmor trap that makes `-f` fail with "Permission
denied" even as root, is in [host_ptp_setup.md](host_ptp_setup.md).

On the target: `ptp profile preset gPTP`.

## What a switched network costs you

Moving from a direct cable to ordinary switches is a bigger change to accuracy than any
profile setting, and it is worth being explicit about why.

**Residence time.** A store-and-forward switch holds each frame for a period that varies
with its queue occupancy. E2E folds the *mean* of that into the path delay, but the
*variance* and any forward/reverse asymmetry land directly in the computed offset. Expect
microsecond-class results that degrade as cross-traffic on the PTP path grows. PTP-aware
switches (transparent or boundary clocks) exist precisely to remove this term.

**IGMP snooping.** PTP's `224.0.1.129` is *not* in the never-pruned `224.0.0.0/24`
link-local range, so a switch doing IGMP snooping with no querier on the segment can
silently prune the group. Either ensure a querier exists on that VLAN or disable snooping
there.

**Switches that discard PTP selectively.** This is not hypothetical here. Finding
[F42](port_review.md) cost days: a site switch forwarded Sync, Follow_Up and Announce to
`224.0.1.129` while **discarding `Delay_Resp` to the same group and the same destination
MAC** — intermittently, which produced repeated false attributions to the firmware before a
direct cable settled it.

| Path | `Delay_Resp` received | Failures |
|---|---|---|
| Through the site switch | 0 | 1 per second, indefinitely |
| Direct cable | 65 (≈1/s) | 0 |

This is a property of an individual switch, not of IPv4 PTP — the protocol is ordinary UDP
multicast and nothing about it is unforwardable, unlike gPTP's reserved MAC. But it is the
only switch this project has data for, and it failed. The lesson is to **verify rather than
assume**: the days F42 cost were spent theorising about firmware downstream of an unchecked
assumption that `Delay_Resp` was arriving at all. With the board running, count them.

```bash
sudo tcpdump -i <iface> -n udp port 319 or udp port 320
```

You want all five of Announce, Sync, Follow_Up, Delay_Req and Delay_Resp. On the target,
`ptp log def on` and watch the `corr_ppb` column — see below.

## Reading the result

With `ptp log def on` the board prints one line per Sync. Two columns matter:

- **`Dt [ns]`** — the offset. It should collapse toward zero.
- **`corr_ppb`** — the frequency correction the servo applies **this cycle**. If it reads
  `0.0000` on every line while the offset drifts, the servo is not running and the clock is
  free-running, however healthy the state machine looks. That was [F59](port_review.md).

`corr_ppb` is an *increment*, not the total correction in force — the addend accumulates it
(`addend += corr_ppb × K` in [`slave.c`](../../../../Modules/flexptp/src/flexptp/slave.c)). So once
the clock is locked `corr_ppb` hovers near zero, and reading it as the oscillator's error
understates that error by orders of magnitude. The total correction currently applied is

```
frequency offset [ppb] = (addend − 3435973836) / 3.436
```

where 3.436 is `PTP_ADDEND_CORR_PER_PPB_F` = 2³² / (5 ns × 250 MHz), i.e. the nominal addend
times 1e-9. On this board that comes to about **−41 ppm** — the ST-Link MCO running fast,
an unremarkable magnitude for an ordinary crystal. Conflating the two quantities is what put
a wrong figure in the docs for months; see [port_review.md](port_review.md).

The equivalent check without logging is `ptp addend`: the nominal free-run value on this port
is **3435973836** (`0xCCCCCCCC`, from [`flexptp_options.h`](../flexptp_options.h)). An
addend still sitting exactly there means the servo has never applied a correction. Likewise
`ptp info` showing the master clock ID equal to the board's own clock ID means it heard no
Announce it recognised and elected itself — the classic profile-mismatch signature.

`PTP_ACCURACY_LIMIT_NS` (100 ns) is the threshold flexPTP uses to declare LOCKED.

## Accuracy expectations

Against a **software-timestamping** master expect microsecond-class results regardless of
profile — enough to prove the link works, not enough to characterise it. flexPTP is capable
of tens of nanoseconds, but only against a hardware-timestamping master.

Two further limits are inherent to this board:

- The MCU is clocked from the ST-Link's noisy 8 MHz MCO. The fitted X3 crystal is the better
  reference but needs both hardware and firmware changes — see the README and finding F55.
- Timestamp granularity is 5 ns, set by the PTP subsecond increment and not improvable on
  this part — see [ptp_clock.md](ptp_clock.md).

## Changing the boot profile

The preset loaded at startup is `FLEXPTP_INITIAL_PROFILE` in [`Src/main.c`](../main.c),
applied from the `PTP_UEV_INIT_DONE` user-event callback:

```c
#define FLEXPTP_INITIAL_PROFILE ("default")
```

**There is no settings store, so runtime changes do not persist.** `ptp profile preset …`
applies immediately but is lost on the next reset; changing the boot preset means editing
that line and reflashing. Note also that loading a profile carries an inherent PTP-subsystem
reset — the servo and state machine restart, though the MCU does not.

Individual axes can be overridden at runtime without switching preset:

```
ptp transport [{ipv4|802.3}]
ptp delmech [{e2e|p2p}]
ptp transpec [{def|gPTP}]
ptp period <delreq|sync|ann> [<lp>|matched]
ptp domain [domain]
ptp priority [<p1> <p2>]
ptp pflags [<flags>]
```

`ptp pflags 2` sets `PTP_PF_SLAVE_ONLY`, which stops the board ever electing itself
grandmaster — useful on a network where a free-running board masquerading as a time source
would be worse than no time at all.
