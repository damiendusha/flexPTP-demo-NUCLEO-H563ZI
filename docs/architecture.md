# Architecture

How this firmware fits together: the module stack, the threads, the interrupts, and the
provenance of every third-party component.

Companion documents: [design_doc.md](design_doc.md) (porting decisions) ·
[testing_architecture.md](testing_architecture.md) (how it is tested, and what is not) ·
[ptp_clock.md](ptp_clock.md) (PTP clock derivation) ·
[ptp_profiles.md](ptp_profiles.md) (default vs gPTP presets) ·
[port_review.md](port_review.md) (findings) ·
[threadx_porting_plan.md](threadx_porting_plan.md) (the RTOS/stack port) ·
[implementation_plan.md](implementation_plan.md) (the original silicon port — historical)

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

A ThreadX application on an STM32H563ZI that runs the flexPTP IEEE 1588 stack over NetX Duo,
with a serial CLI for control. The MCU's Ethernet MAC is driven by a project-owned
register-level driver rather than ST's HAL ETH driver, because PTP hardware timestamping
needs access to the DMA descriptors that the HAL driver abstracts away.

| | |
|---|---|
| MCU | STM32H563ZI, Cortex-M33, 250 MHz, TrustZone disabled (TZEN=0) |
| RTOS | Eclipse ThreadX 6.5.1, `cortex_m33/gnu` port, `TX_SINGLE_MODE_NON_SECURE`, 1 kHz tick |
| RTOS API | CMSIS-RTOS2 throughout — application **and** flexPTP, over ST's `cmsis_rtos_threadx` wrapper. Memory comes from two ThreadX byte pools, not a heap |
| Network stack | Eclipse NetX Duo 6.5.1, IPv4 only, TCP and IPv6 compiled out |
| PTP | flexPTP, Kalman servo, ADDEND clock interface; timestamp clock is `pll1_q_ck` at 250 MHz |
| Console | USART3 → ST-Link VCP, 115200 8N1, TX via GPDMA |
| PHY | LAN8742A over RMII |

---

## Module Inventory

Every third-party component and its provenance. Where behaviour had to change, it was done
through a supported extension point or a compile definition rather than by editing vendored
source. Two submodules are the exception, and both changes are **permanent bug fixes**.

> **Two submodules carry local patches**, all fixing real defects rather than instrumenting
> anything.
>
> **flexPTP — three patch files.** A servo fix
> ([F59](port_review.md#f59--one-duplicated-sync-cycle-latches-the-servo-into-nan-for-ever-resolved));
> an extension point so a port that supplies its own network stack can provide the two IGMP
> group addresses, added by the ThreadX/NetX Duo port; and a fix for the CMSIS-RTOS2 binding
> enqueueing heartbeat events with `osWaitForever` from ThreadX's timer thread, which that
> thread is not allowed to block in — so every heartbeat was silently dropped and the BMCA
> never left `INITIALIZING` ([F80](port_review.md#f80--flexptps-heartbeat-cannot-be-enqueued-from-threadxs-timer-thread-resolved)).
>
> **embfmt — one patch file, four defects.** `%ll` conversions dropped and NaN rendered as
> `0.0000` ([F60](port_review.md#f60--embfmt-drops-ll-conversions-and-renders-nan-as-00000-resolved)),
> plus small `%e` values swallowed by a pre-normalisation rounding constant and a `%f`/`%e`
> rounding carry that subtracted 10 rather than 10^precision
> ([F66](port_review.md#f66--embfmt-misreports-small-e-values-and-any-rounding-carry-resolved)).
> Two findings, four defects, one patch file — the three counts that have been confused for
> each other.
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
> (F60), or the clock never syncs at all because no master is ever selected (F80). Before that
> check existed, a fresh clone built exactly that firmware with no warning.
>
> The temporary F42/F61 instrumentation patch that used to sit alongside them was **dropped**
> once both findings were resolved. `patches/flexptp/README.md` records how to recover it.

| Component | Version | How it is vendored | Modified? |
|---|---|---|---|
| **stm32h5xx-hal-driver** | `v1.7.0-8-g0fff6e2` | git submodule | **As-is.** Configured by `Inc/stm32h5xx_hal_conf.h`; 12 of 61 modules enabled |
| **CMSIS Core (M)** | 5.6 (= CMSIS 5.9.0 content) | in-tree, `Drivers/CMSIS/Include` | **As-is, and already current for CMSIS 5.** Inherited from the H743 project, but verified byte-identical to the final CMSIS 5 release — see the note below. Not a submodule |
| **CMSIS Device STM32H5xx** | 1.7.0 | in-tree, `Drivers/CMSIS/Device` | **As-is.** From `STMicroelectronics/cmsis_device_h5` |
| **CMSIS-RTOS2 API header** | ARM | in-tree, `Drivers/CMSIS/CMSIS_RTOS_V2/cmsis_os2.h` | **As-is.** The header is RTOS-agnostic and survived the kernel swap unchanged. ST's FreeRTOS-backed `cmsis_os2.c`, `freertos_os2.h` and `freertos_mpool.h` were removed with FreeRTOS in S3 |
| **CMSIS-RTOS2 implementation** | [`stm32-mw-cmsis-rtos-tx`](https://github.com/STMicroelectronics/stm32-mw-cmsis-rtos-tx) `v1.4.1` | git submodule | **As-is**, and the one non-MIT component in the tree (SLA0044). No MIT equivalent exists — Eclipse ThreadX ships FreeRTOS, OSEK and POSIX layers but not CMSIS-RTOS2 |
| **ThreadX** | [`eclipse-threadx/threadx`](https://github.com/eclipse-threadx/threadx) `44d7c95`, one commit past `v6.5.1.202602a_rel` (that commit is `SECURITY.md` only) | git submodule | **As-is** (MIT). Configured by `Inc/tx_user.h`. `tx_initialize_low_level.S` is **ours** — Eclipse keeps it in `example_build/` because it is board-specific; see `Startup/tx_initialize_low_level.S` for what changed and what was deliberately left out |
| **NetX Duo** | [`eclipse-threadx/netxduo`](https://github.com/eclipse-threadx/netxduo) `473d192`, two commits past `v6.5.1.202602_rel` (both security-policy docs) | git submodule | **As-is** (MIT). Configured by `Inc/nx_user.h`. Eclipse upstream rather than ST's fork, which is pinned at 6.4.0 and missing published CVE fixes on the IPv4/DHCP receive path this firmware runs — see [threadx_netxduo_feasibility.md](threadx_netxduo_feasibility.md#choosing-the-supply-chain). **`NX_ENABLE_VLAN` is mandatory**: the whole `nx_link` layer, which is the PTP timestamp path, is behind it |
| **lwIP** | — | **removed in S3** | Replaced by NetX Duo. The `LWIP_PBUF_CUSTOM_DATA` injection it needed is gone with it: NetX Duo carries timestamps in its own API |
| **flexPTP** | `76aaf67` (`RC1.0-11`) | git submodule | **Patched — three files now.** A servo fix (F59, `0002-…`), a generic network-stack extension point (`0003-…`), and a non-blocking heartbeat enqueue ([F80](port_review.md#f80--flexptps-heartbeat-cannot-be-enqueued-from-threadxs-timer-thread-resolved), `0004-…`). Plus two external workarounds. (1) `__PRI64_PREFIX=__INT64` compile definition, because flexPTP uses a glibc-internal macro newlib lacks (F47). (2) The STM32H5 hardware port is supplied by this project, since flexPTP ships none |
| **embfmt** | `670a349` | git submodule | **Patched.** Formatted-print backend for `MSG()`. `%ll` conversions were dropped entirely and NaN printed as `0.0000`; see [F60](port_review.md#f60--embfmt-drops-ll-conversions-and-renders-nan-as-00000-resolved) and `patches/embfmt/` |
| **EtherLib** | `ec35be5` | git submodule | **Present but not compiled.** Alternative to NetX Duo, selectable via the `ETH_STACK` CMake variable. See [design_doc.md](design_doc.md#etherlib-porting-guide) |

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
| **EthDrv** | `Drivers/EthDrv/` | Register-level Ethernet MAC driver (`mac_drv.c`), PHY driver, and the NetX Duo glue (`eth_drv_netxduo.c`) |
| **flexPTP NSD** | `Src/flexptp_nsd/` | This project's network-stack driver for flexPTP, supplied through `FLEXPTP_NSD_SRC` rather than using flexPTP's bundled one. Three of its units are C++ and host-tested |
| **blocking_io** | `Modules/blocking_io/` | Blocking FIFO behind the console |
| **flexPTP HW port** | `Src/flexptp_port/` | STM32H563 hardware port for flexPTP (Stage 14) |

`Modules/lwip_port/` — the `sys_arch` layer that mapped lwIP onto FreeRTOS — was deleted in S3
along with both of the things it bridged.

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
        │ standard_output│   │    flexPTP     │   │   NetX Duo    │
        │  + blocking_io │   │  + Kalman servo│   │  (nx_link)    │
        └───────┬────────┘   └───┬────────┬───┘   └───────┬───────┘
                │                │        │               │
                │        ┌───────▼──┐  ┌──▼─────────────┐ │
                │        │ HW port  │  │ Src/flexptp_nsd│ │  flexPTP ports
                │        │ (H563)   │  │ (THIS project) │ │
                │        └───────┬──┘  └──┬─────────────┘ │
                │                │        │               │
        ┌───────▼────────────────▼────────▼───────────────▼───────┐
        │  EthDrv:  mac_drv.c · phy_drv · eth_drv_netxduo.c       │  project drivers
        └─────────────────────────┬───────────────────────────────┘
                                  │
        ┌─────────────────────────▼───────────────────────────────┐
        │      STM32H5 HAL  ·  CMSIS-RTOS2  ·  Eclipse ThreadX    │  vendor
        └─────────────────────────┬───────────────────────────────┘
                                  │
        ┌─────────────────────────▼───────────────────────────────┐
        │              CMSIS Core  ·  STM32H563 hardware          │
        └─────────────────────────────────────────────────────────┘
```

The NSD box is the one that moved. Under lwIP this project used flexPTP's **bundled**
`nsd_lwip.c`; under NetX Duo it supplies its own through flexPTP's `FLEXPTP_NSD_SRC`
extension point, because flexPTP ships no NetX Duo driver. That is why
`Src/flexptp_nsd/` exists and why three of its units were extracted into testable,
dependency-free C++ — see [testing_architecture.md](testing_architecture.md).

Two boundaries are worth calling out:

- **`mac_drv.c` bypasses the HAL ETH driver.** It programs the MAC and its DMA descriptor
  rings directly. This is what makes hardware timestamping reachable; `stm32h5xx_hal_eth.c`
  is compiled into the HAL library but never used.
- **`flexptp_options.h` is the whole flexPTP configuration surface.** It selects the OS
  binding, the clock interface, the servo, and the print/CLI macros, and it is where the
  hardware port is wired in. flexPTP has no other configuration input.
- **`ptp_tx_timestamp.c` exists to break a call chain, not to add a feature.** flexPTP's
  CMSIS-RTOS2 binding blocks when enqueueing a transmit timestamp, and the MAC delivers that
  timestamp from `ETH_IRQHandler`. The file defers the call to a thread. See
  [F80](port_review.md#f80--flexptps-heartbeat-cannot-be-enqueued-from-threadxs-timer-thread-resolved)
  for the same mistake on the heartbeat path.

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
       ├─ serial_io_init()          USART3 + GPDMA1_Ch0 ONLY -- no RTOS objects yet
       ├─ osKernelInitialize()      creates the two ThreadX byte pools
       ├─ MSG_init()                }  everything that allocates comes after the
       ├─ serial_io_start()         }  pools exist: FIFO, event flags, "serialio"
       ├─ tx_thread_stack_error_notify(app_stack_error_handler)
       ├─ osThreadNew(task_startup) → "init"
       └─ osKernelStart()           tx_kernel_enter(); does not return

task_startup ("init")
  ├─ cli_init()                     starts "cli"
  ├─ print_welcome_message()
  ├─ init_ethernet()                NetX Duo: pool, IP instance → "eth ip",
  │                                 ARP/ICMP/UDP/IGMP, DHCP client, driver → "rx", "phy"
  ├─ cmd_init()                     registers CLI commands
  └─ loop { ethernet_poll_dhcp(); osDelay(1000) }

"flexptp" CLI command → reg_task_ptp() → creates the "ptp" thread on demand
  └─ flexptp_user_event_cb(PTP_UEV_INIT_DONE)      Src/main.c
       ├─ ptp_load_profile(FLEXPTP_INITIAL_PROFILE)  "default" — see ptp_profiles.md
       └─ ptp_log_enable(PTP_LOG_DEF | PTP_LOG_BMCA)
```

**The split of `serial_io_init()` is load-bearing, not tidiness.** ST's CMSIS-RTOS2 wrapper
creates its two byte pools inside `osKernelInitialize()`, so nothing can be allocated before
that call. Configuring USART3 first is what lets `serial_io_panic_write()` report a failure in
the second half — which it did, the first time the kernel was swapped in.

PTP is **not** started at boot. The stack comes up only when the operator types `flexptp`
at the CLI, which is why `is_flexPTP_operating()` gates that command.

The profile is loaded from the `PTP_UEV_INIT_DONE` user-event callback, not from `main()`.
There is no settings store, so a profile changed at runtime with `ptp profile preset …` is
lost on the next reset and the compile-time preset is reinstated — see
[ptp_profiles.md](ptp_profiles.md).

---

## Threads

Two priority scales are in play and they run in opposite directions, which is the single
easiest thing to misread in this firmware. **ThreadX counts 0 as most urgent**; CMSIS-RTOS2
counts `osPriorityISR` (56) as most urgent. ST's wrapper maps one to the other as
`tx = osPriorityISR - cmsis`, so `osPriorityNormal` (24) becomes ThreadX **32**. Both numbers
are given below, and `osinfo` prints both for the same reason.

| Thread | ThreadX pri | CMSIS | Stack | Created by | Role |
|---|---|---|---|---|---|
| `System Timer Thread` | **0** | 56 | 1024 B | ThreadX | Software timers; flexPTP's heartbeat runs here |
| `NetX DHCP Client` | **3** | 53 | — | `nx_dhcp_create()` | DHCP state machine |
| `eth ip` | **16** | 40 | 2048 B | `nx_ip_create()` | NetX Duo core; all stack processing is serialised here |
| `rx` | **24** | 32 | 2048 B | `eth_drv_netxduo.c` | Drains the Ethernet RX ring into NetX Duo |
| `ptpts` | **24** | 32 | 1024 B | `ptp_tx_timestamp.c` | Carries transmit timestamps out of the ETH ISR into flexPTP |
| `ptp` | **32** | 24 | 2048 B | `reg_task_ptp()`, on demand | flexPTP core: BMCA, servo, message handling |
| `init` | **32** | 24 | 2048 B | `main()` | One-shot bring-up, then polls DHCP at 1 Hz |
| `cli` | **32** | 24 | 2048 B | `cli_init()` | Console command parsing |
| `serialio` | **32** | 24 | 2048 B | `serial_io_start()` | FIFO → UART DMA; the only writer to USART3 |
| `phy` | **32** | 24 | 1024 B | `eth_drv_netxduo.c` | PHY link polling; also drains the DMA error latch |

Notes:

- **Every thread now states its priority explicitly.** Under FreeRTOS, five of them left
  `attr.priority` at zero and the shim substituted `osPriorityNormal`. ST's ThreadX wrapper
  rejects `osPriorityNone` (0) instead, because it is below `osPriorityIdle` (1), and
  `osThreadNew()` returns NULL. The board booted, printed its banner, and halted.
- **`eth ip` holds the position `tcpip_thread` held**, not the number: highest of the
  application threads, so the stack stays ahead of everything that feeds it.
- **`rx` and `ptpts` sit above the console** at ThreadX 24. For `rx` the reason is unchanged
  from the lwIP driver — every descriptor it has not restored is one the DMA cannot reuse, and
  F61's `inFlight` marker makes that window safe rather than short. For `ptpts` it is latency
  to the servo, not correctness: the timestamp was captured in hardware long before.
- **Memory**: two ThreadX byte pools carved from the RAM above `__RAM_segment_used_end__` —
  24 KB for stacks, the rest for objects. `osinfo` reports both. There is no heap; the
  FreeRTOS `ucHeap` and its `.FreeRTOSHeapSection` are gone.

### RX depths, and which one is the limit

Three numbers govern the receive path, and they are not interchangeable:

| Constant | Value | Role |
|---|---|---|
| `ETH_RX_DESC_CNT` | 24 | descriptors the MAC's DMA can fill before it stalls — a *latch*, not a pause: the engine raises `RBU`, suspends, and only a receive poll demand restarts it (F71) |
| `RX_MSG_Q_LEN` | = `ETH_RX_DESC_CNT` | ISR → `rx_thread` queue depth |
| `NX_PACKET_POOL_PACKET_COUNT` | 8 | frames NetX Duo can hold at once |

`RX_MSG_Q_LEN` is tied to the ring rather than set independently: `ETHHW_ProcessRx()` marks a
descriptor `inFlight` before queueing it and does not revisit it until `rx_thread` restores
it, so one descriptor can never contribute two entries. It was previously 32 — above that
ceiling, so its top third was unreachable.

**The limit is the message queue, not the packet pool** — which is the opposite of what this
section said under lwIP, and the opposite of what the packet-pool comment in `Inc/nx_user.h`
implies. Measured:

| Load | Result |
|---|---|
| 300 pings of 1400 B at 100 / 250 / 500 pkt/s | 0% loss |
| 53 000 UDP frames of 1400 B in 6 s (line rate) | 52 141 delivered, **0 dropped** |
| Same flood with the pool cut from 8 to **2** | **0 dropped** |

The pool never starves because `rx_thread` allocates, copies and releases within one queue
entry, so only one or two packets are ever outstanding from the driver's side. Cutting the
pool to a quarter of its size changed nothing.

What that leaves is `ETHHW_ReadCallback()`, which drops the event when `rx_msg_q` is full —
**silently, and without a counter**. That is the drop path that would actually bite under
sustained overload, and it is the one place in the receive path with no instrumentation.
`drv_rx_dropped_nopacket` was added after an external review found it declared, printed and
never incremented; it is correct now, but the experiments above could not make it fire.

So: `NX_PACKET_POOL_PACKET_COUNT` is deliberately left at 8, and if drops ever do appear the
first thing to instrument is the queue, not the pool.

---

## Interrupts

`__NVIC_PRIO_BITS` is 4 → 16 priority levels, 0 (highest) to 15 (lowest).

| Interrupt | IRQn | Vector idx | Priority | Handler | Defined in |
|---|---|---|---|---|---|
| SVC | −5 | 11 | 0 | `SVC_Handler` | unused — resolves to `Default_Handler` |
| PendSV | −2 | 14 | 15 (set by port) | `PendSV_Handler` | ThreadX `tx_thread_schedule.S` |
| SysTick | −1 | 15 | 15 (`TICK_INT_PRIORITY`) | `SysTick_Handler` | `Startup/tx_initialize_low_level.S` — **ours** |
| **ETH** | 106 | 122 | **7** | `ETH_IRQHandler` | `eth_drv_netxduo.c` |
| **USART3** | 60 | 76 | **12** | `USART3_IRQHandler` | `serial_io.c` |
| **GPDMA1 Ch0** | 27 | 43 | **12** | `GPDMA1_Channel0_IRQHandler` | `serial_io.c` |

**`HardFault`, `MemManage`, `BusFault` and `UsageFault` report and halt** — they are naked
handlers in `Src/main.c` that recover the exception frame, print `CFSR`, `HFSR`, the faulting
`PC` and `LR` through the polled UART writer, and decode the causes this port has actually
hit. Every other vector still resolves to `Default_Handler`, an infinite loop.

That matters more on this part than it looks. ThreadX's Cortex-M33 port programs `PSPLIM` on
every context switch (`tx_thread_schedule.S`), so **a thread stack overflow is trapped by
hardware** (`UFSR.STKOF`) on the offending push. This was equally true under FreeRTOS and for
the same reason — it is a property of the architecture, not of the kernel.

The software check is therefore the backstop, not the primary: `TX_ENABLE_STACK_CHECKING` is
defined in `Inc/tx_user.h` and `app_stack_error_handler()` is registered with
`tx_thread_stack_error_notify()` in `main()`, but it only catches what the hardware cannot —
a stack overrun *and then unwound* before the next context switch. In the ordinary case
`UsageFault_Handler` gets there first. Measured:

| Failure | Reported |
|---|---|
| Task stack overflow | `CFSR` = `0x00100000` → *UFSR.STKOF — stack overflow (PSPLIM)* |
| Call through a NULL pointer | `CFSR` = `0x00000100`, `PC` = `0x00000000` → *BFSR.IBUSERR* |

### Interrupt priorities: the FreeRTOS constraint no longer applies

Under FreeRTOS this section described a hard rule: `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`
was 5, interrupts at 0–4 were never masked by `taskENTER_CRITICAL()` and **must not** call any
`*FromISR` API, and 5–15 could. **That rule is gone.**

ThreadX's Cortex-M port uses **PRIMASK**, not BASEPRI, for its critical sections
(`ports/cortex_m33/gnu/inc/tx_port.h`). So:

- there is no priority ceiling above which the kernel must not be called — **any** ISR may use
  ThreadX services;
- and conversely a kernel critical section masks **everything**, including an interrupt that
  FreeRTOS would have left running.

Nothing regresses here, because all three of this project's interrupts already sat at 7 and 12
— below the old masking level — so both kernels mask them identically. The ordering itself is
unchanged and still deliberate: **ETH (7) preempts the console (12)**, so heavy CLI output
cannot delay Ethernet servicing.

If an interrupt with hard latency requirements is ever added, define `TX_PORT_USE_BASEPRI` and
`TX_PORT_BASEPRI` together in `Inc/tx_user.h` and re-derive the priorities; the ceiling
behaviour then returns.

> **What replaced the rule is a different one, and it caught this port twice.** The constraint
> that now matters is not *which priority* may call the kernel, but *which context may block*.
> CMSIS-RTOS2 forbids a non-zero timeout from an ISR, and ThreadX additionally forbids one from
> its **timer thread** (`txe_queue_send.c` returns `TX_WAIT_ERROR` for `&_tx_timer_thread`).
> flexPTP's CMSIS-RTOS2 binding violates both — on the transmit-timestamp path and on the
> heartbeat path. See `Src/flexptp_nsd/ptp_tx_timestamp.c` and
> [F80](port_review.md#f80--flexptps-heartbeat-cannot-be-enqueued-from-threadxs-timer-thread-resolved).

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
likely to break silently — F48 (a signature drift that failed at compile time) and F42 (which
took days precisely because the fault turned out to be outside the board entirely).

**Receive:**

```
MAC captures RX timestamp into a context descriptor
  → ETHHW_ProcessRx() [ISR] pairs the data descriptor with its context descriptor,
      marks both ETHHW_DescExt.inFlight, and queues the event on rx_msg_q
  → rx_thread [task] copies the frame into an NX_PACKET **at data_start + 2**, and calls
      nx_link_ethernet_packet_received(ip, iface, packet, &NX_LINK_TIME)
  → NetX Duo stashes the timestamp in the first three ULONGs of the packet buffer,
      over the Ethernet header it has just consumed, and routes the packet up
  → nsd_netxduo.c reads it back from nx_packet_data_start and calls ptp_receive_enqueue()
```

**"Pairs" is doing real work in that first line.** `RDES1.TSA` on a data descriptor promises that
a context descriptor *follows*; it does not promise the DMA has written it, and the engine may
release the data descriptor first. So the pairing is three tests, not an assumption — the context
slot must be application-owned, and it must carry `RDES3.CTXT`:

| Context slot | Action |
|---|---|
| still owned by the DMA | **defer**: leave the frame unmarked and retry on the next receive interrupt, exactly as ST's HAL does. Counted as `rx ts: N deferred` |
| owned by us, `CTXT` set | pair them; this is the ordinary case |
| owned by us, `CTXT` clear | deliver the frame **unstamped**; counted as `rx ts: N stray` |

Reading the slot unchecked is not a loud failure, which is why it is worth the three tests: an
armed descriptor holds `DES0 = bufAddr`, an SRAM3 address and therefore a plausible sub-second
nanosecond count. Finding NX-01; the same shape as F59. Both counters read zero over 603 frames
of a full `S8` run.

**The 2-byte offset is not cosmetic.** `NX_PHYSICAL_HEADER` is **16**, not 14, precisely so a
14-byte Ethernet header leaves the IP header 4-byte aligned. Copy the frame to `prepend_ptr`
directly and every IPv4 packet fails its header checksum while ARP — which has none — works
perfectly. That asymmetry is the signature, and it cost a bench session.

On the 802.3 path there is no stashing at all: `nx_link_packet_receive_callback_add()` passes
the `NX_LINK_TIME` to the callback as an argument.

**Transmit:**

```
nsd_netxduo.c sets NX_INTERFACE_CAPABILITY_PTP_TIMESTAMP on the packet and records
    (NX_PACKET * → flexPTP uid) in eth_drv_netxduo.c's pending table
  → eth_drv_netxduo.c sees the flag, takes the uid, and asks the MAC for a timestamp
    via ETHHW_TXOPT_CAPTURE_TS
  → MAC captures the TX timestamp on transmission
  → ETHHW_ProcessTx(), in ETH_IRQHandler, invokes ptp_tx_timestamp_from_isr()
  → that posts to a queue with timeout 0 and the "ptpts" thread calls
    ptp_transmit_timestamp_cb() from thread context
```

**Two things replaced what lwIP needed.** The capability flag is a real `NX_PACKET` field, so
the four `LWIP_PBUF_CUSTOM_DATA` injections are gone — and with them the unversioned `tx_cb`
signature that drifted in F48. But NX_PACKET has no field for the uid, so that association
lives in a side table keyed on the packet address, **taken rather than read** when consumed:
NetX Duo recycles packets, and a stale entry would hand the servo a wrong uid silently.

The final hop is the one that is not optional. flexPTP's CMSIS-RTOS2 binding enqueues with
`osWaitForever`, and `ETHHW_ProcessTx()` runs in interrupt context, where that is rejected
outright. Without the deferral every `t3` is discarded in silence and the servo converges on a
lie — which is why `tools/hil/` checks `ptp_ts` **before** `ptp_sync`.

Because timestamps are captured **in hardware**, PTP accuracy does not depend on thread
scheduling or interrupt latency. Latency affects how quickly the servo *reacts*, not how
accurate its inputs are.

---

## Memory Layout

Regions from `stm32h563zitx_FLASH.ld`; usage from the current Debug build.

| Region | Base | Size | Contents | Used |
|---|---|---|---|---|
| FLASH | `0x08000000` | 2 MB | `.isr_vector`, `.text`, `.rodata`, `.data` LMA | 206 344 B (9.84%) |
| SRAM1 | `0x20000000` | 256 KB | `.data`, `.bss`, main stack, C heap, **ThreadX byte pools** | 80 864 B (30.85%) |
| SRAM2 | `0x20040000` | 64 KB | *(unallocated — deliberate spare capacity)* | 0 B |
| SRAM3 | `0x20050000` | 320 KB | Ethernet DMA descriptors + buffers, **NetX Duo packet pool and IP thread stack** | 89 704 B (27.38%) |

Both smaller than the FreeRTOS/lwIP build they replace (217 160 B FLASH, 133 168 B SRAM1),
despite SRAM3 growing: lwIP's heap and the 30 KB `ucHeap` are gone, and NetX Duo's packet pool
moved to SRAM3 alongside the DMA buffers it feeds.

- `_estack` = `0x20040000`, the top of SRAM1 — confirmed as vector word 0 in the linked image.
- Ethernet DMA buffers live in SRAM3: 48 descriptors plus 48 × 1528 B of frame buffers.
- **`__RAM_segment_used_end__`** is PROVIDEd immediately after the heap/stack reservation and is
  where ThreadX starts. Everything above it becomes `_tx_initialize_unused_memory`, from which
  the CMSIS-RTOS2 wrapper carves its two byte pools. It must come **after** `._user_heap_stack`
  for the same reason the FreeRTOS heap had to come before it: `_sbrk()` hands out everything
  between `_end` and the stack reservation, so a byte pool inside that window would also be
  handed to `malloc()`.
- **No cache maintenance is performed anywhere**, and none is needed: the H563's DCACHE is
  disabled out of reset and `HAL_Init()` does not enable it. Enabling DCACHE later would
  require making the SRAM3 Ethernet region non-cacheable or adding explicit maintenance.
