# Hardware-in-the-loop harness

Drives the board's serial CLI from the host so that every stage of a port can be verified on
real hardware without re-typing commands. Built for the ThreadX/NetX Duo port
([`docs/threadx_porting_plan.md`](../../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_porting_plan.md)), but it is not specific
to it — it is a regression suite for this firmware.

```
hil.py               Board: serial session, cmd(), expect(), reset() over SWD
checks.py            the named checks, and which are valid at each porting stage

The first check in every stage is `build_id`: the linker stamps a SHA-1 over each image,
`osinfo` prints it, and the check compares it against the ELF given by `--flash` or `--elf`.
A mismatch fails the run. Without it -- and it was without it for the whole ThreadX port -- the
runner happily tested whatever firmware happened to be on the board and reported a confident
pass for code that was never loaded (finding GI-01). With no ELF given it SKIPs, which the
runner reports as incomplete: no claim was made, so none is checked.
run.py               the runner
setup_host_net.sh    privileged host-side network setup (direct cable)
baseline.json        golden values captured from the pre-port firmware
```

## Quick start

```bash
tools/hil/run.py --list
tools/hil/run.py --stage S0
tools/hil/run.py --checks boot,cli,osinfo
tools/hil/run.py --flash bazel-bin/projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp-demo.elf --stage S0

# ...or check a board you did not just flash, without reflashing it:
tools/hil/run.py --elf bazel-bin/projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp-demo.elf --stage S0
```

**Say which firmware is on the board.** The monorepo has two, and several expectations differ
between them — the CLI command count, and the interrupt priority table, whose SysTick and SVCall
rows differ because the two kernels *require* different things (see `EXPECTED_IRQ_PRIORITIES_*`
in `checks.py`). `--firmware` selects all of them together:

```bash
tools/hil/run.py --firmware freertos --stage S0
```

It defaults to `threadx`. Running the FreeRTOS image without it does not produce a partial
result — it produces a **failing** one, which is the intended behaviour: a suite that quietly
passes against the wrong table is worth less than no suite.

Exit status: `0` all passed · `1` something failed or regressed · `2` could not reach the
board · `3` passed but incomplete (something skipped).

**A `SKIP` is not a pass.** "We could not test it" and "it works" are different claims, and
conflating them is how F42 survived for days. The runner reports incomplete and returns 3.

## Requirements

| | |
|---|---|
| `pyserial` | `pip install pyserial` |
| ST's OpenOCD | `./build.sh --with-openocd` — builds it into `toolchains/openocd`. Upstream 0.12.0 **cannot** program an STM32H5 |
| udev rule for non-root ST-Link | `sudo cp toolchains/openocd-src/contrib/60-openocd.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger` |

The network checks additionally need the host set up as described below.

## Network checks need the host set up once

`boot`, `cli`, `osinfo`, `cpp_ctor`, `threads` and `ptp_start` need nothing but the ST-Link.
`dhcp`, `ping`, `ptp_ts`, `ptp_sync` and `ptp_flood` need the board to have an address and the host
to run a PTP master -- both root-only.

### Recommended: install it once, then no sudo

```bash
sudo tools/hil/privileged/install.sh
```

After that, as the normal user -- including an agent running as that user:

```bash
systemctl start flexptp-hil-dhcp            # DHCP for the board, on enp5s0 only
systemctl start flexptp-ptp4l-default       # grandmaster, default profile (UDP/IPv4, E2E)
systemctl start flexptp-ptp4l-gptp          # grandmaster, gPTP (802.1AS)
systemctl start flexptp-ptp4l-slave         # ptp4l as SLAVE, to test the board as master
journalctl -u flexptp-ptp4l-gptp -f         # readable because the user is in group adm
```

The three ptp4l units conflict with each other, so starting one stops the others. None of them
starts at boot.

What the install grants and changes:

- **A polkit rule** lets ONE user `start`, `stop` and `restart` exactly those four units: no
  `enable`, no other unit, no other user.
- **The units** each run a fixed command on a fixed interface, from root-owned files in `/etc`. The
  repository is read once, at install, and never again: a root service that ran a user-writable
  file would be a privilege escalation.
- **A NetworkManager profile `flexptp-hil`** pins `192.168.2.1/24` on the interface. That is also
  the fix for the address that kept vanishing. NetworkManager's auto-created "Wired connection 1"
  claimed the interface, ran a DHCP client that never got a lease, and flushed the hand-added
  address on every retry. The new profile outranks it on this interface only; it is not modified.

Undo with `sudo tools/hil/privileged/install.sh --uninstall`. Re-run the install after editing
anything under `tools/hil/privileged/`, since the installed copies are what run.

**The PHC is reset between roles.** `flexptp-ptp4l-slave` steers `/dev/ptp0` to the board and
steps it onto the board's timescale (1970-ish), so on stop it hands the PHC back at system time and
0 ppb. The master units reset only the PHC's *rate* on start, never its *time*. Copying system time
in on every start stepped the master by the PHC's accumulated drift: 17.2 ms on one run, which was
just under flexPTP's 20 ms coarse threshold, so a slaved board slewed it for minutes. That is a
disturbance no test of the board's return to SLAVE can have.

### Alternative: by hand, each session

On a direct cable the host must serve DHCP itself. `setup_host_net.sh` does it per session, as
root -- and the address it adds is the one NetworkManager flushes, unless the profile above is
installed:

```bash
sudo tools/hil/setup_host_net.sh
```

That gives the board-facing NIC `192.168.2.1/24` and starts `dnsmasq` bound to that interface
alone. It refuses to run on an interface carrying the default route, so it cannot answer DHCP
on your main LAN. Tear down with `--stop`.

Override the interface with `HIL_IFACE`:

```bash
sudo HIL_IFACE=eth1 tools/hil/setup_host_net.sh
tools/hil/run.py --stage S0 --iface eth1
```

### For `ptp_sync` and `ptp_ts`, also run a master (by-hand route)

The harness does **not** start `ptp4l` — it needs root and has to outlive a single check. Run
it in another terminal:

```bash
sudo tools/hil/setup_host_net.sh --with-ptp4l
```

or by hand:

```bash
sudo ptp4l -i enp5s0 -H -m --serverOnly=1 --priority1=100
```

`-H` is hardware timestamping. The board-facing NIC on this bench is an **Intel i226**
(`igc`) with a PHC at `/dev/ptp0`, so the script picks `-H` by itself; it falls back to `-S`
(software) only on a NIC without one, and `--software` forces that fallback. Software
timestamping puts the offset floor in the tens of microseconds — that is the host, not the
board.

> Detect a PHC with `ethtool -T <iface> | grep -w hardware-transmit`, **not** with
> `grep 'PTP Hardware Clock'`. ethtool 6.x prints `PTP Hardware Clock: none` for a NIC
> without one, but describes a NIC *with* one as `Hardware timestamp provider index: N` and
> drops the line altogether — so the older grep comes back empty in both cases.

What the board actually achieves against this master, for both firmware images, is measured in
[`docs/l4_performance.md`](../../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/l4_performance.md).
Note that `ptp_sync` below is a 30-second regression gate, not a characterisation.

To run the master in the **gPTP** (802.1AS) profile instead:

```bash
sudo tools/hil/setup_host_net.sh --gptp
```

That installs `tools/linuxptp/flexptp-gm.cfg` to `/etc/linuxptp/` (where AppArmor lets
`ptp4l` read it) and starts the master on it. The board does **not** boot into gPTP — issue
`ptp profile preset gPTP` at the VCP, and re-issue it after every reflash. Anything after
`--` is passed to `ptp4l` verbatim. See
[`docs/host_ptp_setup.md`](../../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/host_ptp_setup.md).

## The flood check

`ptp_flood` saturates the link for 120 s and asserts the board is still there afterwards. It is
the regression test for flexPTP `e873105`, which made every enqueue in the RTOS bindings
non-blocking. Before that commit the lwIP firmware did not stall under load, it **deadlocked**:
lwIP calls `ptp_receive_cb()` from `tcpip_thread` with `LOCK_TCPIP_CORE` held, so a blocking
enqueue there sleeps the thread that owns the core lock while it still holds it, and the wait
is circular. ARP, ICMP and every UDP sender stop with it, and only a reset recovers.

```bash
tools/hil/run.py --elf <elf> --firmware freertos --checks dhcp,ptp_flood
tools/hil/run.py --elf <elf> --checks dhcp,ptp_flood --flood-seconds 30 --flood-pps 20000
tools/hil/flood.py --board 192.168.2.191 --seconds 30      # the flooder on its own
```

**The 120 s default is load-bearing.** The queues have to fill before any of it bites; a 30 s
flood survives even the firmware that has the bug, so a short run proves nothing.

The flooder needs no root. It sends three multicast streams into flexPTP's own receive path
(224.0.1.129:319, :320 and 224.0.0.107:319) plus one unicast stream at the board, pinned to the
board-facing NIC with `IP_MULTICAST_IF` and TTL 1, and it refuses to run on an interface
carrying the default route. On this bench it reaches ~76 000 pps / ~78 Mbit/s on a 100 Mbit
link.

**It asserts liveness, not zero drops.** Under saturation, dropping is the correct behaviour —
that is the entire content of the fix — so a run that dropped nothing would more likely mean
the flood never arrived. What must hold after the flood: the CLI answers, ping is 100% clean,
and PTP returns to SLAVE and produces Sync samples again. The counter deltas are reported
rather than asserted -- with one exception.

**`ptp.drop_txts` must stay at or below 16** (`FLOOD_MAX_DROP_TXTS`, about one `sTxCbFIFO` depth).
It is the one counter that cannot legitimately track the flood: it counts transmit timestamps,
and a PTP slave transmits a handful of frames per second however hard the link is hammered. A
large value means timestamps are being requested for frames that never asked for one, which
was finding U5 -- 284 392 of them in 120 s, while this check reported the number and passed. The
assertion is only made on a counter that was actually read; if it was not, the PASS line says
`drop_txts NOT asserted` instead of claiming a clean result.

It was proven against the defect itself, on the FreeRTOS + lwIP firmware: with both U5
protections removed (upstream's #22 clear in `nsd_lwip.c` and the lwIP driver's own clear) the
check FAILS at 194 237; with only the driver's clear it passes at 0.

Two things it is normal to see, and neither is a failure:

| | |
|---|---|
| `rx.ts_stray` in the hundreds of thousands | the DMA reports RBU (receive ring exhausted) and a frame is separated from the context descriptor carrying its timestamp; the frame is delivered unstamped, which is what NX-01 is for. See `//projects/flexptp_demo_stm32h563/lib/eth/mac_drv.h` |
| `LISTENING -> PRE_MASTER -> MASTER` | the board stops receiving Announce long enough to time out its master and takes over. It returns to SLAVE afterwards, which is what the check asserts |

**`counters` must run before `ptp_flood`, never after.** `check_counters` asserts a healthy-bench
invariant that the flood deliberately breaks; the registry order puts `ptp_flood` last for that
reason.

## The baseline

`baseline.json` is captured from the **pre-port** firmware and is what later stages are
compared against:

```bash
tools/hil/run.py --stage S0 --save baseline.json     # once, before porting
tools/hil/run.py --stage S6 --compare baseline.json  # after each stage
```

`--compare` flags any check that was `PASS` in the baseline and no longer is, plus numeric
regressions outside the tolerances in `run.py`. Tolerances are multipliers, not absolutes:
RTT and free memory move with the host and the build, and the point is to catch a regression,
not to pin noise.

## Adding a check

Add a function `(board, ctx) -> CheckResult` to `checks.py`, register it in `ALL_CHECKS`, and
list it in the `STAGES` entries where it is meaningful. Return `Status.SKIP` with a reason
whenever a prerequisite is missing — never a pass.

Two conventions worth keeping:

- **Assert behaviour, not text.** `check_osinfo` deliberately does not assert *which* kernel
  is running, because it has to keep working across the ThreadX port.
- **Say what a failure means.** `check_ptp_ts` explains that a missing transmit timestamp is
  the CMSIS-RTOS2 ISR defect, because the next person to see it will not remember.

## Why `ptp_ts` runs before `ptp_sync`

`ptp_ts` counts transmit and receive timestamp events separately and fails if either is zero.
It exists because flexPTP's CMSIS-RTOS2 binding invokes the transmit-timestamp callback from
an ISR with a blocking enqueue, which fails silently and drops every `t3` — and a servo
missing `t3` can still *look* like it is converging. See
[`docs/threadx_netxduo_feasibility.md`](../../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_netxduo_feasibility.md).

Check the timestamps are flowing before you believe the offset.

## `ptp_sync` measures the servo settled

"Slaved" only means a master was selected; the servo starts converging there. From a cold reset on
this bench the offset overshoots to about +7.5 µs some 20 s after slaving, and is inside 1 µs after
about 40 s. `ptp_sync` used to sample straight away, so its figure depended on which checks ran
before it: about 800 ns in S8 order, where `ping` and `ptp_ts` give the servo about 25 s, against
7–21 µs as `--checks dhcp,ptp_sync`. Both PASSED, because nothing asserts on the offset, and the
second was misdiagnosed as a bench fault before the cause was found.

It now waits until the board has been slaved for `PTP_CONVERGE_S` (60 s) and reads about 100–150 ns
whatever runs first. The check still does not assert a bound on the offset: it is a regression
gate for "the servo locks", and the characterisation lives in
[`docs/l4_performance.md`](../../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/l4_performance.md).
