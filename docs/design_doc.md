# flexPTP NUCLEO-H563ZI — Design Document

This document captures design decisions, technical notes, and future porting guides
for the flexPTP demo port from NUCLEO-H743ZI (STM32H743, Cortex-M7) to
NUCLEO-H563ZI (STM32H563, Cortex-M33).

## Table of Contents

- [Port Overview](#port-overview)
- [System Clock Tree](#system-clock-tree)
- [Submodule Pinning](#submodule-pinning)
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

## Submodule Pinning

**All four submodules are pinned ahead of the NUCLEO-H743ZI reference project**, because
Stage 1 added them at then-current `master` rather than at the commits the H743 demo
records. This was not a deliberate choice at the time, but it is now a deliberate one to
keep.

| Submodule | This port | H743 reference |
|---|---|---|
| `flexptp` | `76aaf67` | `62fc906` |
| `lwip` | `3d896ba` | `554e104` |
| `embfmt` | `670a349` | `78d8aa4` |
| `etherlib` | `ec35be5` | `1127b3d` |

### Decision: keep the newer pins

flexPTP is five commits ahead:

```
76aaf67  P2P Sync-FollowUp-PDelReq separation
3601a6f  Fix non-atomic sec/nsec register read in ptphw_gettime()
70d2636  C linkage marking added
a60190f  Linux and OS-less working mode, new message buffer and FIFO
12fec5a  CMSIS OS2 support has been added
```

The deciding commit is **`3601a6f`**. `ptphw_gettime()` reads the seconds and nanoseconds
registers separately; without a guard, a seconds rollover between the two reads yields a
timestamp up to a second wrong. Compare:

```c
/* 62fc906 — what pinning back would give us */      /* 76aaf67 — what we have */
pTime->sec     = ETH->MACSTSR;                       do {
pTime->nanosec = ETH->MACSTNR & ETH_MACSTNR_TSSS;        pTime->sec     = ETH->MACSTSR;
                                                         pTime->nanosec = ETH->MACSTNR & ETH_MACSTNR_TSSS;
                                                     } while (pTime->sec != ETH->MACSTSR);
```

Since `Src/flexptp_port/ptp_port_stm32h563.c` is derived from the submodule's H743 port, it
inherits the fixed version. **Pinning back to match the reference project would silently
reintroduce a PTP timestamp bug** — the opposite of what "match the known-good reference"
is supposed to buy. `12fec5a` (CMSIS-OS2 support) is also directly relevant to this project.

### Cost of the decision

Two defects had to be worked around, both consequences of integrating a library the
reference project never did. Details in [port_review.md](port_review.md):

| Finding | Problem | Fix |
|---|---|---|
| **F47** | flexPTP uses `__PRI64_PREFIX`, a glibc-internal macro absent from newlib, so the library does not compile for `arm-none-eabi` | `__PRI64_PREFIX=__INT64` in `FLEXPTP_COMPILE_DEFS` |
| **F48** | `tx_cb`'s tag argument changed from `uint32_t` to `void *`, breaking the TX timestamp path | Aligned `Inc/lwipopts.h` and the `mac_drv.c` call site |

Neither patches a submodule, so `git submodule update` remains safe.

### What to watch

- **F47 should be reported upstream.** It breaks every newlib target, not just this one.
  If upstream fixes it, drop the `__PRI64_PREFIX` define — it is a reserved identifier and
  only defined here under duress.
- **The `tx_cb` contract is unversioned.** `Inc/lwipopts.h` and flexPTP's `nsd_lwip.c` must
  agree on the callback signature, and nothing enforces it. A future flexPTP bump should
  re-check it. A mismatch in the tag's *type* fails at compile time, as it did here; a
  mismatch in its *meaning* would not (F42).
- The other three submodules were not audited commit-by-commit. They build and link; no
  behavioural claim is made about their divergence.

### To revert

Pin all four back with `git -C Modules/<name> checkout <hash>`, then revert the F47 and F48
workarounds — and re-apply the `ptphw_gettime()` atomicity fix by hand, or accept the bug.

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

### Stage 10 — Clock & power init
- Headers → `stm32h5xx_hal.h`, `stm32h5xx_ll_rng.h`
- `RCC_HSE_ON` → **`RCC_HSE_BYPASS`** (ST-Link MCO; comment notes how to revert for a
  fitted X3 crystal)
- Symbol renames: `RCC_PLLSOURCE_HSE` → `RCC_PLL1_SOURCE_HSE`,
  `RCC_PLL1VCOWIDE` → `RCC_PLL1_VCORANGE_WIDE`, `RCC_PLL1VCIRANGE_1` → `RCC_PLL1_VCIRANGE_1`
- Clock tree: dropped `SYSCLKDivider`, `APB4CLKDivider`, `RCC_CLOCKTYPE_D1PCLK1`/`D3PCLK1`;
  added `RCC_CLOCKTYPE_PCLK3`
- **`AHBCLKDivider` = `RCC_SYSCLK_DIV1`** (HCLK 250 MHz, also the PTP clock) and APB
  dividers = `RCC_HCLK_DIV2`. The H5 divider namespaces are *inverted* relative to the H7 —
  see finding F45 in [port_review.md](port_review.md); this is a silent-miscompile trap
- `FLASH_LATENCY_4` → **`FLASH_LATENCY_5`** for 250 MHz at VOS0
- Removed `HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY)` — H5 accepts only
  `PWR_EXTERNAL_SOURCE_SUPPLY`, which does not apply here
- SYSCFG → SBS: `__HAL_RCC_SBS_CLK_ENABLE()`, and `HAL_EnableCompensationCell()` →
  `HAL_SBS_EnableVddIO1CompensationCell()` + `...VddIO2...`
- Welcome message updated for the board and MCU
- **Gate**: `main.c` compiles clean with zero warnings. Verified in isolation with a
  throwaway shim header, because `main.c` → `flexptp_options.h` → `mac_drv.h` still
  includes `stm32h7xx.h` until Stage 12 — a gate dependency the plan had not anticipated

### Stage 11 — Serial I/O GPDMA rewrite
- `__HAL_RCC_DMA1_CLK_ENABLE()` → `__HAL_RCC_GPDMA1_CLK_ENABLE()`
- `DMA1_Stream0` → `GPDMA1_Channel0`; `DMA_REQUEST_USART3_TX` → `GPDMA1_REQUEST_USART3_TX`
- `DMA1_Stream0_IRQHandler` → `GPDMA1_Channel0_IRQHandler` (matches the Stage 5 vector
  table, where GPDMA1_Channel0 is slot 43)
- The H5 `DMA_InitTypeDef` is a different structure, not a renamed one. Field mapping:

  | H7 (stream) | H5 (GPDMA) |
  |---|---|
  | `PeriphInc` / `MemInc` | `DestInc` / `SrcInc` (direction-dependent) |
  | `PeriphDataAlignment` / `MemDataAlignment` | `DestDataWidth` / `SrcDataWidth` |
  | `MemBurst` / `PeriphBurst` | `SrcBurstLength` / `DestBurstLength` — a beat count, 1..64 |
  | `FIFOMode` / `FIFOThreshold` | no equivalent; dropped |
  | `Priority` = `DMA_PRIORITY_VERY_HIGH` | `DMA_HIGH_PRIORITY` |
  | — | `BlkHWRequest`, `TransferAllocatedPort`, `TransferEventMode` (new, mandatory) |

- For memory→peripheral: `SrcInc = DMA_SINC_INCREMENTED` walks the buffer,
  `DestInc = DMA_DINC_FIXED` holds `USART3->TDR`
- `GPIO_SPEED_HIGH` → `GPIO_SPEED_FREQ_HIGH`; the short alias is H7-only (F46)
- USART3 setup otherwise unchanged: PD8/PD9, AF7, 115200 8N1, register callbacks
- Verified: compiles clean, zero warnings

### Stage 12 — Ethernet MAC driver port
The register audit from the plan review held exactly: **no ETH register or descriptor-bit
change was needed.** The H563 and H743 carry the same Synopsys QoS core, so all of
`MACTSCR`, `MACSSIR`, `MACSTSR/NR`, `MACSTSUR/NUR`, `MACTSAR`, `MACACR`, `MACATSSR/NR`,
`MACPPSCR`, `MACPPSTTSR/TTNR`, `MACPPSIR`, `MACPPSWR` and every `ETH_DMA*NDESC*` bit
resolved unchanged. The work was entirely peripheral plumbing:

- Headers: `stm32h7xx.h` → `stm32h5xx.h` in both `mac_drv.c` and `mac_drv.h`
- **ETH_TXD1 PB13 → PB15.** The `HAL_GPIO_Init(GPIOB, …)` call now covers PB15 alone
- **PG2 (RXER) dropped** from the GPIOG init
- **PB5 dropped.** PPS is PG8 and is owned by the flexPTP hardware port, so this driver no
  longer configures a PPS pin at all — one owner, one place
- Clock enables `ETH1MAC`/`ETH1TX`/`ETH1RX` → `ETH`/`ETHTX`/`ETHRX`
- SYSCFG → SBS: `__HAL_RCC_SBS_CLK_ENABLE()`,
  `HAL_SYSCFG_ETHInterfaceSelect(SYSCFG_ETH_RMII)` → `HAL_SBS_ETHInterfaceSelect(SBS_ETH_RMII)`
- **MDIO divider bug fixed (F8).** `ETH_MACMDIOAR_CR_DIV102` is already a positioned mask
  (bit 10); the inherited code shifted it by `ETH_MACMDIOAR_CR_Pos` again, landing in the
  `RDA` field and leaving `CR` = 0. Effect at HCLK 250 MHz:

  | | CR field | divider | MDC | verdict |
  |---|---|---|---|---|
  | inherited (double-shifted) | 0 | ÷42 | 5.95 MHz | over the 2.5 MHz limit |
  | fixed | 4 | ÷102 | 2.45 MHz | within limit |

- `ETHHW_UpdatePTPTime()` annotated as unreachable, with a warning about its suspect
  subtract path (F41). Ported mechanically; not validated, because nothing calls it
- `phy_common.c` needed no change — it reaches the HAL only through `mac_drv.h`
- Verified: `mac_drv.c` and `phy_common.c` both compile clean, zero warnings

### Stage 13 — flexPTP options
- `PTP_MAIN_OSCILLATOR_FREQ_HZ` 240 MHz → **250 MHz** (= HCLK), `PTP_INCREMENT_NSEC` 5.
  Confirmed the core computes `PTP_ADDEND_INIT` = 3435973836 (`0xCCCCCCCC`), K = 1.25,
  ≈0.29 ppb tuning resolution — matching [ptp_clock.md](ptp_clock.md) exactly
- Added `#define PTP_ADDEND_INTERFACE` explicitly (it was auto-selected; the porting guide
  asks for it to be stated)
- Added `#define FLEXPTP_FREERTOS (1)` explicitly (F40). The fallback in `ptp_defs.h`
  already chose it, but the binding is load-bearing and was invisible
- **Removed `PTP_UPDATE_CLOCK`** (F41): the core never references it, and the
  `ETHHW_UpdatePTPTime()` it called has a suspect subtract path. Coarse correction goes
  through `PTP_SET_CLOCK`
- Hardware-port include repointed from the bundled H743 port to the project-local
  `ptp_port_stm32h563.h`
- Left `FLEXPTP_SNPRINTF` alone — the porting guide calls it `SPRINTF`, but the code does
  not (F38)

### Stage 14 — flexPTP H563 hardware port
- Created `Src/flexptp_port/ptp_port_stm32h563.{c,h}`, derived from the bundled
  `ptp_port_stm32h743_lwip.c`. The derivation is deliberately minimal — six changes —
  so it stays diffable against its ancestor:
  header include, `stm32h5xx_hal.h`, `+#include <math.h>`, dropped the unused
  `extern ETH_HandleTypeDef EthHandle` and its commented-out user, and
  `GPIO_SPEED_HIGH` → `GPIO_SPEED_FREQ_HIGH` (F46, as predicted at Stage 11)
- **PPS stays on PG8 / AF11**, unchanged from the H743 port. This file is now the only
  place PPS GPIO is configured
- `ptphw_gettime()` takes `TimestampU *`, and the header says why (F37: the published
  porting guide says `TimestampI *`, but every call site in the core passes `TimestampU *`)
- Wired via flexPTP's custom-port path: `FLEXPTP_HWPORT_SRC` set to an absolute path and
  `FLEXPTP_HWPORT` left unmatched, so the library reports
  *"flexPTP: custom hardware port defined"* and uses the file verbatim
- Two integration defects surfaced only once the library actually compiled — see
  [port_review.md](port_review.md) F47 and F48. Both trace to the same root cause: the
  flexPTP submodule is pinned newer (`76aaf67`) than the commit the H743 demo uses
  (`62fc906`), so this port integrates a library the reference project never did
- After both fixes the flexPTP library, its lwIP network-stack driver and the Kalman servo
  all compile. The only remaining build errors are the Stage 15 header sweep

### Stage 15 — Global HAL header sweep
- `stm32h7xx_hal.h` → `stm32h5xx_hal.h` in `eth_drv_lwip.c`, `eth_drv_etherlib.c`,
  `ethernet_etherlib.c`; `stm32h7xx.h` → `stm32h5xx.h` in `cmsis_os2.c`. The last two were
  the files the original plan's Stage-12 list omitted (F17)
- Remaining `STM32H7` matches in the tree are prose in comments only, kept as deliberate
  historical references
- Two further defects surfaced, both only after every TU compiled:
  - **F49** — `task_ptp_is_operating()` → `is_flexPTP_operating()`, renamed upstream at
    `a60190f`. Third consequence of the newer submodule pin
  - **F50** — the ARMv8-M FreeRTOS port defines `SVC_Handler`, `PendSV_Handler` and
    `SysTick_Handler` directly, so the H7-era remaps in `FreeRTOSConfig.h` were wrong.
    The SysTick remap renamed the CMSIS-RTOS2 shim's *call* to `OS_Tick_Handler()`, which
    nothing defines. Dropped all three remaps and set
    `USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION 1` so the FreeRTOS port owns the handler
- **The firmware links.** First complete build of this port:

  | Region | Used | Size | % |
  |---|---|---|---|
  | FLASH | 221 552 B | 2 MB | 10.6 |
  | SRAM1 | 133 328 B | 256 KB | 50.9 |
  | SRAM2 | 0 B | 64 KB | 0.0 |
  | SRAM3 | 74 888 B | 320 KB | 22.9 |

- Image sanity-checked: entry point `0x080024a9` (Thumb bit set), vector word 0 =
  `0x20040000` = top of SRAM1 exactly as the linker script specifies, word 1 = the entry
  point, hard-float EABI
- One warning remains project-wide (`ETHHW_PrintRingBufStatus` unused) — Stage 16

