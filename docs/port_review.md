# Port Review — Findings & Disposition

The audit trail for this port: what was checked, what was found, and where each finding is
now tracked. It is not a task list — [implementation_plan.md](implementation_plan.md) owns
the work.

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
  — the **device** header, not the family header (see [F3](#f3)).
- File-inventory diff against upstream `epagris/flexPTP-demo-NUCLEO-H743ZI` to confirm the
  Stage 1 copy was faithful, and to recover baseline values for comparison.
- Constant-folding the flexPTP addend macro under GCC to confirm the arithmetic in
  [F2](#f2) rather than reasoning about it.
- Cross-check of the flexPTP README, the published documentation and the porting guide
  (`epagris.github.io/flexPTP`) against the **pinned** submodule `RC1.0-11-g76aaf67`
  (2026-03-06). The website documents whatever is current on `master`; where the two
  disagree, the pinned source wins — see [F37](#f37) and [F38](#f38).

### Phase 2 — implementation (F44 onward)

- Every stage from 5 onward verified by compiling, with the Arm GNU toolchain 15.3.rel1
  fetched into `toolchains/`. Stage gates: HAL library at 8, `main.c` at 10, full link
  at 15.
- Where a file could not yet be compiled in place because of a dependency on a later
  stage, it was verified in isolation using a throwaway shim header outside the repo
  rather than by reordering stages (see Stage 10).
- Linked-image checks: memory-region usage, entry point, and the first two vector-table
  words against the linker script.

Still not done: **no hardware**. Nothing here has executed on a device. Every claim in this
document is a static or build-time one.

---

## Findings by phase

| Phase | Findings | Found by |
|---|---|---|
| Static review | F1–F43 (43) | Reading code, headers and documentation |
| Implementation | F44–F50 (7) | Configuring, compiling and linking |

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
| F3 | H563 has no ETH PTP clock mux — **but the fixed source is `pll1_q_ck`, not HCLK** (partly wrong; corrected) | Critical | [ptp_clock.md](ptp_clock.md#why-there-is-no-clock-source-to-choose), design doc, plan diff table |
| F4 | PLL1_Q cannot produce 200 MHz alongside 250 MHz SYSCLK | Critical | [ptp_clock.md](ptp_clock.md#pll-arithmetic-for-reference) |
| F5 | A 4 ns increment is unreachable on this device | Critical | [ptp_clock.md](ptp_clock.md#why-a-4-ns-increment-is-unreachable) |
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
| F30 | CMSIS Core 5.6 carried over from the H7 project | Minor | plan Open Items |
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
| F41 | `PTP_UPDATE_CLOCK` dead; `ETHHW_UpdatePTPTime()` unreachable and suspect | Minor + latent bug | plan Stage 12 |
| F42 | lwIP TX timestamp plumbing broken — **confirmed live defect**, blocks PTP lock | **OPEN, blocking** | diagnosed on hardware |
| F43 | `PTP_ACCURACY_LIMIT_NS` (100 ns) is the implicit LOCKED criterion | Minor | plan Stage 20 |
| F44 | `CMakePresets.json` requires Ninja; not documented as a prerequisite | Minor | plan Stage 19 |
| F45 | H5 inverts the H7 clock-divider namespaces; the H7 spelling compiles but misconfigures | Critical (silent) | plan Stage 10 (fixed) |
| F46 | `GPIO_SPEED_HIGH` legacy alias does not exist on H5 | Minor | plan Stages 11 (fixed) & 14 |
| F47 | flexPTP uses `__PRI64_PREFIX`, a glibc-internal macro; breaks every newlib build | Blocking | plan Stage 14 (worked around) |
| F48 | `tx_cb` signature drift between pinned flexPTP and this project's `lwipopts.h` | Blocking | plan Stage 14 (fixed) |
| F49 | `task_ptp_is_operating()` renamed to `is_flexPTP_operating()` upstream | Blocking | plan Stage 15 (fixed) |
| F50 | ARMv8-M port names exception handlers directly; the H7-era remaps break the tick | Blocking | plan Stage 15 (fixed) |
| F51 | H563 UID region rejects **byte** reads; `ETH_MAC_ADDR*` byte-indexed it → HardFault | Critical | fixed, verified on hardware |
| F52 | `ptphw_init()` set the PTP addend before the subsecond increment → TSADDREG never clears | Critical | fixed, verified on hardware |
| F53 | PTP timestamp counter is not clocked — PLL1Q output never enabled | **RESOLVED** | fixed, verified on hardware |
| F54 | `ETHHW_SetPTPAddend()` waits unbounded, turning any PTP clock fault into a silent hang | Robustness | open |
| F55 | Welcome message tells users to fit an **8 MHz** oscillator at X3; on MB1404 X3 is **25 MHz** | Wrong advice | plan Stage 19 |

**Every finding is tracked.** F33 and F34 are recorded here only because they are
observations about upstream code that require no action — noted so a future reader does not
spend time on them.

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

### F3 — No ETH PTP clock mux on this device {#f3}

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

### F37 — `PTP_HW_GET_TIME` is documented with the wrong type {#f37}

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

### F38 — `SPRINTF` vs `FLEXPTP_SNPRINTF` {#f38}

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

So the break lies between the MAC writing back a descriptor with `TTSS` set and
`ptp_transmit_timestamp_cb()` receiving it. The next step is instrumentation: count in
`ETHHW_ProcessTx()` how many descriptors are seen with `TTSS` set, versus how many
callbacks are dispatched.

*One candidate worth checking first:* `ETHHW_ISR()` uses `else if` between the RX and TX
branches, so a combined RI+TI interrupt services only RX. TI stays asserted so it should
re-trigger, but the interaction is worth confirming rather than assuming.



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

### F54 — unbounded wait in `ETHHW_SetPTPAddend()`

The function spins on `while (eth->MACTSCR & ETH_MACTSCR_TSADDREG)` with no timeout, eight
times over. That is what converted F53 from "PTP does not lock" into "the flexPTP task
hangs silently and the PTP CLI commands stop responding". Inherited from the H743 driver,
where the wait always completed.

A bounded wait with an error return would have surfaced F53 immediately and left the rest
of the system diagnosable. Not fixed yet, because a silent timeout could equally mask real
faults — the right form is a timeout that reports.

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

Given that HCLK is also the PTP clock on this part, that would have detuned the PTP clock
with no diagnostic. Caught by the Stage 10 compile gate only because the neighbouring APB
lines failed loudly first.

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

## Minor Findings

**F28 — SRAM2 unallocated.** 64 KB with no section assigned. Deliberate spare capacity;
recorded so it reads as a choice rather than an oversight.

**F29 — `.ETHLibPool`.** `Inc/etherlib_options.h` declares the section but the linker script
has no placement (nor did upstream's). Only matters if EtherLib is enabled.

**F30 — CMSIS Core version.** `Drivers/CMSIS/Include` is CMSIS Core 5.6, carried over from
the H7 project, paired with CMSIS device pack 1.7.0. Expected to work with `core_cm33.h`;
first thing to try if unexplained core-header errors appear.

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
