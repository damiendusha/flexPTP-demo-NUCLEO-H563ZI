# Testing Architecture

How this project is tested, what each tier is *for*, and — the part that matters most — what
each tier cannot tell you. A green suite that is trusted beyond its reach is worse than no
suite, because it converts an unknown into a false certainty.

## Table of Contents

- [The three surfaces](#the-three-surfaces)
- [How to run everything](#how-to-run-everything)
- [Tier 1 — host unit tests](#tier-1--host-unit-tests)
- [Tier 2 — real ThreadX on the host](#tier-2--real-threadx-on-the-host)
- [Tier 3 — hardware in the loop](#tier-3--hardware-in-the-loop)
- [What is reachable, measured](#what-is-reachable-measured)
- [Mutation testing is the entry fee](#mutation-testing-is-the-entry-fee)
- [Known gaps](#known-gaps)

Companion documents: [architecture.md](architecture.md) (what is being tested) ·
[port_review.md](port_review.md) (the finding log these tests are calibrated against) ·
[`test/README.md`](../test/README.md) (host-suite mechanics) ·
[`tools/hil/README.md`](../tools/hil/README.md) (hardware harness mechanics)

---

## The three surfaces

| Tier | Where it runs | Needs | Wall clock | Gates |
|---|---|---|---|---|
| **1. Host unit tests** | dev machine / CI, system `gcc` | nothing | 0.2 s | every push |
| **2. Real ThreadX on the host** | dev machine / CI, ThreadX `ports/linux` | nothing | 0.04 s | every push |
| **3. Hardware in the loop** | the NUCLEO board over SWD + serial | board, cable, host NIC | ~3 min | manual |

Tiers 1 and 2 live in the same CMake project and the same `ctest` run; they are separated here
because they answer different questions and fail for different reasons.

The split is deliberate and it is not a maturity ladder — **tier 3 is not a slower version of
tier 1.** They catch disjoint classes of defect. Roughly a quarter of this port's findings were
reachable *only* on hardware, and no quantity of host tests would have moved that number.

## How to run everything

```bash
./build.sh --run-tests
```

Builds the firmware, then builds and runs tiers 1 and 2. To skip the firmware — and with it the
200 MB ARM toolchain download, which the host tests do not need:

```bash
./build.sh --tests-only
```

Either way the underlying commands are plain CMake, if you prefer them directly:

```bash
cmake -S test -B build/test && cmake --build build/test -j && ctest --test-dir build/test --output-on-failure
```

Tier 3 needs the board attached:

```bash
tools/hil/run.py --flash build/flexptp-demo.elf --stage S8
```

**`--flash` is not optional if you want to test what you just built.** Without it the runner
drives whatever firmware is already on the board and reports a confident pass for code that was
never loaded. This has caught out at least one careless operator; see
[`tools/hil/README.md`](../tools/hil/README.md).

## Tier 1 — host unit tests

Pure logic, compiled by the system compiler, no board and no network.

`test/` is a **separate CMake root**, not a target of the firmware build, because the top-level
`CMakeLists.txt` includes `cmake/gcc-arm-none-eabi.cmake` unconditionally and can only ever
produce ARM objects.

### The driver ring tests

| Binary | Cases | What it pins |
|---|---|---|
| `test_rx_ring` | 13 | Receive descriptor state machine: re-delivery (**F61**), the RBU tail-pointer restart (**F71**), copy-pointer advance (**F58**), and the data/context descriptor pairing rules (**NX-01**) |
| `test_tx_ring` | 10 | Transmit completion ordering, including across the 16-bit counter wrap; dropped-vs-sent accounting (**F75**); the per-pass count reset (**F57**) |
| `test_host_harness` | 6 | The harness itself — 32-bit arena and function-pointer round-trips. These fail *first* and say why, because if they regress the driver tests corrupt memory rather than failing cleanly |

`mac_drv.c` required **no changes at all** to become testable: 97 of its 98 register accesses
already go through its `ETH_TypeDef *eth` parameter, so a test simply passes the address of a
fake. The 98th is redirected by macro in `test/mac_drv_uut.c`, which `#include`s the driver's
`.c` verbatim — which also exposes its file-static helpers.

The register definitions are **ST's real ones**. `test/host/` shadows exactly three ARM headers
(`core_cm33.h`, `system_stm32h5xx.h`, `cmsis_compiler.h`) and everything below is the genuine
CMSIS device header and HAL headers from the pinned submodule. Nothing is hand-copied, so a
wrong bit position cannot make a test lie, and signature drift is a compile error rather than a
silent divergence.

### The NSD logic units

`Src/flexptp_nsd/ptp_{tx_registry,ts_codec,endpoint,nsd_lifecycle}` were extracted from
`eth_drv_netxduo.c` and `nsd_netxduo.c` specifically so they could be tested. They are C++ with `extern "C"`
boundaries and they depend on **nothing** — no ThreadX, no NetX Duo, not even the fakes in
`test/host/`. The registry keys on `const void *` rather than `NX_PACKET *` for exactly that
reason.

| Binary | Cases | The property that matters |
|---|---|---|
| `test_ptp_tx_registry` | 12 | **The ABA hazard.** NetX Duo recycles packets, so an entry left behind after consumption would be claimed by a later, unrelated packet at the same address — feeding the servo a real timestamp under the wrong message uid |
| `test_ptp_ts_codec` | 8 | The 12-byte headroom precondition. Below it the three ULONGs were never written, so what is there is payload; returning zeroed-and-false beats a plausible wrong time (the **F59** lesson) |
| `test_ptp_endpoint` | 9 | Five cases of transport × delay mechanism. A wrong destination presents as "no response received", not as a routing bug |
| `test_ptp_nsd_lifecycle` | 14 | **Transactional configuration.** Every socket created is deleted, a failure part-way leaves nothing behind, and a failed configure leaves the transport unset — the three properties **NX-02** got wrong. Driven against a fake stack that counts, so the failure paths are reachable at all |

Before this extraction the network-stack driver had **zero** automated coverage. That is what
the exercise bought, more than any individual assertion.

## Tier 2 — real ThreadX on the host

`test_rtos_context.cpp` links **real ThreadX** (its `ports/linux/gnu` target) and **the real
CMSIS-RTOS2 wrapper** — the same two components the firmware runs, configured by the same
`Inc/tx_user.h`, deliberately shared rather than copied so the kernel under test cannot drift
from the board's.

It exists because one defect class bit this port three separate times, always the same shape:
**a blocking call from a context that may not block.**

1. `ptp_transmit_timestamp_cb()` enqueues with `osWaitForever`, and the MAC delivers transmit
   timestamps from `ETH_IRQHandler`.
2. `ptp_event_enqueue()` does the same, and its highest-frequency caller is the heartbeat timer
   callback, which CMSIS-RTOS2 runs on the RTOS timer thread. That is **F80**: every heartbeat
   rejected, the BMCA stranded in `INITIALIZING`, no master ever selected. It cost a full bench
   session.
3. A comment in `Inc/tx_user.h` that reasoned (2) was safe *because* it was not (1). ThreadX
   rejects both, for different reasons — and that wrong inference is what sent the debugging
   down the receive path first.

A comment cannot stop the fourth. These five cases can, because the restriction is enforced in
ThreadX's own `common/src/txe_queue_send.c`, and `common/src` is identical on every port:

```
From a real osTimer callback (i.e. ThreadX's timer thread):
  osMessageQueuePut(q, &m, 0, osWaitForever)  ->  -2  (osErrorTimeout)  MUST fail
  osMessageQueuePut(q, &m, 0, 0U)             ->   0  (osOK)            MUST succeed
```

Registered as a single `ctest` entry rather than via `gtest_discover_tests`, because discovery
runs the binary to enumerate and this one enters a kernel that never returns.

**What it cannot cover:** the ISR half of the same rule. `IS_IRQ_MODE()` is built from
`__get_IPSR()`, which the host shim hard-codes to 0 because Linux has no exception context. The
transmit-timestamp path stays on the bench. The timer-thread half is the one that cost a
session, and it is fully covered.

## Tier 3 — hardware in the loop

`tools/hil/` drives the board's serial CLI from the host over pyserial, with OpenOCD for flash
and reset. Ten named checks, grouped into per-stage sets so that a partially-ported firmware can
still be tested against the subset that is meaningful at that point.

| Check | Asserts |
|---|---|
| `boot` | PHY identified, link up |
| `cli` | Command table populated |
| `osinfo` | Kernel version and free memory |
| `cpp_ctor` | Static C++ constructors ran before `main` |
| `threads` | Expected thread set exists at expected priorities |
| `dhcp` | Address acquired |
| `ping` | Round-trip from the host, loss and latency |
| `ptp_start` | Addend programmed to the computed nominal |
| `ptp_ts` | T2 and T3 both advancing over 20 samples |
| `ptp_sync` | Reaches `SLAVE`, offset bounded over 30 samples |

Exit status: `0` all passed · `1` failed or regressed · `2` could not reach the board · `3`
passed but incomplete.

**A `SKIP` is not a pass.** "We could not test it" and "it works" are different claims, and
conflating them is how F42 survived for days. The runner reports incomplete and returns 3
rather than letting a gap masquerade as coverage.

`baseline.json` holds golden values captured from the pre-port FreeRTOS/lwIP firmware, so
`--compare` answers the question that actually matters during a port: *is it worse than what it
replaced?*

## What is reachable, measured

Derived against this repository's own finding log rather than estimated. Of the ~23 findings in
[port_review.md](port_review.md) that fall in code this port writes or rewrites:

| Reach | Share | Example findings |
|---|---|---|
| Catchable by a host test | ~50% | F57, F61, F58, F75, F54, F69, F67, F74, F59 |
| Regression-lockable, but discovery needed hardware | ~25% | F71, F76, F52, F68, F72, F73 |
| Irreducibly hardware | ~25% | F70, F62, F51, F53, F42, and the clock-tree findings |

An earlier draft of the porting plan claimed "roughly 80% of the risk stays on the bench". That
was a guess and it was wrong; the figure above is derived. The full working is in
[threadx_porting_plan.md](threadx_porting_plan.md) under *How far mocks actually reach*.

> **The asymmetry matters more than the ratio.** Mocks are excellent at *preventing recurrence*
> and poor at *first discovery*. F71 is the clearest case: the RX ring's RBU latch needs a
> tail-pointer write to restart, and a model would only reproduce that if its author already
> knew. The model would have been wrong in exactly the way the code was wrong, and the suite
> would have been green.

## Mutation testing is the entry fee

A characterisation test that cannot fail is worse than no test — it costs the same to maintain
and buys a false sense of coverage. Every finding pinned here was mutation-tested: the defect
reintroduced, the suite re-run, the source restored and verified byte-identical.

| Mutation | Result |
|---|---|
| Drop `!(bd)->ext.inFlight` from `ETHHW_DESC_NEEDS_PROCESSING` (**F61**) | `RxRing.DoesNotRedeliverAFrameStillInFlight` fails |
| Remove the `DMACRDTPR` write from `ETHHW_RestoreRXDesc` (**F71**) | `RxRing.RestoringWritesTheTailPointerToRestartTheEngine` fails |
| Hoist `found` out of `ETHHW_ProcessTx`'s retry loop (**F57**) | six `TxRing` tests **time out** |
| Widen the ordering delta to `int32_t` | `TxRing.DispatchesInOrderAcrossTheSixteenBitCounterWrap` fails |
| `ptp_tx_registry_take()` reads instead of clearing | 5 tests fail |
| `ptp_tx_registry_cancel()` does nothing | 1 test fails |
| Drop the codec's headroom precondition | 1 test fails |
| Endpoint teardown stops zeroing | 1 test fails |
| Widen the RFC 1112 mask from 23 to 24 bits | 1 test fails — *but only after the test was fixed* |
| Assert the timer-thread blocking put SUCCEEDS (**F80** inverted) | `RtosContext.BlockingEnqueueFromATimerCallbackIsRejected` fails, reporting −2 vs 0 |
| Drop the context descriptor's `OWN` check (**NX-01**) | 2 `RxRing` tests fail — a frame is delivered stamped `1 080 035 208 ns`, the buffer address |
| Drop the stray-context-descriptor skip (**NX-01**) | `RxRing.DoesNotMistakeAStrayContextDescriptorForAFrame` fails |
| Drop the `CTXT` test on the context slot (**NX-01**) | `RxRing.DoesNotReadTheNextFrameAsATimestamp` fails — **and this test exists only because that mutation survived the first round** |
| Mark the sockets created only once the whole sequence succeeded (**NX-02**) | 3 `Lifecycle` tests fail, reporting a socket the stack still holds |
| Leave the transport set across a teardown (**NX-02**) | 4 `Lifecycle` tests fail |

Do the same for any new characterisation test: break the thing, watch it go red, put it back.

Three things this exercise taught, each of which changed the code:

- **F57 presents as a hang, not a wrong value.** With the per-pass count hoisted, `found` never
  reaches zero and the drain loop spins forever. Without a timeout the suite wedges and CI waits
  for its job limit instead of reporting anything. That is why `add_host_test()` sets
  `TIMEOUT 10` — 250× headroom over the 0.04 s the suite actually takes.

- **A third NX-01 branch survived the first mutation round.** Checking that the context slot is
  application-owned is not sufficient, because an ordinary frame in that slot is
  application-owned too — so `if (1)` in place of the `CTXT` test passed the whole suite. The
  discriminating case needs a TSA frame with a *normal* frame behind it, and it asserts both
  halves: that the following frame is not read as a time, and that it is not swallowed.
  Mutation testing found the gap; the test was written afterwards, which is the wrong order and
  is why the round is run at all.

- **The first wrap-ordering test could not fail.** Four timestamped frames straddling the wrap
  are ordered identically by correct modular arithmetic and by the buggy widened subtraction. A
  discriminating case needs two candidates pending *simultaneously* on opposite sides of the
  wrap with neither at delta 1, which is why that test now sends untimestamped filler frames to
  open a gap.

- **One test was vacuous, and only mutation testing exposed it.** The RFC 1112 mask check
  originally asserted against 224.0.1.129, whose third octet is `0x00` — so it passed whether
  the mask was `0x7F` or `0xFF`. It is now exercised through `ptp_endpoint_ipv4_group_mac()`
  with 239.129.1.1, whose third octet is `0x81` and therefore puts the bit in play.

## Known gaps

Stated plainly, because a gap that is written down is a decision and a gap that is not is an
accident.

- **Codegen defects are invisible to tiers 1 and 2.** F70 (a bounded wait optimised away above
  `-Og`) and F62 (an optimisation-dependent HardFault from a byte access the silicon rejects)
  are properties of the ARM build. The host tests compile different code, with a different
  compiler, for a different instruction set. A green run is *no evidence* about either, and is
  most dangerous precisely because it feels like it is.

- **Memory ordering is not modelled.** `__DSB`/`__DMB` are no-ops in the host shim.

- **`drv_rx_dropped_nopacket` is instrumented but unexercised.** The counter was added after an
  external review found it declared, printed and never incremented. Attempts to make it fire —
  53 000 UDP frames at line rate, then again with the packet pool cut from 8 to 2 — produced
  zero drops. The packet pool is not the bottleneck; the rx message queue is, and
  `ETHHW_ReadCallback()` drops events on a full queue *silently and without a counter*. That is
  the drop path that actually bites under load and it remains uninstrumented.

- **The `ETHHW_Init()` GPIO/RCC/NVIC preamble has no seam.** It is the one unparameterised block
  in `mac_drv.c` (roughly lines 51–111). Tier 2 of the original plan would have added one; it
  was not built, because the descriptor state machine was the part with the finding history.

- **The RAM-driver tier was never built.** Running real NetX Duo end-to-end on the host over a
  RAM driver was planned as S5e Option A and deliberately skipped — the recommendation was to
  pull it in only if board access is lost or the submodules start being bumped routinely. See
  [threadx_s5e_plan.md](threadx_s5e_plan.md).

- **gPTP reaching `SLAVE` is unverifiable on this bench.** The host NIC has no PTP hardware
  clock, so 802.1AS `asCapable` never asserts. Receive is proven; selection is not. See
  [host_ptp_setup.md](host_ptp_setup.md).

- **Timing, preemption and races stay on the bench**, as does everything downstream of the
  timestamp capture itself — the PHY, the clock tree, and the wire.
