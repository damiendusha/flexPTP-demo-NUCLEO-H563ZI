# ThreadX + NetX Duo porting plan

Execution plan for replacing FreeRTOS with **Eclipse ThreadX** and lwIP with **Eclipse NetX Duo**,
using ST's CMSIS-RTOS2 wrapper, keeping flexPTP and the project's own MAC driver.

Rationale, evidence and the supply-chain decision are in
[threadx_netxduo_feasibility.md](threadx_netxduo_feasibility.md). This document is the *how*.

Companion documents: [architecture.md](architecture.md) ·
[implementation_plan.md](implementation_plan.md) (the original H743→H563 port, same format) ·
[port_review.md](port_review.md) ·
**[threadx_s6_handoff.md](threadx_s6_handoff.md) — start here to pick up the outstanding stage**

> **Status: NOT STARTED.** Written for approval. No implementation work has begun.

## Table of Contents

- [Decision recap](#decision-recap)
- [Bench prerequisites — verified](#bench-prerequisites--verified)
- [Progress table](#progress-table)
- [Question 1 — Hardware testing after every stage](#question-1--hardware-testing-after-every-stage)
- [Question 2 — C++ and GTest](#question-2--c-and-gtest)
- [Question 3 — Which agent should implement this](#question-3--which-agent-should-implement-this)
- [Stage detail](#stage-detail)
- [Rejected alternatives](#rejected-alternatives)
- [Rollback and abort criteria](#rollback-and-abort-criteria)

---

## Decision recap

| Component | Source | Licence |
|---|---|---|
| Kernel | [eclipse-threadx/threadx](https://github.com/eclipse-threadx/threadx) @ 6.5.1 | MIT |
| Network stack | [eclipse-threadx/netxduo](https://github.com/eclipse-threadx/netxduo) @ 6.5.1 | MIT |
| CMSIS-RTOS2 shim | [stm32-mw-cmsis-rtos-tx](https://github.com/STMicroelectronics/stm32-mw-cmsis-rtos-tx) | SLA0044 |
| PTP | flexPTP (unchanged submodule), bound via `FLEXPTP_CMSIS_OS2` | |
| MAC / PHY | `Drivers/EthDrv/` — **unchanged** | |

Consequences of taking Eclipse upstream rather than ST's fork — chiefly that
`tx_initialize_low_level.S` must be copied from `example_build/` and adapted, and that ST's
`nx_stm32_eth_driver.c` is not vendored (it is unusable here anyway) — are itemised in the
feasibility report's [Choosing the supply chain](threadx_netxduo_feasibility.md#choosing-the-supply-chain).

---

## Bench prerequisites — verified

Checked on this machine on 14 Aug 2026, not assumed:

| Requirement | State |
|---|---|
| Board attached | ✅ `/dev/ttyACM0`, USB `0483:374e` STMicroelectronics **STLINK-V3** |
| OpenOCD (ST fork) | ✅ built at `toolchains/openocd/bin/openocd` |
| `ptp4l` / `phc2sys` | ✅ `/usr/sbin/` — a PTP master is available on the host |
| pyserial | ✅ 3.5 — the CLI can be driven from a script |
| Host C++ toolchain | ✅ g++ 15.2.0, CMake 4.2.3, Ninja 1.13.2 |
| GoogleTest | ❌ not installed — fetch via CMake `FetchContent`, the pattern already used for the FreeRTOS kernel |
| CI | ✅ `.github/workflows/ci.yml` — Debug+Release build, first-party warning gate, patch-applied check, docs link check |

**Everything needed for hardware-in-the-loop testing after every stage is already present.**
That materially changes the plan: it makes per-stage hardware verification the default rather
than an aspiration.

---

## Progress table

Legend: ✅ done · 🔄 in progress · ⬜ pending · ⏸️ optional/deferred

| # | Stage | HW test | Est. | Status |
|---|---|---|---|---|
| **P0** | Feasibility study, supply-chain evidence gathering | — | — | ✅ done |
| **P1** | Supply-chain decision (Eclipse + ST wrapper) | — | — | ✅ done |
| **P2** | This execution plan | — | — | ✅ done |
| **P3** | Plan approved by the maintainer | — | — | ⬜ **pending — blocks everything below** |
| **S0** | Baseline capture + HIL harness (`tools/hil/`) | ✅ 8/8 green, twice | 1 d | ✅ **done** |
| **S1** | C++ proof TU + host GTest tree (`test/`) | ✅ 9/9, `cpp_ctor: ok`, no regressions | 0.5–1 d | ✅ **done** |
| **S1b** | Tier 1+2 host harness: real ST headers + descriptor-ring model; characterisation tests pinning current-driver behaviour | ⬜ host only | 1–1.5 d | ✅ **done — 25 tests, 0.06 s; F57/F61/F71 + wrap ordering all mutation-verified; no production change needed** |
| **S2** | Vendor ThreadX + wrapper; `tx_user.h`; low-level init; linker script | ✅ 9/9, image byte-identical | 0.5–1 d | ✅ **done** |
| **S3** | Kernel swap: CMSIS-RTOS2 on ThreadX, `main.c` reorder, hooks, `osinfo` — **lwIP removed, no network** | ✅ 5/5 | 1.5–2 d | ✅ **done** |
| **S4** | Vendor NetX Duo; `nx_user.h`; IP instance + packet pool; **stub driver** | ✅ 5/5, IP thread alive | 0.5 d | ✅ **done** |
| **S5a** | Driver: `INITIALIZE` / `ENABLE` / `DISABLE` / `GET_STATUS` | ✅ 4/4, PHY + link UP | 0.5 d | ✅ **done** |
| **S5b** | Driver: RX path → `nx_link_ethernet_packet_received()` | ✅ (with S5c) | 1 d | ✅ **done** |
| **S5c** | Driver: TX path, multicast join/leave, deferred processing | ✅ ping 0% @ 1400 B, 0.337 ms | 1 d | ✅ **done** |
| **S5d** | DHCP client | ✅ 192.168.2.191, 6/6 vs baseline | 0.5 d | ✅ **done** |
| **S5e** | *Tier 3:* real ThreadX (`ports/linux`) + real NetX Duo + RAM driver on the host | ⬜ host only | 1–2 d | ⬜ pending |
| **S6** | NSD: UDP/IPv4 E2E + TX-timestamp deferral fix | 🔄 receive proven end-to-end; **BMCA selects no master** | 2–3 d | 🔄 **in progress** |
| **S7** | 802.3 transport, P2P, all three profile presets | ✅ each preset against `ptp4l` | 1–2 d | ⬜ pending |
| **S8** | Cleanup, docs rewrite, CI update, final review | ✅ full regression suite vs S0 baseline | 1.5–2 d | ⬜ pending |
| **B1** | *Bonus:* unit-test the F42 TX-ring ordering logic | ⏸️ | 0.5 d | ⏸️ optional |

**Total: 14–20 working days.** Two days above the feasibility estimate, all of it the S1b/S5e
host-test tiers — bought back if either catches one silent defect, which the finding log suggests
is likely. S6 still carries the variance.

> Update the Status column as stages land. Add a one-line dated note under each stage's detail
> section recording what was actually observed on hardware — the same discipline
> [implementation_plan.md](implementation_plan.md) uses.

---

## Question 1 — Hardware testing after every stage

**Yes, and the bench already supports it.** Ten of the thirteen stages end in a flashable image
with something new and observable; the three that do not (P3, S2, and the build-only half of S1)
are explicitly marked.

### The staging is *designed around* this constraint

The natural shape of this port is two big swaps, each unobservable until complete. That is the
failure mode this project's own history argues hardest against — F42 cost days precisely because
a broken thing looked fine. So the network half is split into four sub-stages (S5a–S5d), each
adding one driver capability that a host-side command can prove:

| Sub-stage | Capability added | How the host proves it |
|---|---|---|
| S5a | Link state | Unplug/replug the cable; console prints `ETH LINK: UP/DOWN` |
| S5b | Receive | `arping <board>` from the host gets replies — proves RX + the IP thread |
| S5c | Transmit | `ping -c 300 -s 1400` at 0% loss — proves TX, buffer handling, and no leaks |
| S5d | DHCP | Address appears on the console and in the router's lease table |

Each is a forward step; nothing here is throwaway.

### The HIL harness (stage S0)

Rather than re-typing CLI commands after every flash, S0 builds `tools/hil/`:

```
tools/hil/
  hil.py          # serial CLI driver: connect, expect(), cmd(), reset via OpenOCD
  flash.py        # wraps toolchains/openocd/bin/openocd program+verify+reset
  checks.py       # the named checks below
  baseline.json   # golden values captured from the pre-port firmware
  run.py          # ./run.py --stage S5c  -> runs the checks valid at that stage
```

Named checks, each independently runnable so a stage can assert only what should work yet:

| Check | Asserts | Valid from |
|---|---|---|
| `boot` | Reset → banner within 3 s, no fault handler output | S0 |
| `cli` | `?` lists the expected command set and slot count | S0 |
| `osinfo` | Kernel name/version and heap figures parse and are non-degenerate | S0 |
| `cpp_ctor` | The C++ proof TU's static constructor printed its marker | S1 |
| `threads` | Expected thread names and priorities are present | S3 |
| `link` | Link up/down transitions are reported | S5a |
| `arp` | Host `arping` succeeds | S5b |
| `ping` | 300 × 1400 B at 0% loss; RTT within 2× of baseline | S5c |
| `dhcp` | Address acquired within 15 s | S5d |
| `ptp_start` | `flexptp` starts; `ptp addend` == `3435973836` | S6 |
| `ptp_ts` | RX **and TX** timestamp counters both non-zero and 1:1 with messages sent | S6 |
| `ptp_sync` | Offset converges below the baseline threshold within N minutes against `ptp4l` | S6 |
| `ptp_profiles` | `default`, `gPTP`, `defp2p` each reach sync | S7 |
| `regression` | All of the above, compared against `baseline.json` | S8 |

**`ptp_ts` is the check that exists because of the feasibility report's finding 1.** flexPTP's
CMSIS-RTOS2 binding silently discards TX timestamps; a sync test alone may still look plausible
while `t3` is missing. Counting timestamps 1:1 is what makes that failure loud. Instrument the
count the way F61's investigation did, and keep the counter behind a CLI command
(`ptp diag ts`) so the check can read it.

### Baseline first

S0 runs the whole suite against **today's FreeRTOS/lwIP firmware** and writes `baseline.json`.
Every later stage compares against it. Without this the port has nothing to be "as good as",
and questions like "was the RTT always 0.40 ms?" become unanswerable.

Capture at minimum: banner text, CLI command list, `ping` loss and mean RTT, DHCP acquisition
time, `ptp addend`, and a 15-minute PTP offset trace against `ptp4l` with
`tools/linuxptp/flexptp-gm.cfg`.

---

## Question 2 — C++ and GTest

### The finding: C++ is already fully enabled. Nobody has used it.

I expected this to be a stage. It is not — every prerequisite is already in the tree:

| Prerequisite | State | Where |
|---|---|---|
| `enable_language(CXX)` | ✅ | `CMakeLists.txt:12` |
| C++20 selected | ✅ | `CMakeLists.txt:19-21` |
| `-fno-rtti -fno-exceptions -fno-threadsafe-statics` | ✅ **already set** | `cmake/gcc-arm-none-eabi.cmake:14` |
| `arm-none-eabi-g++` configured | ✅ | `cmake/gcc-arm-none-eabi.cmake:23` |
| `-lstdc++ -lsupc++` on the link line | ✅ | `CMakeLists.txt:211-212` |
| `.init_array` / `.fini_array` / `.ARM.extab` / `.ARM.exidx` | ✅ | `stm32h563zitx_FLASH.ld:93-125` |
| `__libc_init_array()` called before `main()` | ✅ | `Startup/startup_stm32h563zitx.s:85` |
| Any `.cpp` file in the tree | ❌ **none** | — |

So static constructors will run, exceptions and RTTI are already off, and the unwind sections are
placed. **The whole C++ enablement stage reduces to adding one proof translation unit and
confirming on hardware that it links, that its constructor ran, and that flash size did not jump.**
That is stage S1, half a day, and it is worth doing precisely because the configuration is
untested — a linker script that *looks* right is exactly the class of thing this project has been
bitten by before.

Two rules for firmware C++, to be stated in the code and enforced by review:

- **No dynamic allocation.** No `new`, no `std::vector`, no `std::string`, no `std::function`.
  Static storage, fixed-capacity containers, and `span`-like views only. ThreadX has no heap;
  `operator new` would fall through to newlib `malloc` → `_sbrk` in `Src/sysmem.c`, which is not
  what you want under an RTOS.
- **`extern "C"` at every boundary.** flexPTP's NSD interface, NetX Duo's driver entry and the
  MAC driver's callbacks are all C. C++ lives strictly inside the implementation.

### What should be C++, and what GTest actually buys

The value is not "C++ is nicer". It is that **three pieces of new logic in this port are pure
functions of their inputs, are the exact places the feasibility report flags as high-risk, and
are currently untestable except by plugging in a cable.**

| Unit | What it is | Why it is worth testing | Maps to risk |
|---|---|---|---|
| **`PtpTxRegistry`** | `NX_PACKET*` → flexPTP `uid` correlation table, populated before send, consumed when the MAC reports a TX timestamp | Fixed capacity, entries expire, and NetX **reuses packet pointers** — so a stale entry can hand the wrong `uid` to the servo. Silent, intermittent, and near-impossible to catch on the bench | Finding 1 (TX timestamps) |
| **`PtpTsCodec`** | Extract `{nano_second, second_low, second_high}` from the 12 bytes at `nx_packet_data_start`, verify head-room, convert to flexPTP's `(sec, nsec)` | An undocumented layout convention carrying the timestamp through three protocol layers. Exactly the F48 class of defect | Finding 3 (layout dependency) |
| **`PtpEndpoint`** | The transport × delay-mechanism decision table: destination IPv4, destination MAC, UDP port, EtherType, and which multicast groups to join/leave | Four combinations plus a teardown case, currently a nest of ternaries in `nsd_lwip.c`. Cheap to test, easy to get subtly wrong, and a wrong destination fails as "no response received" | S7 profile work |

Test cases worth naming now, because they are the ones that find bugs:

- `PtpTxRegistry`: capacity exhaustion; **pointer reuse (ABA)** — free a packet, allocate a new
  one at the same address, confirm the stale `uid` is not returned; expiry by tick; lookup of a
  never-registered pointer; double-free.
- `PtpTsCodec`: zero timestamp; second rollover; **head-room below 12 bytes must be rejected, not
  silently read**; unaligned `data_start`.
- `PtpEndpoint`: all four `(transport, delay_mechanism)` pairs for both send and join/leave; the
  `(-1, -1)` teardown case that `ptp_nsd_init()` uses; and that `gPTP`'s
  `01:80:C2:00:00:0E` is never selected for E2E.

### Host test tree

The top-level `CMakeLists.txt` includes the ARM toolchain file unconditionally at line 7, so the
host build **must be a separate CMake root** — not a flag. Proposed:

```
test/
  CMakeLists.txt        # its own project(); FetchContent GoogleTest, pinned
  fakes/
    nx_api.h            # minimal fake NX_PACKET: the four pointers + length
    nx_link.h           # NX_LINK_TIME only
  test_ptp_tx_registry.cpp
  test_ptp_ts_codec.cpp
  test_ptp_endpoint.cpp
```

Built with the host g++, linking the *same* `.cpp` files the firmware compiles. The fake headers
are deliberately tiny: if a fake grows past a screen, the unit under test has too much coupling
and should be reshaped rather than the fake extended.

Wire `ctest` into `.github/workflows/ci.yml` as a third job alongside `build` and `docs`. Host
tests run in seconds and need no board, so they gate every push — which is most of the point.

### How far mocks actually reach

An earlier draft of this document claimed "roughly 80% of the risk stays on the bench". That was
a guess, and it was wrong. Derived against this repository's own finding log, the number is closer
to **half**, and the reason is structural: **the hardware seam this codebase would need for host
testing already exists, in C, by accident of good design.**

#### `mac_drv.c` is already host-compilable

97 of its 98 register accesses go through the `ETH_TypeDef *eth` function parameter, not a
hardcoded base address:

```c
void ETHHW_ProcessTx(ETH_TypeDef *eth) { ... eth->DMACSR ... }
```

`ETH_TypeDef` is a plain struct of `volatile uint32_t`. On the host you allocate one, zero it,
and pass its address. The descriptor rings are reached through `eth->DMACRDLAR` / `eth->DMACTDLAR`,
so they are plain host memory too — and `ETHHW_GetState()` derives the driver state by subtracting
one from `DMACRDLAR`, which works identically.

The single exception is `WRITE_REG(ETH->DMACRDTPR, 0)` in `ETHHW_RestoreRXDesc()`, which is
**deliberate and documented in place** (that function takes no `eth` parameter by design). On the
host it needs one accommodation: define `ETH` as a pointer to the fake. One line.

The only hardware coupling that is *not* parameterised is the GPIO/RCC/NVIC preamble at the top
of `ETHHW_Init()` (roughly `mac_drv.c:51-111`) — a contiguous block of HAL calls that wants a
seam of its own.

**So the descriptor state machine and the register programming are testable today, with no
rewrite of `mac_drv.c` and no CRTP.**

#### What that buys, measured against the finding log

Of the ~23 findings in [port_review.md](port_review.md) that fall in the code this port writes or
rewrites:

| Reach | Count | Findings |
|---|---|---|
| **Catchable by a host test** | 11 | **F57** (TX ring per-pass count never reset), **F61** (RX re-delivery — 57% of frames), **F58** (RX copy never advanced the source pointer), **F75** (dropped TX reported as sent), **F54** (unbounded hardware waits — a mock that never asserts ready catches this instantly), **F69** (unlisted PHY model → boot HardFault), **F67** (console FIFO could not tell full from empty), **F74** (CLI tokenizer overflow), **F59** (servo NaN latch on a zero Sync period), **F60**/**F66** (embfmt) |
| **Regression-lockable, but discovery needed hardware** | 6 | **F71** (RBU latch), **F76** (FCS handed up), **F52** (addend must follow the increment), **F68** (blocking call from an ISR), **F72** (silent assertions), **F73** (missing core lock) |
| **Irreducibly hardware** | 6+ | **F70** (bounded wait compiled away above `-Og`), **F62** (optimisation-dependent HardFault), **F51** (UID region rejects byte access), **F53** (PTP counter not clocked), **F42** (a switch discarding `Delay_Resp`), plus the clock-tree findings F3–F5 |

Roughly **50% reachable, 25% regression-lockable, 25% never**.

> **The asymmetry that matters more than the ratio:** mocks are excellent at *preventing
> recurrence* and poor at *first discovery*. F71 is the clearest case — the RX ring's RBU latch
> needs a tail-pointer write to restart, and a mock would only model that if its author already
> knew. The model would have been wrong in exactly the way the code was wrong, and the suite would
> have been green. Every finding in the middle row shares that shape.

#### Does CRTP help?

**Mostly no, and reaching for it here would be over-engineering.** CRTP earns its keep when a C++
class needs a hardware policy inlined with no indirect call. This codebase already has that
property for free: the policy is a function parameter, and the compiler already sees straight
through it on target. Introducing a CRTP seam over `mac_drv.c` would mean C++-ifying the most
expensive verified artefact in the repository — the rewrite this plan
[explicitly rejects](#rejected-alternatives) — to obtain a testability property it already has.

Where CRTP (or any static policy) *is* the right tool:

| Site | Why |
|---|---|
| The `ETHHW_Init()` GPIO/RCC/NVIC preamble | The one unparameterised block. Though a weak symbol or a function pointer is simpler still, and this is called once at boot, so even a virtual call would cost nothing |
| **New C++ in the NetX driver's ring handling** | If S5b/S5c write their ring bookkeeping in C++, a `template <typename Hw> class RxRing` is apt — the policy is genuinely compile-time and the hot path is per-frame |
| `PtpTxRegistry`'s clock source | Needed for expiry tests. A template parameter or an injected function is fine; CRTP is heavier than the problem |

The hardware *fake* itself should be a plain class the test drives — one that models OWN-bit
transitions, context-descriptor timestamp injection, and RBU on ring exhaustion — not a CRTP
policy. It is a model, and it wants to be readable.

#### Three tiers, in cost order

| Tier | What | Cost | Reaches |
|---|---|---|---|
| **1** | Host-compile `mac_drv.c` unchanged against a fake `ETH_TypeDef` + host descriptor ring | ~1 d | Descriptor state machine, ring ordering, register programming, timeout paths |
| **2** | Seam the `ETHHW_Init()` GPIO/RCC/NVIC preamble | ~0.5 d | Init sequencing and register configuration |
| **3** | **Run the real stack on the host** — ThreadX's `ports/linux` + real NetX Duo + the real NSD over a RAM driver | ~1–2 d | NSD end-to-end, socket lifecycle, IGMP, packet pool behaviour, the ISR/queue contract of finding 1 |

Tier 3 deserves emphasis: **do not mock NetX Duo.** It is portable C, it has a `ports/linux`
target, and upstream runs a `regression_test.yml` workflow against it. Linking the real stack is
both less work and far higher fidelity than any fake.

#### The limits that remain, stated plainly

- **Codegen defects are invisible.** F70 and F62 were both optimisation-dependent — a missing
  `volatile`, and a byte access the silicon rejects. Host tests compile different code with a
  different compiler for a different instruction set. A green suite offers *no* evidence here,
  and is most dangerous precisely because it feels like it does.
- **Mock fidelity is an unbounded liability.** Every behaviour the model gets wrong is a test that
  passes for the wrong reason. Keep the fake small, and write down what it does *not* model.
- **Avoid change-detector tests.** Assert the ring's observable behaviour, not every MMIO store.
  A test that pins each register write will fail on every legitimate refactor and catch nothing.
- **Timing, preemption and races stay on the bench**, as does everything downstream of the
  timestamp capture itself.

**Revised bottom line: about half the port's silent-failure risk becomes reachable off-bench, and
the descriptor state machine — historically this repository's richest source of silent defects —
moves almost entirely into CI.** That is a much stronger case for host testing than the earlier
draft made. It does not reduce the S6 hardware checks by one line.

### Bonus, deliberately kept optional (B1)

`ETHHW_ProcessTx()`'s modular `int16_t` ordering logic — the F42 fix — is the single best unit-test
candidate in the repository, and a host test would have caught the original bug. But extracting it
means editing verified, working code in `mac_drv.c`, which this port otherwise does not touch.
**Not part of the port.** Worth doing afterwards, on its own branch, with its own hardware
regression.

---

## Question 3 — Which agent should implement this

Interpreting the question as: model tier, reasoning effort, and inline-vs-subagent per stage.
(If "untracode" refers to a specific tool I could not identify it — the axes below are the ones
this harness actually exposes.)

### The general recommendation: run this inline, not in subagents

Every subagent spawn starts cold. This port depends on facts established across
`architecture.md`, `port_review.md`, the feasibility study and this plan — the timestamp path,
F42/F48/F53/F58/F61/F71, the supply-chain decision, the `inFlight` discipline. A fresh agent
re-derives all of it, usually less accurately, and its report is a summary rather than the work.
**For the stages that matter, a single session carrying the context beats fan-out.**

Subagents earn their place in exactly two spots, both noted below.

### Per stage

| Stage | Model / effort | Mode | Why |
|---|---|---|---|
| S0 HIL harness | **Sonnet 5** | inline, or a worktree subagent | Self-contained Python against a documented CLI. Well-specified, verifiable by running it. Good delegation candidate — needs almost no repo context |
| S1 C++ + GTest tree | **Sonnet 5** | inline | Mechanical: one TU, one CMake root, three test files. The design is already fixed above |
| S2 Vendor + build | **Sonnet 5** | inline | Submodules, CMake, `tx_user.h`. Verified by the build |
| S3 Kernel swap | **Opus 5, high** | inline | Touches `main.c` ordering, the fault handlers, the priority model and the failure hooks. Several traps documented in the feasibility report; getting the ordering wrong produces a board that does not boot |
| S4 NetX vendoring | **Sonnet 5** | inline | Configuration |
| **S5a–S5d Driver** | **Opus 5, high** | inline | The `inFlight` discipline (F61), the descriptor lifecycle, ISR/thread handoff. A subtly wrong driver passes `ping` and corrupts PTP |
| **S6 NSD + TX timestamps** | **Opus 5, high** | inline | The single highest-risk stage. Finding 1 lives here, and its failure mode is a plausible-looking clock |
| S7 Profiles | **Opus 5** | inline | Mostly bench work; the host must be reconfigured per profile and the failure modes are network-level, not code-level |
| S8 Docs + cleanup | **Sonnet 5** | inline | Large, careful, but low-risk prose and CI edits |
| Residual-FreeRTOS sweep | **Explore agent** | subagent | Genuine fan-out: "find every remaining reference to FreeRTOS symbols, headers or docs across the tree." Exactly what it is for |
| Post-S6 and pre-S8 review | **`/code-review ultra`** | **you must launch it** | Multi-agent branch review. It is user-triggered and billed — I cannot start it. Run it after S6 lands and again before S8 closes |

### Hardware stages cannot be delegated

S5a–S7 need the board, the cable, the console and a human watching an LED. Any agent doing this
work must be in an interactive session with Bash access to `/dev/ttyACM0` and OpenOCD — not a
background task. There is **one board**, so the hardware stages also cannot be parallelised
across agents.

The only genuinely parallelisable work is S0 and S1 (host-side, no board), which could run in a
`worktree`-isolated agent while baseline capture happens on the bench.

### Practical note on effort

Prefer fewer, longer sessions over many short ones for S3–S6. The expensive part of each stage is
reconstructing why the previous decision was made; that is free within a session and costs a
document re-read across sessions. Where a session must end mid-stage, write the state into this
document's stage-detail section before stopping.

---

## Stage detail

### S0 — Baseline capture and HIL harness

**Deliverables:** `tools/hil/` as specified above; `baseline.json`; a short `tools/hil/README.md`.

**Steps**
1. Build and flash the current firmware from a clean tree (`./build.sh`), confirming the two
   submodule patches applied.
2. Write `hil.py` (serial), `flash.py` (OpenOCD), `checks.py`.
3. Run the full suite; commit `baseline.json`.
4. Record the 15-minute PTP offset trace against `ptp4l`.

**Hardware verification:** the whole suite, on the pre-port firmware. If a check cannot pass
*before* any porting, the check is wrong — fix it now, not during S6.

**Exit criteria:** `./tools/hil/run.py --stage S0` green, twice in a row, from a power cycle.

#### Progress — 14 Aug 2026 — **DONE**

Harness delivered: `tools/hil/{hil,checks,run}.py`, `setup_host_net.sh`, `README.md`.
Baseline firmware built (Debug, FLASH 216 888 B / 10.34%, SRAM1 133 160 B / 50.80%), flashed
and verified. **`--stage S0` green 8/8, twice in a row, second run compared against the
saved baseline with no regressions.**

| Check | Baseline value |
|---|---|
| `boot` | banner, `PHY: SMSC LAN8742A (rev. 1) @0`, `ETH LINK: UP (100 Mbps, FULL duplex)` |
| `cli` | **23/64** registered commands |
| `osinfo` | FreeRTOS V11.3.0; 13 840 B free idle, 10 752 B with flexPTP running |
| `dhcp` | `192.168.2.191` from the host's dnsmasq |
| `ping` | 300 × 1400 B, **0% loss, 0.365 ms** mean RTT |
| `ptp_start` | addend **3435973836**, exactly nominal; clock ID `510400feff1e0056` |
| `ptp_ts` | **T2 ×20, T3 ×20, both advancing** |
| `ptp_sync` | `SLAVE`, 30 samples, **|offset| ≤ 11.8 µs** |

The offset figure is the host's software-timestamping floor, not the board: the board-facing
NIC is an RTL8125 reporting `PTP Hardware Clock: none`, so `ptp4l` runs with `-S`. Consistent
with the 15–26 µs typical magnitude already recorded in [host_ptp_setup.md](host_ptp_setup.md).

**Three checks were wrong, and finding that out is what this stage is for.** All three passed
only after being fixed; none was a firmware defect:

| Check | Why it was wrong |
|---|---|
| `ptp_start` | Asserted the addend *equalled* nominal. It is a tuning register — once the servo disciplines, it moves (observed 3435888045 mid-lock). Now asserts a ±1% band, which still catches F2's addend-of-zero |
| `ptp_ts` | Counted `--->`/`<---`, which is `PTP_LOG_TRANSMISSION` — a channel with **no CLI switch** (F56). `ptp log ts` is `PTP_LOG_TIMESTAMPS` and prints T1–T4 blocks. Now parses those, which is a better signal anyway: **T2 is our receive stamp and T3 our transmit stamp**, so asserting both are present, non-zero and advancing is a direct test for the finding-1 failure mode |
| `ptp_sync` | Watched the console for `-> SLAVE`. That transition happens once; whichever PTP check ran second could never see it. Now probes state via `ptp info` (master clock ID ≠ own clock ID), so both checks are independently runnable |

Stage order also changed: `ptp_ts` now runs **before** `ptp_sync`, so a converging-looking
offset is never believed before transmit timestamps have been shown to exist.

Incidental finding, not a defect: flexPTP starts and reaches `LISTENING` with no IP address
at all, contrary to the banner's advice to wait for DHCP.

### S1 — C++ proof TU and host GTest tree

**Deliverables:** one `.cpp` in the firmware; `test/` host tree; a CI job running `ctest`.

**Steps**
1. Add `Src/version_banner.cpp` (or similar) with a file-scope object whose constructor records a
   marker the banner prints. Small, real, and it proves `__libc_init_array` reached C++.
2. Confirm the flash delta is small and the map file shows the `.init_array` entry.
3. Create `test/CMakeLists.txt` as a separate project; `FetchContent` GoogleTest at a **pinned
   tag** (follow the FreeRTOS precedent in `Middlewares/CMakeLists.txt`, and the reasoning in the
   comment there about pinning).
4. Add the three fakes and one trivial passing test to prove the tree.
5. Add the `ctest` job to `.github/workflows/ci.yml`.

**Hardware verification:** flash; `cpp_ctor` check passes; `boot`, `cli`, `osinfo`, `ping`,
`ptp_sync` all still match baseline. **Nothing else may change** — this stage is a control.

**Exit criteria:** hardware regression identical to S0; `ctest` green locally and in CI.

#### Progress — 14 Aug 2026 — **DONE**

`Src/cpp_support.cpp` added and reported through `osinfo`. Every C++ prerequisite was already
configured in this project and had simply never been exercised, so the stage cost almost
nothing — but it was worth doing, because a configuration that only *looks* right is the class
of thing F62 and F70 both were.

Verified on hardware: `osinfo` prints `C++ static init: ok (C++202002)`, and the map confirms
the chain rather than assuming it — `.init_array` holds three entries, the third being
`Src/cpp_support.cpp.obj` at `0x080345d0`, with `_GLOBAL__sub_I_cpp_static_init_status` linked
at `0x08004a20`.

The probe distinguishes the two failure modes it can have: `DID NOT RUN` if dynamic
initialisation never happened, `RAN BUT CLOBBERED` if the constructor ran but a bad
`.data`/`.bss` layout overwrote the object afterwards. Only `ok` passes.

Cost: **FLASH +272 B** (216 888 → 217 160, 10.34% → 10.35%), SRAM1 +8 B. Full suite 9/9 with
no regressions against the S0 baseline.

---

### S2 — Vendor ThreadX and the CMSIS-RTOS2 wrapper

**Deliverables:** two new submodules; `Inc/tx_user.h`; adapted `tx_initialize_low_level.S`;
linker-script region; CMake integration. **Does not link yet** — FreeRTOS is still present.

**Key configuration decisions, all with a reason:**

| Setting | Value | Why |
|---|---|---|
| `TX_MAX_PRIORITIES` | **64** | ST's wrapper `#error`s on anything else |
| `TX_TIMER_TICKS_PER_SECOND` | **1000** | Matches `configTICK_RATE_HZ`; keeps flexPTP's 31 ms heartbeat and `FLEXPTP_MS_TO_TICKS` behaviour identical. The port default is 100 |
| `SYSTEM_CLOCK` (assembler) | **250000000** | `tx_initialize_low_level.S` computes the SysTick reload from it |
| `TX_TIMER_PROCESS_IN_ISR` | **must NOT be defined** | flexPTP's heartbeat callback enqueues with `osWaitForever`, which is illegal from an ISR — see finding 1 |
| `TX_ENABLE_STACK_CHECKING` | on (Debug) | Complements, does not replace, the `PSPLIM` hardware trap |
| `TX_PORT_USE_BASEPRI` | **not set** initially | PRIMASK is the port default. Revisit only if an ISR above the old level-5 boundary is ever added |
| Byte pools | sized from the S3 thread inventory | ~15 KB of stacks today plus headroom; verify against `tx_byte_pool_info_get()` |

**Hardware verification:** the plan expected none, but there is a better test available and
it was used: **the firmware image must be unchanged.** ThreadX is built but not linked, so if
adding it moves FLASH or SRAM by a byte, something is wired in that should not be yet.

#### Progress — 14 Aug 2026 — **DONE**

Submodules pinned: `Modules/threadx` @ `v6.5.1.202602a_rel` (MIT), `Modules/cmsis_rtos_threadx`
@ `v1.4.1` (SLA0044). Both libraries build with **zero warnings**.

FLASH 217 160 B and SRAM1 133 168 B — **identical to S1**. Hardware suite 9/9, no regressions.

Four things the plan did not anticipate, each resolved:

| Issue | Resolution |
|---|---|
| The upstream `example_build/tx_initialize_low_level.S` defines its **own `HardFault_Handler` and `UsageFault_Handler`**, which collide with the reporting handlers in `Src/main.c` | Both omitted from the adapted copy. The project's handlers are what actually catch a stack overflow on this part (`UFSR.STKOF` via `PSPLIM`). What is given up is ThreadX's *recovery* path — S3 wires `_tx_thread_stack_error_handler` in as a notification before the halt |
| TrustZone secure-stack sources warn (`cmse_nonsecure_entry` without `-mcmse`) and are meaningless at TZEN=0 | Excluded from the build **and** `TX_SINGLE_MODE_NON_SECURE` defined. Both are needed: excluding without the define fails to link, defining without excluding merely warns |
| The wrapper `#error`s unless `TX_THREAD_USER_EXTENSION` is defined as exactly `ULONG tx_thread_detached_joinable;` | Added to `tx_user.h`. It backs `osThreadDetach`/`osThreadJoin` |
| The wrapper **forbids `TX_DISABLE_ERROR_CHECKING`** | Removed from `tx_user.h`. The intent had been to compile checks out in Release, mirroring `configASSERT()`; the wrapper depends on ThreadX returning error codes it can translate into `osStatus_t`. Accepted — the firmware is at 10% of flash |

Two configuration facts verified in the artefacts rather than assumed:

- `arm-none-eabi-nm` shows `U __RAM_segment_used_end__` in `libthreadx.a`, so the new
  `PROVIDE` in the linker script has a referent and will resolve when S3 links it. Until then
  `PROVIDE` correctly emits nothing.
- The SysTick reload word compiled to **`0x0003D08F` = 249 999 = 250 MHz / 1000 − 1**, proving
  `tx_user.h` reached the assembler and that both `SYSTEM_CLOCK` and
  `TX_TIMER_TICKS_PER_SECOND` took effect. A wrong tick rate here would otherwise not surface
  until flexPTP's heartbeat drifted in S6.

---

### S3 — Kernel swap

**Deliverables:** FreeRTOS and lwIP both removed; CMSIS-RTOS2 on ThreadX; a booting board with a
working console and CLI and **no networking**.

**The four things that must change, from the feasibility report:**
1. **`main.c` ordering.** `serial_io_init()` creates the `serialio` thread before
   `osKernelInitialize()`. ThreadX's byte pools do not exist that early. Split the function into
   hardware setup (before) and thread creation (after). The existing comment at `main.c:266-275`
   anticipates exactly this.
2. **Failure hooks.** `vApplicationStackOverflowHook`, `vApplicationMallocFailedHook`,
   `vAssertCalled`, `vApplicationTickHook`, `vApplicationIdleHook` → ThreadX equivalents, or
   removal with the reason recorded. **Keep the naked fault handlers unchanged** — they are what
   actually catches overflows on this part, and ThreadX's CM33 port sets `PSPLIM` too.
3. **`Src/cmds.c` `osinfo`.** `vPortGetHeapStats()` → `tx_byte_pool_info_get()`; report both pools.
4. **`Src/flexptp_port/ptp_port_stm32h563.c:119`.** `vTaskDelay(pdMS_TO_TICKS(10))` → `osDelay(10)`.

Also: `Inc/flexptp_options.h` switches `FLEXPTP_FREERTOS` → `FLEXPTP_CMSIS_OS2`, and the comment
there explaining the choice (F40) must be rewritten rather than deleted.

**Hardware verification:** `boot`, `cli`, `osinfo`, `cpp_ctor`, `threads`. Deliberately break a
thread's stack to confirm the `UsageFault` handler still reports `UFSR.STKOF` — the report claims
this survives the kernel change, and the claim should be tested, not trusted.

**Exit criteria:** board boots to a fully usable CLI on ThreadX. `ping` and PTP are expected to be
gone; the checks for them are not run at this stage.

#### Progress — 14 Aug 2026 — **DONE**

`--stage S3` green 5/5. FreeRTOS is gone from the image (`nm`: zero `xTaskCreate`/`vTaskDelay`/
`pvPortMalloc`/`xQueueGeneric` symbols, 65 `_tx_*`). FLASH **149 408 B / 7.12%** and SRAM1
**67 312 B / 25.68%**, down from 217 160 / 133 168 — lwIP, its 30 KB FreeRTOS heap and the
SRAM3 Ethernet buffers all left with it.

`osinfo` on the running board:

```
OS: ThreadX
Kernel version: 6.5.1
Free OS memory:
  stacks: 18656 bytes free in 5 fragments
  objects: 7108 bytes free in 12 fragments
Threads:
  System Timer Thread pri=0 (cmsis 56) state=3 runs=339
  serialio pri=32 (cmsis 24) state=0 runs=16
  init pri=32 (cmsis 24) state=4 runs=11
  cli pri=32 (cmsis 24) state=0 runs=8
C++ static init: ok (C++202002)
```

The three application threads land at ThreadX priority 32 = CMSIS 24, confirming the
wrapper's `tx = osPriorityISR - cmsis` mapping in the only way that counts.

**Three defects found on hardware, none predicted by the plan.**

**1. `osThreadNew()` rejects a zeroed priority — five threads would never have been created.**
The plan anticipated that priorities invert. It did not anticipate that the two CMSIS-RTOS2
implementations *disagree about `osPriority_t = 0`*. FreeRTOS's shim substitutes
`osPriorityNormal`; ST's ThreadX wrapper compares against `osPriorityIdle` (1), finds
`osPriorityNone` (0) below it, and **returns NULL**.
[architecture.md](architecture.md#threads) records that five of the nine threads relied on
that default. Symptom: the board booted, printed the banner, and halted in
`serial_io_start()` — which it could report only because S3 had already split USART3's
hardware init to before the kernel. Fixed by stating the priority at every `osThreadNew()`
site, which is what the architecture doc always claimed was happening.

**2. `MSG("%-10s", …)` is a HardFault, not a formatting wart.** embfmt's `pfn_string()` is
`string_copy(outbuf, va_arg(char *), free_space)` — no width, no left-justify, no NULL guard.
The `-` flag desynchronises the argument walk and the next `%s` dereferences whatever it
picks up: precise BusFault, `CFSR = 0x00008200`, faulting address `0x00000000`. Same family as
F60 and F66, and the fault handlers introduced for those printed the diagnosis directly.

**3. `%lu` prints garbage.** `C++%lu` of `202002` rendered as `C++867591983928594`. `unsigned
long` is 32-bit here, so `%u` with a cast is lossless. Also F60/F66 family.

Two smaller things resolved along the way:

- **flexPTP cannot build against an unrecognised network stack.** `ptp_defs.c` `#error`s
  unless it sees `LWIP`, `ETHLIB` or `__linux__`, while its own message asks the integrator to
  "define `PTP_IGMP_PRIMARY` and `PTP_IGMP_PEER_DELAY`" — for which it offers no mechanism,
  both being `const` objects it defines itself. Added as
  `patches/flexptp/0003-generic-netstack-igmp-addresses.patch`, a 2-line extension point.
  This is S6 work done early: the NetX Duo NSD would have hit it too. Worth reporting upstream.

- **The docs link checker went red**, because `docs/reviews/` and `port_review.md` reference
  files S3 deleted. Those links were correct when written. The checker now accepts that exact
  set by path rather than the archive being rewritten to hide it.

---

### S4 — Vendor NetX Duo, IP instance, stub driver

**Deliverables:** NetX Duo submodule; `Inc/nx_user.h`; `Src/ethernet/ethernet_netxduo.c` creating
the packet pool and IP instance with a driver that answers `INITIALIZE`/`ENABLE` and does nothing
else.

`nx_user.h` decisions: `NX_ENABLE_INTERFACE_CAPABILITY` **on** (required for the PTP timestamp
flag), IPv6 **off**, NetX Secure **not compiled**, packet pool sized against the current
`PBUF_POOL_SIZE = 8` arrangement and placed in SRAM3 alongside the DMA buffers.

**Hardware verification:** `boot`, `cli`, `threads` — the NetX IP thread must appear. No traffic.

---

### S5a–S5d — The driver

**Deliverable:** `Drivers/EthDrv/eth_drv_netxduo.c`, built on the **unchanged** `mac_drv.c`.

Carry these forward from `eth_drv_lwip.c` deliberately, not by accident:

- The **`inFlight` marker discipline** (F61). Without it the driver re-delivered 57% of frames.
  The mechanism is independent of the stack above it and must be reproduced exactly.
- The **`_Static_assert` on the state/descriptor layout** (`eth_drv_lwip.c:61`) — `ETHHW_GetState()`
  derives its pointer by subtracting one from `DMACRDLAR`.
- **`rx` thread above the console, below the IP thread**, and the reasoning for it.
- The **bounded over-length TX frame check** and `ERR_IF`-equivalent semantics (N14): a dropped
  frame must not be reported as a successful transmit.

Add one that is new: **assert the ≥12-byte head-room precondition** before
`nx_link_ethernet_packet_received()` can stash a timestamp over the Ethernet header.

#### S5b–S5d progress — 15 Aug 2026 — **DONE**

Landed together: the natural first proof of receive is an ARP reply, and a reply needs
transmit, so the two are not separately observable. `--stage S5d --compare baseline.json`
green **6/6, no regressions**. `ping` 300 × 1400 B at **0% loss, 0.337 ms** against the
baseline's 0.365 ms; DHCP returns **192.168.2.191**, the same address lwIP got.

**Four defects, all found on hardware, none catchable by the host tests.** Each is exactly the
class the feasibility study said would stay on the bench, and each was silent in a different
way. What made them tractable was `ethinfo`, a driver/stack counter dump added mid-stage:
"it does not work" is not a diagnosis, and the difference between *the driver never saw the
frame*, *the driver dropped it*, and *the stack refused it* is invisible from the console.

| # | Defect | How it presented |
|---|---|---|
| 1 | `nx_interface_link_up` set once at `NX_LINK_INITIALIZE` and never updated | Console said `ETH LINK: UP`, receive worked, and **the board transmitted nothing at all** — the IP thread will not send on an interface it believes is down. Host counted zero frames in twelve seconds |
| 2 | **No 2-byte alignment shim on receive** | `NX_PHYSICAL_HEADER` is **16**, not 14, precisely so a 14-byte Ethernet header leaves the IP header 4-byte aligned. Copying the frame to `prepend_ptr` directly put it at 2 mod 4 and **every IPv4 packet failed its header checksum** — `ip: rx 11 (invalid 11, dropped 11, cksum 11)` — while ARP, which has no checksum, round-tripped perfectly. That asymmetry is the signature |
| 3 | Checksum offload not declared to NetX Duo | `ETHHW_Transmit()` sets `CIC_IPHDR_PAYLOAD_INSERT_PHDR_CALC` unconditionally, so the MAC *always* inserts checksums. A stack that also computes them leaves a complete value where the MAC expects a pseudo-header, and every IP frame goes out wrong. lwIP was told via `CHECKSUM_BY_HARDWARE`; NetX Duo's equivalent is `nx_interface_capability_flag`, which is why `NX_ENABLE_INTERFACE_CAPABILITY` is on. IGMP is deliberately excluded — the MAC's payload engine does not cover it |
| 4 | **`nx_interface_address_mapping_needed` left false** | The one that took longest. `nx_ip_driver_packet_send()` gates *all* destination resolution — broadcast, multicast **and the ARP lookup** — on this flag. False (a zeroed `NX_INTERFACE`) means NetX Duo skips the lot and hands the driver a request with the physical address still zero. Frames left perfectly formed apart from a destination MAC of `00:00:00:00:00:00`; `tx: 16 sent, 0 dropped` against a host receiving nothing. Found by dumping the frame head: `00 00 00 00 00 00 \| 56 00 1e 00 04 51 \| 08 00 \| 45 00 …`. The flag is false by default because it exists for point-to-point links; Ethernet is the other case |

Also of note: **`NX_ENABLE_VLAN` is mandatory**, and this corrects the feasibility study. The
whole of `nx_link.c` — lines 28 to 1560, every `nx_link_*` function, `NX_LINK_TIME`, and the
PTP timestamp stashing — sits inside one `#ifdef NX_ENABLE_VLAN`. The study recorded that as a
quirk of ST's driver, which `#error`s without it. It is upstream, and applies to any port
using `nx_link` at all. No VLAN tagging is configured; the flag purely compiles the layer in.

**Hardware verification:** as per the S5a–S5d table above. `ping` at 0% loss over 300 × 1400 B is
the gate for S5c; do not proceed to S6 on a lossy link.

#### S5a progress — 15 Aug 2026 — **DONE**

`--stage S5a` green 4/4. `Drivers/EthDrv/eth_drv_netxduo.c` created, on the **unchanged**
`mac_drv.c`. On the board:

```
PHY: SMSC LAN8742A (rev. 1) @0
ETH LINK: UP (100 Mbps, FULL duplex)
...
  phy pri=32 (cmsis 24) state=4 runs=30
```

Carried over from `eth_drv_lwip.c` rather than rewritten: the `_Static_assert` on the
state/descriptor layout, the ring depths and buffer size, the SRAM3 placement, and the
`phy_thread` that also drains the DMA error latch (F68 — the abnormal flags cannot be printed
from the ISR that detects them).

Three things the port shaped differently:

- **The MAC address is split, not copied.** lwIP took a 6-byte `netif->hwaddr`; NetX Duo wants
  a 16-bit MSW and a 32-bit LSW — the same form `nx_link_ethernet_packet_send()` takes its
  destination in. Written out byte by byte rather than `memcpy`'d, because getting the split
  wrong yields an address that looks plausible in a debugger and is wrong on the wire.
- **No `nx_interface_address_length` field exists**, unlike the guess in the first draft; NetX
  Duo takes the address purely from the msw/lsw pair.
- **The first poll no longer announces `DOWN`.** Auto-negotiation has simply not finished at
  that point, and reporting it made a normal boot look like a cable fault. Genuine transitions
  after the first poll are still reported. The `boot` check correspondingly now reads the
  **last** link state rather than the first, and fails if it is not `UP`.

---

### S6 — The NSD, and the TX timestamp fix

**Deliverable:** `Src/flexptp_nsd/nsd_netxduo.c` wired via flexPTP's `FLEXPTP_NSD_SRC` (no
submodule patch), plus the three C++ units from question 2.

**Do the TX-timestamp deferral first, before anything else in this stage.** Register a
`txTsCbPtr` that only stashes and posts with timeout 0, and call `ptp_transmit_timestamp_cb()`
from a thread. If this is left until the end, every sync failure in the stage has two candidate
causes instead of one.

**Hardware verification:** `ptp_start`, then **`ptp_ts` before `ptp_sync`**. Confirm TX
timestamps are arriving 1:1 *before* looking at whether the clock converges — a converging-looking
clock with no `t3` is the exact trap finding 1 describes.

Then `ptp_sync` against `ptp4l`, compared to the S0 baseline trace. Accuracy should be
**unchanged**, not better: timestamps come from the same hardware through the same driver. A
materially different offset means something is wrong, in either direction.

**Exit criteria:** offset trace statistically indistinguishable from baseline over 15 minutes.
Run `/code-review ultra` here.

#### Progress — 15 Aug 2026 — **IN PROGRESS, not passing**

Written and on the board: `Src/flexptp_nsd/nsd_netxduo.c` (the four NSD functions over
UDP/IPv4) and `Src/flexptp_nsd/ptp_tx_timestamp.c` (the finding-1 deferral). The network layer
is **unregressed** — `--stage S5d --compare baseline.json` still 6/6 — and the host tests still
pass.

**What is proven to work.** Instrumented end to end with `ethinfo`:

```
udp: rx 37 (invalid 0, dropped 0, cksum 0), tx 2
nsd: notify 35, packets 35, enqueued 35, tx-requested 0
nsd rx types: sync 14 delreq 0 fup 14 delresp 0 ann 7
nsd last rx: 44 bytes
  08 12 00 2c 00 00 ...
```

So: the IGMP join goes out correctly (verified on the wire — destination MAC `01:00:5e:00:01:81`,
IP protocol 2, to 224.0.1.129); Sync, Follow_Up and Announce all arrive in the right
proportions on both the event and general sockets; each is a well-formed PTPv2 message at the
right offset with the right length; and all 35 are handed to `ptp_receive_enqueue()`. flexPTP's
`ptp` thread is alive and has woken 52 times, so it is consuming them.

**What does not work.** BMCA selects nothing: `ptp info` reports
`Master clock ID: 0000000000000000`, no state transition is logged even with `ptp log info`
and `ptp log bmca` enabled, and consequently no `Delay_Req` is ever sent
(`tx-requested 0`). The transport, domain and delay mechanism all read correctly
(`IPv4`, `0`, `E2E`).

**Where the next session should start.** The fault is above the NSD — messages of the right
shape reach flexPTP and it declines to act on them — so the remaining candidates are inside
flexPTP's own processing under the `FLEXPTP_CMSIS_OS2` binding, which this port is the first
to exercise here. Worth checking in order:

1. Whether `ptp_process_packet()` rejects on the `versionPTP` byte. The received value is
   `0x12` — PTPv2 in the low nibble, `minorVersionPTP = 1` in the high nibble, which is what
   a current `ptp4l` emits. Note the **lwIP baseline accepted the same traffic from the same
   `ptp4l` instance**, so this is only a candidate if something else differs.
2. The `sRawRxMsgBuf` path: whether `msgb_alloc()` is returning NULL and the
   `MSGB_ERR_FULL` branch is silently swallowing every message. `ptp log info on` should
   print "The PTP receive packet buffer is full" if so, and it prints nothing — which is
   itself informative.
3. Whether the notification/uid queue pairing in the CMSIS-RTOS2 binding is losing the
   association — the binding posts to two separate queues per message and only the
   FreeRTOS path has been exercised in the field.

Diagnostics are all in place for that work: `ethinfo` now reports driver, IP, UDP, ARP and NSD
counters, a message-type histogram, the head of the last received PTP payload and the head of
the last transmitted frame.

**One real bug was found and fixed along the way:** `ptp_tx_timestamp_init()` was never called,
so the deferral queue would have been NULL when the first transmit timestamp arrived and every
`t3` would have been dropped in the ISR — the exact failure the module exists to prevent,
reintroduced by forgetting to wire it up. Now initialised from `ptp_nsd_init()`, idempotently.

---

### S7 — Transports, delay mechanisms, profiles

802.3 via `nx_link_ethernet_packet_send()` / `nx_link_packet_receive_callback_add()` — which
replaces the `LWIP_HOOK_UNKNOWN_ETH_PROTOCOL` hack and the manual EtherType parse entirely — plus
P2P and L2 multicast join.

**Hardware verification:** each of `default`, `defp2p` and `gPTP` reaching sync, with the host
matched to each. `gPTP` needs a direct cable — see [ptp_profiles.md](ptp_profiles.md) and the
switch caveat in the README.

---

### S8 — Cleanup, documentation, CI

Rewrite [architecture.md](architecture.md) (the module inventory, thread table, and **the
FreeRTOS priority-constraint section, which becomes wrong** — ThreadX uses PRIMASK and the
level-5 rule disappears). Update the README's software-structure section and the docs table. Add
the mixed-licence note. Update `.github/workflows/ci.yml`. Record the port in a new
`docs/reviews/` entry if a review produced findings.

**Hardware verification:** the full regression suite against `baseline.json`, from a clean clone,
on both Debug and Release.

---

## Rejected alternatives

| Alternative | Why rejected |
|---|---|
| **Pivot stage: ThreadX with lwIP still on top** | Isolates the two variables, which is genuinely attractive. But `Modules/lwip_port/sys_arch.c` is 607 lines of raw FreeRTOS (0 CMSIS-RTOS2 calls), and lwIP's `contrib/ports/` offers only freertos/unix/win32 — no CMSIS-RTOS2 port to borrow. That is ~600 lines written to be deleted two stages later. The S5a–S5d sub-staging buys most of the same isolation for none of the cost |
| **Native ThreadX API throughout, no CMSIS-RTOS2** | ~400 lines of churn in files this port otherwise never opens, and it forfeits the wrapper that makes S3 cheap. Only justified if a single SLA0044 file is unacceptable |
| **Reuse ST's `nx_stm32_eth_driver.c`** | HAL_ETH-based — the exact abstraction this project bypassed to reach the DMA descriptors that timestamping needs. Its PTP mode is also hard-wired to `NX_PTP_CLIENT` types and `#error`s without `NX_ENABLE_VLAN` |
| **Replace flexPTP with NetX Duo's PTP client** | Slave-only, no BMCA, no P2P, no 802.3, no gPTP, single-master. Would delete most of the CLI surface this demo exists to show |
| **Big-bang: ThreadX and NetX Duo in one stage** | Nothing observable until both work. This project's own history is the argument against |
| **C++ rewrite of the MAC driver** | `mac_drv.c` is the most expensive verified artefact in the repo and this port does not need to touch it. Rewriting it would import all of F53/F54/F58/F61/F71's risk for no benefit |

---

## Rollback and abort criteria

The port lives on a branch off `master`; `master` keeps a working FreeRTOS/lwIP firmware
throughout. Every stage is a commit that builds and (from S3 on) flashes.

**Abort and reassess if:**

- S6 cannot reach a stable sync within **3 days** of bench work. That is double the estimate and
  indicates something structural in the timestamp path rather than a bug.
- PTP offset is materially worse than baseline and the cause is not identified within a day.
  Accuracy should be *unchanged*; an unexplained difference means the timestamp path is not doing
  what it appears to.
- Byte-pool or packet-pool sizing cannot be made to fit SRAM alongside the existing DMA buffers.

**Partial-success exit:** S0–S3 alone (ThreadX, no network) is not shippable, but S0–S1 are —
the HIL harness and host test tree are useful to `master` regardless of whether the port
continues, and should be merged there independently of the rest.
