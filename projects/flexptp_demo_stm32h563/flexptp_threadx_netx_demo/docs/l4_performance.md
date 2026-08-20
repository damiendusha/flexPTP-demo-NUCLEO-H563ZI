# Layer 4 Synchronisation Performance — FreeRTOS vs ThreadX

Measured accuracy of both firmware images in the **`default`** profile (plain IEEE 1588 over
UDP/IPv4 with E2E delay measurement), against a hardware-timestamping host grandmaster on a
direct cable.

**Conclusion up front:** both images synchronise to **sub-microsecond** accuracy and are
**indistinguishable from one another** at the resolution this bench can resolve — RMS offset
**187 ns** (FreeRTOS) against **203 ns** (ThreadX), p99 **520 ns** against **500 ns**. The
headline numbers do *not* separate the two kernels, and this document does not claim they do.

Two things did separate them, neither of them servo quality:

- FreeRTOS lost **11 seconds of Sync** in one incident mid-run (14 missing cycles over the
  run against ThreadX's 2), which cost a 3.2 µs excursion and dominates its raw tail
  statistics. Cause not established — see [The FreeRTOS dropout](#the-freertos-dropout).
- ThreadX carries a **systematic +134 ns offset** where FreeRTOS sits at +43 ns. A constant
  bias that survives E2E path-delay subtraction points at the timestamp point, not the servo.

> **These figures supersede the software-timestamped ones** quoted elsewhere in these docs.
> The bench NIC is now an Intel i226 with a PTP hardware clock; the previous RTL8125 had none,
> and its tens-of-microseconds floor was the host's, not the board's. Measured 2026-08-20.

## Table of Contents

- [Conditions](#conditions)
- [Results](#results)
- [The FreeRTOS dropout](#the-freertos-dropout)
- [What separates the two images](#what-separates-the-two-images)
- [What this does not establish](#what-this-does-not-establish)
- [Reproducing it](#reproducing-it)

## Conditions

Identical for both runs, and the second run followed the first by 20 minutes on the same
bench with nothing else changed.

| | |
|---|---|
| Host grandmaster | `ptp4l -i enp5s0 -H -m --serverOnly=1 --priority1=100` (linuxptp 4.4) |
| Host NIC | Intel **i226** (`igc`), PHC `/dev/ptp0`, `hwstamp_ctl` reporting `tx_type 1` / `rx_filter 1` |
| Topology | **Direct cable.** No switch — see [ptp_profiles.md](ptp_profiles.md) for why one is worth avoiding here (F42) |
| Link | 100BASE-TX, full duplex |
| Profile | `default` — UDP/IPv4, E2E. The boot preset; no profile command issued |
| Sync rate | 1/s (`logSyncPeriod` 0) |
| Board clock | ST-Link MCO at 8 MHz — the noisy reference, see [ptp_clock.md](ptp_clock.md) |
| Window | 120 s warm-up (captured, excluded) then **15 min** steady state, ~900 samples each |
| FreeRTOS build | `9dddae39ac34283a4c739c7bf96ed17ea3f7defa` |
| ThreadX build | `842caa02f56254f671b62f6855dd2946596e0fb0` |

Each run verified the board's `Build ID` against the flashed ELF before recording a sample —
the GI-01 guard. Without it a run can characterise firmware that was never loaded.

The warm-up is captured rather than slept through. A reset restarts the servo, so the first
minute or two is convergence rather than performance; separating it in analysis is more
honest than starting the clock late and implying the trace was steady from its first sample.

## Results

Offset from master (`Dt [ns]`), steady state, **all samples**:

| | FreeRTOS/lwIP | ThreadX/NetX Duo |
|---|---:|---:|
| Samples | 886 | 898 |
| Mean | +22.5 ns | +134.0 ns |
| Std dev | 280.8 ns | 151.8 ns |
| RMS | 281.7 ns | 202.5 ns |
| \|offset\| p50 | 124 ns | 182 ns |
| \|offset\| p95 | 478 ns | 331 ns |
| \|offset\| p99 | 979 ns | 500 ns |
| \|offset\| max | **3,232 ns** | 551 ns |
| Missing Sync cycles | 14 | 2 |
| Converged to <1 µs | 27.7 s | 24.2 s |

The same, **excluding the 874–895 s dropout and the servo's recovery from it**. This is the
comparison worth reading: it is the steady-state behaviour of both images with one
non-recurring incident set aside.

| | FreeRTOS/lwIP | ThreadX/NetX Duo |
|---|---:|---:|
| Samples | 875 | 877 |
| Mean | +43.2 ns | +133.1 ns |
| Std dev | 181.9 ns | 153.5 ns |
| **RMS** | **186.9 ns** | **203.2 ns** |
| \|offset\| p50 | 122 ns | 184 ns |
| \|offset\| p95 | 466 ns | 346 ns |
| \|offset\| p99 | 520 ns | 500 ns |
| \|offset\| max | 618 ns | 551 ns |
| Servo correction std dev | 15.6 ppb | 9.1 ppb |
| Path delay | 559.0 ± 7.4 ns | 563.6 ± 6.6 ns |

FreeRTOS holds the lower median, ThreadX the tighter spread and the steadier servo. Both
readings are well inside what a single 15-minute run can distinguish.

**A cross-check worth recording:** both images converged on an addend of ≈3,435,880,000
against a nominal 3,435,973,836 — the same **−27 ppm** pull, measured independently by two
servos on two network stacks. That agreement is evidence both are disciplining correctly, and
the pull itself is the ST-Link MCO, not either firmware.

The measured **path delay of ~560 ns** is the real one-way delay of a 100BASE-TX PHY pair,
now that both ends timestamp in hardware. It matters beyond this document: 802.1AS budgets
**800 ns** for `neighborPropDelayThresh`, so the gPTP route has roughly 240 ns of headroom on
this link — real, but not generous. See [host_ptp_setup.md](host_ptp_setup.md).

## The FreeRTOS dropout

At t = 874.7 s the FreeRTOS run stopped producing Sync log lines for **11.02 seconds** — ten
missing cycles. On resumption the offset was **−3,218 ns**, and the servo took about six
samples to pull back under 1 µs. The six largest \|offset\| values in the entire run are the
six consecutive samples following this gap.

That is why the raw table above reads so much worse for FreeRTOS than the filtered one, and
why the filtered one is the fair comparison. It is one incident, not a distribution.

What is established:

- **The host was not involved.** No link event, no `ptp4l` fault, no `FAULTY` transition in
  the journal at that time; the master kept sending. (The link bounces visible at 17:05 are
  the reflash *between* the two runs.)
- **The board logged nothing.** No console output during the gap.
- **The servo genuinely stopped disciplining.** The offset diverged to 3.2 µs, which a merely
  stalled console would not produce — the servo corrects on every Sync whether or not a line
  is printed. Nor was it buffered output flushed late: samples resumed at a clean 1 s cadence
  rather than in a burst.

What is **not** established: the cause. And the reason is instrumentation —

| | drop/refusal counters instrumented | `ethstat` counters total |
|---|---:|---:|
| FreeRTOS | **2 of 12** | 7 |
| ThreadX | **12 of 12** | 30 |

ThreadX's run is clean on all twelve, with `ptpts.posted == ptpts.delivered == 1003` — every
transmit timestamp posted was delivered. FreeRTOS instruments only `rx.ts_deferred` and
`rx.ts_stray`, both zero. So the FreeRTOS firmware cannot account for its own anomaly.

The Phase 9 diagnostics backport (commit `1d4d684`) evidently landed partially. Closing that
gap is a prerequisite for trusting FreeRTOS numbers at this precision: an unexplained
11-second outage is exactly the class of defect the ThreadX counters exist to catch.

## What separates the two images

The one difference that is stable across the whole run rather than concentrated in an
incident is the **mean offset: +133 ns (ThreadX) against +43 ns (FreeRTOS)**, a ~90 ns
constant.

E2E delay measurement subtracts the *measured* path delay, so a symmetric link leaves no
residual. A constant residual therefore indicates residual asymmetry between the transmit and
receive timestamp points — a property of where each stack's driver captures the timestamp
relative to the wire, not of servo tuning. 90 ns is about 18 ticks of the 5 ns timestamp
granularity.

This is a hypothesis consistent with the data, not a diagnosis; it has not been traced to a
specific code path.

## What this does not establish

- **One run each.** A single 15-minute window per image, taken 20 minutes apart. Differences
  of tens of nanoseconds between the two columns are not significant and should not be used
  to rank the kernels.
- **A shared, noisy reference.** Both ran off the ST-Link's 8 MHz MCO. The −27 ppm pull and
  much of the residual jitter are that reference. The fitted X3 25 MHz crystal is the better
  one and is documented in the firmware banner and [port_review.md](port_review.md) (F55) —
  untested on this port.
- **Layer 4 only.** The `gPTP` (802.1AS) route was not measured. It is now plausible on this
  bench for the first time, since hardware timestamping removes the software-timestamp peer
  delay inflation that kept the port from becoming `asCapable`.
- **Nothing about long-term stability.** 15 minutes says nothing about holdover, thermal
  drift, or behaviour across a link flap.

## Reproducing it

Host side, one command — it serves DHCP on the direct cable and starts the master with the
right timestamping mode for the interface:

```bash
sudo tools/hil/setup_host_net.sh --with-ptp4l
```

Then per image: flash, wait for `SLAVE`, and record `ptp log def on` for the window. The
harness covers the health-check form of this in `ptp_sync`:

```bash
tools/hil/run.py --firmware freertos --flash <elf> --checks dhcp,ptp_start,ptp_ts,ptp_sync
```

`ptp_sync` asserts a 30-second window and reports mean and worst-case offset, which is a
regression gate rather than a characterisation. The traces behind this document were 15-minute
captures with the warm-up separated, analysed for percentiles, servo stability and missing
cycles; that collector is not yet in the tree.
