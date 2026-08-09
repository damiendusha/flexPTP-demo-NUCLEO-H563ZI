# flexPTP NUCLEO-H563ZI — Design Document

This document captures design decisions, technical notes, and future porting guides
for the flexPTP demo port from NUCLEO-H743ZI (STM32H743, Cortex-M7) to
NUCLEO-H563ZI (STM32H563, Cortex-M33).

## Table of Contents

- [Port Overview](#port-overview)
- [System Clock Tree](#system-clock-tree)
- [PTP Clock](#ptp-clock) — full derivation in [ptp_clock.md](ptp_clock.md)
- [EtherLib Porting Guide](#etherlib-porting-guide)
- [TrustZone Migration Guide](#trustzone-migration-guide)
- [RMII Pin Map](#rmii-pin-map)
- [RXER Pin Note](#rxer-pin-note)
- [mac_drv.c Comparison](#mac_drvc-comparison)
- [Stage Log](#stage-log)

Companion documents: [ptp_clock.md](ptp_clock.md) (PTP clock derivation) ·
[port_review.md](port_review.md) (review findings and verified-clean audit) ·
[implementation_plan.md](implementation_plan.md) (staged work plan)

---

## Port Overview

**Source**: NUCLEO-H743ZI — STM32H743ZI, Cortex-M7 @ 480 MHz, DP-FPU, stm32h7xx HAL  
**Target**: NUCLEO-H563ZI — STM32H563ZI, Cortex-M33 @ 250 MHz, SP-FPU, stm32h5xx HAL

Key decisions:
- PTP clock: HCLK 250 MHz / 5 ns increment (no clock mux exists on this device — see
  [PTP Clock](#ptp-clock))
- HSE: Bypass mode (stock Nucleo, ST-Link MCO at 8 MHz)
- FPU: Keep `double` in Kalman filter, accept software emulation
- Network stack: LwIP only (initial port)
- TrustZone: Disabled (TZEN=0, ARM_CM33_NTZ)
- mac_drv.c: Register-level port

---

## System Clock Tree

**SYSCLK = 250 MHz, HCLK = 250 MHz.** This is the H563's maximum, and on this part it is
also the PTP timestamp clock, so it is not merely a performance choice.

```
HSE 8 MHz (RCC_HSE_BYPASS — ST-Link MCO, no crystal fitted)
  │
  ├─ PLLM = HSE_VALUE / 2000000 = 4      →  VCI    =   2 MHz   (RCC_PLL1_VCIRANGE_1)
  ├─ PLLN = TARGET_SYSCLK_MHZ  = 250     →  VCO    = 500 MHz   (RCC_PLL1_VCORANGE_WIDE)
  └─ PLLP = 2                            →  SYSCLK = 250 MHz
                                             │
       AHBCLKDivider  = RCC_HCLK_DIV1     →  HCLK   = 250 MHz  ← also ptp_ker_ck
       APB1/2/3 Div   = RCC_APBx_DIV2     →  PCLK   = 125 MHz

  FLASH_LATENCY_5, voltage scaling VOS0
```

Comparison with the source project: the H743 ran SYSCLK 480 MHz with `RCC_HCLK_DIV2`,
giving HCLK — and therefore a PTP clock — of 240 MHz.

### Why these values

**VCO is pinned at 500 MHz.** `PLLP` forbids odd division factors and the PLL1 wide VCO
range is 192–836 MHz. For SYSCLK = 250 MHz, `PLLP = 2` (VCO 500) is the only solution;
`PLLP = 4` would need a 1000 MHz VCO, above the ceiling.

**`AHBCLKDivider` must be `RCC_HCLK_DIV1`.** The H563 has no Ethernet PTP clock
multiplexer, so the timestamp unit is fed from HCLK directly. Carrying over the H743's
`RCC_HCLK_DIV2` would give HCLK 125 MHz and a PTP clock running at 0.375× real time —
a silent failure rather than an obvious one. See [ptp_clock.md](ptp_clock.md).

**`PLLN = TARGET_SYSCLK_MHZ` is exact, not approximate.** Because VCI is 2 MHz and
`PLLP` is 2, SYSCLK in MHz equals `PLLN` numerically. The H743 project relied on the same
identity at 480. Consequently `configCPU_CLOCK_HZ` in `FreeRTOSConfig.h` is the single
source of truth for the PLL: setting it to 250000000 drives the whole chain, and
`init_pll()` needs no arithmetic change.

**APB at DIV2 (125 MHz)** matches the H743 structure and is deliberate rather than
inherited. The H563 would permit DIV1, but nothing in this project needs it: USART3's baud
divisor is computed by the HAL from PCLK1, and the Ethernet MDIO `CR` divider keys off
HCLK, not PCLK. DIV2 leaves headroom on the peripheral buses at no cost.

**`FLASH_LATENCY_5` with VOS0** is required for 250 MHz; the H743 used
`FLASH_LATENCY_4` at its own operating point.

---

## PTP Clock

The timestamp unit is clocked from **HCLK at 250 MHz** with a **5 ns** subsecond
increment.

| Parameter | Value |
|---|---|
| `PTP_MAIN_OSCILLATOR_FREQ_HZ` | 250 000 000 (= HCLK) |
| `PTP_INCREMENT_NSEC` | 5 |
| Initial addend | 3435973836 (`0xCCCCCCCC`) |
| Timestamp granularity | 5 ns |
| Frequency tuning range | +25 % |
| Frequency tuning resolution | ≈ 0.29 ppb |

Three things are worth knowing without reading further:

1. **There is nothing to configure.** The STM32H563 has no Ethernet PTP kernel-clock
   multiplexer (`RCC_CCIPR5.ETHPTPCLKSEL` does not exist on this device, only on later
   H5 parts). `ptp_ker_ck` is hardwired to HCLK, so no `HAL_RCCEx_PeriphCLKConfig()` call
   is required — or possible.
2. **The subsecond increment must exceed the PTP clock period.** With a 4 ns HCLK period,
   5 ns is the smallest usable increment. Choosing 4 ns produces an addend that does not
   fit in 32 bits and a clock that never ticks.
3. **Timestamp granularity is 5 ns — the same as the H743 source project — and cannot be
   improved on this part.** A finer increment would need a *faster* PTP clock (≥ 262 MHz),
   which exceeds the device's 250 MHz HCLK ceiling.

Two settings elsewhere are load-bearing for this: the AHB prescaler must be
`RCC_HCLK_DIV1` (not the H743's `DIV2`), and the MDIO divider `CR = DIV102` is correct
only for a 250 MHz HCLK.

> **Full derivation, the `K > 1` constraint, the PLL arithmetic, and the record of why the
> originally planned 200 MHz / PLL1_Q configuration was abandoned:
> [ptp_clock.md](ptp_clock.md).**

---

## EtherLib Porting Guide

*To be completed in Stage 17.*

The EtherLib in-house Ethernet stack is included as a git submodule but is not
compiled in the initial port. This section will document what needs to change to
enable EtherLib support.

---

## TrustZone Migration Guide

*To be completed in Stage 17.*

The initial port runs in non-secure flat mode (TZEN=0) using the `ARM_CM33_NTZ`
FreeRTOS port. This section will document how to migrate to a TrustZone-partitioned
build when needed.

---

## RMII Pin Map

The RMII pin assignment is *almost* identical between the two Nucleo boards, with one
exception that is easy to miss because everything else lines up.

| Signal | NUCLEO-H743ZI | NUCLEO-H563ZI |
|---|---|---|
| RMII_REF_CLK | PA1 | PA1 |
| RMII_MDIO | PA2 | PA2 |
| RMII_CRS_DV | PA7 | PA7 |
| RMII_MDC | PC1 | PC1 |
| RMII_RXD0 | PC4 | PC4 |
| RMII_RXD1 | PC5 | PC5 |
| RMII_TX_EN | PG11 | PG11 |
| RMII_TXD0 | PG13 | PG13 |
| **RMII_TXD1** | **PB13** | **PB15** |
| RMII_RXER | PG2 | dropped (see below) |
| PPS_OUT | PG8 | PG8 |

All pins use `GPIO_AF11_ETH` on both parts.

**Consequence for the port:** `ETHHW_InitClocks()` in `Drivers/EthDrv/mac_drv.c` groups
PB5 and PB13 into a single `HAL_GPIO_Init(GPIOB, ...)` call. On the H563 that call reduces
to **PB15 alone**. Leaving PB13 configured as AF11 and PB15 unconfigured produces a link
that negotiates and receives normally but never transmits — a failure mode that looks like
a PHY or DMA problem rather than a pin-map problem.

**On PPS and PB5:** the H743 project configures PB5 as ETH AF11 in `mac_drv.c` *and*
separately configures PG8 as ETH AF11 in the flexPTP hardware port. PG8 is the pin that
actually carries the pulse; the PB5 setup is vestigial. This port keeps **PG8** and drops
PB5, so PPS GPIO is configured in exactly one place — the flexPTP hardware port. If PPS is
ever moved, that port file is the only file to change.

---

## RXER Pin Note

*To be completed in Stage 17; the GPIO change itself lands in Stage 12.*

The H743 project configures PG2 as ETH_RMII_MII_RXER. This pin has been dropped
in the H563 port. This section will explain why and how to re-enable it.

---

## mac_drv.c Comparison

*To be completed in Stage 17.*

This section will provide a detailed comparison of:
1. The custom register-level MAC driver (`mac_drv.c`)
2. The ST HAL ETH driver (`stm32h5xx_hal_eth.c`)
3. The original H7 implementation

---

## Stage Log

### Stage 1 — Create new repository, copy structure
- Created new repository at `/home/damien/git/flexPTP-demo-NUCLEO-H563ZI`
- Copied all application source (Src/, Inc/) except `stm32h7xx_hal_conf.h`
- Copied Drivers/EthDrv/ and Drivers/CMSIS/ (H7 device headers still present, will be swapped in Stage 3)
- Copied build infrastructure (CMakeLists.txt, CMakePresets.json, build.sh, cmake/, Middlewares/)
- Added git submodules: embfmt, lwip, flexptp, etherlib
- Copied Modules/blocking_io/ and Modules/lwip_port/ as regular directories
- NOT copied: stm32h7xx-hal-driver submodule, H7 linker script, H7 startup file, H7 HAL conf
- Note: `Src/system_stm32h7xx.c` and `Middlewares/CMakeLists.txt` still reference H7 — will be updated in later stages

### Stage 2 — Swap HAL submodule
- Added `Drivers/stm32h5xx-hal-driver` submodule from STMicroelectronics GitHub
- H5 HAL includes ETH driver (`stm32h5xx_hal_eth.c/h`) — confirmed present

### Stage 3 — Add CMSIS H5 device headers
- Sourced device headers from `STMicroelectronics/cmsis_device_h5` GitHub repo
- Added `stm32h563xx.h`, `stm32h5xx.h`, `system_stm32h5xx.h` to `Drivers/CMSIS/Device/ST/STM32H5xx/Include/`
- Removed H7 device headers (`STM32H7xx/Include/stm32h735xx.h`, `stm32h743xx.h`, `stm32h7xx.h`, `system_stm32h7xx.h`)
- Replaced `Src/system_stm32h7xx.c` with `Src/system_stm32h5xx.c` (from cmsis_device_h5 templates)
- Updated `Drivers/CMSIS/CMakeLists.txt` include path: `STM32H7xx` → `STM32H5xx`

### Stage 4 — New linker script
- Created `stm32h563zitx_FLASH.ld` with flat SRAM layout
- Memory regions: FLASH (2MB), SRAM1 (256K), SRAM2 (64K), SRAM3 (320K)
- Removed H7-specific regions: ITCMRAM, DTCMRAM, RAM_D1, RAM_D2, RAM_D3
- `.data`, `.bss`, stack, heap, FreeRTOS heap → SRAM1
- Ethernet DMA buffers (`.ETHBufferSection`, `.ETHStateAndDecripSection`) → SRAM3
- lwIP heap → SRAM3
- Removed SAI section (not used in this port)
- Added DCACHE coherency note for SRAM3 Ethernet region
- H7 linker script (`stm32h735igkx_FLASH.ld`) was not copied to this repo

### Stage 5 — New startup file
- Created `Startup/startup_stm32h563zitx.s` (`.cpu cortex-m33`)
- **Vector table generated programmatically** from the `IRQn_Type` enum in
  `stm32h563xx.h` rather than transcribed, so slot numbering cannot drift from CMSIS
  or from `HAL_NVIC_*` calls
- 131 IRQ slots (0..130), 128 defined + 3 reserved gaps emitted as `.word 0`;
  147 vector entries total = 588 bytes
- ARMv8-M: exception 7 is `SecureFault_Handler` (unreachable with TZEN=0, retained
  for layout correctness)
- `.fpu softvfp` retained, matching both the H743 project and ST's own H5 template;
  the file emits no FP instructions
- Reset_Handler follows the ST/upstream convention: set SP, `bl SystemInit`, copy
  `.data`, zero `.bss`, `__libc_init_array`, `main`. Note `main()` also calls
  `SystemInit()` — harmless, and unchanged from the H743 project
- Verified: assembles clean for `-mcpu=cortex-m33`; relocations confirm
  GPDMA1_Channel0 at index 43, USART3 at 76, USB_DRD_FS at 90, ETH at 122
- No H7 startup file existed in this repo to delete (`Startup/` was empty from Stage 1)

### Stage 6 — HAL configuration header
- Created `Inc/stm32h5xx_hal_conf.h` by transforming the HAL submodule's
  `stm32h5xx_hal_conf_template.h` programmatically, so it stays close to ST's baseline
- Modules enabled (mirroring the H743 project): CORTEX, CRC, DMA, ETH, FLASH, GPIO,
  HASH, PWR, RCC, RNG, TIM, UART. The other 49 are commented out.
- `HSE_VALUE` 25 MHz (template default) → **8 MHz** (Nucleo ST-Link MCO)
- `USE_HAL_UART_REGISTER_CALLBACKS` 0 → **1** — mandatory, `serial_io.c` calls
  `HAL_UART_RegisterCallback()`; the H743 project also sets this
- Ethernet block carried over from the H743 conf with the H5 unique-ID base:
  `UNIQUE_DEV_ID_BASE` `0x1FF1E800` → **`0x08FFF800`**, which feeds `ETH_MAC_ADDR0..5`
- `ETH_TX_DESC_CNT` / `ETH_RX_DESC_CNT` = 24 (H5 HAL defaults to 4, so this override
  is required, not cosmetic)
- No H7 HAL conf existed in this repo to delete (deliberately not copied in Stage 1)
- Verified: a TU including `stm32h5xx_hal.h` + `stm32h5xx_ll_rng.h` compiles clean under
  `-Wall -Wextra`, and `FLASH_LATENCY_5` / `RCC_HSE_BYPASS` / `ETH_MAC_ADDR1` all resolve

### Stage 7 — Consolidate build configuration into CMakeLists.txt
- Absorbed `cmake/vscode_generated.cmake` into `CMakeLists.txt` and deleted it, along
  with its `include()`. The file was headed "shall not be touched by user", so leaving
  the MCU configuration there meant one extension regeneration could silently revert it
- Values carried across **verbatim** (still Cortex-M7 / H735 at this point) so the diff
  is purely structural; retargeting is Stage 8
- Absorbed: `cpu_PARAMS`, `sources_SRCS`, `include_c_DIRS`, `linker_script_SRC`.
  The remaining appends were all empty and were dropped
- Removed `list(APPEND CMAKE_MODULE_PATH "{{sr:cmake_path}}")` — an unsubstituted
  template placeholder inherited from upstream (F31)
- Removed two include paths pointing at directories that do not exist (F32):
  `Middlewares/Inc` in `CMakeLists.txt`, `${LWIP_PORT_DIR}/include` in `Modules/CMakeLists.txt`
- `.vscode` CMake integration no longer feeds the build; a regenerated file would simply
  be ignored since nothing includes it

### Stage 8 — Build-system retarget
- `cpu_PARAMS`: `-mcpu=cortex-m7 -mfpu=fpv5-d16` → **`-mcpu=cortex-m33 -mfpu=fpv5-sp-d16`**
  (`-mthumb -mfloat-abi=hard` unchanged)
- `MCU_TYPE` `STM32H743xx` → **`STM32H563xx`**
- Linker script → `stm32h563zitx_FLASH.ld`; startup → `Startup/startup_stm32h563zitx.s`
- `Drivers/CMakeLists.txt` rewritten: target `H7HAL` → **`H5HAL`**, CMSIS include path
  `STM32H7xx` → `STM32H5xx`, and the 127-file H7 source list replaced by the 120
  non-template sources the H5 submodule ships. Sources whose module is disabled in
  `stm32h5xx_hal_conf.h` compile to empty translation units, so the list can be generated
  mechanically rather than curated
- `Src/CMakeLists.txt`: `system_stm32h7xx.c` → `system_stm32h5xx.c`, fixing the dangling
  reference left by Stage 3 (F10) that prevented the tree from configuring at all
- **Gate passed**: `cmake --preset debug` configures, and `libH5HAL.a` builds from 121
  objects with zero errors and zero warnings
- Note: the preset requires the Ninja generator (F44); `build.sh` uses the CMake default
  instead, so the two build paths have different prerequisites

### Stage 9 — FreeRTOS configuration & kernel port
- `configCPU_CLOCK_HZ` 480 MHz → **250 MHz**. This also drives the PLL: `init_pll()`
  computes `PLLN` from it, so it is the single source of truth for SYSCLK
  (see [System Clock Tree](#system-clock-tree))
- Added the ARMv8-M block the CM33 port hard-`#error`s without:
  `configENABLE_TRUSTZONE 0`, `configRUN_FREERTOS_SECURE_ONLY 0`,
  `configENABLE_FPU 1`, `configENABLE_MPU 0`
- `configENABLE_MVE` deliberately **left undefined** — the CM33 port errors out unless it
  is absent or 0
- Pre-existing settings that turned out to already be correct: `configUSE_16_BIT_TICKS 0`
  (satisfies FreeRTOS 11's tick-width requirement, so `configTICK_TYPE_WIDTH_IN_BITS` is
  not needed) and `configUSE_PORT_OPTIMISED_TASK_SELECTION 0` (mandatory here, since
  `configMAX_PRIORITIES` is 56 and the optimised selector caps at 32)
- `Middlewares/CMakeLists.txt`: `FREERTOS_PORT` → **`GCC_ARM_CM33_NTZ_NONSECURE`**. Note
  the plan said `GCC_ARM_CM33_NTZ`; that is not one of the kernel's option names.
  Include path and exported `device_header` → `portable/GCC/ARM_CM33_NTZ/non_secure`
- **Kernel pinned to `V11.3.0`**, replacing `GIT_TAG main`. Tracking a moving branch while
  changing the port was a reproducibility hazard (F24)
- **Gate passed**: kernel builds clean; the object produced is
  `portable/GCC/ARM_CM33_NTZ/non_secure/port.c.obj`, confirming the CM33 port is selected

