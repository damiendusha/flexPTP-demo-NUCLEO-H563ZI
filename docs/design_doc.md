# flexPTP NUCLEO-H563ZI — Design Document

This document captures design decisions, technical notes, and future porting guides
for the flexPTP demo port from NUCLEO-H743ZI (STM32H743, Cortex-M7) to
NUCLEO-H563ZI (STM32H563, Cortex-M33).

## Table of Contents

- [Port Overview](#port-overview)
- [PTP Clock Optimization Guide](#ptp-clock-optimization-guide)
- [EtherLib Porting Guide](#etherlib-porting-guide)
- [TrustZone Migration Guide](#trustzone-migration-guide)
- [RXER Pin Note](#rxer-pin-note)
- [mac_drv.c Comparison](#mac_drvc-comparison)
- [Stage Log](#stage-log)

---

## Port Overview

**Source**: NUCLEO-H743ZI — STM32H743ZI, Cortex-M7 @ 480 MHz, DP-FPU, stm32h7xx HAL  
**Target**: NUCLEO-H563ZI — STM32H563ZI, Cortex-M33 @ 250 MHz, SP-FPU, stm32h5xx HAL

Key decisions:
- PTP clock: 200 MHz / 5 ns increment (matching H7 config)
- HSE: Bypass mode (stock Nucleo, ST-Link MCO at 8 MHz)
- FPU: Keep `double` in Kalman filter, accept software emulation
- Network stack: LwIP only (initial port)
- TrustZone: Disabled (TZEN=0, ARM_CM33_NTZ)
- mac_drv.c: Register-level port

---

## PTP Clock Optimization Guide

*To be completed in Stage 11/14.*

The initial port uses PLL1_Q = 200 MHz with a 5 ns hardware increment, matching the
H7 configuration. This section will document how to optimize to 250 MHz (4 ns increment)
for improved PTP resolution.

### Current Configuration (200 MHz)
- `PTP_MAIN_OSCILLATOR_FREQ_HZ` = 200,000,000
- `PTP_INCREMENT_NSEC` = 5
- Addend calculation: `2^32 * increment_ns / (1e9 / osc_freq)`

### Future Optimization (250 MHz)
- `PTP_MAIN_OSCILLATOR_FREQ_HZ` = 250,000,000
- `PTP_INCREMENT_NSEC` = 4
- PLL1_Q must be reconfigured to output 250 MHz
- Kalman filter gains may need re-tuning

---

## EtherLib Porting Guide

*To be completed in Stage 14.*

The EtherLib in-house Ethernet stack is included as a git submodule but is not
compiled in the initial port. This section will document what needs to change to
enable EtherLib support.

---

## TrustZone Migration Guide

*To be completed in Stage 14.*

The initial port runs in non-secure flat mode (TZEN=0) using the `ARM_CM33_NTZ`
FreeRTOS port. This section will document how to migrate to a TrustZone-partitioned
build when needed.

---

## RXER Pin Note

*To be completed in Stage 10.*

The H743 project configures PG2 as ETH_RMII_MII_RXER. This pin has been dropped
in the H563 port. This section will explain why and how to re-enable it.

---

## mac_drv.c Comparison

*To be completed in Stage 10/14.*

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
