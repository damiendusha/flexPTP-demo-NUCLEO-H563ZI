# Port Review — Findings & Disposition

The audit trail for this port: what was checked, what was found, and where each finding is
now tracked. It is not a task list.

> **Findings are records, not descriptions of the current tree.** This log spans both ports —
> H743 → H563, then FreeRTOS/lwIP → ThreadX/NetX Duo. Findings raised against the lwIP-era
> firmware name files that **no longer exist** (`eth_drv_lwip.c`, `Inc/lwipopts.h`,
> `Inc/FreeRTOSConfig.h`, `Modules/lwip_port/`), and they are deliberately left as written:
> a finding rewritten to match a later tree loses the evidence that made it a finding.
>
> For the current state see [architecture.md](architecture.md). For which findings each test
> tier can catch — and the roughly 50/25/25 split between reachable, regression-lockable and
> irreducibly-hardware — see [testing_architecture.md](testing_architecture.md).

**This is a living document, written in two phases.** Findings **F1–F43** come from a
static review of the plan and the repository at commit `e2d9556` (Stage 4 complete),
before anything had been compiled. Findings **F44 onward** were found during
implementation, by building — see [Findings by phase](#findings-by-phase) for why that
split is worth noticing.

## Table of Contents

- [Method](#method)
- [Findings by phase](#findings-by-phase)
- [Disposition Summary](#disposition-summary)
- [Critical Findings](#critical-findings)
- [Build-Blocking Findings](#build-blocking-findings)
- [Plan Structure Findings](#plan-structure-findings)
- [Preconditions Requiring Hardware or Datasheet](#preconditions-requiring-hardware-or-datasheet)
- [flexPTP Library & Documentation Findings](#flexptp-library--documentation-findings)
- [Second-Review and ThreadX-Port Findings](#second-review-and-threadx-port-findings)
- [Minor Findings](#minor-findings)
- [Verified Clean](#verified-clean)
- [H743 Baseline Reference Data](#h743-baseline-reference-data)

---

## Method

### Phase 1 — static review (F1–F43)

- Full read of every application source file, both CMake trees, the linker script and the
  build/IDE configuration.
- Symbol-level verification of every HAL and CMSIS identifier the port depends on, against
  `Drivers/stm32h5xx-hal-driver/Inc/` and `Drivers/CMSIS/Device/ST/STM32H5xx/Include/stm32h563xx.h`
  — the **device** header, not the family header (see [F3](#f3--no-eth-ptp-clock-mux-on-this-device)).
- File-inventory diff against upstream `epagris/flexPTP-demo-NUCLEO-H743ZI` to confirm the
  Stage 1 copy was faithful, and to recover baseline values for comparison.
- Constant-folding the flexPTP addend macro under GCC to confirm the arithmetic in
  [F2](#f2--both-planned-configurations-produce-an-addend-of-zero) rather than reasoning about it.
- Cross-check of the flexPTP README, the published documentation and the porting guide
  (`epagris.github.io/flexPTP`) against the **pinned** submodule `RC1.0-11-g76aaf67`
  (2026-03-06). The website documents whatever is current on `master`; where the two
  disagree, the pinned source wins — see [F37](#f37--ptp_hw_get_time-is-documented-with-the-wrong-type) and [F38](#f38--sprintf-vs-flexptp_snprintf).

### Phase 2 — implementation (F44 onward)

- Every stage from 5 onward verified by compiling, with the Arm GNU toolchain 15.3.rel1
  fetched into `toolchains/`. Stage gates: HAL library at 8, `main.c` at 10, full link
  at 15.
- Where a file could not yet be compiled in place because of a dependency on a later
  stage, it was verified in isolation using a throwaway shim header outside the repo
  rather than by reordering stages (see Stage 10).
- Linked-image checks: memory-region usage, entry point, and the first two vector-table
  words against the linker script.

### Phase 3 — hardware bring-up (F51 onward)

Everything from F51 on was found by running the firmware on a board. That is a different kind
of evidence from Phases 1 and 2, and the document mixes all three, so **check which phase a
finding came from before trusting how it was established**:

| Phase | Findings | Evidence |
|---|---|---|
| Static review | F1–F43 | reading code, headers and documentation |
| Implementation | F44–F50 | configuring, compiling and linking |
| Hardware bring-up | F51–F66 | executing on the target, read over SWD and the console |
| Independent review | F67–F79 | two later review passes over the tree; see [reviews/](reviews/README.md) |
| ThreadX/NetX Duo port | F80 | the S6 bench session |

> **This paragraph used to read "Still not done: **no hardware**. Nothing here has executed on
> a device."** It was true when Phases 1 and 2 were written and has not been true since F51.
> It is corrected rather than deleted because the split between what was *reasoned* and what
> was *measured* is the most useful thing this document records — see
> [Findings by phase](#findings-by-phase).

---

## Findings by phase

| Phase | Findings | Found by |
|---|---|---|
| Static review | F1–F43 (43) | Reading code, headers and documentation |
| Implementation | F44–F50 (7) | Configuring, compiling and linking |
| **Hardware bring-up** | **F51–F66 (16)** | **Executing on the target; SWD register reads, console captures and `tcpdump`** |
| Independent review | F67–F79 (13) | Two later review passes over the tree, by more than one model |
| ThreadX/NetX Duo port | F80 (1) | The S6 bench session — and the only finding here with no N-number, for that reason |

The third phase is the one that changed the most conclusions. It produced the two defects
that were invisible to both earlier phases (F51's byte-access fault, F52's init ordering),
corrected two findings the static review had got wrong (F3's HCLK inference, and F5 which
followed from it), and resolved F42 by proving the fault was outside the board entirely. It
also produced the only two findings that were *retracted*: F5, and B6 in the later
`final_review_opus_5p0.md`, both disproved by measurement rather than argument.

The split is worth recording, because the two phases caught different *kinds* of problem
and the second kind was not reachable by inspection:

- **F45** (inverted clock-divider namespaces) is a silent miscompile. It was caught only
  because the neighbouring APB lines failed loudly first — inspection had not flagged it.
- **F47, F48, F49** all trace to one root cause the static review did identify (the
  submodule pins diverge from the H743 reference) but whose *consequences* only became
  visible once the library compiled.
- **F50** did not appear until **link**, after every translation unit had compiled
  cleanly. For a port of this kind, "it compiles" is a weak signal.

A useful calibration: the static review found 43 issues and was thorough enough that
Stage 12's register audit came out exactly as predicted — but it still missed seven, four
of them blocking. Reviewing a port and building it are not substitutes for each other.

---

## Disposition Summary

| # | Finding | Severity | Now tracked in |
|---|---|---|---|
| F1 | PTP baseline misstated: H743 uses 240 MHz, not 200 MHz | Critical | [ptp_clock.md](ptp_clock.md#history) |
| F2 | Both planned PTP configs have K = 1.0 → addend of 0 | Critical | [ptp_clock.md](ptp_clock.md#the-k--1-constraint), plan Stage 13 |
| F3 | H563 has no ETH PTP clock mux — **but the fixed source is `pll1_q_ck`, not HCLK** (partly wrong; corrected) | Critical | [ptp_clock.md](ptp_clock.md#why-the-source-is-pll1_q_ck), design doc, plan diff table |
| F4 | PLL1_Q cannot produce 200 MHz alongside 250 MHz SYSCLK | Critical | [ptp_clock.md](ptp_clock.md#pll-arithmetic-for-reference) |
| F5 | A 4 ns increment is unreachable on this device (**partly wrong; corrected** — reachable via `PLLQ = 1`, but not adopted) | Critical | [ptp_clock.md](ptp_clock.md#why-a-4-ns-increment-is-not-adopted) |
| F6 | AHB prescaler must change `DIV2` → `DIV1` | Critical | plan Stage 10, [ptp_clock.md](ptp_clock.md#coupled-decisions) |
| F7 | ETH_TXD1 moves PB13 → PB15 | Critical | design doc RMII Pin Map, plan decisions + Stage 12 |
| F8 | MDIO divider write double-shifts a positioned mask | Critical | plan Stage 12 (with before/after code) |
| F9 | No flexPTP hardware port exists for H5 | Critical | plan **Stage 14** 🆕 |
| F10 | `Src/CMakeLists.txt` references deleted `system_stm32h7xx.c` | Build | plan Stage 8 + Stage 3 follow-up note |
| F11 | SYSCFG renamed to SBS on H5 | Build | plan Stages 10 & 12, diff table |
| F12 | `USE_HAL_UART_REGISTER_CALLBACKS` must be `1U` | Build | plan Stage 6 |
| F13 | `Drivers/CMakeLists.txt` is a 127-file H7 source list | Build | plan Stage 8 |
| F14 | MCU flags still `cortex-m7` / `fpv5-d16` | Build | plan Stage 8 |
| F15 | `MCU_TYPE` still `STM32H743xx` | Build | plan Stage 8 |
| F16 | FreeRTOS port still `GCC_ARM_CM7` | Build | plan Stage 8 |
| F17 | Header-sweep stage listed only 2 of 4 affected files | Build | plan Stage 15 |
| F18 | H7 PWR supply API does not exist on H5 | Build | plan Stage 10, diff table |
| F19 | H7 RCC clock-tree fields and PLL symbol names differ | Build | plan Stage 10 |
| F20 | No intermediate build gates | Process | plan [Build gates](implementation_plan.md#build-gates) |
| F21 | Old Stage 13 was a catch-all bucket | Process | split into plan Stages 8 & 16 |
| F22 | Old Stage 14 "create design_doc.md" — already existed | Process | plan Stage 17 reframed as *complete* |
| F23 | No stage owned README / build.sh / launch.json | Process | plan **Stage 19** 🆕 |
| F24 | FreeRTOS `GIT_TAG main` is a floating dependency | Process | plan Stage 9, decisions, risks |
| F25 | TZEN option byte is a boot precondition | Precondition | plan Stage 20 |
| F26 | PPS pin undecided; H743 configures two | Resolved | **PG8 chosen** — plan decisions, Stages 12 & 14 |
| F27 | ETH DMA reach into SRAM3 unverified | Precondition | plan Open Items, Stage 17 |
| F28 | SRAM2 (64 KB) unallocated | Minor | plan Stage 4 note |
| F29 | `.ETHLibPool` has no linker placement | Minor | plan Stage 4 note, Stage 17 |
| F30 | CMSIS Core 5.6 carried over from the H7 project | **Not an issue** | verified identical to CMSIS 5.9.0 |
| F31 | Unexpanded `{{sr:cmake_path}}` template placeholder | Minor | plan Stage 7 |
| F32 | Two CMake include paths point at non-existent dirs | Minor | plan Stage 7 |
| F33 | ST header comment bug on `MACSSIR` masks | None | this document only |
| F34 | `/DISCARD/ libc.a` block inherited from ST template | None | this document only |
| F35 | `double` in Kalman filter on an SP-FPU part | None | plan decisions (accepted) |
| F36 | Distro/upstream OpenOCD 0.12.0 cannot program the H563 | Blocking (tooling) | plan **Stage 18** 🆕 |
| F37 | Porting guide types `PTP_HW_GET_TIME` as `TimestampI *`; code passes `TimestampU *` | Critical (trap) | plan Stage 14 |
| F38 | Guide requires `SPRINTF`; code calls `FLEXPTP_SNPRINTF` | Trap | plan Stage 13 |
| F39 | flexPTP has never been ported to Cortex-M33 / ARMv8-M | Risk | plan Open Items |
| F40 | No OS-binding macro selected; silently defaults to `FLEXPTP_FREERTOS` | Build (latent) | plan Stage 13 |
| F41 | `PTP_UPDATE_CLOCK` dead; `ETHHW_UpdatePTPTime()` unreachable and wrong | **RESOLVED** | removed as dead code rather than fixed unexercised |
| F42 | **The switch silently discarded PTP Delay_Resp** — nothing in the port was at fault | **RESOLVED** | proven by direct cable |
| F59 | **One duplicated Sync cycle latches the Kalman servo into NaN for ever** | **RESOLVED** | root-caused, fixed, verified on hardware |
| F60 | embfmt cannot parse `%ll`, and renders NaN as a plausible `0.0000` | **RESOLVED** | reproduced on the host, fixed |
| F61 | **`ETHHW_ProcessRx()` re-delivers frames already queued with the receive thread** | **RESOLVED** | measured: 57% of frames duplicated; 0 after the fix |
| F62 | **Release build HardFaulted at the UID region** — F51's fix held only at `-O0` | **RESOLVED** | `volatile` added; verified on hardware |
| F63 | lwIP DHCP `-Wstringop-overflow` in optimised builds | **Documented** | not reachable here; third-party, unpatched |
| F64 | ST's OpenOCD will not build as the plan specified — `-Werror` and a mis-named prerequisite | **RESOLVED** | both corrected in `build.sh`, cold run verified |
| F65 | `.vscode/launch.json` used `$_CHIPNAME.cpu0`, which does not exist on H5 | **RESOLVED** | FreeRTOS thread awareness was silently broken |
| F66 | embfmt misprints `%e` below ~5e-8 and corrupts any rounding carry in `%f` | **RESOLVED** | found by the Stage 20 CLI sweep; fixed and verified on hardware |
| F58 | `eth_drv_lwip.c` RX copy never advances the source pointer across a chained pbuf | **RESOLVED** | proven by shrinking `PBUF_POOL_BUFSIZE`: DHCP failed entirely, then worked |
| F43 | `PTP_ACCURACY_LIMIT_NS` (100 ns) is the implicit LOCKED criterion | Minor | plan Stage 20 |
| F44 | `CMakePresets.json` requires Ninja; not documented as a prerequisite | Minor | plan Stage 19 |
| F45 | H5 inverts the H7 clock-divider namespaces; the H7 spelling compiles but misconfigures | Critical (silent) | plan Stage 10 (fixed) |
| F46 | `GPIO_SPEED_HIGH` legacy alias does not exist on H5 | Minor | plan Stages 11 (fixed) & 14 |
| F47 | flexPTP uses `__PRI64_PREFIX`, a glibc-internal macro; breaks every newlib build | Blocking | plan Stage 14 (worked around) |
| F48 | `tx_cb` signature drift between pinned flexPTP and this project's `lwipopts.h` | Blocking | plan Stage 14 (fixed) |
| F49 | `task_ptp_is_operating()` renamed to `is_flexPTP_operating()` upstream | Blocking | plan Stage 15 (fixed) |
| F50 | ARMv8-M port names exception handlers directly; the H7-era remaps break the tick | Blocking | plan Stage 15 (fixed) |
| F51 | H563 UID region rejects **byte** reads; `ETH_MAC_ADDR*` byte-indexed it → HardFault | Critical | fixed — but the first fix held only at `-O0`, see F62 |
| F52 | `ptphw_init()` set the PTP addend before the subsecond increment → TSADDREG never clears | Critical | fixed, verified on hardware |
| F53 | PTP timestamp counter is not clocked — PLL1Q output never enabled | **RESOLVED** | fixed, verified on hardware |
| F54 | Six unbounded hardware waits; any clock, supply or PHY fault hangs the caller silently | **RESOLVED** | all bounded and reporting; the 8× addend loop was superstition |
| F55 | Welcome message told users to fit an **8 MHz** oscillator at X3; on MB1404 X3 is **25 MHz** and already fitted | **RESOLVED** | banner rewritten with the full recipe |
| F56 | `PTP_LOG_TRANSMISSION` exists in flexPTP's enum but has no CLI switch | Minor (cost hours) | documented |
| F57 | `ETHHW_ProcessTx()` never reset its per-pass descriptor count — could re-dispatch a stale index | Latent bug | fixed |

| F67 | The console FIFO could not tell full from empty | **RESOLVED** [N1, N7] | occupancy is a modular difference of two running totals; host-tested (TS-01) |
| F68 | Console output from an interrupt hung the board | **RESOLVED** [N6] | interrupt context is refused and counted, not served |
| F69 | A known vendor with an unlisted model was a boot HardFault | **RESOLVED** [N4] | fixed in the PHY driver |
| F70 | The bounded transmit wait was compiled away above `-Og` | **RESOLVED** [N2, N9] | `volatile` on the descriptor; verified in the disassembly at three levels |
| F71 | A momentary RX ring exhaustion killed reception permanently | **RESOLVED** [N3] | tail-pointer write on restore; host-tested and mutation-verified |
| F72 | Every lwIP assertion was a silent no-op | **RESOLVED** [N5] | historical — lwIP was removed in S3 |
| F73 | `init_ethernet()` built the netif without the core lock | **RESOLVED** | historical — lwIP was removed in S3 |
| F74 | CLI tokenizer: a one-byte overflow and shared state | **RESOLVED** [N8, N12] | host-tested and mutation-verified (TS-02) |
| F75 | A dropped transmit was reported to lwIP as sent | **RESOLVED** [N14] | the NetX Duo driver reports `NX_TX_QUEUE_DEPTH`; host-tested |
| F76 | The MAC handed the FCS up with every received frame | **RESOLVED** [N13] | CST/ACS enabled; host-tested |
| F77 | Two traps on the EtherLib receive path | **RESOLVED** [N10, N11] | EtherLib is present but not compiled — see [design_doc.md](design_doc.md#etherlib-porting-guide) |
| F78 | Two configuration statements that did not hold | **RESOLVED** [N15, N16] | the same class as NX-03, which corrected a third |
| F79 | The flash figure is not reproducible across build directories | **documented** [N17] | compare deltas, not absolutes |
| F80 | **flexPTP's heartbeat cannot be enqueued from ThreadX's timer thread** | **RESOLVED** | `patches/flexptp/0004`; pinned by a host test against a real kernel |

**Every finding is tracked.** F33 and F34 are recorded here only because they are
observations about upstream code that require no action — noted so a future reader does not
spend time on them.

> **This table ran F1–F66 and still said "every finding is tracked"** until finding DS-02 of
> [`review_v1_opus_5p0.md`](review_v1_opus_5p0.md). Fourteen findings — a fifth of the log, and
> the fifth containing most of the ThreadX/NetX Duo work — had a section each and no row here,
> and the two phase tables above stopped at F66 as well. All three are corrected.

---

## Critical Findings

### F1 — PTP baseline misstated

The plan described 200 MHz / 5 ns as "matching the H7 config". Upstream
`Inc/flexptp_options.h` reads `PTP_MAIN_OSCILLATOR_FREQ_HZ (240000000)`. On the H743 the
PTP unit is clocked from HCLK = 480/2 = 240 MHz.

### F2 — Both planned configurations produce an addend of zero

`K = f_ptp × increment_ns / 10⁹` must exceed 1, because `addend = 2³² / K` is written to a
32-bit register. The plan's initial config (200 × 5) and its "optimisation" (250 × 4) both
give exactly K = 1.0. Confirmed by constant-folding the flexPTP macro:

```
240MHz/5ns : 3579139413.3 -> 3579139413     (H743, works)
250MHz/5ns : 3435973836.8 -> 3435973836     (selected, works)
200MHz/5ns : 4294967296.0 -> 0              (plan, initial)
250MHz/4ns : 4294967296.0 -> 0              (plan, "optimised")
125MHz/5ns : 6871947673.6 -> 2576980377     (if AHB DIV2 kept — 0.375× rate)
```

Full derivation in [ptp_clock.md](ptp_clock.md).

### F3 — No ETH PTP clock mux on this device

`RCC_CCIPR5_ETHPTPCLKSEL`, `RCC_CCIPR4_ETHPTPDIV`, `ETHCLKSEL` and `ETHREFCLKSEL` do not
exist in `stm32h563xx.h`. The HAL guards all ETHPTP clock-selection code with
`#if defined(RCC_CCIPR5_ETHPTPCLKSEL)`, so it compiles out entirely on this part.

**Generalisable lesson:** `stm32h5xx_hal_rcc_ex.h` is a *family* header covering devices up
to the H5F5/H5E5 series. Reading it alone will suggest peripherals and clock paths the
H563 does not have. Verify every RCC/peripheral capability against `stm32h563xx.h`.

> ### ⚠ Correction (hardware bring-up, 2026-08-09)
>
> The *absence of the mux* is correct. The conclusion drawn from it — that the PTP clock
> therefore defaults to **HCLK** — was **wrong**, and it caused F53.
>
> RM0481 Table 121 *(Kernel clock distribution overview)* carries two rows for the PTP
> clock: one gated by `ETHPTPCLKSEL` and marked *"available only on STM32H543/553xx"*, and
> a second, selector-less row whose source column is **`pll1_q_ck`**. The mux-less parts —
> including the H563 — are hardwired to `pll1_q_ck`.
>
> The header evidence supported "no mux" but said nothing about the fixed source; the
> device header cannot express that, only the reference manual can. Acting on the HCLK
> inference left `PLL1QEN` clear, so the PTP counter never ran.
>
> **The original implementation plan's instinct — "PTP clock: PLL1_Q" — was right**, and
> this review overrode it. Only the plan's 200 MHz figure was unattainable.

### F4 — PLL1_Q cannot produce 200 MHz

`PLLP` forbids odd dividers and the PLL1 wide VCO range is 192–836 MHz, so SYSCLK = 250 MHz
forces VCO = 500 MHz (PLLP = 2; PLLP = 4 would need a 1000 MHz VCO). PLL1Q and PLL1R are
integer dividers of that VCO: 500 / 250 / 166.67 / 125 / 100 MHz. 200 MHz is not on the list.

### F5 — A 4 ns increment is unreachable

Timestamp granularity equals the subsecond increment, and the increment must exceed the PTP
clock period. A 4 ns increment with H743-parity headroom (K = 1.2) needs 300 MHz; even a
marginal K = 1.05 needs 262.5 MHz. The device ceiling is 250 MHz. Granularity is therefore
5 ns — the same as the H743 — and cannot be improved.

> ### ⚠ Correction (same root cause as F3)
>
> The conclusion followed from the F3 error: if the PTP clock were HCLK, it would indeed be
> capped at 250 MHz. It is not — `clk_ptp_ref_i` is `pll1_q_ck`, and `PLLQ = 1` would give
> **500 MHz and K = 2.0**, a valid 4 ns configuration.
>
> It remains **not adopted**, but for engineering reasons rather than impossibility: the
> maximum frequency of `clk_ptp_ref_i` has not been checked against the datasheet, `PLLQ = 1`
> would pin PLL1Q at 500 MHz and remove it as a source for anything else, and the gain —
> 5 ns to 4 ns of quantisation — sits far below the noise floor of this board's
> ST-Link-sourced clock. See
> [ptp_clock.md](ptp_clock.md#why-a-4-ns-increment-is-not-adopted).

### F6 — AHB prescaler

`Src/main.c` uses `RCC_HCLK_DIV2` (H743: 480 → 240). On the H563, SYSCLK 250 MHz is already
at the HCLK ceiling, so the divider must be 1. Carrying DIV2 over yields HCLK 125 MHz, K =
0.625, and a wrapped addend — a clock at 0.375× real time rather than an obvious failure.

### F7 — ETH_TXD1 pin

The plan asserted the RMII pin map was identical on both boards. It is not: ETH_TXD1 is
PB13 on the H743 board and **PB15** on the H563 board (confirmed by the maintainer).
`mac_drv.c` groups PB5 and PB13 into one `HAL_GPIO_Init(GPIOB, …)` call, so this is a
one-line change with a disproportionate failure mode: link up, RX fine, TX silently dead.

### F8 — MDIO divider double-shift

```c
WRITE_REG(eth->MACMDIOAR, ETH_MACMDIOAR_CR_DIV102 << ETH_MACMDIOAR_CR_Pos);
```

`ETH_MACMDIOAR_CR_DIV102` is already a positioned mask (`0x400`, bit 10). Shifting it a
further 8 bits produces `0x40000`, which lands in the `RDA` field and leaves `CR` = 0 → ÷42.
At HCLK 250 MHz that is ~6 MHz MDC against a 2.5 MHz limit. Pre-existing in the H743 code;
inherited here.

### F9 — No flexPTP hardware port for H5

The flexptp submodule ships ports for H743 / F407 / CH32F207 / TM4C1294 / MK64F / Linux
only. `Modules/CMakeLists.txt` selects `H743_LWIP`. The H743 port includes
`stm32h7xx_hal.h` directly and performs its own PPS GPIO setup, so it cannot be reused.
The plan's Stage 11 line "verify/update PTP port file includes" understated this by roughly
a whole source file.

---

## Build-Blocking Findings

Each of these stops compilation or linking. They are individually small; the point of
listing them is that none had an owning stage before the review.

| # | Detail |
|---|---|
| F10 | Stage 3 replaced `Src/system_stm32h7xx.c` with `system_stm32h5xx.c` but left the old name in `Src/CMakeLists.txt`. The tree does not configure today. |
| F11 | `__HAL_RCC_SYSCFG_CLK_ENABLE` → `__HAL_RCC_SBS_CLK_ENABLE`; `HAL_SYSCFG_ETHInterfaceSelect(SYSCFG_ETH_RMII)` → `HAL_SBS_ETHInterfaceSelect(SBS_ETH_RMII)`; `HAL_EnableCompensationCell()` → `HAL_SBS_EnableVddIO1CompensationCell()` / `…VddIO2…`. Affects `mac_drv.c` and `main.c`. |
| F12 | `serial_io.c` calls `HAL_UART_RegisterCallback()`, which requires `USE_HAL_UART_REGISTER_CALLBACKS 1U` in the HAL conf. Upstream sets it; the plan's Stage 6 list omitted it. |
| F13 | `Drivers/CMakeLists.txt` names 127 H7 source files, a `H7HAL` target, and an `STM32H7xx` CMSIS include path. There is no `Src/Legacy/` ETH driver on H5. |
| F14 | `cmake/vscode_generated.cmake` still has `-mcpu=cortex-m7 -mfpu=fpv5-d16`, the H735 linker script and the H735 startup file. Target: `-mcpu=cortex-m33 -mfpu=fpv5-sp-d16`. |
| F15 | `set(MCU_TYPE STM32H743xx)` in the root `CMakeLists.txt`. |
| F16 | `Middlewares/CMakeLists.txt`: `FREERTOS_PORT` `GCC_ARM_CM7`, include path `portable/GCC/ARM_CM7/r0p1`, and the exported `device_header` all need the `ARM_CM33_NTZ/non_secure` equivalents. |
| F17 | The header-sweep stage named `eth_drv_lwip.c` and `eth_drv_etherlib.c` but missed `Src/ethernet/ethernet_etherlib.c` and `Drivers/CMSIS/CMSIS_RTOS_V2/cmsis_os2.c`. |
| F18 | `HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY)` — H5 declares the function but defines no `PWR_LDO_SUPPLY`; the only accepted value is `PWR_EXTERNAL_SOURCE_SUPPLY`. Drop the call. `PWR_REGULATOR_VOLTAGE_SCALE0` and `PWR_FLAG_VOSRDY` do exist. |
| F19 | H5 `RCC_ClkInitTypeDef` has no `SYSCLKDivider` or `APB4CLKDivider`, and no `RCC_CLOCKTYPE_D1PCLK1`/`D3PCLK1` (use `RCC_CLOCKTYPE_PCLK3`). Renames: `RCC_PLLSOURCE_HSE` → `RCC_PLL1_SOURCE_HSE`, `RCC_PLL1VCOWIDE` → `RCC_PLL1_VCORANGE_WIDE`, `RCC_PLL1VCIRANGE_1` → `RCC_PLL1_VCIRANGE_1`. |

---

## Plan Structure Findings

**F20 — No build gates.** All compilation was deferred to a single late stage, so every
error in Stages 5–12 would surface at once. The plan now builds after Stages 7, 9, 13 and 15.

**F21 — Catch-all stage.** The old Stage 13 ("Build & fix compilation") silently contained
the CMake rewrite, the HAL source list, the FreeRTOS port and the MCU flags. Split into
Stage 7 (retarget) and Stage 15 (cleanup).

**F22 — Redundant stage.** The old Stage 14 said "Create `docs/design_doc.md`", but Stage 1
already created it and it is filled incrementally. Reframed as *complete*.

**F23 — Unowned metadata.** README (entirely H743, plus a reference to a GIF that was
deliberately not copied), `build.sh`'s header comment, and four H743 references in
`.vscode/launch.json` had no stage.

**F24 — Floating dependency.** `FetchContent … GIT_TAG main` for FreeRTOS-Kernel, on a
project that is changing the kernel port. Pin it before the first build so a failure cannot
be caused by an upstream commit.

---

## Preconditions Requiring Hardware or Datasheet

**F25 — TZEN.** The flat `0x08000000` layout and the `ARM_CM33_NTZ` port assume TZEN is
disabled (factory default). A board previously provisioned with TZEN = 1 will not boot this
image.

**F26 — PPS pin. ✅ Resolved: PG8.** The H743 project drives PPS from **PG8**, configured by
the flexPTP port file, while `mac_drv.c` *separately* configures PB5 as ETH AF11. The plan's
table recorded only PB5, which was the wrong one of the two. The H563 port keeps PG8 and
drops the PB5 configuration, leaving PPS GPIO setup in exactly one place (the flexPTP
hardware port, Stage 14). Verified on hardware by Stage 20's oscilloscope check.

**F36 — OpenOCD cannot program the H563 out of the box.** The installed OpenOCD 0.12.0 has
no `target/stm32h5x.cfg`, no H5 board file, and no `stm32h5x` flash driver — `flash bank …
stm32h5x …` is rejected with *"Invalid command argument"*. Because the driver is compiled
in rather than expressed in tcl, vendoring ST's `.cfg` files onto a stock build does **not**
work. ST's fork (`STMicroelectronics/OpenOCD`, tag `openocd-cubeide-v2.1.0`) carries
`target/stm32h5x.cfg` and `board/st_nucleo_h5.cfg`. Resolved by building the fork from
`build.sh` (Stage 18). Note the board file is generic `st_nucleo_h5.cfg`; there is no
`st_nucleo_h563zi.cfg` equivalent to the H743's board file.

**F27 — ETH DMA and SRAM3.** SRAM1/2/3 are all on the AHB matrix and `HAL_Init()` leaves
DCACHE off, so no cache maintenance is expected for the Ethernet buffers in SRAM3. Confirm
empirically rather than assuming.

---

## flexPTP Library & Documentation Findings

These concern the flexPTP library contract rather than this repository. They are recorded
because two of them will actively mislead whoever implements the hardware port.

### F37 — `PTP_HW_GET_TIME` is documented with the wrong type

Both the published guide and the pinned `manual/porting.dox:46,54` state that `pt` is of
type `TimestampI *`. The code disagrees at every call site:

```c
slave.c:185               TimestampU tu;  PTP_HW_GET_TIME(&tu);
settings_interface.c:202  void ptp_time(TimestampU * pT) { PTP_HW_GET_TIME(pT); }
```

and `ptp_port_stm32h743_lwip.h:23` declares `void ptphw_gettime(TimestampU * pTime)`.
**The documentation is wrong, not the code.** Writing the H563 port from the guide yields a
signature mismatch. Stage 14's instruction to derive from `ptp_port_stm32h743_lwip.c` avoids
this; the reason is now stated there so the correct type is not "corrected" to match the docs.

### F38 — `SPRINTF` vs `FLEXPTP_SNPRINTF`

The guide lists `SPRINTF(str,n,...)` among the required printing routines. The pinned code
calls `FLEXPTP_SNPRINTF` (`timeutils.c:135`) and never references `SPRINTF`. Our
`Inc/flexptp_options.h` defines `FLEXPTP_SNPRINTF`, which matches the code and is correct.
Do not "fix" it to match the guide.

### F39 — first ARMv8-M target

The README states flexPTP has been *"ported to and tested on ARM Cortex-M4 and Cortex-M7
based microcontrollers"*; the supported list is TM4C1294, STM32F4xx, STM32F7xx, STM32H7xx,
MK64F and Linux. This port will therefore be flexPTP's **first Cortex-M33 / ARMv8-M
target**. The missing H5 hardware port (F9) is the visible part; the unquantified part is
whether the library core carries any ARMv7-M assumptions (alignment, `__attribute__` usage,
atomics). Tracked as an Open Item rather than a stage, since there is nothing specific to
fix until something breaks.

### F40 — OS binding is implicit

The guide requires defining `FLEXPTP_CMSIS_OS2` **or** `FLEXPTP_FREERTOS`.
`Inc/flexptp_options.h` defines neither, so `ptp_defs.h` falls back to `FLEXPTP_FREERTOS`.
The result is that flexPTP calls `xTaskCreate` directly while the demo application uses
CMSIS-OS2 (`osThreadNew`, `osDelay`) and the root `CMakeLists.txt` defines `CMSIS_OS2`.
This works — CMSIS-OS2 is a FreeRTOS wrapper — but the binding is invisible and load-bearing.
Made explicit in Stage 13.

Verified clean alongside it: `FLEXPTP_TASK_PRIORITY` is 24 against `configMAX_PRIORITIES`
of 56, and `FLEXPTP_TASK_STACK_SIZE` defaults to 2048.

### F41 — `PTP_UPDATE_CLOCK` is dead, and conceals a suspect calculation

The guide's required hardware-clock list is `PTP_HW_INIT`, `PTP_SET_CLOCK`,
`PTP_SET_ADDEND`, `PTP_HW_GET_TIME`. `PTP_UPDATE_CLOCK` is not among them, and the pinned
core never references it. Our `flexptp_options.h` defines it anyway, and that macro is the
**only** caller of `ETHHW_UpdatePTPTime()` — so the function is unreachable.

That matters because the subtract path looks wrong:

```c
/* Drivers/EthDrv/mac_drv.c:697 */
nsec = 1000000000 - (nsec >> 1);
nsec |= (1 << 31);
```

The STM32 MAC expects `10⁹ − nsec` with bit 31 set to mark subtraction; the `>> 1` has no
evident justification and would produce roughly a 2× time error. It is harmless today
because nothing calls it. Recorded here because it is invisible until someone re-enables
coarse correction through this path.

### F42 — lwIP TX timestamp plumbing is broken *(CONFIRMED on hardware)*

**The predicted failure happened, in the predicted form.** PTP reaches SLAVE state and
never converges.

**Symptom.** `(P)Del_Req #N: no response received!` once per second, indefinitely. Servo
correction stays `0.0000`; the addend never moves off its initial `0xCCCCCCCC`.

**The network is not at fault.** `tcpdump` shows a complete, correct E2E exchange:

```
21:11:22.223003  192.168.1.113.319 > 224.0.1.129.319: delay req,  seq 296, clock 0x56001efffe000451
21:11:22.223195  192.168.1.119.320 > 224.0.1.129.320: delay resp, seq 296, port identity: 0x56001efffe000451
```

The master answers in 192 µs with the matching sequence ID and the board's own port
identity. The board receives the Delay_Resp and **discards it**.

**Where it is discarded** — `slave.c`, in the Delay_Resp branch:

```c
// try fetching Delay_Req timestamp
if (!ptp_read_and_clear_transmit_timestamp(RPMT_DELAY_REQ, &S.slave.scd.t[T3])) {
    return;                         // <-- taken every time
}
```

Without T3 there is no path delay, so no offset, so no correction. The T3/T4 values printed
by `ptp log ts on` are stale — T4 precedes T3, which is physically impossible.

**Narrowed to the TX path only.** Receive-side hardware timestamping works: T1/T2 carry
real values, so the MAC captures RX timestamps and they reach flexPTP through the context
descriptor. It is specifically the *transmit* timestamp that never arrives.

Ruled out so far:

- `ETHHW_TXOPT_CAPTURE_TS` is `0b11` and `eth_drv_lwip.c` passes exactly that, so the
  `txOpts == ETHHW_TXOPT_CAPTURE_TS` equality test in `ETHHW_Transmit()` succeeds and
  `ETH_DMATXNDESCRF_TTSE` is set.
- `0b11` includes the interrupt bit, so `ETH_DMATXNDESCRF_IOC` is set too and a TX
  interrupt is raised.
- `MACTSCR.TXTSSTSM` is 0, so the MAC reports TX timestamps **in the descriptor**, not in
  `MACTTSSNR/SR`.
- The failure is systematic (135+ consecutive), not a race with the RX branch of the ISR.
- `ETHHW_ProcessTx()` does run — TX descriptors are found zeroed with `tsCbPtr` cleared,
  which only that function does.

### Instrumented result — the driver is not at fault

Counters added to `mac_drv.c` and read over SWD, one atomic snapshot:

| Counter | Value |
|---|---|
| `txTsRequested` — `ETHHW_Transmit()` set `TTSE` | 698 |
| `isrTi` — ETH ISR saw a TX interrupt | 698 |
| `procTxCalls` — `ETHHW_ProcessTx()` ran | 698 |
| `descWithTs` — descriptors found carrying a timestamp | 698 |
| `cbCandidates` — reached the dispatch site | 698 |
| `cbPtrNull` — callback pointer was NULL | **0** |
| `cbDispatched` — callback actually invoked | **698** |

**698 for 698, with no losses at any stage.** The MAC captures every requested transmit
timestamp, writes it back with `TTSS` set, `ETHHW_ProcessTx()` finds it, and the callback
fires with a valid pointer. `mac_drv.c`, `eth_drv_lwip.c` and the F48 tag round-trip are
all correct.

`isrBoth` = 0, so the `else if` between the ISR's RX and TX branches was never implicated —
that path did not occur once. (It was changed to independent `if`s anyway, being latently
wrong, but it was not this defect.)

### Where the timestamp is actually lost

Inside flexPTP, in its own transmit bookkeeping:

```
ptp_transmit_timestamp_cb(uid) → sTxCbFIFO → PTN_TRANSMIT_DONE handler
    → msgb_get_by_uid(&sRawTxMsgBuf, ts.uid)      ← miss here drops the timestamp
    → msgb_set_sent(...)
                     ↓
slave.c: ptp_read_and_clear_transmit_timestamp(RPMT_DELAY_REQ)
    → msgb_get_sent_by_tag(&sRawTxMsgBuf, RPMT_DELAY_REQ)   ← NULL → Delay_Resp discarded
```

Either `msgb_get_by_uid()` fails to match when the timestamp arrives, or the message is
matched but its **tag** is not `RPMT_DELAY_REQ` at lookup time. Both are message-buffer
bookkeeping in the submodule, not project code.

*This message-buffer theory was not borne out* — see the resolution above. The library was
behaving correctly; the driver was starving it of transmit timestamps.

### Resolution — PTP synchronises

After the ISR change described below, the servo runs and holds lock. 150 consecutive
samples against a `linuxptp` grandmaster:

| Metric | Value |
|---|---|
| Time error, mean | **−535 ns** |
| Time error, std dev | 83 µs |
| \|error\| median | 47 µs |
| Best sample | **−69 ns** |
| Addend range | 3435557497 … 3436028351 |
| Steady-state frequency offset | **−41 ppm** |

The mean of −535 ns over 150 samples is the meaningful figure: the servo holds the clock
centred rather than drifting. The addend is tuned around the `0xCCCCCCCC` derived in
[ptp_clock.md](ptp_clock.md), settling ~41 **ppm** below nominal — the ST-Link MCO's real
frequency error, measured and corrected.

> **Corrected: this figure read "−41 ppb" until re-measured on 2026-08-12.** The magnitude
> was right and the unit was wrong by a factor of 1000. −41 ppb would be 141 addend units;
> the departure from nominal is ~141 **thousand**. The quoted addend range above is itself
> the proof — its own midpoint is tens of ppm from nominal, which no ppb-scale figure can
> explain.
>
> The error is easy to make because two different quantities are both reported in ppb:
>
> | Quantity | Where | What it is |
> |---|---|---|
> | `corr_ppb` column | servo log line | the **per-cycle increment** the servo applies this cycle. Mean tends toward zero once locked; it is a residual drive, not an offset |
> | `(addend − 0xCCCCCCCC) / K` | not logged; derive it | the **total** frequency offset now applied, where `K` = `PTP_ADDEND_CORR_PER_PPB_F` ≈ 3.436 addend units per ppb |
>
> They differ because [`slave.c`](../../../../Modules/flexptp/src/flexptp/slave.c) *accumulates*:
> `compAddend = addend + (int64_t)(tuning_ppb * K)`. Confirmed against 209 strictly
> consecutive log lines — `addend[n] − addend[n−1]` matches `corr_ppb[n] × K` with a maximum
> residual of **1.00 unit**, exactly the `(int64_t)` truncation.
>
> Re-measured 2026-08-12 over 285 and 211 samples in separate runs: **−41.40 ppm** and
> **−40.47 ppm** (stderr 0.96 ppm), against a mean `corr_ppb` of +182 and a median of +27 —
> which is the residual drive, near zero as expected, and nowhere near −41 of anything.
> Tens of ppm is an unremarkable figure for an ordinary crystal oscillator.

The ±83 µs spread is **host-side, not the board**. The test NIC has no PHC, so `-S` places
master timestamps in software at the mercy of kernel scheduling. The −69 ns sample shows
what the hardware achieves when that jitter happens to cancel.

### The ISR `else if` was NOT the cause — disproven by experiment

`ETHHW_ISR()` used `else if` between its receive and transmit branches, so a combined
RI+TI interrupt serviced only RX. That looked like an excellent explanation. **It is wrong.**

Reverting the change and re-testing showed the instrumentation counters unchanged:
**187 of 187** transmit timestamps requested, captured, found and dispatched, with
`isrBoth` = 0. The transmit path is unaffected by the branch structure. The change is kept
because the exclusive form is latently wrong under load, not because it fixes anything.

A second hypothesis — that halting the core over SWD was perturbing the system into working
— was also tested and disproven: a halt/resume in the failing state changed nothing.

> **Two wrong attributions in a row, and the reason is worth recording.** `isrBoth = 0` was
> first read as evidence *against* the ISR theory, then the theory was adopted anyway when
> the symptom disappeared after a reflash. The counter had shipped in the same build as the
> fix, so it only ever measured the post-fix world, and the symptom's disappearance was
> coincidental. Correlation across a reflash is not evidence; only the revert experiment
> was.

### What is actually established

- **PTP does synchronise.** Two independent sessions, servo running and holding:
  mean time error −535 ns over 150 samples in the first, best single samples of −69 ns and
  −2634 ns. The addend is tuned around the derived `0xCCCCCCCC`, settling ~41 **ppm** below
  nominal — the ST-Link MCO's real frequency error, measured and corrected. (This read
  "ppb" until re-measured on 2026-08-12; see the correction above.)
- **The ±83–120 µs spread is host-side**, from software timestamping on a NIC with no PHC.
- **There is a startup failure lasting 40–60 s.** From `flexptp` start, every Delay_Req
  reports *"no response received"*, then synchronisation begins and is stable thereafter.
  Observed in 2 of 2 sessions with the current build.
- **The transmit timestamp path is not at fault** — 100 % delivery in both builds.
- **The Delay_Resp is on the wire and correct** — verified by `tcpdump`.

So the failure is in flexPTP's matching of a delivered timestamp to the Delay_Req that
requested it (`msgb_get_by_uid` / `msgb_get_sent_by_tag`), and it is **transient**: whatever
state blocks the match clears itself within a minute.

*One observation not yet followed up:* with `else if` restored the failure persisted through
Del_Req #261 (~4.5 minutes) without recovering, versus 40–60 s with independent branches.
That is suggestive but was observed once, and one run does not distinguish it from
variance.

### RESOLVED: the network was discarding Delay_Resp

Connecting the host NIC directly to the board, removing the switch and changing nothing
else, settles it:

| Path | Delay_Resp received | Failures |
|---|---|---|
| Through the site switch | **0** | 1 per second, indefinitely |
| **Direct cable** | **65** (≈1/s) | **0** |

The switch was forwarding Sync, Follow_Up and Announce to the same multicast group
(224.0.1.129) and the same destination MAC, while discarding Delay_Resp. Nothing in this
port was ever at fault.

**This also explains the intermittency, and every false attribution.** The switch forwarded
Delay_Resp *occasionally*. Twice the board synchronised through it, each time shortly after
a code change, and each time the change was credited with the fix — the ISR `else if`, then
the `ETHHW_ProcessTx()` rewrite. Both were coincidence: an external, intermittent cause
produced apparent confirmation for whatever had just been altered. Reverting disproved each
in turn, but the pattern kept recurring because the real variable was outside the system
under test entirely.

Confirmed in passing: the board's DHCP lease on the direct link reported MAC
`56:00:1e:00:04:51`, exactly the address derived from the device UID — **F51's fix verified
end to end**.

### Earlier localisation: the MAC never receives it

Instrumenting `ETHHW_ReadCallback()` — every frame the MAC hands up, classified by UDP port
and PTP message type, plus a count of frames dropped because `rx_msg_q` was full:

| Measure | Value |
|---|---|
| Frames offered by the MAC | 239 |
| UDP 319 (event) / 320 (general) | 69 / 103 |
| Sync (0x0) | 69 |
| Follow_Up (0x8) | 69 |
| Announce (0xB) | 34 |
| **Delay_Resp (0x9)** | **0** |
| Dropped, `rx_msg_q` full | **0** |

The frame never enters the descriptor ring. Every software layer is therefore exonerated:
flexPTP, the NSD, lwIP, the queue between the ISR and the RX thread, and the MAC driver.
Frames to the *same* multicast MAC arrive normally in both directions.

### Hypotheses tested

| # | Hypothesis | Outcome |
|---|---|---|
| 1 | The frame never reaches this board | **Remaining** — all on-chip explanations eliminated |
| 2 | The MAC address filter rejects it | **Ruled out**: `MACPFR` = `0x410`, `PM` (pass all multicast) = 1. A multicast frame cannot be filtered while another to the same MAC passes. |
| 3 | PTP snapshot config filters reception (`SNAPTYPSEL`) | **Disproven by experiment**: rebuilt with `SNAPTYPSEL = 0b00`; Delay_Resp still 0 of 245 frames. RM0481 was right that these bits select what is *timestamped*, not what is *received*. |
| 4 | The MAC receives it but discards it as errored | **Ruled out**: `MMCRCRCEPR` (CRC errors) = 0, `MMCRAEPR` (alignment errors) = 0, `DMACMFCR` (missed frames) = 0. |

Live register state confirming 2 and 4:

```
MACTSCR = 0x00012E03   TSENA=1 TSCFUPDT=1 TSVER2ENA=1 TSIPENA=1 TSIPV4ENA=1
                       TSEVNTENA=0 TSMSTRENA=0 SNAPTYPSEL=0b01
MACPFR  = 0x00000410   PR=0  PM=1  DBF=0  HPF=1
CRC errors 0, alignment errors 0, DMA missed frames 0
```

### Conclusion: the frame does not arrive at the board

Every layer from flexPTP down to the MAC's receive filter and error counters has been
measured and exonerated. `tcpdump` proves only that the **host transmitted** the Delay_Resp;
it says nothing about whether the switch forwarded it to the board's port.

The board receives Sync, Follow_Up and Announce — all to the same multicast group
(224.0.1.129) and the same destination MAC — so plain group forwarding works. Something
about the Delay_Resp specifically is not being delivered across the network.

**`hybrid_e2e` tested — no change.** With `ptp4l --hybrid_e2e=1`, which replies to a
Delay_Req with a **unicast** Delay_Resp, a 30-second delta at the MAC gave:

| Message | Δ30 s |
|---|---|
| Sync | +31 |
| Follow_Up | +31 |
| Announce | +15 |
| **Delay_Resp** | **0** |

So the fault is not multicast forwarding. A unicast Delay_Resp addressed directly to this
board traverses the same path and also fails to arrive.

Worth noting what the MAC cannot be doing: every one of these messages carries the **same
destination MAC** (`01:00:5E:00:01:81`, derived from 224.0.1.129) in the multicast case, so
with `PM = 1` the MAC is incapable of distinguishing them. Whatever discriminates must sit
outside the chip.

**Remaining test — direct cable.** Connect the host NIC straight to the board, removing the
switch entirely. It changes several variables at once, which is why it was not the first
choice, but every cheaper discriminator is now exhausted. If Delay_Resp arrives on a direct
link, the switch is discarding it; if it still does not, attention returns to the host's
transmit path, since `tcpdump` on the sending host proves only that the frame was handed to
the kernel, not that it reached the wire.

### Earlier localisation: lwIP never delivers Delay_Resp

Instrumenting `ptp_receive_cb()` — what lwIP actually hands to the PTP network-stack
driver — is decisive:

| Message | Type | Delivered | Port |
|---|---|---|---|
| Sync | 0x0 | 65 | 319 (event) |
| Follow_Up | 0x8 | 65 | 320 (general) |
| Announce | 0xB | 33 | 320 (general) |
| **Delay_Resp** | **0x9** | **0** | — |

163 callbacks, `shortPkt` = 0, `onOther` = 0. **The general socket on port 320 works** — it
delivers 98 packets — yet not one is a Delay_Resp, while `tcpdump` shows them on the wire
~200 µs after each Delay_Req.

The drop is therefore *below* the network-stack driver, inside lwIP or the Ethernet receive
path, and it is **selective**: same socket, same port, same multicast group, one message
type discarded.

This eliminates an entire class of causes at once — the PTP stack, the NSD, socket binding,
IGMP membership and the domain/`transportSpecific` gate. None of them can discriminate
between two messages arriving on the same PCB.

What distinguishes Delay_Resp from Follow_Up at packet level: it is the only inbound
message this node *solicits*, arriving hard on the heels of our own transmit, and it is a
different length (54 vs 44 bytes of PTP payload). A pbuf or RX-descriptor interaction with
the transmit path fits that shape — which returns attention to `eth_drv_lwip.c` and
`mac_drv.c`, but on the **receive** side, which has not been instrumented at all.

### Earlier localisation: the Delay_Resp never reaches flexPTP

Measured layer by layer, from the wire downwards:

| Layer | Delay_Resp seen? | Evidence |
|---|---|---|
| Wire | **yes** — correct sequence ID and requesting port identity | `tcpdump` |
| lwIP / network-stack driver | **lost here** | — |
| flexPTP core dispatcher | **no** — `drespIn` = 0, `filtered` = 0 of 165 messages | `coredbg` |
| Slave handler | no — `delayRespSeen` = 0 of 136 messages | `slvdbg` |
| `ptp_read_and_clear_transmit_timestamp()` | never called — `readTagCalls` = 0 | `ptpdbg` |
| Transmit timestamp path | fully healthy — 85 requested / 85 dispatched / 85 handled | `ethdbg` + `ptpdbg` |

`filtered = 0` is the decisive figure: the packet is not being rejected by the
domain/`transportSpecific` gate, it simply never arrives. Everything above that line —
the MAC driver, the descriptor handling, the timestamp bookkeeping, the message buffer,
`slave.c` — was **downstream of a packet that never appears**, and was never at fault.
`slave.c`'s early return on a missing T3, the original diagnosis, is unreachable in this
failure.

**What makes Delay_Resp different** from the Announce and Follow_Up that arrive on the same
port-320 socket: it is the only inbound message *solicited* by this node, and the only one
arriving ~200 µs after our own transmission. A receive-buffer or pbuf interaction around the
transmit path is the shape to look at next — instrument `ptp_receive_cb()` in `nsd_lwip.c`
to count arrivals per port and message type.

### Measured facts (stable across many runs)

| Fact | Method |
|---|---|
| Driver dispatches **exactly one** timestamp per request — 125/125, `cbPtrNull` = 0 | SWD counters |
| `uid` and tag (2 = `RPMT_DELAY_REQ`) correct on **both** halves of the match | `PTP_LOG_TRANSMISSION` |
| Delay_Resp correct on the wire, answered in ~192 µs | `tcpdump` |
| flexPTP emits ~**1.15** `---> uid` lines per send — duplicates like `---> 3` at ticks 306 *and* 309 | console log |
| PTP *does* synchronise sometimes: mean −535 ns over 150 samples, best −69 ns | servo log |

The duplicates come from **separate ISR invocations**, not one loop iterating twice, and the
driver counters prove it dispatches once. So the duplication is generated inside flexPTP,
downstream of a clean dispatch — `ptp_transmit_timestamp_cb()` writes `sTxCbFIFO` and posts
`PTN_TRANSMIT_DONE` as two independent queue operations, and the handler that drains them
prints the `---> uid` line. More prints than dispatches means the notification count and the
FIFO depth have drifted apart, so the handler re-reads a stale entry.

### Four disproven attributions — and why

Each was plausible, each was disproven by measurement, and the pattern is worth recording
because it recurred:

| Attribution | Disproved by |
|---|---|
| ETH ISR `else if` starving TX | Revert: still 187/187 timestamps delivered |
| Debugger halt perturbing the system | Halt/resume in the failing state changed nothing |
| flexPTP message-buffer `uid`/tag mismatch | `PTP_LOG_TRANSMISSION`: both halves match correctly |
| `ETHHW_ProcessTx()` double-dispatching | Counters after the rewrite: 125/125, still failing |
| Missing T3 blocking the Delay_Resp branch | `readTagCalls` = 0 — that code is never reached |

**The recurring error was theorising downstream of an unverified assumption.** Every one of
those five attributions assumed the Delay_Resp was arriving and being mishandled. None of
them tested that assumption, which took one counter to falsify. Instrumenting from the
outside in — wire, then stack, then library — would have found this in one pass instead of
five.

A second, compounding error: treating "changed something, then it worked" as confirmation.
Twice the symptom disappeared after a reflash and was credited to the change; both times a
revert showed the change was irrelevant. Only the experiments that *reproduced the failure
on demand* produced reliable information.

### Status — SUPERSEDED (retained as history)

> Everything in this subsection describes the state of the investigation *before* the direct-
> cable experiment. **F42 is resolved** — see [the resolution above](#resolved-the-network-was-discarding-delay_resp).
> The next step proposed here was never needed, because the fault was not in the flexPTP
> submodule or anywhere else on the board.

~~F42 is **open**.~~ The next measurement — counting `PTN_TRANSMIT_DONE` handler entries
against `sTxCbFIFO` depth — is inside the flexPTP submodule, so it needs a decision about
instrumenting vendored code.

### Superseded diagnosis

This finding previously read "lwIP TX timestamp plumbing broken", attributing the defect to
this project's driver code. The instrumentation above disproves that. The scope was wrong;
the symptom and the blocking status were not.



The README states the low-level lwIP driver must be instrumented to convey RX and TX
timestamps, and links a guide to the H743 `ethernetif.c` amendments. This project does not
use vendor `ethernetif.c`; `Drivers/EthDrv/eth_drv_lwip.c` carries its own plumbing —
RX timestamps from context descriptors via `ETHHW_ReadCallback`, TX timestamps via the
descriptor-extension `tsCbPtr` callbacks driven from `ETHHW_ProcessTx`.

Stage 12 rewrites the code immediately around that machinery and Stage 15 touches
`eth_drv_lwip.c` only for header renames, so nothing confirms the path still works. A
silently broken TX-timestamp path presents as "PTP runs but never converges", which is the
hardest symptom to attribute correctly. Explicit checks added to Stages 14 and 20.

### F51 — the H563 UID region rejects byte accesses

`ETH_MAC_ADDR0..5` derive the MAC address by indexing
`((const uint8_t *)UNIQUE_DEV_ID_BASE)[0..5]`. On the H743 that worked. On the H563 it
raises a **precise bus fault** which escalates to HardFault, wedging the firmware — the
console died mid-banner at exactly 256 bytes because the `serialio` thread was left waiting
on a TX-complete that could never arrive.

Diagnosed over SWD: `CFSR` = `0x00008200` (PRECISERR | BFARVALID), `BFAR` = `0x08FFF800`
— the UID base itself. Access width is the discriminator:

| Access | Result |
|---|---|
| word (`mdw`) | ✅ `001e0055 30355104` |
| half-word (`mdh`) | ✅ `0055 001e` |
| **byte (`mdb`)** | ❌ bus error |

The address was right; the *access width* assumption was carried over from the H7 without
being questioned. Fixed by reading the UID as `uint32_t` and extracting bytes in software
via `UNIQUE_DEV_ID_BYTE(n)`. Verified: full banner, PHY detected, link up, DHCP lease.

**Generalisable:** a peripheral or information-block address being *correct* says nothing
about which access widths it supports. This class of bug is invisible to review, compiles
clean, links clean, and only appears on silicon.

### F52 — PTP addend must be programmed after the subsecond increment

`ptphw_init()`, inherited from the H743 port, programmed `MACTSAR` (addend) before
`MACSSIR` (subsecond increment). Writing `MACTSAR` sets `TSADDREG`, which the hardware only
self-clears once the timestamp counter is running — and with `MACSSIR` still zero it never
runs. `ETHHW_SetPTPAddend()` therefore spun with `MACSSIR` = 0 and `MACSTSR` frozen.

ST's own `HAL_ETH_PTP_SetConfig()` confirms the required order: `TSENA` → `MACSSIR` →
`MACTSAR` → `TSADDREG`. `ptphw_init()` now follows that sequence. Verified on hardware:
`MACSSIR` = `0x00050000` and `MACTSAR` = `0xCCCCCCCC` are both programmed correctly after
the fix.

### F53 — PTP timestamp counter is not clocked *(RESOLVED)*

**Root cause: `PLL1QEN` was never set, so `pll1_q_ck` — the PTP reference clock on this
device — was never generated.** `HAL_RCC_OscConfig()` programmes the `PLLQ` divider but
leaves the output gate closed; `__HAL_RCC_PLL1_CLKOUT_ENABLE(RCC_PLL1_DIVQ)` is required.
This follows directly from the F3 correction above: the port had been built on the belief
that PTP ran from HCLK, so nothing ever enabled PLL1Q.

**Fix:** `PLLQ` 4 → 2 (VCO 500 MHz → `pll1_q_ck` 250 MHz) plus the output enable, in
`init_pll()`. Every derived quantity in [ptp_clock.md](ptp_clock.md) survived unchanged,
because PLL1Q is configured to the same 250 MHz that HCLK happens to run at.

**Verified on hardware:** `RCC_PLL1CFGR` = `0x00030417` (`PLL1QEN` set), `MACTSCR` =
`0x00012e03` (`TSADDREG` clear — the addend update now completes), and `MACSTSR` advancing
7 → 11 across ~4 s of wall clock.

*Original investigation, retained because the elimination was what pointed at the clock:*


After F52, both PTP registers hold their intended values — `MACSSIR` = `0x00050000` and
`MACTSAR` = `0xCCCCCCCC`, exactly as derived in [ptp_clock.md](ptp_clock.md) — but the
counter never advances. `MACSTSR` and `MACSTNR` both read 0 across **4 seconds of genuine
run time**, sampled from two separate debug sessions with the core resumed in between.

**Characterised, not guessed:**

| Observation | Method |
|---|---|
| `TSADDREG` is software-writable and software-clearable | Wrote `MACTSCR` = `0x00012e03`, read back `0x00012e03` |
| Setting `TSADDREG` never completes | Wrote `0x00012e23`; reads back set, indefinitely |
| Not a firmware-logic fault | Reproduced entirely from the debugger with the core **halted** |
| Firmware is stuck in the addend wait loop | `PC` = `0x080045c4`, inside `ETHHW_SetPTPAddend` (`0x080045a6`–`0x08004604`) |
| `TSINIT` reads 0 because it was never *reached*, not because it completed | The Stage-14 ordering puts `InitPTPTime` after `SetPTPAddend` |

**Ruled out:**

- No separate PTP clock enable exists — `RCC_AHB1ENR` offers only `ETHEN`, `ETHTXEN`,
  `ETHRXEN`, all three enabled.
- **RM0481 confirms F3 rather than contradicting it.** `ETHPTPCLKSEL` (CCIPR5[11:10]) and
  `ETHPTPDIV` (CCIPR4[31:28]) *are* documented, but each carries the note *"only available
  on STM32H543/553xx devices"*. The CMSIS header's omission for the H563 is therefore
  correct, and `clk_ptp_ref_i` should default to `rcc_hclk1` — which is demonstrably
  running at 250 MHz.
- The MAC is fully functional: link up, DHCP lease acquired, so ETH is clocked.
- RM0481 §63 (explicitly *"applies only to the STM32H562/63/73xx devices"*) describes the
  standard fine-correction accumulator and documents no additional enable.

So the register block accepts writes but the update engine never runs, while the reference
clock that should drive it is present. Next step is the ETH chapter's clock/reset
requirements and the device errata sheet — this now looks more like a missing enable
condition or a silicon erratum than a configuration mistake.

### F55 — the inherited welcome message gives wrong hardware advice

`print_welcome_message()` carries this text over from the H743 project:

> *"We highly recommend to solder a 8 MHz oscillator onto the designated X3 pads to achieve
> the best results!"*

On the NUCLEO-H563ZI (MB1404) that is wrong in three ways, per UM3115:

1. **X3 is already fitted**, not empty pads — the board ships with the crystal.
2. **X3 is 25 MHz**, not 8 MHz (*"25 MHz, 6 pF, and 20 ppm"*).
3. **Selecting it is not just soldering a part**: it needs SB3 and SB4 ON, SB49 OFF, and
   C70/C71 populated with 5.6 pF.

The underlying advice is still sound — the default HSE is the ST-Link MCO at 8 MHz, which
the project README describes as noisy, and the crystal is the better reference. But a user
following the current text would fit the wrong part.

Acting on it also requires firmware changes: `HSE_VALUE` becomes 25000000, and `PLLM` must
be re-derived, since `init_pll()` assumes `HSE_VALUE / 2000000` yields an integer in the
2–4 MHz `RCC_PLL1_VCIRANGE_1` window (25/2 = 12.5 does not).

**Resolved.** The banner now states that X3 is already fitted at 25 MHz, and gives the whole
recipe rather than half of it:

| | Value | |
|---|---|---|
| Hardware | SB3, SB4 **ON**; SB49 **OFF**; C70/C71 at 5.6 pF | per UM3115 |
| `HSE_VALUE` | 25000000 | |
| `HSEState` | `RCC_HSE_ON` | a crystal, not the MCO's bypass input |
| `PLLM` | **5** → VCI 5 MHz | `HSE_VALUE / 2000000` is 12.5 and silently truncates to 12 |
| `PLLRGE` | `RCC_PLL1_VCIRANGE_2` | the 4–8 MHz window, not 2–4 |
| `PLLN` | **100** → VCO 500 MHz | the `PLLN = TARGET_SYSCLK_MHZ` identity holds only at a 2 MHz VCI |
| `PLLP` / `PLLQ` | 2 / 2, unchanged | SYSCLK 250 MHz, `pll1_q_ck` 250 MHz |

The banner says explicitly that changing `HSE_VALUE` alone is not enough and **fails
silently**, and that the path is untested on this port — nobody has run this board from X3.

### F56 — `PTP_LOG_TRANSMISSION` has no CLI switch

`logging.h` defines `PTP_LOG_TRANSMISSION`, but `cli_cmds.c` registers only
`ptp log {def|corr|ts|info|locked|bmca}`. Attempting to enable it returns *"Unknown command
or bad parameter"*, silently — the CLI reports the whole command as unrecognised rather
than the channel name.

This cost real time: two separate sessions appeared to "start working when transmission
logging was enabled", when in fact the command had been rejected and the sync starting was
coincidence. It can only be enabled programmatically, via
`ptp_log_enable(PTP_LOG_TRANSMISSION, true)` from `flexptp_user_event_cb()`.

### F57 — `ETHHW_ProcessTx()` never reset its per-pass descriptor count

`descsWithTimestamp` was declared outside the `do…while` and never reset per pass, so each
re-scan *added* to the previous count instead of replacing it. The count stayed positive
after the ring drained, and the loop ran again with `firstIteration` still true and `minIdx`
left over from the previous pass — dispatching a descriptor that had already been processed
and cleared.

Rewritten: the scan state is per-pass, the loop exits when a pass finds nothing, and the
descriptor is released *before* the callback runs so it cannot be observed twice.

**This is a real latent bug and the fix is worth keeping, but it is not the cause of F42** —
the counters read 125/125 after the change, with the failure unaffected.

### F59 — one duplicated Sync cycle latches the servo into NaN for ever *(RESOLVED)*

With the delay exchange finally working, the board reached SLAVE, identified the correct
master, and computed a valid offset — yet applied **no correction at all**:

| | Before `ptp reset` | After `ptp reset` |
|---|---|---|
| Offset | 2.18 → 3.62 ms, **diverging** | 5.19 ms → **−45 µs**, converged |
| Addend | frozen at 3435894134 | tuned, Δ −40815 |
| `corr_ppb` | **0 of 77** samples | **50 of 67** non-zero |

The drift while uncorrected was ~18.7 ppm — the free-running oscillator error, exactly what
the servo exists to remove.

The first recorded hypothesis — leftover servo or BMCA state from the preceding failure
period — was **wrong**. The retained serial captures contain the moment of failure, and it
is a single event.

#### The trigger

Eleven consecutive log lines from `direct.log`, one per Sync. The servo is working, then it
is not, and the boundary is exact:

```
T1                       T4                       offset      addend    corr_ppb
1786312152 466717025  1786312152 448344766       -54568   3435909890   -3185.5125
1786312153 466824103  1786312152 448344766       -29726   3435894134   -4585.7671
1786312153 466824103  1786312152 448344766       -29726   3435894134       0.0000  <-- duplicate
1786312154 467150630  1786312153 471275973       -12196   3435894134       0.0000
1786312155 467218774  1786312154 494273634       -13047   3435894134       0.0000
   ... 74 more, addend never moves again ...
```

Two consecutive corrections carry the **same T1** — the same master Sync timestamp. There
is exactly one such duplicate in the entire capture, and it is the first zero sample. Every
sample after it is zero.

#### The mechanism

`slave.c` derives the servo's time base from consecutive Sync timestamps:

```c
subTime(&measSyncPeriod, &syncMa, &(S.slave.prevSyncMa));
int64_t measSyncPeriod_ns = nsI(&measSyncPeriod);
```

A repeated Sync makes `syncMa == prevSyncMa`, so `measSyncPeriod_ns == 0`. In
`kalman_filter_run()` that value is a divisor twice over:

```c
double skew = ((double)(dt - dt_prev)) / ((double)pAux->measSyncPeriodNs);
double DT   = ((double)pAux->measSyncPeriodNs) * 1E-09;   // -> insert_DT(0)
```

The repeated cycle also repeats the offset, so `dt - dt_prev` is zero as well:
**0.0 / 0.0 = NaN**, not infinity. `insert_DT(0)` then fills the measurement covariance `R`
with infinities. NaN enters the state vector `x`, and from that point every prediction and
correction step propagates it. **The filter never recovers**, because nothing in flexPTP
ever tests for it.

#### Why it was invisible

Two independent pieces of arithmetic conspire to make a dead servo look like a healthy one:

- **`corr_ppb` printed `0.0000`, not `nan`.** embfmt's `pfn_double()` splits the value into
  integer and fractional parts with `(uint64_t)d`. Converting NaN to an integer is undefined
  in C, and Arm's `VCVT` returns **0** — so a NaN formats as a perfectly plausible zero.
  (On x86 the same code prints `9223372040447430051.92…`, which would have been noticed
  immediately. See F60.)
- **The addend froze rather than exploding.** `ptp_tune_clock()` does
  `(int64_t)(tuning_ppb * PTP_ADDEND_CORR_PER_PPB_F)`; the same Arm conversion turns NaN
  into 0, so the addend was incremented by zero, forever.

Had either behaved like x86, this would have been a five-minute diagnosis.

#### Proof

Compiled the **unmodified** `kalman_filter.c` for the host and replayed the offsets from the
capture, with `measSyncPeriodNs = 0` on the eleventh sample only:

```
cycle  measSyncPeriodNs        dt        corr_ppb
    9        1000000000    -29726      -4584.4570      <- matches the board (-4585.7671)
   10                 0    -29726             nan      <- the duplicate
   11        1000000000    -12196            -nan
   12        1000000000    -13047             nan      ... and every sample thereafter
```

The replayed values track the board to four significant figures, and one zero-length period
is sufficient and permanent. Same exercise on embfmt confirmed `% 8.4f` of a NaN yields
`       0.0000`.

#### The fix

Three guards, because any one of them alone would have prevented this:

| Where | Guard |
|---|---|
| `slave.c`, `ptp_perform_correction()` | Skip the cycle when `measSyncPeriod_ns <= 0`, so no servo — Kalman, PID or otherwise — is ever handed a zero time base |
| `servo/kalman_filter.c` | Skip a non-positive period, and **reset the filter** if the state or the tuning output goes non-finite, so a single bad sample costs one cycle rather than the session |
| `slave.c`, `ptp_tune_clock()` | Ignore a non-finite tuning value instead of converting it to an unpredictable integer |

Both were submodule changes, carried as `patches/flexptp/0002-servo-nan-latch-fix.patch` until
upstream took them: they are now in flexPTP itself as
[`2adbe6a`](https://github.com/epagris/flexPTP/commit/2adbe6a99c55666e43f297416e5e0452b51128b7)
([PR #15](https://github.com/epagris/flexPTP/pull/15)), and the pin carries them unpatched.

#### Verified on hardware

Reflashed, board on the direct link against `ptp4l -i enp5s0 -S -m --serverOnly=1
--priority1=100`, `default` profile:

| | Before the fix | After the fix |
|---|---|---|
| Samples applying a correction | 0 of 77 | **190 of 191** (the one zero is the servo's own first cycle) |
| Offset, last 60 samples | diverging past 3.6 ms | signed mean **+40 ns** (locked), magnitude 26 µs typical, 87 µs peak |
| `mpd_ns` / `sync_period_ns` columns | printed as `d u` | `~30 µs` / `~1.0001 s` |

The residual is the host's software-timestamping floor, not the board.

This matters more than the fix does: the stack presented as entirely healthy — SLAVE state,
correct master clock ID, delay exchange flowing, offsets computed and logged once a second —
while silently never disciplining the clock. **Every visible indicator was green.**

### F60 — embfmt drops `%ll` conversions and renders NaN as `0.0000` *(RESOLVED)*

Two defects in `Modules/embfmt/embformat.c`, both of which cost real time during F59.

**`ll` is not parsed.** `process_format_word()` accepts at most one `l`:

```c
word->length = LEN_NORMAL;
if (*str == 'l') { word->length = LEN_LONG; str++; }
```

For `%lld` the designator lookup then sees `l`, returns `UNKNOWN`, and the conversion is
dropped — leaving the trailing `d` as literal text and consuming no argument. This is why
every servo line ended in a bare `d u`:

```
... 3435894134  0.0000 d u
                       ^ ^-- sync_period_ns, "% 9llu"
                       +---- mpd_ns, "% 9lld"
```

flexPTP uses `__PRI64_PREFIX` (`"ll"` under newlib) throughout, so the mean path delay, the
measured Sync period and the coarse-correction threshold message were **all unprintable**.
The mean path delay would have exposed F42 far sooner. `LEN_LONG` already reads
`int64_t`/`uint64_t`, so accepting a second `l` is a two-line change.

**NaN formats as a plausible number.** `pfn_double()` has no non-finite path; see F59 for
why that is worse than it sounds. Now prints `nan` / `inf` / `-inf`.

Recorded in
[`patches/embfmt/0001-formatting-fixes.patch`](../../../../patches/embfmt/0001-formatting-fixes.patch).
Both are worth reporting upstream: they are target-independent.

### F61 — `ETHHW_ProcessRx()` re-delivers frames already queued *(RESOLVED)*

F59's trigger — one frame delivered twice, with identical T1, T2 *and* offset — turned out to
be a race in the RX descriptor handoff, and a far larger one than the PTP symptom suggested.

#### The race

`ETHHW_ProcessRx()` decided whether a descriptor still needed processing from the OWN bit
alone:

```c
#define ETHHW_DESC_OWNED_BY_APPLICATION(bd)  (!(((bd)->desc.DES3) & ETH_DMARXNDESCRF_OWN))
```

It walks *backwards* from `DMACCARDR` to find the oldest application-owned descriptor, then
forwards, queueing each frame on `rx_msg_q`. On the normal path the descriptor is deliberately
**not** restored — ownership returns to the DMA only later, in `rx_thread`, after the frame has
been copied into a pbuf and passed to lwIP.

So the OWN bit is clear for the entire span between the MAC writing the frame and the thread
restoring it. `ETHHW_ProcessRx()` runs from the ISR, which preempts that thread. **Any RX
interrupt arriving inside the span re-scans the queued descriptors and delivers those frames
again** — same buffer, same captured timestamp. That is why the duplicate carried an identical
T2: T2 is the board's own hardware RX timestamp, so a retransmission could never have produced
it.

#### How often

Instrumented `ETHHW_ProcessRx()` to count the contiguous run of already-queued descriptors the
old backward walk would have swept in:

| Traffic | Frames | Would have been re-delivered |
|---|---|---|
| PTP only, ~40 s | 204 | **116 (57%)**, across 70 of 204 passes |
| + 3000 pings at 500 pps | 3022 | 13 (0.4%) |

The rate is inverted from the intuition. Under sustained load `rx_thread` is already awake and
spinning on the queue, so it restores each descriptor almost immediately and the window closes.
At low rates the thread must be woken from `osMessageQueueGet()`, and PTP traffic arrives in
tight pairs — Sync then Follow_Up, Delay_Req then Delay_Resp — so the second frame's interrupt
lands squarely in the wake-up window.

**Over half of all received frames were duplicated**, not one in 150. The PTP-visible rate was
far lower because a duplicate only produces a duplicate *correction* when it is a Follow_Up
arriving in `SWaitFollowUp`; in any other state `slave.c` discards it. F59 was the one case
where a duplicate got through to something that mattered.

#### The fix

Give the descriptor the state it was missing. `ETHHW_DescExt` had an unused `uint16_t pad0`,
so `inFlight` costs nothing in layout:

| Change | |
|---|---|
| `ETHHW_DESC_NEEDS_PROCESSING()` | application-owned **and** not in flight; used by both walks. An in-flight descriptor is always older than anything fresh, so it is the correct boundary and no new frame is skipped |
| `ETHHW_ProcessRx()` | sets `inFlight` **before** dispatching, not after — the thread may consume and restore the descriptor before the callback returns, and setting the flag then would strand it permanently |
| `ETHHW_RestoreRXDesc()` | returns ownership **first**, clears `inFlight` second. The reverse order leaves a window where the descriptor looks free and fresh; this order makes a concurrent ISR see OWN set and skip it |

#### Verified

With the fix, and the counters still in place: 201 servo samples over five minutes, **zero
duplicate corrections** by the F59 signature test, 200 of 201 applying a correction. Rebuilt
without instrumentation: 111 of 111 correcting, zero duplicates, offset signed mean +16 ns with
a magnitude of 28 µs.

### F66 — embfmt misreports small `%e` values and any rounding carry *(RESOLVED)*

Found by actually exercising every CLI command for Stage 20, which is the point of that
verification item.

`ptp servo st` reported `sigma_theta^2 = 5.100000e-8`. The default is **`1E-16`**. The other
two were wrong in the same direction, and the arithmetic identifies the cause exactly:

| Variable | True value | `+ 0.51 × 10⁻⁷` | Reported |
|---|---|---|---|
| `sigma_theta^2` | 1e-16 | 5.100000e-8 | `5.100000e-8` |
| `sigma_gamma^2` | 1e-12 | 5.100100e-8 | `5.100100e-8` |
| `sigma_theta_m^2` | 5e-11 | 5.105000e-8 | `5.105000e-8` |

`pfn_double()` added its rounding constant — `0.51 × 10^-(precision+1)`, i.e. `0.51e-7` at the
default precision — **before** normalising the exponent. The constant is sized for the
fixed-point form, so against an unnormalised value any magnitude below it is simply swallowed.
Zero printed as `5.100000e-8` as well.

Fixed by normalising first and rounding second, with a guard for zero (which never enters
`[1,10)`, so the normalisation loop would not terminate).

#### And a second bug underneath it

Checking the fix against values that straddle a rounding carry exposed an unrelated defect in
the shared fraction code:

```c
uint32_t additional_integer = frac_part / power(10, fmt->precision);
frac_part -= additional_integer * 10;      /* should be * power(10, precision) */
```

When rounding carried into a new integer digit, only `10` was removed from the fraction
instead of `10^precision`, leaving the remainder intact:

| Input | Before | After | Correct |
|---|---|---|---|
| `9.9999999` | `10.999990` | `10.000000` | `10.000000` |
| `1.9999999` | `2.999990` | `2.000000` | `2.000000` |
| `99.9999999` | `100.999990` | `100.000000` | `100.000000` |

**This one affects `%f`, not just `%e`** — the code is shared. It is latent in this project
rather than harmless: `corr_ppb` is printed with `% 8.4f`, so a correction of 9.99999 ppb
would have printed as `10.9990`.

#### Verified

On the target after the fix, the three servo commands report `1.000000e-16`, `1.000000e-12`
and `5.000000e-11` — matching `kalman_filter.c`'s defaults exactly.

One residual is left alone and documented in `patches/embfmt/README.md`: `%e` renders a
mantissa that rounds up to ten as `10.000000e+0` rather than `1.000000e+1`, and the exponent
is not zero-padded. Both are cosmetic and the value is correct.

### F65 — the debugger's FreeRTOS awareness was broken *(RESOLVED)*

Both OpenOCD configurations in `.vscode/launch.json` ran:

```
$_CHIPNAME.cpu0 configure -rtos FreeRTOS
```

`cpu0` is H743 naming. On the H5 the target is `stm32h5x.cpu`, and OpenOCD rejects the other:

```
invalid command name "stm32h5x.cpu0"
```

So RTOS thread awareness — the reason that line exists — never worked in the VS Code debugger,
and the failure appears only in the OpenOCD server log where it is easy to miss. Corrected to
`$_CHIPNAME.cpu`, and both forms were run against the board to confirm which one the target
accepts.

Found only because Stage 19 checked the configuration against hardware instead of proof-reading
it. The board name, `device`, `svdFile` and config-file substitutions in the same file were all
wrong too, but those fail loudly; this one did not.

### F64 — ST's OpenOCD does not build as specified *(RESOLVED)*

Stage 18's recipe was written from the documentation, not from a run. A cold execution found
two errors in it.

**`-Werror` is on by default and the tree does not survive current GCC.** OpenOCD's
`configure.ac` defaults `gcc_werror` to the value of `gcc_warnings`, which is enabled for git
checkouts. ST's fork is 0.12.0-era and GCC 15.2 rejects it:

```
cc1: all warnings being treated as errors
make[2]: *** [src/jtag/drivers/libocdjtagdrivers_la-angie.lo] Error 1
make[2]: *** [src/jtag/drivers/libocdjtagdrivers_la-ulink.lo]  Error 1
make[2]: *** [src/jtag/drivers/libocdjtagdrivers_la-ti_icdi_usb.lo] Error 1
```

All three are USB adapter drivers this project never uses, but the build stops regardless.
`--disable-werror` is therefore **required**, not optional, and the configure line is now:

```
./configure --prefix="${OPENOCD_DIR}" --enable-stlink --disable-werror
```

**The prerequisite list named a binary that does not exist.** The plan said to check for
`libtool`. On Debian and Ubuntu the `libtool` *package* installs `/usr/bin/libtoolize` and
**no** `/usr/bin/libtool`, so checking the obvious name reports a failure on a machine that is
fully ready to build — exactly what happened on this host, which had already built OpenOCD
successfully. The check tests `libtoolize`, which is what `./bootstrap` actually invokes.

Verified by a genuinely cold run: `toolchains/openocd` and `toolchains/openocd-src` moved
aside, then `./build.sh --with-openocd` exercised clone, bootstrap, configure, make and
install from nothing, and went on to build the firmware. The resulting binary programmed the
board:

```
** Programming Finished **
** Verify Started **
** Verified OK **
** Resetting Target **
```

and the board booted and took its DHCP lease.

### F62 — the release build HardFaulted; F51's fix was optimisation-dependent *(RESOLVED)*

Stage 16 asked for a clean build. Building the **release** preset for the first time revealed
that it had never worked: no serial output, no DHCP, no ping. The debug build on the same
tree was fine.

Halted over SWD:

```
xpsr 0x21000003   exception 3 = HardFault
CFSR 0x00008200   BFARVALID | PRECISERR
BFAR 0x08FFF800   the UID region
HFSR 0x40000000   FORCED
pc   0x0800235c   Default_Handler self-loop
```

The same address as F51. The F51 fix reads the UID through a `const uint32_t *` and extracts
bytes by shifting, which is correct C — but the pointee is ordinary constant memory as far as
GCC is concerned, so **nothing forbids load narrowing**. At `-Og` it converted the word load
and shift straight back into byte loads. Visible in the disassembly of `low_level_init`:

```
ldrb.w  r1, [r3, #2049]   @ 0x801     base r3 = 0x08FFF000
ldrb.w  r1, [r3, #2050]   @ 0x802
ldrb.w  r1, [r3, #2051]   @ 0x803
```

Byte reads at 0x08FFF800–805 — precisely the access F51 established as faulting. The debug
build worked only because `-O0` optimises nothing.

**Fix:** `volatile const uint32_t *`. `volatile` forbids the compiler from changing the width
or number of accesses; the shifting extraction is then honoured as written. After the change
the same function emits:

```
ldr.w   r3, [r2, #2048]   @ 0x800
ldr.w   r3, [r2, #2052]   @ 0x804
```

Verified on hardware: the release build now boots, acquires 192.168.2.141, answers ping, and
runs PTP — 91 of 92 samples applying a correction, zero duplicates, no timeout reports.

The wider lesson is that a workaround for a hardware access-width constraint **must** be
expressed with `volatile`. Written as ordinary C it is not a constraint at all, merely a
suggestion the optimiser is free to ignore — and it will, at any level above `-O0`.

### F63 — lwIP DHCP `-Wstringop-overflow` in optimised builds *(RESOLVED by P6.6)*

The release build emits four instances of:

```
dhcp.c:1454:30: warning: writing 1 byte into a region of size 0 [-Wstringop-overflow=]
  options[options_out_len++] = value;      /* dhcp_option_byte */
  note: at offset 68 into destination object 'options' of size 68
```

inlined into `dhcp_discover` and `dhcp_reboot`. The buffer is `DHCP_OPTIONS_LEN` = 68 (lwIP's
`DHCP_MIN_OPTIONS_LEN` default). The bound on `options_out_len` exists only inside
`LWIP_ASSERT`.

**Correction (finding D-a).** This section used to say that assertion "compiles to nothing here
— `LWIP_DEBUG` is not defined". That is wrong twice over. `LWIP_ASSERT` is gated on
`LWIP_NOASSERT`, not `LWIP_DEBUG` (`lwip/debug.h:115`), and `LWIP_NOASSERT` is defined nowhere
in this project — so the assertion was always compiled. The real reason the optimiser had no
constraint to reason from is that the assertion did not *stop*: the old macro printed and fell
through, so the write happened whether the bound held or not, and GCC correctly reported the
worst case.

That also made the mistake load-bearing rather than cosmetic: believing lwIP's assertions were
compiled out is precisely why nobody noticed they had never worked (F72).

Not reachable in this configuration. `LWIP_NETIF_HOSTNAME` is 0, so neither path emits the
variable-length hostname option; the longest message is `dhcp_reboot` at roughly 16 bytes —
message type (3), requested IP (6), parameter request list (5–6), end marker (1) — against
68 available.

No longer present. P6.6 gave `LWIP_PLATFORM_ASSERT` a halt, which makes the assertion
`noreturn` as far as GCC is concerned, so the bound on `options_out_len` finally constrains the
path that follows. The Release build went from **4 warnings to 0** with no change to lwIP
itself — the submodule stayed pinned and unpatched, exactly as this section wanted.

### F58 — RX pbuf copy never advanced the source pointer *(RESOLVED)*

`eth_drv_lwip.c`, copying a received frame into a pbuf chain:

```c
for (q = p; (q != NULL) && (size_left > 0); q = q->next) {
    u16_t copy_size = MIN(size_left, q->len);
    memcpy(q->payload, evt.data.rx.payload, copy_size);   /* source never advances */
    size_left -= copy_size;
}
```

Every segment was filled from the *start* of the frame, so any chained pbuf was corrupted.
Latent as configured — `PBUF_POOL_BUFSIZE` and `ETH_BUFFER_SIZE` are both 1528, so a frame
never spans two pool buffers. Inherited from the H743 project.

**Tested rather than assumed.** Temporarily setting `PBUF_POOL_BUFSIZE` to 256 makes a
1500-byte frame span six pbufs. The result was more decisive than expected: the board could not
complete **DHCP** — OFFER and ACK are several hundred bytes, so they chained, were corrupted,
and lwIP dropped them. The board never acquired an address and was unreachable.

With the source pointer advanced, on the same 256-byte configuration: DHCP completed
immediately, and `ping` at 100, 500, 1000 and 1400-byte payloads all returned 0% loss with
`-p` pattern verification passing — so content, not merely delivery, is correct.
`PBUF_POOL_BUFSIZE` was then restored to 1528.

### F54 — unbounded hardware waits *(RESOLVED)*

Recorded against `ETHHW_SetPTPAddend()`, but the driver had **six** of them, all the same
shape — set a bit, spin until the hardware clears it, forever if it never does:

| Site | Hangs on |
|---|---|
| `ETHHW_Init()` | DMA software reset (`SWR`) |
| `ETHHW_ReadPHYRegister()` | MDIO busy — a missing or unpowered PHY hangs the boot |
| `ETHHW_WritePHYRegister()` | MDIO busy |
| `ETHHW_InitPTPTime()` | `TSINIT` |
| `ETHHW_UpdatePTPTime()` | `TSUPDT` / `TSINIT` |
| `ETHHW_SetPTPAddend()` | `TSADDREG`, eight times over |

This is what turned F53 from a diagnosable fault into a silent hang: the timestamp clock was
absent, `TSADDREG` never cleared, the flexPTP task stopped inside the driver, and the PTP CLI
went dead with nothing said about why.

All six now go through one helper:

```c
static bool ETHHW_WaitBitsClear(volatile uint32_t *reg, uint32_t mask, const char *what);
```

The bound is a **spin count, not a time**. These handshakes resolve in nanoseconds; the helper
must work before any timebase is guaranteed (the DMA reset runs during `ETHHW_Init`) and must
be callable from an ISR. 100 000 spins is a few milliseconds at 250 MHz — six orders of
magnitude of headroom. On expiry it prints which handshake failed. A *silent* timeout would
have been worse than the hang, so it is deliberately loud.

`ETHHW_SetPTPAddend()` now returns `bool`, and the port layer reports a refusal directly:
*"hardware clock did not accept the addend; timestamp unit is not running"* — the message F53
should have produced.

#### The 8× loop was superstition

`ETHHW_SetPTPAddend()` wrote the addend and pulsed `TSADDREG` **eight times per call**, once
per servo update. Almost certainly a workaround for F52 (the init-ordering bug, since fixed)
rather than anything the hardware requires — RM0481 asks for one write.

Reduced to one and measured, rather than assumed:

| | 8 writes | 1 write |
|---|---|---|
| Samples applying a correction | 111 of 111 | 119 of 120 |
| Distinct addend values observed | — | **120 of 120** — every write lands |
| Offset magnitude, last 60 | 28.5 µs | 20.2 µs |
| Timeout reports | — | none |

Every single write reaches the hardware, so the repetition bought nothing. The servo is
unaffected; the difference in offset magnitude is run-to-run variation on a
software-timestamping master, not an improvement to claim.

### F41 — dead relative clock-update path removed *(RESOLVED)*

`ETHHW_UpdatePTPTime()` was unreachable — its only caller was the `PTP_UPDATE_CLOCK` macro,
which the flexPTP core never invokes; coarse correction goes through `PTP_SET_CLOCK` →
`ETHHW_InitPTPTime()`. Its subtract path was also wrong:

```c
nsec = 1000000000 - (nsec >> 1);   /* the >> 1 has no justification */
```

RM0481's relative update wants `10^9 − nsec` with the ADDSUB bit set. The shift halves every
backward correction.

**Removed rather than fixed.** Repairing arithmetic in a function that has never executed
produces something that *looks* verified and is not — a worse trap than its absence, and the
kind of thing that costs a day when someone eventually routes through it. Git history keeps
it. The header declaration went with it, and `mac_drv.c` carries a note saying a relative
update should be written against the reference manual rather than resurrected.

Verified: clean build, zero warnings, and a hardware run afterwards since the edit sits next
to the coarse-correction path — 87 of 88 samples correcting, no duplicates, no timeout
reports, offset magnitude 13.8 µs.

### F49 — `task_ptp_is_operating()` renamed upstream

`Src/cmds.c` calls `task_ptp_is_operating()`, which existed at the H743-pinned `62fc906`
but was renamed to `is_flexPTP_operating()` at `a60190f` — the same commit behind F47.
Third consequence of the newer submodule pin, and the one that fails most gently:
an implicit-declaration error rather than anything subtle.

### F50 — ARMv8-M names its exception handlers differently

The Cortex-M7 FreeRTOS port exports `vPortSVCHandler`, `xPortPendSVHandler` and
`xPortSysTickHandler`, so the H743 project remapped them onto the CMSIS names in
`FreeRTOSConfig.h`:

```c
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler OS_Tick_Handler
```

The ARMv8-M port instead defines `SVC_Handler`, `PendSV_Handler` and `SysTick_Handler`
directly. Two of the three remaps therefore become no-ops, but the SysTick one is actively
harmful: it renames the *call site* inside the CMSIS-RTOS2 shim to `OS_Tick_Handler()`,
which nothing defines — `undefined reference to 'OS_Tick_Handler'` at link.

Fixed by dropping all three remaps and setting
`USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION 1`, so the shim does not emit its own
`SysTick_Handler` and the FreeRTOS port owns it. Note this failure only appears at **link**
time, after every translation unit has compiled — a reminder that "it compiles" is a weak
signal for a port like this.

### F47 — flexPTP uses a glibc-internal macro

`cli_cmds.c`, `master.c` and `slave.c` use `__PRI64_PREFIX` directly (12 occurrences).
That is a **glibc implementation detail**, not a standard macro; newlib does not define it
and instead spells the same thing `__INT64` (in `sys/_intsup.h`). So the library does not
compile for `arm-none-eabi` at all:

```
error: expected ')' before '__PRI64_PREFIX'
    MSG("%" __PRI64_PREFIX "u %u\n", t.sec, t.nanosec);
```

This is an **upstream regression, not something inherited**. The commit the H743 demo pins,
`62fc906`, has zero occurrences; they entered at `a60190f` ("Linux and OS-less working
mode"), where glibc spellings leaked into shared code.

Worked around without patching the submodule, by mapping the macro onto newlib's
equivalent via `FLEXPTP_COMPILE_DEFS`:

```cmake
set(FLEXPTP_COMPILE_DEFS ${symbols_c_SYMB} __PRI64_PREFIX=__INT64)
```

`__INT64` is used rather than a hardcoded `"ll"` so it follows whatever type the target
uses for `int64_t`; all three affected files include `<inttypes.h>` before first use.
Worth reporting upstream — any newlib target is affected, not just this one.

### F48 — TX timestamp callback signature drift

`Inc/lwipopts.h` injects `tx_cb` and `tag` into `struct pbuf` through lwIP's
`LWIP_PBUF_CUSTOM_DATA` hook — this is the "instrumented lwIP driver" flexPTP's README
requires. The declared signature had drifted from what the pinned flexPTP assigns to it:

| | signature |
|---|---|
| `lwipopts.h` (from the H743 project) | `void (*)(uint32_t, uint32_t, uint32_t)` |
| pinned flexPTP `ptp_transmit_cb` | `void (*)(uint32_t, uint32_t, void *)` |

Fixed by aligning `lwipopts.h` to the library and updating the call site in
`mac_drv.c`, which casts the stored pointer before invoking it. The descriptor extension
still carries the tag as a `uint32_t`, which is fine on a 32-bit target but is now
explicitly cast rather than implicitly punned.

This is precisely the plumbing F42 warned was unverified by any stage. It failed at compile
time here; had the signatures merely differed in the tag's *meaning* rather than its type,
it would have failed silently at run time as "PTP syncs but never locks".

**Common root cause with F47:** Stage 1 added the flexPTP submodule at then-current
`master` (`76aaf67`) rather than the commit the H743 demo pins (`62fc906`), so this port
integrates a newer library than the reference project ever did.

### F46 — `GPIO_SPEED_HIGH` is an H7-only legacy alias

H5 provides only `GPIO_SPEED_FREQ_HIGH`; the shorter `GPIO_SPEED_HIGH` spelling used
throughout the H743 project does not exist. Unlike F45 this fails loudly, so it is a
nuisance rather than a hazard. Fixed in `serial_io.c` at Stage 11.

**It will recur at Stage 14.** The flexPTP hardware port being derived from,
`ptp_port_stm32h743_lwip.c:134`, uses the same spelling for the PPS pin setup, as do the
F407 and H743-EtherLib ports. `mac_drv.c` happens to use the long form already.

### F45 — inverted clock-divider namespaces between H7 and H5

On the STM32H7, `RCC_ClkInitTypeDef.AHBCLKDivider` takes `RCC_HCLK_DIVn` and the APB
dividers take `RCC_APBx_DIVn`. On the STM32H5 both are shifted one level:

| Field | H7 constants | H5 constants |
|---|---|---|
| `AHBCLKDivider` | `RCC_HCLK_DIVn` | **`RCC_SYSCLK_DIVn`** |
| `APB1/2/3CLKDivider` | `RCC_APBx_DIVn` | **`RCC_HCLK_DIVn`** |

The APB half is benign: `RCC_APB1_DIV2` simply does not exist on H5, so it fails to
compile. The AHB half is not. `RCC_HCLK_DIV2` exists on **both** families with different
encodings — `RCC_CFGR_HPRE_3` on H7, `RCC_CFGR2_PPRE1_2` on H5 — so carrying the H7
spelling across compiles silently and writes a PPRE-encoded value into the HPRE field,
yielding the wrong AHB divider.

That would have yielded a wrong AHB divider with no diagnostic — HCLK at 125 MHz instead of
250, halving bus throughput and putting the MDIO `CR = DIV102` pairing out of range. Caught
by the Stage 10 compile gate only because the neighbouring APB lines failed loudly first.

*(This paragraph previously added "and that would have detuned the PTP clock, since HCLK is
also the PTP clock on this part". It would not: `clk_ptp_ref_i` is hardwired to `pll1_q_ck`
and is unaffected by the AHB divider. See the F3 correction above.)*

### F44 — undocumented Ninja dependency

`CMakePresets.json` sets `"generator": "Ninja"`, so `cmake --preset debug` fails with
*"unable to find a build program corresponding to Ninja"* on a machine without it.
`build.sh` never uses presets — it calls `cmake -S -B` and gets the platform default
(Unix Makefiles) — so the two documented build paths have different prerequisites and
only one of them is self-describing. Neither the README nor the plan mentioned Ninja.
Found while running the Stage 8 build gate.

### F43 — the LOCKED threshold is the real pass criterion

`PTP_ACCURACY_LIMIT_NS` defaults to 100 ns and is what moves the stack into the LOCKED
state. Stage 20 previously said only "converging offset"; it now names the threshold.

---

## Second-Review and ThreadX-Port Findings

F67 onward, from two different sources — which this heading used to conflate.

**F67–F79** were raised by the second review pass
([`final_review_v2_opus_5p0.md`](reviews/final_review_v2_opus_5p0.md),
[`consolidated_review_v2.md`](reviews/consolidated_review_v2.md)); the N-numbers in brackets are
the identifiers those documents use, and their absence is the visible sign of the other source.

**F80** has no N-number because it was not found by review at all. It came out of the S6 bench
session of the ThreadX/NetX Duo port, and it is why `patches/flexptp/0004` exists.

### F67 — the console FIFO could not tell full from empty *(RESOLVED)* [N1, N7]

`BFifo` derived occupancy from its two indices alone:

```c
uint32_t bfifo_monotonic_write_index(const BFifo *fifo) {
    return (fifo->readIdx > fifo->writeIdx) ? (fifo->writeIdx + fifo->size) : fifo->writeIdx;
}
uint32_t bfifo_get_used(const BFifo *fifo) {
    return (bfifo_monotonic_write_index(fifo) - fifo->readIdx);
}
```

With no count and no reserved slot, `writeIdx == readIdx` has to mean both *empty* and *full*,
and `bfifo_push()` reaches that state by design: it clamps the push to `bfifo_get_free()` and
then wraps the index. Simulated on the real parameters — `size` 16384, both indices at 0, one
16384-byte push — `used` reads back as **0** and `free` as the whole buffer.

#### What that costs

`serial_io_thread()` wakes on the read semaphore, `bfifo_read()` returns 0, `bfifo_pop()`
short-circuits without advancing `readIdx`, and `HAL_UART_Transmit_DMA(…, 0)` returns
`HAL_ERROR` — so the drain thread spins through its `osDelay(1)` branch at 1 kHz while
producers, told the buffer is entirely free, overwrite the backlog nobody emitted.

P5.4 made this *easier* to hit, not harder. `bfifo_push_all()` loops until everything is in, so
a message larger than the remaining space fills the buffer **exactly** — which is precisely the
state that reads back as empty. The truncation P5.4 removed was the lesser of the two failures.

A second defect sat in the same file. `writeSem` was doing two jobs — "space exists" and "no
other writer inside `bfifo_push()`" — and `bfifo_pop()` released it unconditionally from the
consumer thread, including while a producer was mid-push. That hands the token to a second
writer, and both then compute their block sizes from the same `writeIdx` [N7].

#### Measured on hardware

Ten `?` help dumps requested back to back with flexPTP running (~2.5 KB each, ~25 KB total
against a 16 KB FIFO and an 11.2 KB/s line rate), counting complete dumps at the receiver:

| Firmware | Bytes received | Dumps started | Dumps completed | |
|---|---|---|---|---|
| `d359782` (pre-fix) | 8 605 | 3 / 10 | 4 / 10 | output lost |
| with F67 fixed | 24 962 | 10 / 10 | 10 / 10 | nothing lost |

The pre-fix throughput — 0.3 KB/s against a line rate of 11.2 — is the stalled drain thread
showing up in the measurement.

#### The fix

| Change | |
|---|---|
| `pushedTotal` / `poppedTotal` | free-running byte counters; `used` is their modular difference, so full (`== size`) and empty (`== 0`) are different states by construction |
| single-writer discipline | each counter has exactly one writer — producer and consumer respectively — so no critical section is needed, and a concurrently-updated read is stale only in the safe direction for both sides |
| no size constraint | deliberately not the mask-on-access scheme: `bfifo_create()` takes an arbitrary runtime size, so a power-of-two requirement would be an unenforced precondition on every future caller |
| `writeMtx` | writer exclusion, separated from the space signal, which closes N7 |
| level-triggered semaphores | both waits re-check the real occupancy after waking, so a stale or spurious signal costs an iteration instead of corrupting state |
| `bfifo_create()` return | now checked by both callers; it was discarded at each, so a heap exhaustion at boot would have produced a dead console with no message |

### F68 — console output from an interrupt hung the board *(RESOLVED)* [N6]

Three comments in three files disagreed about whether `MSG()` may be called from an ISR.
`mac_drv.c` said its timeout reports "must be callable from an ISR"; `standard_output.c`
skipped the `MSG()` mutex in interrupt context on that basis, reasoning that "a garbled message
beats a silently dropped one"; `serial_io.c` said `serial_io_write()` "must NOT be called from
an ISR". Nothing in the code decided the question.

#### What actually happened

The CMSIS-RTOS2 shim refuses a blocking acquire in interrupt context and returns immediately
([`cmsis_os2.c:1969`](../../../../Drivers/CMSIS/CMSIS_RTOS_V2/cmsis_os2.c#L1969)), so `bfifo_push()` ran
unserialised and, with the FIFO full, its wait loop did not wait — it spun with interrupts
disabled. Reproduced on hardware with a scaffold that called `MSG()` from every 64th receive
interrupt, under a saturated console and a 500 pps ping flood:

| | Result |
|---|---|
| ISR reaching `MSG()` → FIFO | **board hung hard** — 100% ping loss, console dead, no fault report |
| ISR output refused and counted | 12/12 help dumps, 0% ping loss, CLI responsive |

#### Two things the review did not anticipate

**Serving the ISR from the polled writer cannot be made safe.** It was implemented, measured
and withdrawn. Poking `TDR` by hand consumes the `TC` event HAL's DMA completion path is
waiting for, so `serial_io_tx_cplt()` never runs and the drain thread is stranded. Gating on
`DMAT` is not enough — `UART_DMATransmitCplt()` clears `DMAT` and *then* waits on `TC`, leaving
a window where the bit reads clear while HAL still needs the event. Gating on `gState` is not
enough either: the ISR can land inside `HAL_UART_Transmit_DMA()` between the state change and
the `DMAT` arming, and clear the DMA request the thread just enabled. Both variants left the
board answering pings at 0% loss while never printing another character — a loud failure traded
for a quiet one, which is the wrong direction for this project.

**The FIFO already had an unsynchronised interrupt producer.** `serial_io_rx_callback()` runs
in the USART3 receive ISR and pushes every keystroke into the CLI's input FIFO
([`cli.c:475`](../../lib/cli/cli.c#L475)). Its `osSemaphoreAcquire()` had always been failing
silently there, so that push proceeded with no exclusion at all against a concurrent thread
push. Making the acquire failure fatal — the obvious hardening — therefore *dropped every
keystroke* and left a board that booted, printed and pinged normally with a completely dead
CLI. Caught on hardware; it is why writer exclusion is now a critical section rather than a
mutex, that being the only primitive both contexts can use.

#### The contract, now enforced in code rather than described in comments

| Layer | Behaviour |
|---|---|
| `MSG()` | refuses interrupt context outright. Not only the write path is unusable there: `lineBuf` is shared and its lock cannot be taken, and a corrupted buffer can lose its terminator, at which point `insert_carriage_return()`'s `strlen()` runs off the end |
| `serial_io_write()` | thread-only; an ISR call is counted and discarded |
| `bfifo_push()` | never waits in interrupt context — pushes what fits and returns. Writer exclusion is a short critical section, which is what the ISR producer needs |
| `serial_io_panic_write()` | unchanged, and still the right tool for a path that is about to halt |
| interrupt diagnostics | latch state, let a thread report it |
| visibility | `osinfo` prints the discarded count, so the loss is measured rather than assumed. Verified: 56 drops reported after a scaffolded run |

Two robustness fixes came out of the same work. The drain thread's completion wait is now
bounded at 500 ms with an abort on expiry — it was `osWaitForever` on a flag only one callback
ever sets, so any lost completion parked the console for the session, the same shape as F51.
And `bfifo_create()`'s return value is now checked at both call sites.

### F69 — a known vendor with an unlisted model was a boot HardFault *(RESOLVED)* [N4]

```c
static const char *phyName = NULL;
...
    default:
        phyName = "\0";
        break;
    }

    return phyName[0] != '\0';   // NULL when the OUI matched but the model did not
```

`phy_populate_handlers()` assigns `phyName` on exactly two paths: a full match, or an
unrecognised **OUI**. A PHY whose vendor is in the table but whose model is not — any SMSC part
that is not LAN8720A/8742A/8670, any TI part that is not DP83848/TC813/TD510E, any Realtek that
is not RTL8201F — leaves both inner `default: break;` arms with the pointer untouched, and the
return statement reads address 0. `phy_print_full_name()` repeats it one line later.

This is the sibling of **P1.3**, which fixed the *unknown-PHY* NULL callback in
`phy_get_link_status()` and did not revisit this site. It is the likelier of the two on real
hardware: vendors change model numbers far more often than they change OUIs.

#### Measured

Scaffolded by forcing `phyId.model = 0x3F` after the ID read, so the board's real LAN8742A
(OUI `0x1F0`, an entry that *is* in the table) presents as an unlisted model:

| | Boot result |
|---|---|
| before | `*** FATAL: HardFault` · `BFSR.BFARVALID - faulting address = 0x00000000`, halted inside `low_level_init()` |
| after | `PHY: UNKOWN, OUI: 1F0, MODEL: 63 (rev. 1) @0`, board runs |

The fault reporter added in P2 named the address directly, which is what made this a one-shot
diagnosis rather than an SWD session — the point of that work.

With the fix the link stays down and no address is acquired, because no handler matched and
`phy_get_link_status()`'s P1.3 guard reports the link as down. That is the correct degradation:
the board says what it found and keeps its console, instead of dying before the network is up.

#### The fix

`phyName` is initialised to `""` rather than `NULL`, and the outer `default` no longer needs to
assign anything. Both reads then behave for every combination of matched and unmatched IDs.

### F70 — the bounded transmit wait was compiled away above `-Og` *(RESOLVED)* [N2, N9]

P2.3 bounded the descriptor-ownership wait in `ETHHW_Transmit()` with the driver's spin-count
idiom. `ETHHW_Desc` declared its four DMA-written words as plain `uint32_t`, so the load was
loop-invariant — no barrier, no call, no side effect in the body — and above `-Og` the compiler
hoisted it and deleted the loop.

| Level | Before | After |
|---|---|---|
| `-Og` | `ldr` inside the loop — 100 000 real retries | unchanged |
| `-O2` | one `ldr`, then `blt` **forward** to the drop path | loop restored |
| `-O3` | same, failure path tail-called into `MSG` | loop restored |

At `-O2` the "bounded wait" was a single test: a descriptor still held by the DMA meant the
frame was dropped on the spot. That mattered because `CMakeLists.txt` offers `-O2` and `-Os` as
measured, hardware-tested alternatives and wires `-O2` into `RelWithDebInfo`. Those runs were
real; what they did not cover is that the retry loop was not in those images.

The `-O2` object after the fix, with the retry visible:

```
 62a:  subs.w ip, ip, #1        <- spin counter
 62e:  beq.n  6ac               <- exhausted: report and drop
 630:  ldr    r0, [r3, #12]     <- DES3 re-read every iteration
 632:  cmp    r0, #0
 634:  blt.n  62a               <- still owned by the DMA: retry
```

Same lesson as F62, which is why that finding says a workaround for a hardware access
constraint must be expressed with `volatile` or it is not a constraint at all. The lesson had
been drawn for the UID region and not extended to the descriptor rings.

#### Barriers, in the same edit [N9]

`volatile` orders the descriptor stores as far as the *compiler* is concerned and says nothing
about the store buffer or the fabric. Both handoffs now place `__DMB()` between the descriptor
body and the `OWN` store, and `ETHHW_Transmit()` places `__DSB()` before the tail-pointer write
— which is what ST's HAL does in `HAL_ETH_Transmit()`. `dmb sy` and `dsb sy` are present in the
generated code at all three levels.

#### Verified on hardware, at all three optimisation levels

| Build | Flash | Result |
|---|---|---|
| `Debug` (`-O0`) | 228 152 B | boot, PHY, link, DHCP, flexPTP → MASTER, addend 3435973836, ping 1400 B 0% loss |
| `Release` (`-Og`) | 181 100 B | as above, 0% loss |
| `RelWithDebInfo` (`-O2`) | 171 412 B | as above, 0% loss |

No `ETH: TX descriptor … frame dropped` report appeared in any run. `RelWithDebInfo` had never
been run on hardware before — `CMakeLists.txt` says so — and now has been.

> An earlier draft of this finding claimed the size table in `CMakeLists.txt` was wrong,
> because `RelWithDebInfo` came out 9.7 KB smaller than `Release`. That comparison was invalid
> — those are two different configurations, not two `-O` levels. Re-measured properly in P6.12,
> one build path, one variable: the table's *relationships* hold. See F79.

### F71 — a momentary RX ring exhaustion killed reception permanently *(RESOLVED)* [N3]

Three defects in one path, each of which hid the next.

**1. The DMA could not report the condition.** `ETHHW_Start()` enabled `NIE`, `RIE` and `TIE`
and nothing else. `DMACSR.AIS` is the OR of the *unmasked* abnormal conditions, so without
`AIE` and the per-condition enables it never asserts — and the error branch in `ETHHW_ISR()`
was therefore unreachable from the day it was written. Confirmed by instrumenting the report
path first: a scaffold that latched a flag by hand printed correctly, while a real ring
exhaustion printed nothing at all.

**2. Nothing restarted a suspended receive engine.** `DMACRDTPR` was written once at init and
never again. When the DMA reaches a descriptor the application still owns it raises `RBU` and
suspends; returning ownership afterwards is not enough, the tail pointer has to be written
again to issue a receive poll demand.

**3. The report was a comment, not code.** The abnormal branch acknowledged the flags with a
note claiming it reported them, and printed nothing.

#### Measured, with a scaffold stalling `rx_thread` for 1.5 s under a 500 pps flood

Everything identical except the poll demand:

| | during the stall | after it | board said |
|---|---|---|---|
| **without** poll demand | 85.1% loss, run stretched 4 s → 18 s | **100% loss, permanently** | one RBU |
| **with** poll demand | 6.25% loss | **0% loss, fully recovered** | RBU, then 5 more while draining |

`ETH: 1 DMA error interrupt(s), DMACSR flags 0x00000484: RBU(rx ring exhausted) TBU(tx ring
exhausted)`

This settles what the second review could only mark as unverified: on this MAC an `RBU`
suspend really is terminal without a poll demand. It is no longer a hypothesis about what
RM0481 says — the board demonstrates it, in both directions, on demand.

#### The fix

| Change | |
|---|---|
| `ETHHW_Start()` | enables `AIE` plus `RBUE`, `FBEE`, `RSE`, `TXSE`, `CDEE` and `RWTE`. `TBUE` is deliberately left off: transmit-buffer-unavailable is the normal idle transition and would report a fault several times a second |
| `ETHHW_RestoreRXDesc()` | `__DSB()` then `WRITE_REG(ETH->DMACRDTPR, 0)` — one store per restored descriptor, unconditional, which is cheaper than testing whether it is needed and correct even for an `RBU` that arrived while flags were being cleared |
| `ETHHW_ISR()` | latches the abnormal flags and a count instead of discarding them. It cannot print: this is interrupt context, where console output is refused (F68) |
| `ETHHW_ReportDmaError()` | drains the latch and prints, decoding each flag. Called from `phy_thread`, which already runs every 500 ms in thread context |

#### Verified after removing the scaffolds

Boot, PHY, link, DHCP, `osinfo` heap 13 840 B, flexPTP → MASTER, addend 3435973836, ping
1400 B 0% loss, 2000-packet 500 pps flood 0% loss, console back-pressure 10/10 dumps. No DMA
error reported in normal operation.

### F72 — every lwIP assertion was a silent no-op *(RESOLVED)* [N5]

`Modules/lwip_port/arch/cc.h` carried lwIP's stock macro:

```c
#define LWIP_PLATFORM_ASSERT(x) do {printf("Assertion \"%s\" failed at line %d in %s\n", \
                                     x, __LINE__, __FILE__); } while(0)
```

It could neither print nor stop.

**It could not print.** `printf` → `_write` → `__io_putchar`, which is declared `__weak` in
`Src/syscall.c` and defined nowhere. The linker resolved the undefined weak symbol to address 0
and relaxed the call away entirely — visible in the image:

```
080041a0 <_write>:
  80041aa:  ldrb.w  r0, [r4], #1
  80041ae:  nop.w              <-- "bl __io_putchar" was here
  80041b2:  adds    r5, #1
```

`_write()` walked the buffer, emitted nothing and returned `len`, so every byte was reported
written and none left the chip.

**It did not stop.** No halt, no `abort()`. A failed assertion formatted a string into the void
and carried on into the condition it was guarding.

The cost was not theoretical. `Inc/lwipopts.h` deliberately enables
`LWIP_FREERTOS_CHECK_CORE_LOCKING` and `LWIP_ASSERT_CORE_LOCKED()`, which route every unlocked
lwIP call into `sys_check_core_locking()` → `LWIP_ASSERT`. That is the automated form of the bug
P3.6 fixed by hand in `check_dhcp_state()`, and **it had never been able to fire.**

#### Verified

Scaffolded a genuine violation — `netif_set_link_up()` called from `phy_thread`, which does not
hold the core lock:

```
*** FATAL: lwIP assertion failed
  Function called without core lock
  at /home/damien/.../Modules/lwip_port/sys_arch.c
Halted.
```

Three things followed from the one change:

| | |
|---|---|
| the check works | a core-lock violation is now caught the moment it happens, by name |
| **13 736 bytes** of flash returned | Release 181 100 → 167 364 B. `printf` and `_vfprintf_r` are no longer in the link; they were pulled in by `eth_drv_lwip.c`'s two `LWIP_ASSERT`s and by nothing else |
| **F63 resolved for free** | a halting assertion is `noreturn`, so GCC can finally use the `options_out_len` bound. Release went from 4 warnings to 0, with lwIP still pinned and unpatched |

`serial_io_panic_write()` was already the right tool and already existed: polled, no FIFO, no
DMA, no RTOS object, safe with interrupts disabled or the scheduler stopped.

### F73 — `init_ethernet()` built the netif without the core lock *(RESOLVED)*

Found by F72, within one boot of the lwIP assertions becoming real — which is the best argument
for that change that could have been asked for.

`init_ethernet()` calls `netif_add()`, `netif_set_default()` and `netif_set_link_callback()`
from the `init` thread. With `LWIP_TCPIP_CORE_LOCKING = 1` all three require the core lock, and
`netif_add()` asserts it outright (`netif.c:297`). None of them held it, and had not since the
port began. Backtrace from the halt:

```
#0  sys_check_core_locking ()            at Modules/lwip_port/sys_arch.c:598
#1  netif_add (netif=0x2000dd14 <intf>)  at Modules/lwip/src/core/netif.c:297
#2  init_ethernet ()                     at Src/ethernet/ethernet_lwip.c:85
#3  task_startup (arg=0x0)               at Src/main.c:174
```

This is the same class as P3.6, which found the equivalent problem in `check_dhcp_state()` by
inspection and noted at the time that it was "the only place in the project that got this
wrong". It was not; it was the only place anyone had looked. The automated check that would
have caught both had been inert since the port began (F72).

**Fix.** `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()` around the whole group. The lock is held
across `netif_add()` deliberately: it calls `ethernetif_init()`, which calls `netif_set_up()` —
another API that asserts the lock — so one bracket around the group is both simpler and more
correct than three.

**Verified.** Boot clean with no assertion, DHCP 192.168.2.141, flexPTP → MASTER, addend
3435973836, ping 1400 B 0% loss, and a soak of flexPTP + a 500 pps flood + 12 back-to-back help
dumps: 12/12 dumps, CLI responsive, 1500-packet flood 0% loss, no assertion anywhere.

### F74 — CLI tokenizer: a one-byte overflow and shared state *(RESOLVED)* [N8, N12]

Three defects in `tokenize_cli_line()` and its callers, all in `Src/cliutils/cli.c`.

**A one-byte `.bss` overflow.** `sLineBuf` is `CLI_LINEBUF_LENGTH + 1` (192 bytes) and
`get_line()` can fill all of it, but the working copy was `static char
pLineCpy[CLI_LINEBUF_LENGTH]` — 191. A maximum-length command line was `strcpy`'d one byte past
the end of a static object.

**Shared state across two threads.** That buffer was `static`, and `strtok()` keeps its state
globally. `tokenize_cli_line()` is entered from the `cli` thread parsing input *and* from
`cli_register_command()` on the `ptp` thread, which registers ~19 commands when flexPTP starts
and re-registers them on `ptp reset`. `modifyMtx` protects the command table, not this buffer.

**Unterminated `strncpy`.** Tokens of at least `MAX_TOK_LEN` (32) characters and help lines of
at least `MAX_HELP_LEN` (192) were left without a terminator, so later `strcmp()` and `strlen()`
walked into the next array element. Latent — the longest help line is ~75 characters — but
nothing enforced that.

**Fix.** The copy is a local, one byte larger; `strtok_r()` replaces `strtok()`; and a
`copy_token()` helper copies `n-1` and terminates, used for tokens and for the help line. 192
bytes of stack on a 2 KB task costs nothing, and removing the shared state removes the race
rather than guarding it.

**Verified on hardware.** A 191-character line (the overflow case), a 250-character line and a
40-character token are each rejected with the normal "Unknown command" response and no fault;
`osinfo`, `flexptp`, `ptp addend` and `?` (21 registered commands listed) all behave.

### F75 — a dropped transmit was reported to lwIP as sent *(RESOLVED)* [N14]

`ETHHW_Transmit()` can drop a frame — the descriptor is still owned by the DMA when the bounded
wait expires — but it returned `void`, and `low_level_output()` unconditionally did
`LINK_STATS_INC(link.xmit); return ERR_OK;`. The stack could not see the loss, and `link.drop`,
which the over-length branch immediately above is careful to bump, stayed at zero.

`ETHHW_Transmit()` now returns `bool`; `low_level_output()` maps a drop onto `ERR_IF` plus
`link.drop` and `ifoutdiscards`. `ERR_IF` rather than `ERR_MEM` for the reason already spelled
out in that function: lwIP's own warning is that `ERR_MEM` from `linkoutput` "can lead to
strange results", because the stack does not retry a frame dropped for memory reasons. The
EtherLib path returns non-zero from `ethdrv_output()` for the same condition.

**Verified on hardware** by scaffolding every 8th transmit to fail: ping loss came out at
exactly 12.5%, the board stayed responsive, and the round-trip maximum rose to ~1 s — lwIP
retrying, which is precisely the behaviour that was missing when the drop was invisible.
Scaffold removed: 30 pings of 1400 B, 0% loss.

### F76 — the MAC handed the FCS up with every received frame *(RESOLVED)* [N13]

`WRITE_REG(eth->MACCR, mode | ETH_MACCR_IPC)` left `CST` and `ACS` clear, so the MAC neither
stripped the CRC nor the pad, and the packet length in `DES3` — which becomes
`evt.data.rx.size` and then `pbuf->tot_len` — counted it.

Measured directly, by printing the received length for a 1400-byte ping:

| | reported length | wire frame |
|---|---|---|
| before | **1446** | 1442 |
| after | **1442** | 1442 |

Nothing misbehaved, which is why it went unnoticed: lwIP trims IPv4 by header length, ARP
ignores trailing bytes, and flexPTP parses PTP by field. But every RX pbuf carried four junk
bytes, a maximum frame occupied 1518 of the 1528-byte buffer rather than 1514, and any future
code trusting that length would have been wrong by four. ST's HAL enables both bits by default
(`stm32h5xx_hal_eth.c:3042,3048`).

**Fix.** `mode | ETH_MACCR_IPC | ETH_MACCR_CST | ETH_MACCR_ACS`. `CST` covers Type frames —
everything here, since IPv4, ARP and PTP-over-802.3 all use an EtherType above 1500 — and `ACS`
covers the length-field case.

**Verified.** Boot, PHY, link, DHCP, `osinfo` heap 13 840 B, flexPTP → MASTER, addend
3435973836. Ping at payloads of 18, 64, 500, 1400 and 1472 bytes: 0% loss at every size. A
2000-packet 500 pps flood: 0% loss.

> **Not exercised: the gPTP L2 *receive* path.** The board has no PTP peer on this bench, and
> `hook_unknown_ethertype()` — which computes `pbuf->len - 14` and is therefore the one consumer
> that reads the length directly — only runs for received `0x88F7` frames. Driving it needs a
> master on the wire, and `ptp4l` here is AppArmor-confined to configs under `/etc/linuxptp/`
> and needs root, which is a change to the host rather than to this project. The change moves
> that length from four-too-large to exact, so it can only improve that path, but it has not
> been observed. Worth one run against a real master when the setup in
> [host_ptp_setup.md](host_ptp_setup.md) is next up.

### F77 — two traps on the EtherLib receive path *(RESOLVED)* [N10, N11]

Neither is reachable today — `ETH_STACK` is `LWIP` — but P5.21/P5.22 prepared this path so its
first build would not also be a debugging session, and these are exactly that kind of trap.

**A descriptor retired for every allocation failure.** `ETHHW_ReadCallback()` returned 0 when
`dynmem_alloc()` failed. 0 means "the receive thread has taken this descriptor and will restore
it", and `ETHHW_ProcessRx()` has already marked it `inFlight` on that understanding — but
nobody took it. Each failure removed one descriptor from the ring permanently; 24 of them and
reception stops, which is F71 arrived at from the other direction. The lwIP path gets this
right in its queue-full branch. Now returns `ETHHW_RET_RX_PROCESSED`, and the `MSG()` at that
site is gone: it runs in the ETH ISR, where console output is refused (F68). The drop is
counted and reported from `phy_thread()`, alongside the DMA errors.

**A stack array sized by the wire.** `udp_recv_cb()` declared `char str[packet->payloadSize +
1]` — a VLA whose length came from the received datagram, with no bound and no zero-length
guard, `memcpy`'d into. At the 1500-byte MTU that is a 1.5 KB allocation inside the sieve
callback's thread, chosen by whoever sent the packet. Now a fixed 256-byte buffer with the copy
clamped.

**Verified by building the configuration**, which is as far as it can be taken here: with
`ETH_STACK` set to `ETHERLIB` the project compiles and links clean — 176 840 B flash, no
first-party warnings. That is itself worth recording, since the docs describe this path as not
compiled.

> It does **not** run. Flashed as an experiment, it reaches `ethinf_print_info()` and then
> HardFaults with `BFSR.BFARVALID - faulting address = 0x00000008`. Bringing EtherLib up on this
> target remains out of scope and untouched by these two fixes; what has changed is that the
> first person to try it will not also be chasing a stranded descriptor ring and a
> wire-controlled VLA. `ETH_STACK` is back to `LWIP`.

### F78 — two configuration statements that did not hold *(RESOLVED)* [N15, N16]

**`LWIP_PROVIDE_ERRNO` was defined despite the `#undef`.** `Inc/lwipopts.h` says "LIBC ships
errno" and undefines the macro to say so, but `lwip/opt.h` includes `lwipopts.h` first and
`arch/cc.h` only later via `lwip/arch.h` — so the bare `#define LWIP_PROVIDE_ERRNO` in `cc.h`
ran after the undef and quietly won. lwIP provided its own `errno` after all, the opposite of
the documented configuration.

Removing the define is only half the job, and the half that does not compile alone: lwIP
requires `cc.h` to supply `errno` when it is not providing one, and `api/err.c` needs the `E*`
constants at file scope. Caught immediately by building — 20 `'ENOMEM' undeclared` errors —
and fixed by including `<errno.h>`, which is what "LIBC ships errno" meant all along.

**CI could not fail on a failed build.** GitHub runs `run:` steps with `bash -e`, not
`bash -eo pipefail`, so `./build.sh … 2>&1 | tee build.log` exited with `tee`'s status — always
0. A compile or link error would not have failed the step, and nothing downstream would have
caught it: the warning gate passes trivially on a truncated log, the patch check is independent
of the build, and `upload-artifact` only *warns* when its paths are missing. A broken build
could produce a green tick.

Verified locally: `bash -e -c 'false | tee /dev/null'` exits 0, and the same with
`set -o pipefail` exits 1. The workflow now sets it explicitly under `shell: bash`.

**Verified on hardware** after both changes: Debug and Release build clean (0 warnings,
Release 167 420 B), boot, DHCP 192.168.2.141, heap 13 840 B, flexPTP → MASTER, addend
3435973836, ping 1400 B 0% loss, and a 1500-packet 500 pps flood 0% loss.

### F79 — the flash figure is not reproducible across build directories *(documented)* [N17]

Three sources reported three different Release sizes for the same tree: `CMakeLists.txt` said
180 496 B, one review measured 180 504 B, another 180 120 B.

Cause, confirmed with `strings` on the ELF: `configASSERT()` embeds `__FILE__`, and the FreeRTOS
kernel is fetched by `FetchContent` **into the build tree**, so six absolute paths of the form
`<builddir>/_deps/freertos_kernel-src/*.c` end up in `.rodata`. Rebuilding the identical tree at
`build/Release` instead of a long scratchpad path moved the figure by **336 bytes**.

The `-Og` / `-O2` / `-Os` comparison in `CMakeLists.txt` was re-measured properly for this —
one build directory, one variable — and its *relationships* hold:

| level | flash | vs `-Og` | as documented |
|---|---|---|---|
| `-Og` | 167 404 B | — | — |
| `-O2` | 167 708 B | +304 B | +456 B |
| `-Os` | 165 140 B | −2 264 B | −2 128 B |

The absolute numbers moved down ~13 KB when P6.6 removed newlib's `printf` from the link. The
comment now carries the fresh figures and says to compare deltas rather than absolutes.

> An earlier draft of F70 claimed the table was simply wrong, on the strength of
> `RelWithDebInfo` measuring 9.7 KB smaller than `Release`. That was a bad comparison — two
> different configurations, not two `-O` levels — and it is corrected there.

#### Hygiene, closed in the same pass

| | |
|---|---|
| **H-2** | The `docs` CI job checked out without submodules while `check_docs_links.py` requires every relative target to exist. No document links into `Modules/` today — the reviews reference submodule files in prose for exactly this reason — but the first one that did would have failed CI while being valid locally. `submodules: recursive` makes the trap impossible instead of merely avoided |
| **H-3** | `-Wl,--gc-sections` sat in `CMAKE_C_FLAGS`, where the compiler ignores it — but CMake passes that variable to the **link** as well, so it was doing real work from an odd place, and `target_link_options()` never had it. Simply deleting it grew the Release image from 167 420 B to **233 924 B**; that is how the mistake was caught. It now lives in `target_link_options()`, the `-f` flags stay in the toolchain file, and both sides say not to move one without the other. Release is byte-identical to before at 167 420 B |
| **H-4** | `lwipopts.h`'s `LWIP_HTTPD_*` block configures an httpd that is not compiled — lwIP's core Filelists only, and nothing calls `httpd_init()`. Kept, but now says so |
| **H-5** | `_Min_Heap_Size` in the linker script constrains nothing: `_sbrk()` bounds the newlib heap against `_estack - _Min_Stack_Size` and never consults it. `_Min_Stack_Size` *is* load-bearing. Both now say which is which |

Also corrected: `architecture.md`'s RX depth table called `ETH_RX_DESC_CNT` "descriptors the
MAC's DMA can fill before it stalls", which understated it — the stall is a latch, not a pause
(F71, finding D-b).

### F80 — flexPTP's heartbeat cannot be enqueued from ThreadX's timer thread *(RESOLVED)*

Found in S6 of the ThreadX port. The second defect in flexPTP's `FLEXPTP_CMSIS_OS2` binding,
and the same shape as the first (F40): a blocking call made from a context that is not allowed
to block.

`ptp_event_enqueue()` posted with `osWaitForever`:

```c
ok = osMessageQueuePut(sEventFIFO, event, 0, osWaitForever) == osOK;
```

Its highest-frequency caller is `ptp_heartbeat_tmr_cb()`, and CMSIS-RTOS2 runs `osTimer`
callbacks in the RTOS's timer thread. ThreadX refuses to suspend that thread —
`Modules/threadx/common/src/txe_queue_send.c`:

```c
if (wait_option != TX_NO_WAIT) {
    ...
    else {
        TX_THREAD_GET_CURRENT(current_thread)
        if (current_thread == &_tx_timer_thread) {
            status = TX_WAIT_ERROR;
        }
    }
}
```

The CMSIS-RTOS2 wrapper maps every `tx_queue_send()` status that is neither `TX_SUCCESS` nor
`TX_QUEUE_FULL` to `osErrorTimeout`, and `ptp_event_enqueue()` folds that into a bare `false`
that no caller reads. So **every** heartbeat was rejected, silently, at 32 Hz.

The consequence is not a slow clock, it is no clock at all. `ptp_process_event()` never saw
`PTP_CEV_HEARTBEAT`, so `S.ticks` stayed at 0 and `ptp_bmca_tick()` never ran. The BMCA FSM
therefore never left `PTP_BMCA_INITIALIZING` — and `INITIALIZING` is the one state for which
`ptp_handle_announce_msg()` has no arm, so it falls through `default:` and discards the
Announce. Every Announce arrived, was parsed, and was thrown away.

**Why it looked like a receive fault.** Receive was flawless throughout — the NSD's counters
showed 95 packets enqueued in the right 2:2:1 proportions — and the visible symptom was
`Master clock ID: 0000000000000000` with no BMCA logging at all. The silence was itself the
evidence: `SBMC_PRINT_LOG` only fires on a *transition*, and there were none to print.

#### How it was found

Four counters on top of `ethinfo`, no print statements, no submodule instrumentation:

| counter | reading | what it eliminated |
|---|---|---|
| `PTP_UEV_ANNOUNCE_RECVED` | **19**, equal to the NSD's Announce count | `ptp_process_packet()` is reached and the domain / `transportSpecific` / version gate passes. Kills all three candidates the S6 handoff ranked |
| `ptp_get_tick()` | **0** | no heartbeat event has ever been processed |
| `tx_timer_info_get()` over `_tx_timer_created_ptr` | `active=1 reschedule=31` | the timer itself is running and rescheduling correctly, so the fault is in the callback, not the timer |
| a probe `osTimer` that mirrored flexPTP's exactly | fired **1225** times, `IPSR == 0`, `osMessageQueuePut(..., osWaitForever)` → **−2** | a periodic timer works; a *blocking queue put from its callback* does not — and not because of interrupt context |

The `IPSR == 0` reading is the one that mattered. It ruled out the obvious explanation (the
callback running in an ISR, which is F40's mechanism) and left only the one ThreadX actually
implements: a restriction on the timer thread specifically, not on interrupt context.

#### Fixed

`patches/flexptp/0004-cmsis-os2-event-enqueue-from-timer-thread.patch` — a zero timeout for
both the event and its notification. Dropping an event when the queue is full is the correct
failure mode for a timer callback, and it also removes a latent self-deadlock:
`ptp_bmca_handle_state_change()` enqueues from `task_ptp()` itself, the only thread that drains
that queue, so a full queue there would have blocked forever.

`--stage S6 --compare baseline.json` is 9/9 with no regressions and `ptp_ts` passing before
`ptp_sync`. The offset figures are in the S6 progress note in
[threadx_porting_plan.md](threadx_porting_plan.md); note that `ptp_sync` **records** the offset
but does not threshold it — it passes on reaching `SLAVE` with enough samples, and `--compare`
has no tolerance entry for the offset. Do not read a green `ptp_sync` as an accuracy assertion.

Worth reporting upstream. The FreeRTOS branch has the same shape — `xQueueSend(...,
portMAX_DELAY)` from the timer service task risks deadlocking it rather than failing cleanly —
but only the CMSIS-RTOS2 branch is patched here, because that is the one this port exercises
and the one that was measured.

---

## Minor Findings

**F28 — SRAM2 unallocated.** 64 KB with no section assigned. Deliberate spare capacity;
recorded so it reads as a choice rather than an oversight.

**F29 — `.ETHLibPool`.** `Inc/etherlib_options.h` declares the section but the linker script
has no placement (nor did upstream's). Only matters if EtherLib is enabled.

**F30 — CMSIS Core version.** Raised because `Drivers/CMSIS/Include` was inherited from the
H7 project and looked stale at "5.6". It is not. `5.6` is the **Core(M)** version, versioned
separately from the CMSIS pack, and CMSIS 5.9.0 — the last CMSIS 5 release — reports the same.
All 30 files are byte-identical to `CMSIS/Core/Include` at tag `5.9.0` of
`ARM-software/CMSIS_5`, so there is nothing to refresh within CMSIS 5.

The original advice — "a bump is the first thing to try if odd core-header errors appear" —
was therefore **wrong and has been removed**: there is no CMSIS 5 bump to make. CMSIS 6 is a
restructuring rather than a refresh, and ST's pinned HAL and device pack are validated against
CMSIS 5. See the note in [architecture.md](architecture.md).

**F31 — Unexpanded template placeholder.** `CMakeLists.txt:7` contains a literal
`list(APPEND CMAKE_MODULE_PATH "{{sr:cmake_path}}")` — an unsubstituted placeholder from the
STM32 VS Code extension template, present upstream too. Harmless (it appends a nonsense
path) but worth deleting while Stage 7 is in the file.

**F32 — Dead include paths.** `CMakeLists.txt:41` adds `${CMAKE_SOURCE_DIR}/Middlewares/Inc`
and `Modules/CMakeLists.txt:58` adds `${LWIP_PORT_DIR}/include`; neither directory exists.
Harmless, also inherited.

**F33 — ST header comment bug (no action).** In `stm32h563xx.h`:

```c
#define ETH_MACMACSSIR_SSINC_Msk  (0xFFUL << ETH_MACMACSSIR_SSINC_Pos) /*!< 0x0000FF00 */
```

The value is `0x00FF0000`; the comment is wrong, and the macro carries a doubled `MACMAC`
prefix. The `_Pos` is correct at 16, so `mac_drv.c`'s `increment << 16` writes the right
field. Do not "fix" the driver to match the comment.

**F34 — `/DISCARD/` block (no action).** The linker script discards `libc.a`, `libm.a` and
`libgcc.a` debug sections via a block inherited verbatim from the ST template. Present
upstream and working; left alone.

**F35 — `double` on an SP-FPU part (no action).** The Kalman servo runs once per Sync
(1–8 Hz). Software double emulation is immaterial at that rate. Decision accepted as planned.

---

## Verified Clean

Negative results, recorded so they are not re-investigated.

| Check | Result |
|---|---|
| Stage 1 file inventory vs upstream | Faithful. Only intentional omissions (H7 HAL conf, startup, linker, HAL submodule, README GIF). |
| CMSIS device pack vs HAL version | Both 1.7.0 — matched. |
| ETH register set | Every register `mac_drv.c` touches exists on H563 with identical names: `MACTSCR`, `MACSSIR`, `MACSTSR/NR`, `MACSTSUR/NUR`, `MACTSAR`, `MACTSSR`, `MACACR`, `MACATSSR/NR`, `MACPPSCR`, `MACPPSTTSR/TTNR`, `MACPPSIR`, `MACPPSWR`. Same Synopsys QoS core. |
| ETH descriptor bit macros | All `ETH_DMARXNDESCRF_*`, `ETH_DMARXNDESCWBF_*`, `ETH_DMATXNDESCRF_*`, `ETH_DMATXNDESCWBF_*` present in `stm32h5xx_hal_eth.h`. |
| `ETH` symbol under TZEN=0 | Resolves to `ETH_NS`. No source change needed. |
| `MACSSIR` field position | `SSINC_Pos` = 16, so `increment << 16` is correct (see F33). |
| Linker section names | `.ETHBufferSection`, `.ETHStateAndDecripSection`, `.FreeRTOSHeapSection`, `.lwIPHeapSection` all match the `__attribute__((section(...)))` declarations exactly. |
| Cache state after `HAL_Init()` | Neither ICACHE nor DCACHE is enabled. SRAM3 Ethernet placement needs no maintenance as built. |
| Required H5 symbols exist | `FLASH_LATENCY_5`, `RCC_HSE_BYPASS`, `GPIO_AF11_ETH` (= `0x0B`), `ETH_IRQn` (= 106), `PWR_REGULATOR_VOLTAGE_SCALE0`, `PWR_FLAG_VOSRDY`. |
| SRAM sizing | SRAM1: ~30 KB FreeRTOS heap + `.data`/`.bss` + 2.5 KB stack/heap in 256 KB. SRAM3: ~73 KB ETH buffers + 1.5 KB descriptors + 16 KB lwIP heap in 320 KB. Comfortable. |
| Repository state | Working tree clean; four commits matching Stages 1–4; `docs/` tracked. |

Added during the implementation phase:

| Check | Result |
|---|---|
| ETH register audit, in practice | Confirmed: Stage 12 needed **zero** register or descriptor-bit changes. `mac_drv.c` compiled against the H5 headers unmodified apart from peripheral plumbing, exactly as the static review predicted. |
| Vector table correctness | Linked image: word 0 = `0x20040000` (top of SRAM1, matching the linker script), word 1 = entry point `0x080024a9` with the Thumb bit set. |
| Memory footprint | FLASH 10.6%, SRAM1 50.9%, SRAM3 22.9%, SRAM2 unused. The Stage 4 sizing assumptions held; no region is close to full. |
| Addend arithmetic, in practice | The core computes `PTP_ADDEND_INIT` = 3435973836 (`0xCCCCCCCC`), matching [ptp_clock.md](ptp_clock.md) exactly. |
| FreeRTOS port selection | The CM33 NTZ non-secure `port.c.obj` is the object actually built — verified from the build tree, not inferred from the CMake variable. |
| Toolchain ABI | `readelf` reports hard-float EABI on the linked image, consistent with `-mfloat-abi=hard` throughout. |

---

## H743 Baseline Reference Data

Recovered from upstream for comparison. Useful when deciding whether a difference is
intentional.

| Item | H743 project value |
|---|---|
| `PTP_MAIN_OSCILLATOR_FREQ_HZ` | 240 000 000 |
| `PTP_INCREMENT_NSEC` | 5 |
| `configCPU_CLOCK_HZ` | 480 000 000 |
| `configTOTAL_HEAP_SIZE` | 30 KB |
| `UNIQUE_DEV_ID_BASE` | `0x1FF1E800` (feeds `ETH_MAC_ADDR0..5`) |
| `ETH_TX_DESC_CNT` / `ETH_RX_DESC_CNT` | 24 / 24 |
| Register callbacks enabled | UART and SAI only |
| HAL modules enabled | HAL, CORTEX, CRC, DMA, ETH, FLASH, GPIO, HASH, PWR, RCC, RNG, TIM, UART |
| Linker placement | Everything in `RAM_D1`; `_estack` at top of `RAM_D1`; SAI buffer in `RAM_D3` |
| Startup file | `startup_stm32h735igkx.s` — an **H735** vector table used on an H743. Do not diff the H563 table against it. |
| PPS pin | PG8 (flexPTP port); PB5 additionally configured in `mac_drv.c` |
