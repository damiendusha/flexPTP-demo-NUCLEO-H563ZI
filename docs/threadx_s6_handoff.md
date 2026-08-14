# S6 handoff — flexPTP over NetX Duo does not select a master

A self-contained brief for picking up stage S6 of the ThreadX/NetX Duo port with no prior
context. Everything needed to be productive in ten minutes.

Background, if you want it: [threadx_porting_plan.md](threadx_porting_plan.md) (the staged
plan, with a progress note per stage) and
[threadx_netxduo_feasibility.md](threadx_netxduo_feasibility.md) (why the port is shaped this
way). **You do not need to read either to start.**

---

## The one-line problem

Correctly-formed PTP messages reach flexPTP and it declines to act on them: BMCA selects no
master, so no `Delay_Req` is ever sent and the clock never syncs.

## Repository state

Branch **`threadx-port`**, 12 commits, working tree clean apart from the two submodule
patch trees (expected — see `patches/`).

Stages S0–S5d are **done and green**. S6 is the only one outstanding.

```bash
export PATH="$PWD/toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi/bin:$PATH"
cmake --build build/s6 -j                     # or ./build.sh -t Debug -b build/s6
cd tools/hil && ./run.py --flash ../../build/s6/flexptp-demo.elf --stage S6
```

The bench is live: board on `/dev/ttyACM0`, host at `192.168.2.1/24` on `enp5s0` serving DHCP,
`ptp4l -i enp5s0 -S -m --serverOnly=1 --priority1=100` already running. The board takes
`192.168.2.191`.

`--stage S5d --compare baseline.json` is **6/6 with no regressions** — the network layer under
PTP is sound, so do not start there.

---

## What is already proven, and how

Do not re-derive these. Each was measured on hardware with `ethinfo`, a CLI command added for
exactly this purpose:

```
udp: rx 37 (invalid 0, dropped 0, cksum 0), tx 2
nsd: notify 35, packets 35, enqueued 35, tx-requested 0
nsd rx types: sync 14 delreq 0 fup 14 delresp 0 ann 7
nsd last rx: 44 bytes
  08 12 00 2c 00 00 00 00 00 00 00 00 00 00 00 00
```

| Established | Evidence |
|---|---|
| The IGMP join is correct **on the wire** | transmitted frame dump: destination MAC `01:00:5e:00:01:81`, EtherType 0800, IP protocol `02`, destination `224.0.1.129` |
| Sync, Follow_Up and Announce all arrive, on both sockets | the message-type histogram above, in the right 2:2:1 proportions |
| Each is a well-formed PTPv2 message at the right offset and length | `08 12 00 2c` = messageType 8 (Follow_Up), versionPTP 2 with `minorVersionPTP` 1, messageLength 44 — and the reported length is 44 |
| All of them reach `ptp_receive_enqueue()` | `enqueued 35` |
| flexPTP's `ptp` thread is alive and consuming | `osinfo` shows `ptp pri=32 (cmsis 24) state=5 runs=52` |
| The transport configuration is right | `ptp transport` → IPv4, `ptp domain` → 0, `ptp delmech` → E2E |

## What fails

- `ptp info` → `Master clock ID: 0000000000000000`
- No BMCA state transition, **even with `ptp log info on` and `ptp log bmca on`** — total silence
- No `Delay_Req` ever transmitted (`tx-requested 0`)

---

## The constraint that narrows this

**The lwIP baseline accepted the same traffic from the same running `ptp4l` instance, on the
same day.** `baseline.json` records `ptp_sync` passing with `|offset| ≤ 11.8 µs`.

So the wire format is not the variable. What changed underneath flexPTP is exactly two things:

1. the OS binding, `FLEXPTP_FREERTOS` → `FLEXPTP_CMSIS_OS2` (`Inc/flexptp_options.h`)
2. the network stack driver, `nsd_lwip.c` → `Src/flexptp_nsd/nsd_netxduo.c`

**This port is the first to exercise flexPTP's CMSIS-RTOS2 binding on this hardware.** It
already contains one known defect (the ISR/`osWaitForever` transmit-timestamp bug that
`Src/flexptp_nsd/ptp_tx_timestamp.c` works around). A second defect in the same binding is a
live possibility, not a remote one.

## Candidates, in the order worth checking

1. **The receive queue pairing in the CMSIS-RTOS2 binding.**
   `Modules/flexptp/src/flexptp/task_ptp.c`. `ptp_receive_enqueue()` posts the message uid to
   `sRxPacketFIFO` **and** a notification to `sNotificationFIFO` as two separate operations,
   and `task_ptp()` pops them as two separate operations. The FreeRTOS path is field-proven;
   this one is not. Look particularly at `if (notification == PTN_EVENT)` — `==`, where the
   other three arms use `&`. A heartbeat notification arriving between the two posts would be
   read as an event and the receive dropped.

2. **`msgb_alloc()` returning NULL.** If the receive message buffer is exhausted, every message
   is discarded. `ptp log info on` is supposed to print "The PTP receive packet buffer is
   full" — and prints nothing, which argues against it, but the logging itself runs through
   the same binding so absence of the message is weak evidence. Worth confirming directly.

3. **A version or field check in `ptp_process_packet()`.** The received `versionPTP` byte is
   `0x12` (PTPv2, `minorVersionPTP` 1) which is what a current `ptp4l` emits. Ranked last
   *because* the baseline accepted it — but if candidates 1 and 2 are cleared, verify rather
   than assume.

The fastest discriminator between 1/2 and 3: instrument `ptp_process_packet()` entry. If it is
never reached, the fault is in the queue plumbing; if it is reached and returns early, it is a
content check.

---

## Tools already at your disposal

- **`ethinfo`** (CLI) — driver state, and counters for driver/IP/UDP/ARP/NSD, a PTP
  message-type histogram, the head of the last received PTP payload, and the head of the last
  transmitted frame. Four of the five defects found in this port were found with counters like
  these rather than by reading code. Extend it rather than adding print statements.
- **`tools/hil/`** — `./run.py --stage S6 --compare baseline.json`, and `--checks ptp_ts` etc.
  for one at a time. `hil.py` can be imported directly for ad-hoc poking; see the examples
  throughout this session's commits.
- **`test/`** — 25 host unit tests, `ctest --test-dir build/test`, no board needed.

## Ground rules for this codebase

- **Do not modify `Drivers/EthDrv/mac_drv.c`.** It is the most expensive verified artefact in
  the repo and this port has deliberately not touched it. If you believe it is at fault, say so
  rather than editing it.
- **Prefer patching nothing in `Modules/`.** flexPTP already carries three patches in
  `patches/flexptp/`; a fourth is acceptable if the defect is genuinely upstream's, but it must
  be a real fix worth reporting upstream, and `build.sh` must be updated to apply it.
- **A `SKIP` is not a pass.** `run.py` exits 3 for "passed but incomplete" on purpose.
- **Commit per stage**, with the reasoning in the message — see `git log` for the register.
- **Do not run `/code-review`**; the maintainer has asked to hold off.
- Report what you actually observed. If something is still broken at the end, say so plainly.

## Definition of done for S6

`./run.py --stage S6 --compare baseline.json` green, with `ptp_ts` passing **before**
`ptp_sync` — that ordering exists because a servo missing `t3` can still look like it is
converging, and `ptp_ts` is what excludes it.
