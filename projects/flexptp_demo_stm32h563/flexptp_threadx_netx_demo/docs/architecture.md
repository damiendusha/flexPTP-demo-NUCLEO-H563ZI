# Architecture

How this firmware fits together: the module stack, the threads, the interrupts, and the
provenance of every third-party component.

Companion documents: [design_doc.md](design_doc.md) (porting decisions) ·
[testing_architecture.md](testing_architecture.md) (how it is tested, and what is not) ·
[ptp_clock.md](ptp_clock.md) (PTP clock derivation) ·
[ptp_profiles.md](ptp_profiles.md) (default vs gPTP presets) ·
[l4_performance.md](l4_performance.md) (measured layer 4 accuracy) ·
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
| RTOS | Eclipse ThreadX 6.5.2, `cortex_m33/gnu` port, `TX_SINGLE_MODE_NON_SECURE`, 1 kHz tick |
| RTOS API | CMSIS-RTOS2 throughout — application **and** flexPTP, over ST's `cmsis_rtos_threadx` wrapper. Memory comes from two ThreadX byte pools, not a heap |
| Network stack | Eclipse NetX Duo 6.5.2, IPv4 only, TCP and IPv6 compiled out |
| PTP | flexPTP, Kalman servo, ADDEND clock interface; timestamp clock is `pll1_q_ck` at 250 MHz |
| Console | USART3 → ST-Link VCP, 115200 8N1, TX via GPDMA |
| PHY | LAN8742A over RMII |

---

## Module Inventory

Every third-party component and its provenance. Each is a git repository fetched by Bazel and
pinned by commit in [`bazel/repositories.bzl`](../../../../bazel/repositories.bzl) — not a
submodule, and not a working tree anyone can edit. Where behaviour had to change, it was done
through a supported extension point or a compile definition rather than by editing vendored
source, with two exceptions, both carried as patches that **the fetch applies**:

> **flexPTP — ten patches**, each a fix filed upstream, or ready to be, and deleted from here as
> its pull request merges: U1 (#20), U6 (#26), U8–U10 (#34–#36), U11–U14 (#37–#40) and U15, not
> yet filed. What each fixes, how it was
> verified, and the commands to regenerate them are in
> [`patches/flexptp/README.md`](../../../../patches/flexptp/README.md); the review that found them
> is [`docs/flexptp_upstream_review.md`](../../../../docs/flexptp_upstream_review.md).
>
> **embfmt — one patch file, four defects.** `%ll` conversions dropped and NaN rendered as
> `0.0000` ([F60](port_review.md#f60--embfmt-drops-ll-conversions-and-renders-nan-as-00000-resolved)),
> plus small `%e` values swallowed by a pre-normalisation rounding constant and a `%f`/`%e`
> rounding carry that subtracted 10 rather than 10^precision
> ([F66](port_review.md#f66--embfmt-misreports-small-e-values-and-any-rounding-carry-resolved)).
> Two findings, four defects, one patch file — the three counts that have been confused for
> each other. See [patches/embfmt/README.md](../../../../patches/embfmt/README.md).
>
> **A patch cannot be skipped any more.** The patched tree is what lands in Bazel's repository
> cache, so there is no working tree that can be pristine, patched, or neither. That matters
> because omitting one never broke the build: it produced a firmware that was silently wrong — the
> servo latched to NaN on the first duplicated Sync (F59) while printing a plausible `0.0000`
> correction (F60), or the clock never synced because no master was ever selected (F80). Those
> three flexPTP fixes, the first series this project carried, were taken upstream in `RC1.0-17`:
> [PR #15](https://github.com/epagris/flexPTP/pull/15), [PR #18](https://github.com/epagris/flexPTP/pull/18)
> and commit `e873105`.

| Component | Version | How it is vendored | Modified? |
|---|---|---|---|
| **stm32h5xx-hal-driver** | `v1.7.0-13-gdeaf9d1`, upstream `main` head | Bazel git repository | **As-is.** Configured by `lib/runtime/stm32h5xx_hal_conf.h`; 12 of 61 modules enabled |
| **CMSIS Core (M)** | 5.6 (= CMSIS 5.9.0 content) | in-tree, `third_party/CMSIS/Include` | **As-is, and already current for CMSIS 5.** Inherited from the H743 project, but verified byte-identical to the final CMSIS 5 release — see the note below. Not fetched |
| **CMSIS Device STM32H5xx** | 1.7.0 | in-tree, `third_party/CMSIS/Device` | **As-is.** From `STMicroelectronics/cmsis_device_h5` |
| **CMSIS-RTOS2 API header** | ARM | in-tree, `third_party/CMSIS/CMSIS_RTOS_V2/cmsis_os2.h` | **As-is.** The header is RTOS-agnostic: the same one serves both firmwares, and it is why the CLI and console in `//projects/flexptp_demo_stm32h563/lib` name no RTOS |
| **CMSIS-RTOS2 implementation** | [`stm32-mw-cmsis-rtos-tx`](https://github.com/STMicroelectronics/stm32-mw-cmsis-rtos-tx) `v1.4.1` | Bazel git repository | **As-is**, and the one non-MIT component in the tree (SLA0044). No MIT equivalent exists — Eclipse ThreadX ships FreeRTOS, OSEK and POSIX layers but not CMSIS-RTOS2 |
| **ThreadX** | [`eclipse-threadx/threadx`](https://github.com/eclipse-threadx/threadx) `b2868c3` = `v6.5.2.202603b_rel`, upstream `master` head | Bazel git repository | **As-is** (MIT). Configured by `tx_user.h`. `tx_initialize_low_level.S` is **ours** — Eclipse keeps it in `example_build/` because it is board-specific; see `startup/tx_initialize_low_level.S` for what changed and what was deliberately left out |
| **NetX Duo** | [`eclipse-threadx/netxduo`](https://github.com/eclipse-threadx/netxduo) `c690f59` = `v6.5.2.202603b_rel`, upstream `master` head | Bazel git repository | **As-is** (MIT). Configured by `nx_user.h`. Eclipse upstream rather than ST's fork, which is pinned at 6.4.0 and missing published CVE fixes on the IPv4/DHCP receive path this firmware runs — see [threadx_netxduo_feasibility.md](threadx_netxduo_feasibility.md#choosing-the-supply-chain). **`NX_ENABLE_VLAN` is mandatory**: the whole `nx_link` layer, which is the PTP timestamp path, is behind it |
| **lwIP** | — | not in this firmware | Removed from it in S3 and replaced by NetX Duo, which carries timestamps in its own API. lwIP is now the network stack of the FreeRTOS firmware |
| **flexPTP** | `fe4b071` (`RC1.0-26`), upstream `master` head | Bazel git repository, **nine patches** | **Patched** — see above. Plus two external workarounds: (1) the `__PRI64_PREFIX=__INT64` compile definition (`bazel/flexptp.bzl`), because flexPTP uses a glibc-internal macro newlib lacks (F47); (2) the STM32H5 hardware port is supplied by this project (`lib/ptp/ptp_port_stm32h563.c`), since flexPTP ships none |
| **embfmt** | `670a349`, upstream head | Bazel git repository, one patch | **Patched.** Formatted-print backend for `MSG()`. `%ll` conversions were dropped entirely and NaN printed as `0.0000`; see [F60](port_review.md#f60--embfmt-drops-ll-conversions-and-renders-nan-as-00000-resolved) and `patches/embfmt/` |
| **EtherLib** | — | **removed** | Was an alternative network stack, compiled but never run on this port; removed in the Bazel migration (decision D9) |

> **"CMSIS Core (M) 5.6" is not a stale version.** `5.6` is the **Core(M)** version from
> `cmsis_version.h`, which is versioned separately from the CMSIS *pack*. CMSIS 5.9.0 — the
> final CMSIS 5 release — also reports Core(M) 5.6, so the number says nothing about age.
>
> Checked rather than assumed: all **30 files** in `third_party/CMSIS/Include` are byte-identical
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

Project-owned code that would otherwise look like a third-party module:

| Component | Location | Role |
|---|---|---|
| **MAC and PHY drivers** | `lib/eth/` | Register-level Ethernet MAC driver (`mac_drv.c`) and PHY driver, shared by both firmwares |
| **NetX Duo glue** | `eth/` | `eth_drv_netxduo.c` (the link driver) and `ethernet_netxduo.c` (pool, IP instance, DHCP) |
| **flexPTP NSD** | `ptp/` and `lib/ptp/` | This project's network-stack driver for flexPTP (`ptp/nsd_netxduo.c`), used instead of flexPTP's bundled ones. Its four logic units are C++, in `lib/ptp/`, and host-tested |
| **flexPTP HW port** | `lib/ptp/ptp_port_stm32h563.c` | STM32H563 hardware port for flexPTP (Stage 14) |
| **CLI, console, FIFO** | `lib/cli/`, `lib/io/`, `lib/blocking_io/` | The command line, `MSG()` and the USART3/GPDMA console, the blocking FIFO behind it |
| **Runtime and startup** | `lib/runtime/`, `lib/startup/` | syscalls, the heap, C++ support, `SystemInit()`, the reset vector, the linker script, the HAL configuration |

`lib/` is `//projects/flexptp_demo_stm32h563/lib`, shared with the FreeRTOS firmware; everything
else above is in this project's directory.

The `sys_arch` layer that maps lwIP onto FreeRTOS left this firmware in S3 with both of the things
it bridged; it lives on in the FreeRTOS firmware, as `lwip_port/`.

> **Every pin is at its upstream head** as of November 2026, each bump verified on hardware —
> see [`docs/flexptp_upstream_review.md`](../../../../docs/flexptp_upstream_review.md), §7. The
> original pins were already ahead of the NUCLEO-H743ZI reference project, which is the root cause
> of findings F47–F49.

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
                │        │ HW port  │  │ ptp/, lib/ptp  │ │  flexPTP ports
                │        │ (H563)   │  │ (THIS project) │ │
                │        └───────┬──┘  └──┬─────────────┘ │
                │                │        │               │
        ┌───────▼────────────────▼────────▼───────────────▼───────┐
        │  lib/eth: mac_drv.c · phy_drv  ·  eth/eth_drv_netxduo.c │  project drivers
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
extension point, because flexPTP ships no NetX Duo driver. That is why `ptp/nsd_netxduo.c`
exists and why four of its units were extracted into testable, dependency-free C++ in `lib/ptp/`
— see [testing_architecture.md](testing_architecture.md).

Two boundaries are worth calling out:

- **`mac_drv.c` bypasses the HAL ETH driver.** It programs the MAC and its DMA descriptor
  rings directly. This is what makes hardware timestamping reachable; `stm32h5xx_hal_eth.c`
  is compiled into the HAL library but never used.
- **`flexptp_options.h` is the whole flexPTP configuration surface.** It selects the OS
  binding, the clock interface, the servo, and the print/CLI macros, and it is where the
  hardware port is wired in. flexPTP has no other configuration input.
- **`ethinfo` is composed by the application, not by the driver.** It prints three layers —
  the MAC driver's counters, the NSD's, and flexPTP's core state — and `cmds.c` calls all
  three in order. The driver deliberately does **not** reach up into the NSD: it used to, through
  four hand-written `extern` declarations inside `eth_drv_report()`'s body, which is four
  chances for the signature drift that is F48.
- **`ptp_tx_timestamp.c` exists to break a call chain, not to add a feature.** flexPTP's
  CMSIS-RTOS2 binding blocks when enqueueing a transmit timestamp, and the MAC delivers that
  timestamp from `ETH_IRQHandler`. The file defers the call to a thread. See
  [F80](port_review.md#f80--flexptps-heartbeat-cannot-be-enqueued-from-threadxs-timer-thread-resolved)
  for the same mistake on the heartbeat path.

---

## Boot Sequence

```
Reset_Handler                       lib/startup/startup_stm32h563zitx.s
  ├─ set SP = _estack (0x20040000)
  ├─ SystemInit()                   lib/runtime/system_stm32h5xx.c — VTOR
  ├─ copy .data, zero .bss
  ├─ __libc_init_array()
  └─ main()                         main.c
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
  └─ flexptp_user_event_cb(PTP_UEV_INIT_DONE)      main.c
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

**That is a decision, not an omission.** Every reset returns the board to a state the source
describes, which is what you want from a demo and from a bench instrument: nothing accumulates
invisibly across sessions, and "it behaves differently from your board" is never explained by
settings someone typed a month ago. If persistence is ever wanted, the shape is one flash
sector, written only on an explicit `ptp save`, with a magic-and-version header and a fallback
to the compile-time preset on any mismatch — never written implicitly by a `ptp profile preset`,
which would reintroduce exactly the invisible state this avoids.

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

**This table is asserted, not just written down**: `osinfo` enumerates the threads with their
priorities on both scales, and the `threads` check in `tools/hil/` compares every one of them
against the values above. Before that it checked only that `serialio`, `init` and `cli` sat at
32 — the part of the table with no load-bearing relationship in it (finding TS-05).

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

### Scheduling: equal priorities do round-robin, at 4 ms

Four threads sit at ThreadX 32 — `init`, `cli`, `serialio` and `ptp` — so how they share the CPU
is a real question and the answer changed with the kernel. ST's CMSIS-RTOS2 wrapper passes
`RTOS2_DEFAULT_TIME_SLICE` (4) to every `tx_thread_create()`, so equal-priority threads
round-robin every **4 ticks = 4 ms**. Under FreeRTOS with `configUSE_TIME_SLICING` they
round-robined every tick.

Nothing here depends on it: every one of those four blocks — on a queue, an event flag, a FIFO
or `osDelay()` — so none can starve another. Recorded because it is a behavioural difference the
kernel swap introduced silently, and because `RTOS2_DEFAULT_TIME_SLICE` is overridable as a define on the wrapper's target in
`bazel/cmsis_rtos_threadx.BUILD` if a future thread ever does spin.

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
section said under lwIP, and the opposite of what the packet-pool comment in `nx_user.h`
implies. Measured:

| Load | Result |
|---|---|
| 300 pings of 1400 B at 100 / 250 / 500 pkt/s | 0% loss |
| 53 000 UDP frames of 1400 B in 6 s (line rate) | 52 141 delivered, **0 dropped** |
| Same flood with the pool cut from 8 to **2** | **0 dropped** |

The pool never starves because `rx_thread` allocates, copies and releases within one queue
entry, so only one or two packets are ever outstanding from the driver's side. Cutting the
pool to a quarter of its size changed nothing.

What that leaves is `ETHHW_ReadCallback()`, which drops the event when `rx_msg_q` is full.
**That path is now counted too** (`rx: … N dropped (queue full)`), so the receive path has no
silent drop left in it.

Measured after instrumenting it: **70 600 UDP frames of 1 400 B in 8 s**, 69 595 delivered,
**0 queue drops** — `rx_thread` at ThreadX 24 keeps up with the DMA at line rate. So all three
receive drop counters are now instrumented and all three are unexercised, which is the honest
state: the receive path has no *observed* bottleneck, and if one ever appears it will say which
of the three it is instead of leaving the reader to guess.

`NX_PACKET_POOL_PACKET_COUNT` is deliberately left at 8.

### There is no watchdog, and every failure path halts

Deliberate, and worth stating because the alternative is assumed. `Error_Handler()`,
`app_stack_error_handler()`, `fault_report()`, `_sbrk()`'s exhaustion path and both
`serial_io_start()` failures all end in `for (;;)` with a message on the console. No IWDG is
configured anywhere.

For a bench demo that is the right trade: this firmware's whole thesis is that a failure should
be *loud*, and an auto-rebooting board is the opposite — it destroys the console output that
says what happened and presents as an intermittent fault. Halting with a decoded `CFSR`, a
faulting `PC` and a sentence naming the constant to raise is worth more than uptime here.

**It would be the wrong trade in a product**, and the change is not large: enable the IWDG in
`main()`, refresh it from the `serialio` thread (the lowest-priority thread that must keep
running for the console to work, so a refresh proves more than a timer callback would), and
report the reset cause from `RCC_RSR` at boot so a watchdog reset is distinguishable from a
power cycle.

---

## Interrupts

`__NVIC_PRIO_BITS` is 4 → 16 priority levels, 0 (highest) to 15 (lowest).

| Interrupt | IRQn | Vector idx | Priority | Handler | Defined in |
|---|---|---|---|---|---|
| MemManage | −12 | 4 | **0** | `MemManage_Handler` | `main.c` — reports and halts |
| BusFault | −11 | 5 | **0** | `BusFault_Handler` | `main.c` — reports and halts |
| UsageFault | −10 | 6 | **0** | `UsageFault_Handler` | `main.c` — reports and halts |
| SVC | −5 | 11 | **15** | `SVC_Handler` | unused — resolves to `Default_Handler` |
| PendSV | −2 | 14 | **15** | `PendSV_Handler` | ThreadX `tx_thread_schedule.S` |
| SysTick | −1 | 15 | **4** | `SysTick_Handler` | `startup/tx_initialize_low_level.S` — **ours** |
| **ETH** | 106 | 122 | **7** | `ETH_IRQHandler` | `eth_drv_netxduo.c` |
| **USART3** | 60 | 76 | **12** | `USART3_IRQHandler` | `serial_io.c` |
| **GPDMA1 Ch0** | 27 | 43 | **12** | `GPDMA1_Channel0_IRQHandler` | `serial_io.c` |

**These are read back from the registers by `osinfo` and asserted by the `irq_prio` check in
`tools/hil/`**, which is new, and which exists because two rows of this table were wrong for the
life of the port (finding NX-04). `SysTick` in particular is **not** `TICK_INT_PRIORITY`:
`HAL_Init()` sets that (15) and `_tx_initialize_low_level` then overrides it, so the header that
looks like the setting is not the setting. A priority set in three places by three different
components is exactly the sort of thing that must be checked rather than documented.

**Why SysTick sits above Ethernet.** It is a real change from the FreeRTOS build, where the tick
was at 15 below both peripherals, and it is deliberate: the tick drives the timer thread, which
is where flexPTP's heartbeat runs, and F80 is the finding about that heartbeat not arriving.
Nothing depends on the old ordering — ThreadX masks with PRIMASK rather than BASEPRI so there is
no ceiling for kernel calls, and PendSV at 15 cannot preempt the ETH ISR whatever the tick does.
The handler itself is `HAL_IncTick` plus `_tx_timer_interrupt`.

**`HardFault`, `MemManage`, `BusFault` and `UsageFault` report and halt** — they are naked
handlers in `main.c` that recover the exception frame, print `CFSR`, `HFSR`, the faulting
`PC` and `LR` through the polled UART writer, and decode the causes this port has actually
hit. Every other vector still resolves to `Default_Handler`, an infinite loop.

That matters more on this part than it looks. ThreadX's Cortex-M33 port programs `PSPLIM` on
every context switch (`tx_thread_schedule.S`), so **a thread stack overflow is trapped by
hardware** (`UFSR.STKOF`) on the offending push. This was equally true under FreeRTOS and for
the same reason — it is a property of the architecture, not of the kernel.

The software check is therefore the backstop, not the primary: `TX_ENABLE_STACK_CHECKING` is
defined in `tx_user.h` **in Debug only**, and `app_stack_error_handler()` is registered with
`tx_thread_stack_error_notify()` in `main()` unconditionally. It only catches what the hardware
cannot — a stack overrun *and then unwound* before the next context switch. In the ordinary case
`UsageFault_Handler` gets there first, in both configurations. Measured:

| Failure | Reported |
|---|---|
| Task stack overflow | `CFSR` = `0x00100000` → *UFSR.STKOF — stack overflow (PSPLIM)* |
| Call through a NULL pointer | `CFSR` = `0x00000100`, `PC` = `0x00000000` → *BFSR.IBUSERR* |

### Debug and Release differ in more than optimisation level

Three switches are conditional on `DEBUG`, each individually reasoned where it is defined, and
worth collecting in one place because together they mean the two images do not behave
identically at the API boundary. `DEBUG` is defined by `--config=dbg` (in `toolchain/BUILD.bazel`,
for every compile action, so NetX Duo and the application agree on `NX_PACKET`'s layout); plain
`bazel build` is fastbuild, `-O0` *without* it, and is neither configuration:

| Switch | Debug | Release | Consequence |
|---|---|---|---|
| `TX_ENABLE_STACK_CHECKING` (`tx_user.h`) | on | **off** | Release has no software stack check. The PSPLIM hardware trap is unaffected and is the primary mechanism in both |
| `NX_DISABLE_ERROR_CHECKING` (`nx_user.h`) | off | **on** | Release calls NetX Duo's `_nx_*` internals directly. A misuse that returns `NX_PTR_ERROR` in Debug is undefined behaviour in Release — this is what turned [NX-02](review_v1_opus_5p0.md) from a diagnosable error into a corrupted socket list |
| `NX_ENABLE_PACKET_DEBUG_INFO` (`nx_user.h`) | on | off | 12 bytes per `NX_PACKET`; owner/file/line for a packet that never came back |

`TX_DISABLE_ERROR_CHECKING` is **not** defined in either: ST's CMSIS-RTOS2 wrapper `#error`s on
it, because it translates ThreadX's return codes into `osStatus_t`.

Both configurations are hardware-tested, every time: the verification matrix runs S8 and every
scenario on each ([testing_architecture.md](testing_architecture.md#build-configurations-what-debug-and-release-mean)).
The point of the table is that "Release is Debug with optimisation" is not true here, and the
difference is on the error paths — which are the paths least likely to be exercised before they
matter. The Bazel migration once lost `DEBUG`, and for a while `--config=dbg` built none of these;
the symbol table is how to tell — a debug image links NetX Duo's `_nxe_*` checks, a release
image does not.

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
`TX_PORT_BASEPRI` together in `tx_user.h` and re-derive the priorities; the ceiling
behaviour then returns.

> **What replaced the rule is a different one, and it caught this port twice.** The constraint
> that now matters is not *which priority* may call the kernel, but *which context may block*.
> CMSIS-RTOS2 forbids a non-zero timeout from an ISR, and ThreadX additionally forbids one from
> its **timer thread** (`txe_queue_send.c` returns `TX_WAIT_ERROR` for `&_tx_timer_thread`).
> flexPTP's CMSIS-RTOS2 binding violates both — on the transmit-timestamp path and on the
> heartbeat path. See `ptp/ptp_tx_timestamp.c` and
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

Regions from `lib/startup/stm32h563zitx_FLASH.ld`; usage measured on the 2026-11-27 debug and release images.

| Region | Base | Size | Contents | Debug | Release |
|---|---|---|---|---|---|
| FLASH | `0x08000000` | 2 MB | `.isr_vector`, `.text`, `.rodata`, `.data` LMA | 224 704 B (10.71%) | 151 376 B (7.22%) |
| SRAM1 | `0x20000000` | 256 KB | `.data`, `.bss`, main stack, C heap, **ThreadX byte pools** | 88 816 B (33.88%) | 88 576 B (33.79%) |
| SRAM2 | `0x20040000` | 64 KB | *(unallocated — deliberate spare capacity)* | 0 B | 0 B |
| SRAM3 | `0x20050000` | 320 KB | Ethernet DMA descriptors + buffers, **NetX Duo packet pool and IP thread stack** | 89 704 B (27.38%) | 89 608 B (27.35%) |

Smaller than the FreeRTOS/lwIP build this firmware replaced (217 160 B FLASH, 133 168 B SRAM1,
measured then), despite SRAM3 growing: lwIP's heap and the 30 KB `ucHeap` are gone, and NetX Duo's packet pool
moved to SRAM3 alongside the DMA buffers it feeds.

- `_estack` = `0x20040000`, the top of SRAM1 — confirmed as vector word 0 in the linked image.
- Ethernet DMA buffers live in SRAM3: 48 descriptors plus 48 × 1528 B of frame buffers.
- **`__RAM_segment_used_end__`** is PROVIDEd immediately after the heap/stack reservation and is
  where ThreadX starts. Everything above it becomes `_tx_initialize_unused_memory`, from which
  the CMSIS-RTOS2 wrapper carves its two byte pools.
- **There are three allocators in SRAM1, not two**, and until finding NX-03 the newlib heap
  overlapped the other two. `_sbrk()` bounded itself against `_estack - _Min_Stack_Size`, on
  ST's template assumption that the heap and the MSP stack are the only things growing towards
  each other — 181 776 bytes above `_end`, of which only the first 2 560 were the heap's. The
  rest was the byte pools. It now bounds itself at `__RAM_segment_used_end__`, which is what
  makes the ordering above actually mean something.

  | Allocator | Region | Size | Reported by |
  |---|---|---|---|
  | newlib heap (`malloc`) | `_end` → `__RAM_segment_used_end__` | 10 240 B | `osinfo`, high-water mark |
  | ThreadX stack pool | above `__RAM_segment_used_end__` | 24 KB | `osinfo` |
  | ThreadX object pool | after it | 8 KB | `osinfo` |
  | MSP stack | grows down from `_estack` | 2 KB reserved | — |

  **The heap is sized from measurement**: the high-water mark is 3 600 bytes, it appears when
  flexPTP starts, and it does not grow over hundreds of CLI commands. Those 3 600 bytes were
  previously taken out of the byte pools. Exhaustion **reports and halts** rather than
  returning `ENOMEM`, because the only allocator here is newlib's float conversion and it does
  not tolerate a failure — capping the heap at the old figure wedged the console thread after
  seven commands while the rest of the firmware ran on.
- **No cache maintenance is performed anywhere**, and none is needed: the H563's DCACHE is
  disabled out of reset and `HAL_Init()` does not enable it. Enabling DCACHE later would
  require making the SRAM3 Ethernet region non-cacheable or adding explicit maintenance.
