# flexPTP NUCLEO-H563ZI — Design Document

This document captures design decisions, technical notes, and future porting guides
for the flexPTP demo port from NUCLEO-H743ZI (STM32H743, Cortex-M7) to
NUCLEO-H563ZI (STM32H563, Cortex-M33).

## Table of Contents

- [Port Overview](#port-overview)
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
