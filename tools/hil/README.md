# Hardware-in-the-loop harness

Drives the board's serial CLI from the host so that every change can be verified on real
hardware without re-typing commands. Built for the ThreadX/NetX Duo port
([`docs/threadx_porting_plan.md`](../../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_porting_plan.md)),
it is now the regression suite for **both** firmwares. How it fits with the host tests, and the
procedures that use it, are in
[`testing_architecture.md`](../../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md).

```
hil.py               Board: serial session, cmd(), expect(), reset() over SWD
checks.py            the named checks, and which are valid at each porting stage
run.py               the runner for the checks
scenarios.py         long-running scenarios: fix regressions, floods, robustness, health
scenario.py          the runner for the scenarios
ptp_fuzz.py          malformed and foreign PTP traffic classes, used by the `fuzz` scenario
swd.py               look inside a running or dead board over SWD: snapshot, heap, stacks, fault, console, ptpq
matrix.sh            the full verification: both firmwares x release and debug, host suites, S8, every scenario
flood.py             the saturation flooder (no root)
setup_host_net.sh    privileged host-side network setup (direct cable)
privileged/          the systemd units and polkit rule that make the above sudo-free
baseline.json        golden values captured from the pre-port firmware
test_*.py            host unit tests: bazel test //tools/hil/...
```

The first check in every stage is `build_id`: the linker stamps a SHA-1 over each image,
`osinfo` prints it, and the check compares it against the ELF given by `--flash` or `--elf`.
A mismatch fails the run. Without it -- and it was without it for the whole ThreadX port -- the
runner happily tested whatever firmware happened to be on the board and reported a confident
pass for code that was never loaded (finding GI-01). With no ELF given it SKIPs, which the
runner reports as incomplete: no claim was made, so none is checked.

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
| ST's OpenOCD | nothing to do. The harness uses `$OPENOCD` (or `--openocd`) if given, and otherwise builds `//tools/flash:openocd_exec`, the one `bazel run …:flash` uses — minutes the first time, a no-op after. Upstream 0.12.0 **cannot** program an STM32H5; `--//tools/flash:openocd=local` uses a fork you already have, and the harness follows it |
| udev rule for non-root ST-Link, once per machine | `sudo cp "$(bazel info output_base)/external/+openocd+openocd_src/contrib/60-openocd.rules" /etc/udev/rules.d/ && sudo udevadm control --reload-rules && sudo udevadm trigger` |

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

## Scenarios: the long tests

`run.py`'s checks are small and stage-gated. The scenarios are the opposite: each takes minutes,
drives the host's ptp4l units, floods the link or speaks PTP at the board, and asserts what the
firmware must do under that load. They are the final-test campaigns of November 2026
([`docs/flexptp_upstream_review.md`](../../docs/flexptp_upstream_review.md), sections 6 and 7)
made permanent, and they are what found the three FreeRTOS bugs described there.

```bash
tools/hil/scenario.py --list
tools/hil/scenario.py --firmware freertos --elf <elf> fixes          # one preset
tools/hil/scenario.py --firmware threadx  --elf <elf> gptp stopstart  # named scenarios
tools/hil/scenario.py --firmware freertos --elf <elf> --flash all --save run.json
```

`all` takes about 50 minutes on ThreadX (18 scenarios; `u4` and `pup` are lwIP-only) and 60 on
FreeRTOS (20). With `--elf` the board's build ID is compared with the image first, and **the run
refuses to start on a mismatch** -- an hour against the wrong firmware is the most expensive way
there is to learn nothing. `--elf` also enables the SWD parts of `console_flood`, `stacks`,
`health`, `gptp_flood` and `master_flood`. Exit status is 0 only if no assertion failed.

To run everything — the host suites, then S8 and `all` on both firmwares in release and in debug —
use `matrix.sh`; see [below](#the-full-matrix).

### PASS, FAIL and OBSERVED

Two kinds of result, and the distinction is the point:

- **PASS / FAIL** -- something the firmware must do. A FAIL is a regression.
- **OBSERVED** -- worth recording, but not a defect of this firmware: how long the CLI is
  starved during a saturation flood, whether ptp4l kept the board during one. These were
  measured on every image including the pre-session one, so asserting them would make the suite
  permanently red on healthy firmware.

What *is* asserted under load is what must hold **afterwards**: the CLI answers within 30 s of a
flood ending, the console is intact (a complete `osinfo`), ping is clean, and PTP resyncs. Every
one of the November FreeRTOS bugs failed exactly those.

The one firmware-specific expectation is whether the CLI is served at 20 kpps
(`CLI_SERVED_AT_20KPPS` in `scenarios.py`): ThreadX answers in 0.2 s, so it is asserted there;
FreeRTOS + lwIP gives the CLI no CPU at that rate on every image tested, including with FreeRTOS
and lwIP put back to their old pins, so there it is observed.

### The scenarios

| Scenario | Proves | Host unit |
|---|---|---|
| `u1` | a remote master's clock identity is not byte-reversed (U1, #20) | default |
| `u2` | the logid flag survives `ptpcfg store`/`load`, and the record is accepted (U2, #21) | default |
| `u3` | no orphan `[LOG-LCKD]` prefixes (U3, #25) | default |
| `u4` | 802.3 destinations follow the profile, gPTP → defp2p (U4 #24, U7 #23). lwIP only | default |
| `pup` | the IEC/IEEE 61850-9-3 preset sends to `01:1B:19` and `01:80:C2:00:00:0E`, never the old typo (#28, #29). lwIP only | default |
| `gptp` | the gPTP preset selects a ptp4l gPTP grandmaster and **converges**: 8 Sync/s, max offset < 1 µs over 60–120 s after slaving | gptp |
| `smss` | SLAVE → MASTER → SLAVE: the servo recovers with no non-finite output (#33's scenario) | default |
| `stopstart` | `flexptp stop`/start twice: the network stack survives the teardown and PTP resyncs (U9 #34, U11) | default |
| `stopstart_busy` | ten stops while the PTP task is busy logging: each answered within 3 s, and afterwards the console intact, the stack up, PTP back. U11 waits for the task to stop where it holds nothing; this proves the wait is short and strands nothing | default |
| `master_delresp` | as master, Delay_Resp carries `logMinDelayReqInterval`, for delreq 0 and SYNCMATCHED (U8 #36, issue #30) | slave |
| `gptp_flood` | a gPTP slave through a 120 s saturation flood is still the host's slave afterwards, at full rate; with `--elf`, flexPTP's queues checked over SWD for items stranded without notifications (U14) | gptp |
| `master_flood` | the board as master through a flood: ptp4l back in SLAVE after, last 10 offsets < 1 µs; with `--elf`, the same queue check (U14) | slave |
| `stopstart_flood` | two stop/start cycles during a saturation flood complete, the stack lives, PTP resyncs | default |
| `stopstart_load` | the same at 20 kpps; answered within 5 s where the CLI is served | default |
| `cli_flood` | `osinfo` every 15 s through a saturation flood: the CLI comes back and the console is intact | default |
| `cli_load` | the same at 20 kpps; answered within 5 s where the CLI is served | default |
| `console_flood` | a flood with **nothing** sent to the console, then three complete `osinfo`s and a ping; with `--elf`, the console thread checked over SWD for a stale completion flag | default |
| `fuzz` | malformed and foreign PTP traffic is ignored (below) | default |
| `stacks` | over SWD, after the deepest CLI commands: every task keeps ≥ 256 B of untouched stack | default |
| `health` | the end-of-run gate: ping, a complete `osinfo`, fresh Sync samples; with `--elf`, a FreeRTOS heap walk, and a snapshot and fault decode if the board is dead | default |

The units are `flexptp-ptp4l-{default,gptp,slave}` from `privileged/`; a scenario starts the one
it needs and restores the default master when it is done, in a `finally`, so one failure does not
poison the next. `u4` and `pup` read the 802.3 destination histogram only the lwIP driver keeps.

`stacks`' 256-byte floor is from measurement: the FreeRTOS CLI held **56 bytes** under load before
its `osinfo` table moved off the stack, and overflowed; it holds 640 and more now.

## Malformed and foreign PTP (`fuzz`)

Nine classes, 30 s each at 10 packets/s to 224.0.1.129, with the board slaved to the host's ptp4l
in the default profile; each is judged by the board's Sync cycles against a quiet reference
window. A slave must ignore all of them:

| Class | Packet | Why it must be ignored |
|---|---|---|
| `trunc1` | 1 byte, value 0x00, to 319 | shorter than a PTP header |
| `trunc33` | a foreign two-step Sync cut to 33 bytes | shorter than a PTP header |
| `badver` | a foreign two-step Sync, `versionPTP` 1 | not PTPv2 |
| `badtype` | messageType 0x5 | reserved |
| `garbage` | 1–200 random bytes to 319 and 320 | |
| `oversize` | a foreign two-step Sync padded to 1400 bytes | not from the selected master |
| `domain1` | a foreign Sync in domain 1 | not this domain |
| `foreign` | a well-formed foreign two-step Sync, domain 0 | not from the selected master |
| `announce` | a foreign Announce, priority1 255 | worse than the host: BMCA keeps the host |

Asserted per class: the Sync rate holds (≥ 80% of the reference) with no offset over 10 µs, and
the host stays master. Asserted at the end: the stack is up and PTP has resynced.

flexPTP `RC1.0-26` ignores only `badtype`, `garbage`, `domain1` and `announce`. The first four
classes stop synchronisation outright, 0 of 30 cycles on both firmwares, because nothing compares
the received length or `versionPTP` with the header (finding U12); `oversize` and `foreign`
because the slave takes Sync and Follow_Up from any sender (U13). This scenario is the hardware
regression test for both fixes: with them, every class passes. Multicast loopback is off, so the
host's own ptp4l never sees any of it.

## Looking inside the board: `swd.py`

The firmware's only diagnostic channel is its console, and the failures that matter most take the
console down with them. `swd.py` reads the target over SWD instead, from symbols alone -- it needs
no debug info, so it works on a release image, which has none:

```bash
tools/hil/swd.py --elf <elf> snapshot   # PC, exception, task sampled 40x; tick; scheduler state
tools/hil/swd.py --elf <elf> heap       # FreeRTOS heap_4: every block, the free list, cross-checked
tools/hil/swd.py --elf <elf> stacks     # every task's (thread's) untouched stack bytes
tools/hil/swd.py --elf <elf> fault      # CFSR/HFSR decoded, exception frames found and symbolised
tools/hil/swd.py --elf <elf> console    # the console thread: FIFO, TX-complete flag, USART3, GPDMA
tools/hil/swd.py --elf <elf> ptpq       # flexPTP's five queues, read 3x: are items and wakeups in step?
tools/hil/swd.py --elf <elf> all        # snapshot, heap, stacks, console, ptpq
```

It never leaves the core halted: everything runs inside one Tcl `catch` that resumes the core
whatever happens. An earlier ad-hoc script did not, exited with the core halted after an error,
and produced a perfect imitation of a wedged board -- frozen tick, no ping, no console. A heap or
stack read halts the core once, for the time it takes to dump about 30 KB; `snapshot` halts it 40
times for well under a millisecond each. Use them on a running board sparingly -- PTP will see a
gap -- and freely on a dead one.

### Reading a wedged board

Run `snapshot` first, then follow what it says:

| Snapshot shows | Next | What it was in November 2026 |
|---|---|---|
| tick not advancing, PC in `vApplicationStackOverflowHook`, entered from PendSV | `heap`, `stacks` | **CLI stack overflow** in `osinfo`: the heap walk stops at the overflowing stack's own allocation header |
| exception `HardFault`, PC in `fault_report` | `fault` | **stale `pbuf->tx_cb` called from the ETH ISR**: IACCVIOL, stacked PC `0xfffffe00`, LR in `ETHHW_ProcessTx` |
| idle, tick advancing, ping fine, but replies garbled | `console` | **console thread a completion flag ahead**: FIFO empty, DMA idle, flag set |
| idle, tick advancing, ping fine, console fine, but PTP deaf: no Pdelay_Req answered, own master for good | `ptpq` | **items queued without notifications** (U14): PTP thread waiting on an empty notification FIFO, 12 frames in `sRxPacketFIFO`, 3 heartbeats in `sEventFIFO` (ThreadX, after a gPTP flood) |
| idle, everything fine | `health` scenario | the problem is on the host side, or already gone |

`fault` finds the frame by its shape -- an xPSR with sane reserved bits and a Thumb return address
in flash -- because a handler that has called into C no longer has the frame pointer in a
register. It does **not** require the stacked PC, or even the xPSR's Thumb bit, to be sane: the
HardFault above stacked T clear, because a `blx` to an even address clears it.

### What it assumes

- Symbols from `arm-none-eabi-nm`, found via `$ARM_TOOLCHAIN_BIN`, `PATH`, or the toolchain Bazel
  downloaded (`~/.cache/bazel/.../external/*arm_gnu_toolchain`): a machine that can build can
  inspect.
- Structure layouts as configured here: FreeRTOS `TCB_t` with no MPU wrappers or list-integrity
  bytes, `heap_4`; ThreadX `TX_THREAD` with an empty `TX_THREAD_EXTENSION_0`, stacks filled with
  `TX_STACK_FILL`. The offsets are named constants at the top of `swd.py`; a kernel bump that
  moves them shows up as nonsense names in `stacks` and in `test_swd.py`'s fixtures, which are
  laid out the same way.
- Some statics share a name across files (`flags`, `fifo`, `lineBuf`); `console` uses the ones
  that sit beside `serial_io.c`'s `uart`.
- `ptpq` reads `task_ptp.c`'s five queue handles and then the queues they point at, as FreeRTOS
  `Queue_t` (`uxMessagesWaiting`, `uxLength`, the waiting-to-receive list) or ThreadX `TX_QUEUE`
  (`tx_queue_enqueued`, `tx_queue_capacity`, the suspended count), whichever kernel the image has.
  flexPTP pushes an item, then its notification, and the task takes one item per notification, so
  the items queued may exceed the notifications by at most a producer or the task caught between
  its two steps. `ptpq` flags more than that in all three of its reads, a second apart.

## The full matrix

```bash
tools/hil/matrix.sh                     # ~4.5 h: both firmwares x release (opt) and debug (dbg)
tools/hil/matrix.sh --build-only        # host suites in three modes, the four images, their build IDs
tools/hil/matrix.sh --skip-host --configs opt --firmwares threadx --stage S0 --scenarios health
```

The full verification in one command. It runs `bazel test //...` in fastbuild, `opt` and `dbg`;
builds the four images and records each one's build ID and size; then, for each image, flashes it,
runs S8, and runs every scenario — skipping the scenarios for an image that failed S8. Everything
lands in `build/matrix-<date>/`: the ELFs (keep them; `swd.py` needs the exact image), every log and
JSON report, and `summary.txt`. Exit 0 only if every step passed. FreeRTOS's S8 is incomplete by
design, its one `threads` SKIP, and that skip alone is accepted.

Why both configurations, and what each one catches that the other cannot, is in
[`testing_architecture.md`](../../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md#build-configurations-what-debug-and-release-mean).
A plain `bazel build` image is neither: fastbuild is `-O0` without `DEBUG`.

## Unit tests

```bash
bazel test //tools/hil/...
```

`test_find_openocd.py` pins where the harness's OpenOCD comes from -- `$OPENOCD`, else Bazel's, built before it is used; `test_swd.py` drives the SWD parsers over
synthetic target memory, including the corruptions actually found -- a stack overflowed into its
own heap header, a looping free list, a stacked PC of `0xfffffe00` with T clear, and flexPTP's
queues as the stuck ThreadX board had them;
`test_scenarios.py` pins the parsers that decide PASS or FAIL on the board (a `ptp info` reply
cut off mid-line once read `00a0c9` as a clock identity) and the malformed-PTP packet builders.
