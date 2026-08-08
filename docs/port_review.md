# Port Review — Findings & Disposition

A review of [implementation_plan.md](implementation_plan.md) and the repository state as of
commit `e2d9556` (Stage 4 complete). This document is the audit trail: what was checked,
what was found, and where each finding is now tracked. It is not a task list — the plan
owns the work.

## Table of Contents

- [Method](#method)
- [Disposition Summary](#disposition-summary)
- [Critical Findings](#critical-findings)
- [Build-Blocking Findings](#build-blocking-findings)
- [Plan Structure Findings](#plan-structure-findings)
- [Preconditions Requiring Hardware or Datasheet](#preconditions-requiring-hardware-or-datasheet)
- [Minor Findings](#minor-findings)
- [Verified Clean](#verified-clean)
- [H743 Baseline Reference Data](#h743-baseline-reference-data)

---

## Method

- Full read of every application source file, both CMake trees, the linker script and the
  build/IDE configuration.
- Symbol-level verification of every HAL and CMSIS identifier the port depends on, against
  `Drivers/stm32h5xx-hal-driver/Inc/` and `Drivers/CMSIS/Device/ST/STM32H5xx/Include/stm32h563xx.h`
  — the **device** header, not the family header (see [F3](#f3)).
- File-inventory diff against upstream `epagris/flexPTP-demo-NUCLEO-H743ZI` to confirm the
  Stage 1 copy was faithful, and to recover baseline values for comparison.
- Constant-folding the flexPTP addend macro under GCC to confirm the arithmetic in
  [F2](#f2) rather than reasoning about it.

Not done: no compilation (the tree does not yet configure — see [F10](#f10)), no hardware.

---

## Disposition Summary

| # | Finding | Severity | Now tracked in |
|---|---|---|---|
| F1 | PTP baseline misstated: H743 uses 240 MHz, not 200 MHz | Critical | [ptp_clock.md](ptp_clock.md#history) |
| F2 | Both planned PTP configs have K = 1.0 → addend of 0 | Critical | [ptp_clock.md](ptp_clock.md#the-k--1-constraint), plan Stage 12 |
| F3 | H563 has no ETH PTP clock mux at all | Critical | [ptp_clock.md](ptp_clock.md#why-there-is-no-clock-source-to-choose), design doc, plan diff table |
| F4 | PLL1_Q cannot produce 200 MHz alongside 250 MHz SYSCLK | Critical | [ptp_clock.md](ptp_clock.md#pll-arithmetic-for-reference) |
| F5 | A 4 ns increment is unreachable on this device | Critical | [ptp_clock.md](ptp_clock.md#why-a-4-ns-increment-is-unreachable) |
| F6 | AHB prescaler must change `DIV2` → `DIV1` | Critical | plan Stage 9, [ptp_clock.md](ptp_clock.md#coupled-decisions) |
| F7 | ETH_TXD1 moves PB13 → PB15 | Critical | design doc RMII Pin Map, plan decisions + Stage 11 |
| F8 | MDIO divider write double-shifts a positioned mask | Critical | plan Stage 11 (with before/after code) |
| F9 | No flexPTP hardware port exists for H5 | Critical | plan **Stage 13** 🆕 |
| F10 | `Src/CMakeLists.txt` references deleted `system_stm32h7xx.c` | Build | plan Stage 7 + Stage 3 follow-up note |
| F11 | SYSCFG renamed to SBS on H5 | Build | plan Stages 9 & 11, diff table |
| F12 | `USE_HAL_UART_REGISTER_CALLBACKS` must be `1U` | Build | plan Stage 6 |
| F13 | `Drivers/CMakeLists.txt` is a 127-file H7 source list | Build | plan Stage 7 |
| F14 | MCU flags still `cortex-m7` / `fpv5-d16` | Build | plan Stage 7 |
| F15 | `MCU_TYPE` still `STM32H743xx` | Build | plan Stage 7 |
| F16 | FreeRTOS port still `GCC_ARM_CM7` | Build | plan Stage 8 |
| F17 | Header-sweep stage listed only 2 of 4 affected files | Build | plan Stage 14 |
| F18 | H7 PWR supply API does not exist on H5 | Build | plan Stage 9, diff table |
| F19 | H7 RCC clock-tree fields and PLL symbol names differ | Build | plan Stage 9 |
| F20 | No intermediate build gates | Process | plan [Build gates](implementation_plan.md#build-gates) |
| F21 | Old Stage 13 was a catch-all bucket | Process | split into plan Stages 7 & 15 |
| F22 | Old Stage 14 "create design_doc.md" — already existed | Process | plan Stage 16 reframed as *complete* |
| F23 | No stage owned README / build.sh / launch.json | Process | plan **Stage 17** 🆕 |
| F24 | FreeRTOS `GIT_TAG main` is a floating dependency | Process | plan Stage 8, decisions, risks |
| F25 | TZEN option byte is a boot precondition | Precondition | plan Stage 18 |
| F26 | PPS pin undecided; H743 configures two | Precondition | plan decisions + Open Items |
| F27 | ETH DMA reach into SRAM3 unverified | Precondition | plan Open Items, Stage 16 |
| F28 | SRAM2 (64 KB) unallocated | Minor | plan Stage 4 note |
| F29 | `.ETHLibPool` has no linker placement | Minor | plan Stage 4 note, Stage 16 |
| F30 | CMSIS Core 5.6 carried over from the H7 project | Minor | plan Open Items |
| F31 | Unexpanded `{{sr:cmake_path}}` template placeholder | Minor | plan Stage 7 |
| F32 | Two CMake include paths point at non-existent dirs | Minor | plan Stage 7 |
| F33 | ST header comment bug on `MACSSIR` masks | None | this document only |
| F34 | `/DISCARD/ libc.a` block inherited from ST template | None | this document only |
| F35 | `double` in Kalman filter on an SP-FPU part | None | plan decisions (accepted) |

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

**F26 — PPS pin.** The H743 project drives PPS from **PG8**, configured by the flexPTP port
file, while `mac_drv.c` *separately* configures PB5 as ETH AF11. The plan's table recorded
only PB5. The H563 port should pick one pin, verify it against the datasheet and the Nucleo
headers, and configure it in exactly one place.

**F27 — ETH DMA and SRAM3.** SRAM1/2/3 are all on the AHB matrix and `HAL_Init()` leaves
DCACHE off, so no cache maintenance is expected for the Ethernet buffers in SRAM3. Confirm
empirically rather than assuming.

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
