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
- [Ethernet & PTP Clocking](#ethernet--ptp-clocking)
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
| PTP | flexPTP, Kalman servo, ADDEND clock interface; timestamp clock is `pll1_q_ck` at 250 MHz |
| Console | USART3 → ST-Link VCP, 115200 8N1, TX via GPDMA |
| PHY | LAN8742A over RMII |

---

## Module Inventory

Every third-party component and its provenance. Where behaviour had to change, it was done
through a supported extension point or a compile definition rather than by editing vendored
source. Two submodules are the exception, and both changes are **permanent bug fixes**.

> **Two submodules carry local patches**, both fixing real defects rather than instrumenting
> anything: a servo fix in flexPTP ([F59](port_review.md#f59--one-duplicated-sync-cycle-latches-the-servo-into-nan-for-ever-resolved))
> and three formatting fixes in embfmt ([F60](port_review.md#f60--embfmt-drops-ll-conversions-and-renders-nan-as-00000-resolved),
> [F66](port_review.md#f66--embfmt-misreports-small-e-values-and-any-rounding-carry-resolved)).
> Because the parent repository records only a submodule's commit pointer, working-tree edits
> inside `Modules/` are invisible to a clone and are destroyed by `git submodule update` —
> which is why the patches are kept in-tree. Neither is target-specific; both are worth
> reporting upstream. See [patches/flexptp/README.md](../patches/flexptp/README.md) and
> [patches/embfmt/README.md](../patches/embfmt/README.md).
>
> **`build.sh` applies them**, immediately after the submodule update, and **refuses to
> build** if a submodule is neither pristine nor patched. This matters because omitting them
> does not break the build — it produces a firmware that is silently wrong: the servo latches
> to NaN on the first duplicated Sync (F59) while printing a plausible `0.0000` correction
> (F60). Before that check existed, a fresh clone built exactly that firmware with no warning.
>
> The temporary F42/F61 instrumentation patch that used to sit alongside them was **dropped**
> once both findings were resolved. `patches/flexptp/README.md` records how to recover it.

| Component | Version | How it is vendored | Modified? |
|---|---|---|---|
| **stm32h5xx-hal-driver** | `v1.7.0-8-g0fff6e2` | git submodule | **As-is.** Configured by `Inc/stm32h5xx_hal_conf.h`; 12 of 61 modules enabled |
| **CMSIS Core (M)** | 5.6 (= CMSIS 5.9.0 content) | in-tree, `Drivers/CMSIS/Include` | **As-is, and already current for CMSIS 5.** Inherited from the H743 project, but verified byte-identical to the final CMSIS 5 release — see the note below. Not a submodule |
| **CMSIS Device STM32H5xx** | 1.7.0 | in-tree, `Drivers/CMSIS/Device` | **As-is.** From `STMicroelectronics/cmsis_device_h5` |
| **CMSIS-RTOS2 shim** | ST distribution | in-tree, `Drivers/CMSIS/CMSIS_RTOS_V2` | **Patched — one `#include` directive.** In `cmsis_os2.c`, `#include "stm32h7xx.h"` became `#include "stm32h5xx.h"`. No header file was copied or rewritten; see the note below. Vendored copy, not a submodule, so the edit is in-repo and visible in git history |
| **FreeRTOS-Kernel** | `V11.3.0` | CMake `FetchContent`, pinned tag | **As-is.** Configured by `Inc/FreeRTOSConfig.h` |
| **lwIP** | `3d896ba` (`STABLE-2_2_1_RELEASE-34`) | git submodule | **As-is, but extended.** `Inc/lwipopts.h` injects four fields into `struct pbuf` via lwIP's `LWIP_PBUF_CUSTOM_DATA` hook — a supported extension point, not a patch. See [The PTP Timestamp Path](#the-ptp-timestamp-path) |
| **flexPTP** | `76aaf67` (`RC1.0-11`) | git submodule | **Patched.** One permanent servo fix (F59, `patches/flexptp/0002-…`), and two external workarounds. (1) `__PRI64_PREFIX=__INT64` compile definition, because flexPTP uses a glibc-internal macro newlib lacks (F47). (2) The STM32H5 hardware port is supplied by this project, since flexPTP ships none |
| **embfmt** | `670a349` | git submodule | **Patched.** Formatted-print backend for `MSG()`. `%ll` conversions were dropped entirely and NaN printed as `0.0000`; see [F60](port_review.md#f60--embfmt-drops-ll-conversions-and-renders-nan-as-00000-resolved) and `patches/embfmt/` |
| **EtherLib** | `ec35be5` | git submodule | **Present but not compiled.** Alternative to lwIP, selectable via the `ETH_STACK` CMake variable |

> **"CMSIS Core (M) 5.6" is not a stale version.** `5.6` is the **Core(M)** version from
> `cmsis_version.h`, which is versioned separately from the CMSIS *pack*. CMSIS 5.9.0 — the
> final CMSIS 5 release — also reports Core(M) 5.6, so the number says nothing about age.
>
> Checked rather than assumed: all **30 files** in `Drivers/CMSIS/Include` are byte-identical
> to `CMSIS/Core/Include` at tag `5.9.0` of `ARM-software/CMSIS_5`. Fetching CMSIS 5 directly
> would therefore change **nothing**. The five headers this project actually compiles —
> `core_cm33.h`, `cmsis_version.h`, `cmsis_compiler.h`, `cmsis_gcc.h`, `mpu_armv8.h`, all
> reached through `stm32h5xx.h`, never included directly — are among them.
>
> **CMSIS 6 would be a real migration**, not a refresh. Core(M) 6.1 restructures the tree:
> the compiler and feature headers move under `m-profile/` (`cmsis_gcc_m.h`,
> `armv8m_mpu.h`, `armv8m_pmu.h`, `armv81m_pac.h`, `armv7m_cachel1.h`, `armv7m_mpu.h`),
> `cmsis_armcc.h` is gone with Arm Compiler 5 support, `cmsis_clang.h` is new, and
> `core_cm33.h` differs by about 1 050 lines and no longer includes `mpu_armv8.h` itself.
> The constraint is not the compiler — this project uses GCC — but ST: the pinned
> `stm32h5xx-hal-driver` and CMSIS device pack 1.7.0 are validated against CMSIS 5. The right
> way to move is to take the CMSIS Core that ships with the STM32CubeH5 release matching the
> device pack, not the newest tag available.

> **What "patched" means for the CMSIS-RTOS2 shim.** It is easy to read the row above as a
> header having been copied or converted. It was neither.
>
> The shim is four files, taken verbatim from the H743 project in Stage 1 (`d0f0b2a`). The
> whole of its subsequent history is one line in `cmsis_os2.c`, made in Stage 15 (`51031d9`):
>
> ```diff
> --- a/Drivers/CMSIS/CMSIS_RTOS_V2/cmsis_os2.c
> +++ b/Drivers/CMSIS/CMSIS_RTOS_V2/cmsis_os2.c
> @@ -34,7 +34,7 @@
> -#include "stm32h7xx.h"
> +#include "stm32h5xx.h"
> ```
>
> `git diff d0f0b2a HEAD -- Drivers/CMSIS/CMSIS_RTOS_V2/` reports **1 file changed, 1
> insertion, 1 deletion**; `cmsis_os2.h`, `freertos_mpool.h` and `freertos_os2.h` have never
> been touched.
>
> The device header itself was **not** modified, converted or hand-written.
> `stm32h5xx.h` is ST's own CMSIS device header, added unmodified as part of the H5 device
> pack in Stage 3 (`f4e4b2f`) and living at
> `Drivers/CMSIS/Device/ST/STM32H5xx/Include/stm32h5xx.h` — the *CMSIS Device STM32H5xx* row
> above. `stm32h7xx.h` does not exist anywhere in this repository; the H7 device headers were
> removed in Stage 3, which is exactly why this `#include` had to change.
>
> So the shim was retargeted at a different device header, not ported.

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
| `rx` | **32** (`osPriorityAboveNormal`) | 2048 B | `eth_drv_lwip.c` | Drains the Ethernet RX ring into lwIP |
| `serialio` | 24 (default) | 2048 B | `serial_io_init()` | FIFO → UART DMA; the only writer to USART3 |
| `phy` | 24 (default) | 1024 B | `eth_drv_lwip.c` | PHY link-state polling |
| `Tmr Svc` | 2 | 1040 B | FreeRTOS | Software timers; flexPTP's heartbeat runs here |
| `IDLE` | 0 | 520 B | FreeRTOS | |

Notes:

- **Threads created with `attr.priority` left zero get `osPriorityNormal` (24)**, not
  priority 0. `osPriorityNone` is 0 and the shim substitutes Normal. Five of the nine
  threads rely on this default, so they are peers at 24 and round-robin against each other.
- **`rx` sits above the console at 32**, so a burst of CLI output cannot round-robin against
  the thread that restores RX descriptors to the DMA. This mirrors the interrupt ordering the
  project already chose deliberately (ETH at 7 preempts the console at 12). Note that no
  benefit was *measurable* on the bench: with `rx` at 24 and at 32, 400 pings at 1400 B under
  continuous CLI load both gave 0% loss and ~0.40 ms mean RTT. A 115200-baud console cannot
  generate enough load to starve it. The ordering is kept as correct-by-construction, not as
  a demonstrated optimisation.
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

**`HardFault`, `MemManage`, `BusFault` and `UsageFault` report and halt** — they are naked
handlers in `Src/main.c` that recover the exception frame, print `CFSR`, `HFSR`, the faulting
`PC` and `LR` through the polled UART writer, and decode the causes this port has actually
hit. Every other vector still resolves to `Default_Handler`, an infinite loop.

That matters more on this part than it looks. The Cortex-M33 port programs `PSPLIM` on every
context switch, so **a task stack overflow is trapped by hardware** (`UFSR.STKOF`) on the
offending push, long before FreeRTOS's `configCHECK_FOR_STACK_OVERFLOW` pattern check would
run at the next switch. `vApplicationStackOverflowHook()` is therefore almost never the thing
that catches an overflow here — this handler is. Measured:

| Failure | Reported |
|---|---|
| Task stack overflow | `CFSR` = `0x00100000` → *UFSR.STKOF — stack overflow (PSPLIM)* |
| Call through a NULL pointer | `CFSR` = `0x00000100`, `PC` = `0x00000000` → *BFSR.IBUSERR* |

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

## Ethernet & PTP Clocking

The Ethernet MAC takes **five separate clocks**, and confusing them costs real debugging
time — the PTP clock in particular is *not* the 50 MHz RMII reference clock, and *not*
HCLK. RM0481 Table 714 lists them as distinct signals:

| Signal | Source on this board | Frequency | Enabled by | Symptom if absent |
|---|---|---|---|---|
| `eth_hclk` | AHB bus clock | 250 MHz | `RCC_AHB1ENR.ETHEN` | Register access faults |
| `eth_tx_clk` | derived from RMII ref | 25/2.5 MHz | `RCC_AHB1ENR.ETHTXEN` | No transmit |
| `eth_rx_clk` | derived from RMII ref | 25/2.5 MHz | `RCC_AHB1ENR.ETHRXEN` | No receive |
| `eth_rmii_ref_clk` | **LAN8742A PHY → PA1** | **50 MHz** | board wiring (SB58) | Link never comes up |
| **`clk_ptp_ref_i`** | **`pll1_q_ck`** | **250 MHz** | **`RCC_PLL1CFGR.PLL1QEN`** | **Timestamps frozen; MAC otherwise fine** |

Two consequences worth internalising:

- **The 50 MHz RMII reference clock is generated by the PHY, not the MCU.** UM3115: *"The
  PHY RMII_REF_CLK generates the 50 MHz clock for the STM32H5 series microcontroller."*
  It drives the MAC's TX/RX datapath. It has nothing to do with PTP timestamping.
- **`clk_ptp_ref_i` is hardwired to `pll1_q_ck` on the H563** — there is no mux
  (`ETHPTPCLKSEL` exists only on STM32H543/553xx), and the fixed source is *not* HCLK.
  Because it is gated by `PLL1QEN` alone, forgetting the output enable produces a MAC that
  links, does DHCP and passes traffic perfectly while every timestamp stays zero. That was
  finding F53; see [ptp_clock.md](ptp_clock.md).

### Board hardware preconditions

| Requirement | Detail |
|---|---|
| **JP6 must be ON** | Connects PB15 to RMII_TXD1. UM3115: *"Make sure that JP6 is ON when using Ethernet."* PB15 is shared with `I2S_A_SD`. |
| HSE source | Default is the ST-Link MCO at **8 MHz** (SB49 ON), which is what `HSE_VALUE` assumes |
| X3 crystal | Fitted on MB1404 but **25 MHz**, and disconnected by default (SB3/SB4 OFF) — see F55 |

---

## The PTP Timestamp Path

This is the part of the system where the layering is least obvious, and the part most
likely to break silently (see F42, F48).

**Receive:**

```
MAC captures RX timestamp into a context descriptor
  → ETHHW_ProcessRx() [ISR] pairs the data descriptor with its context descriptor,
      marks both ETHHW_DescExt.inFlight, and queues the event on rx_msg_q
  → rx_thread [task] copies the frame into a pbuf, stores the timestamp into
      pbuf->time_s / time_ns, hands it to lwIP, then ETHHW_RestoreRXDesc()
      returns ownership to the DMA and clears inFlight
  → nsd_lwip.c reads those fields and calls ptp_receive_enqueue()
```

The `inFlight` marker is what makes the handoff safe. The descriptor's OWN bit is clear for
the whole span between the MAC writing the frame and `rx_thread` restoring it, so ownership
alone cannot distinguish "newly received" from "already queued" — and `ETHHW_ProcessRx()`
runs from the ISR, which can re-enter that span freely. Without the marker the driver
re-delivers queued frames; measured at **57% of all frames** under ordinary PTP traffic
(F61).

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

**Both halves of this path are verified working on hardware at the driver level.** Receive
timestamps arrive via the context descriptor (T1/T2 carry real values), and transmit
timestamps were instrumented at 698-for-698 through `ETHHW_ProcessTx()` into the callback.
What remains broken (F42) is downstream of this diagram, inside flexPTP's message buffer:
the timestamp is delivered but never matched back to the Delay_Req that requested it.

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
