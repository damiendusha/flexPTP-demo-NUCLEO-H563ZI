# Architecture

How this firmware fits together: the module stack, the threads, the interrupts, and the
provenance of every third-party component.

Companion documents: [design_doc.md](design_doc.md) (porting decisions) ·
[ptp_clock.md](ptp_clock.md) (PTP clock derivation) ·
[port_review.md](port_review.md) (findings) ·
[implementation_plan.md](implementation_plan.md) (staged work plan)

## Table of Contents

- [System Overview](#system-overview)
- [Module Inventory](#module-inventory)
- [Layering](#layering)
- [Boot Sequence](#boot-sequence)
- [Threads](#threads)
- [Interrupts](#interrupts)
- [The PTP Timestamp Path](#the-ptp-timestamp-path)
- [Memory Layout](#memory-layout)

---

## System Overview

A FreeRTOS application on an STM32H563ZI that runs the flexPTP IEEE 1588 stack over lwIP,
with a serial CLI for control. The MCU's Ethernet MAC is driven by a project-owned
register-level driver rather than ST's HAL ETH driver, because PTP hardware timestamping
needs access to the DMA descriptors that the HAL driver abstracts away.

| | |
|---|---|
| MCU | STM32H563ZI, Cortex-M33, 250 MHz, TrustZone disabled (TZEN=0) |
| RTOS | FreeRTOS V11.3.0, `ARM_CM33_NTZ` non-secure port, heap_4 |
| RTOS API | CMSIS-RTOS2 shim for application threads; flexPTP calls FreeRTOS directly |
| Network stack | lwIP 2.2.1 |
| PTP | flexPTP, Kalman servo, ADDEND clock interface |
| Console | USART3 → ST-Link VCP, 115200 8N1, TX via GPDMA |
| PHY | LAN8742A over RMII |

---

## Module Inventory

Every third-party component and its provenance. **No submodule is patched** — verified with
`git submodule foreach git status --porcelain`, which reports all five clean. Where
behaviour had to change, it was done through a supported extension point or a compile
definition, never by editing vendored source.

| Component | Version | How it is vendored | Modified? |
|---|---|---|---|
| **stm32h5xx-hal-driver** | `v1.7.0-8-g0fff6e2` | git submodule | **As-is.** Configured by `Inc/stm32h5xx_hal_conf.h`; 12 of 61 modules enabled |
| **CMSIS Core (M)** | 5.6 | in-tree, `Drivers/CMSIS/Include` | **As-is.** Inherited from the H743 project, not refreshed |
| **CMSIS Device STM32H5xx** | 1.7.0 | in-tree, `Drivers/CMSIS/Device` | **As-is.** From `STMicroelectronics/cmsis_device_h5` |
| **CMSIS-RTOS2 shim** | ST distribution | in-tree, `Drivers/CMSIS/CMSIS_RTOS_V2` | **Patched.** One line: `stm32h7xx.h` → `stm32h5xx.h` (Stage 15). This is a vendored copy, not a submodule, so the edit is in-repo and visible in git history |
| **FreeRTOS-Kernel** | `V11.3.0` | CMake `FetchContent`, pinned tag | **As-is.** Configured by `Inc/FreeRTOSConfig.h` |
| **lwIP** | `3d896ba` (`STABLE-2_2_1_RELEASE-34`) | git submodule | **As-is, but extended.** `Inc/lwipopts.h` injects four fields into `struct pbuf` via lwIP's `LWIP_PBUF_CUSTOM_DATA` hook — a supported extension point, not a patch. See [The PTP Timestamp Path](#the-ptp-timestamp-path) |
| **flexPTP** | `76aaf67` (`RC1.0-11`) | git submodule | **As-is, with two external workarounds.** (1) `__PRI64_PREFIX=__INT64` compile definition, because flexPTP uses a glibc-internal macro newlib lacks (F47). (2) The STM32H5 hardware port is supplied by this project, since flexPTP ships none |
| **embfmt** | `670a349` | git submodule | **As-is.** Formatted-print backend for `MSG()` |
| **EtherLib** | `ec35be5` | git submodule | **Present but not compiled.** Alternative to lwIP, selectable via the `ETH_STACK` CMake variable |

Project-owned code that would otherwise look like a third-party module:

| Component | Location | Role |
|---|---|---|
| **EthDrv** | `Drivers/EthDrv/` | Register-level Ethernet MAC driver (`mac_drv.c`), PHY driver, and the lwIP/EtherLib glue |
| **lwIP port** | `Modules/lwip_port/` | `sys_arch` layer mapping lwIP onto FreeRTOS |
| **blocking_io** | `Modules/blocking_io/` | Blocking FIFO behind the console |
| **flexPTP HW port** | `Src/flexptp_port/` | STM32H563 hardware port for flexPTP (Stage 14) |

> **Submodule pins are all ahead of the NUCLEO-H743ZI reference project.** This is a
> deliberate, documented decision — see [design_doc.md](design_doc.md#submodule-pinning).
> It is the root cause of findings F47–F49.

---

## Layering

```
        ┌─────────────────────────────────────────────────────────┐
        │  main.c · cmds.c · cli  ·  flexptp_options.h            │  application
        └─────────────────────────────────────────────────────────┘
                │                    │                    │
        ┌───────▼────────┐   ┌───────▼────────┐   ┌───────▼───────┐
        │ standard_output│   │    flexPTP     │   │     lwIP      │
        │  + blocking_io │   │  + Kalman servo│   │               │
        └───────┬────────┘   └───┬────────┬───┘   └───────┬───────┘
                │                │        │               │
                │        ┌───────▼──┐  ┌──▼────────────┐  │
                │        │ HW port  │  │ nsd_lwip.c    │  │  flexPTP ports
                │        │ (H563)   │  │ (bundled)     │  │
                │        └───────┬──┘  └──┬────────────┘  │
                │                │        │               │
        ┌───────▼────────────────▼────────▼───────────────▼───────┐
        │   EthDrv:  mac_drv.c  ·  phy_drv  ·  eth_drv_lwip.c     │  project drivers
        └─────────────────────────┬───────────────────────────────┘
                                  │
        ┌─────────────────────────▼───────────────────────────────┐
        │        STM32H5 HAL  ·  CMSIS-RTOS2  ·  FreeRTOS         │  vendor
        └─────────────────────────┬───────────────────────────────┘
                                  │
        ┌─────────────────────────▼───────────────────────────────┐
        │              CMSIS Core  ·  STM32H563 hardware          │
        └─────────────────────────────────────────────────────────┘
```

Two boundaries are worth calling out:

- **`mac_drv.c` bypasses the HAL ETH driver.** It programs the MAC and its DMA descriptor
  rings directly. This is what makes hardware timestamping reachable; `stm32h5xx_hal_eth.c`
  is compiled into the HAL library but never used.
- **`flexptp_options.h` is the whole flexPTP configuration surface.** It selects the OS
  binding, the clock interface, the servo, and the print/CLI macros, and it is where the
  hardware port is wired in. flexPTP has no other configuration input.

---

## Boot Sequence

```
Reset_Handler                       Startup/startup_stm32h563zitx.s
  ├─ set SP = _estack (0x20040000)
  ├─ SystemInit()                   Src/system_stm32h5xx.c — VTOR
  ├─ copy .data, zero .bss
  ├─ __libc_init_array()
  └─ main()                         Src/main.c
       ├─ SystemInit()              (called again; harmless, inherited from H743)
       ├─ HAL_Init()
       ├─ init_osc_and_clk()        VOS0 → PLL1 → 250 MHz → SBS comp cells
       ├─ init_randomizer()         RNG seeds rand()
       ├─ init_mpu()                HAL_MPU_Disable()
       ├─ serial_io_init()          USART3 + GPDMA1_Ch0; starts "serialio"
       ├─ osKernelInitialize()
       ├─ osThreadNew(task_startup) → "init"
       └─ osKernelStart()           does not return

task_startup ("init")
  ├─ cli_init()                     starts "cli"
  ├─ print_welcome_message()
  ├─ init_ethernet()                lwIP init → "tcpip_thread", "rx", "phy"; MAC+PHY up
  ├─ cmd_init()                     registers CLI commands
  └─ loop { osDelay(1000) }         stays resident, idle

"flexptp" CLI command → reg_task_ptp() → creates the "ptp" thread on demand
```

PTP is **not** started at boot. The stack comes up only when the operator types `flexptp`
at the CLI, which is why `is_flexPTP_operating()` gates that command.

---

## Threads

Priorities below are raw FreeRTOS priorities. `configMAX_PRIORITIES` is 56. The CMSIS-RTOS2
shim passes `osPriority_t` values straight through to `xTaskCreate`, so the CMSIS and
FreeRTOS scales are the same numbers here — `osPriorityNormal` *is* 24.

| Thread | Priority | Stack | Created by | Role |
|---|---|---|---|---|
| `tcpip_thread` | **40** (`osPriorityHigh`) | 3072 B | lwIP via `sys_thread_new` | lwIP core; all stack processing is serialised here |
| `ptp` | **24** | 2048 B | `reg_task_ptp()`, on demand | flexPTP core: BMCA, servo, message handling |
| `init` | 24 (default) | 2048 B | `main()` | One-shot bring-up, then idles at 1 Hz |
| `cli` | 24 (default) | 2048 B | `cli_init()` | Console command parsing |
| `rx` | 24 (default) | 2048 B | `eth_drv_lwip.c` | Drains the Ethernet RX ring into lwIP |
| `serialio` | 24 (default) | 2048 B | `serial_io_init()` | FIFO → UART DMA; the only writer to USART3 |
| `phy` | 24 (default) | 1024 B | `eth_drv_lwip.c` | PHY link-state polling |
| `Tmr Svc` | 2 | 1040 B | FreeRTOS | Software timers; flexPTP's heartbeat runs here |
| `IDLE` | 0 | 520 B | FreeRTOS | |

Notes:

- **Threads created with `attr.priority` left zero get `osPriorityNormal` (24)**, not
  priority 0. `osPriorityNone` is 0 and the shim substitutes Normal. Six of the nine
  threads rely on this default, so they are all peers at 24 and round-robin against each
  other.
- **`tcpip_thread` is the only thread above the pack.** Everything else at 24 can be
  preempted by it, which is what keeps the network stack responsive under console load.
- **The `ptp` thread can be wedged by the MAC driver.** `ETHHW_SetPTPAddend()` waits on
  `MACTSCR.TSADDREG` with no timeout; if the PTP timestamp unit is not running, that wait
  never returns and the thread is stuck inside the driver, taking the `ptp pps` and
  `ptp addend` CLI commands with it. See F53/F54 in [port_review.md](port_review.md).
- **`ptp` sits at the same priority as the CLI and the console writer.** flexPTP's timing
  does not depend on winning that race, because timestamps are captured in hardware — see
  below.
- **Heap**: FreeRTOS `heap_4`, 30 KB, in `.FreeRTOSHeapSection` in SRAM1. Thread stacks are
  allocated from it, so the nine threads above account for roughly 15 KB.

---

## Interrupts

`__NVIC_PRIO_BITS` is 4 → 16 priority levels, 0 (highest) to 15 (lowest).

| Interrupt | IRQn | Vector idx | Priority | Handler | Defined in |
|---|---|---|---|---|---|
| SVC | −5 | 11 | 0 | `SVC_Handler` | FreeRTOS `portasm.c` |
| PendSV | −2 | 14 | 15 (set by port) | `PendSV_Handler` | FreeRTOS `portasm.c` |
| SysTick | −1 | 15 | 15 (`TICK_INT_PRIORITY`) | `SysTick_Handler` | FreeRTOS `port.c` |
| **ETH** | 106 | 122 | **7** | `ETH_IRQHandler` | `eth_drv_lwip.c` |
| **USART3** | 60 | 76 | **12** | `USART3_IRQHandler` | `serial_io.c` |
| **GPDMA1 Ch0** | 27 | 43 | **12** | `GPDMA1_Channel0_IRQHandler` | `serial_io.c` |

Every other vector resolves to `Default_Handler`, an infinite loop.

### The FreeRTOS priority constraint

`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` is **5**. On Cortex-M this partitions the
priority space:

```
  0 ─ 4   ABOVE the kernel's masking level.
          Never masked by taskENTER_CRITICAL(). MUST NOT call any FreeRTOS *FromISR API.
  5 ─ 15  At or below the masking level. May call *FromISR APIs.
```

All three project interrupts sit at 7 or 12, so all three are permitted to call
`*FromISR` APIs — which they do, via the CMSIS-RTOS2 message queue and event flags. Anything
added above priority 5 in future must not touch FreeRTOS.

The ordering is deliberate: **ETH (7) preempts the console (12)**, so heavy CLI output
cannot delay Ethernet servicing. Both sit below the kernel masking level.

### Handler responsibilities

- **`ETH_IRQHandler`** → `ETHHW_ISR()`. On RX, posts a notification to the `rx` thread. On
  TX, runs `ETHHW_ProcessTx()` **in interrupt context**, which walks the TX ring and
  invokes the transmit-timestamp callback for each completed descriptor.
- **`USART3_IRQHandler`** → `HAL_UART_IRQHandler()`, driving the single-byte RX interrupt
  that feeds the CLI.
- **`GPDMA1_Channel0_IRQHandler`** → `HAL_DMA_IRQHandler()`, completing console TX and
  setting the event flag the `serialio` thread waits on.

---

## The PTP Timestamp Path

This is the part of the system where the layering is least obvious, and the part most
likely to break silently (see F42, F48).

**Receive:**

```
MAC captures RX timestamp into a context descriptor
  → ETHHW_ProcessRx() pairs the data descriptor with its context descriptor
  → ETHHW_ReadCallback() in eth_drv_lwip.c stores it into pbuf->time_s / time_ns
  → nsd_lwip.c reads those fields and calls ptp_receive_enqueue()
```

**Transmit:**

```
flexPTP sets pbuf->tx_cb = ptp_transmit_cb and pbuf->tag = uid   (nsd_lwip.c)
  → eth_drv_lwip.c copies both into the TX descriptor extension
    and sets ETH_DMATXNDESCRF_TTSE to request a timestamp
  → MAC captures the TX timestamp on transmission
  → ETHHW_ProcessTx(), in ETH_IRQHandler, invokes tx_cb(ts_s, ts_ns, tag)
```

`time_s`, `time_ns`, `tx_cb` and `tag` are **not** lwIP fields. They are injected into
`struct pbuf` by `Inc/lwipopts.h` through lwIP's `LWIP_PBUF_CUSTOM_DATA` hook — this is the
"instrumented lwIP driver" flexPTP's documentation requires.

**The `tx_cb` signature is an unversioned contract** between `Inc/lwipopts.h` and flexPTP's
`nsd_lwip.c`, and nothing enforces it. It has already drifted once (F48). Re-check it on
any flexPTP bump.

Because timestamps are captured **in hardware**, PTP accuracy does not depend on thread
scheduling or interrupt latency. Latency affects how quickly the servo *reacts*, not how
accurate its inputs are.

---

## Memory Layout

Regions from `stm32h563zitx_FLASH.ld`; usage from the Stage 15 build.

| Region | Base | Size | Contents | Used |
|---|---|---|---|---|
| FLASH | `0x08000000` | 2 MB | `.isr_vector`, `.text`, `.rodata`, `.data` LMA | 221 552 B (10.6%) |
| SRAM1 | `0x20000000` | 256 KB | `.data`, `.bss`, main stack, C heap, FreeRTOS heap | 133 328 B (50.9%) |
| SRAM2 | `0x20040000` | 64 KB | *(unallocated — deliberate spare capacity)* | 0 B |
| SRAM3 | `0x20050000` | 320 KB | Ethernet DMA descriptors + buffers, lwIP heap | 74 888 B (22.9%) |

- `_estack` = `0x20040000`, the top of SRAM1 — confirmed as vector word 0 in the linked image.
- Ethernet DMA buffers live in SRAM3: 48 descriptors plus 48 × 1528 B of frame buffers.
- **No cache maintenance is performed anywhere**, and none is needed: the H563's DCACHE is
  disabled out of reset and `HAL_Init()` does not enable it. Enabling DCACHE later would
  require making the SRAM3 Ethernet region non-cacheable or adding explicit maintenance.
