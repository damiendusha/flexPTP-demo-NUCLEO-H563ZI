# Feasibility: porting to ThreadX + NetX Duo

An assessment of what it would take to replace FreeRTOS with **Eclipse ThreadX** and lwIP with
**NetX Duo**, keeping flexPTP, the H563 MAC driver and the CLI.

Companion documents: [architecture.md](architecture.md) (what exists today) ·
[design_doc.md](design_doc.md) (porting decisions) ·
[port_review.md](port_review.md) (findings)

Everything asserted about ThreadX and NetX Duo below was read from the sources listed in
[Provenance](#provenance), not recalled. Claims that were *not* verified are marked **(unverified)**.

## Table of Contents

- [Verdict](#verdict)
- [Why this port is cheaper than it looks](#why-this-port-is-cheaper-than-it-looks)
- [Inventory: what moves, what dies, what is new](#inventory-what-moves-what-dies-what-is-new)
- [Part 1 — The RTOS half (ThreadX)](#part-1--the-rtos-half-threadx)
- [Part 2 — The network half (NetX Duo)](#part-2--the-network-half-netx-duo)
- [The three things that will actually cost you](#the-three-things-that-will-actually-cost-you)
- [Choosing the supply chain](#choosing-the-supply-chain)
- [Effort and staging](#effort-and-staging)
- [Risk register](#risk-register)
- [Recommendation](#recommendation)
- [Provenance](#provenance)

---

## Verdict

**Feasible, and the RTOS half is close to free. The network half is a real port, and one
specific defect will silently break PTP if it is not fixed first.**

Three findings shape the whole estimate:

1. **The application is already RTOS-portable.** Project code makes ~50 RTOS calls and
   essentially all of them are CMSIS-RTOS2, not FreeRTOS. ST ships a maintained CMSIS-RTOS2
   wrapper over ThreadX ([`stm32-mw-cmsis-rtos-tx`](https://github.com/STMicroelectronics/stm32-mw-cmsis-rtos-tx)),
   which the STM32CubeH5 package already carries for this exact MCU family. Most of `Src/`
   compiles unchanged.

2. **NetX Duo has first-class hardware-timestamp plumbing that lwIP does not.** This project
   had to inject four fields into `struct pbuf` via `LWIP_PBUF_CUSTOM_DATA` and hook
   `LWIP_HOOK_UNKNOWN_ETH_PROTOCOL` to get PTP timestamps and 802.3 frames through lwIP.
   NetX Duo's link layer (`nx_link.h`) has `NX_LINK_TIME`, a receive path that carries a
   timestamp, a registered-callback path for arbitrary EtherTypes including a named
   `NX_LINK_ETHERNET_PTP` (0x88F7), and a per-packet
   `NX_INTERFACE_CAPABILITY_PTP_TIMESTAMP` flag. The unversioned `tx_cb` contract that
   already broke once (F48) goes away.

3. **flexPTP's CMSIS-RTOS2 binding has an ISR bug that this port walks straight into.**
   Switching flexPTP from `FLEXPTP_FREERTOS` to `FLEXPTP_CMSIS_OS2` — the obvious move —
   silently discards **every transmit timestamp**. Details in
   [The three things](#1-flexptps-cmsis-rtos2-binding-drops-every-tx-timestamp-must-fix).
   It is cheap to work around, but it must be found before the bench work, not during it.

Nothing found is a blocker. There is no capability in the current firmware that ThreadX and
NetX Duo cannot provide.

---

## Why this port is cheaper than it looks

The layering that [architecture.md](architecture.md#layering) describes turns out to be the
thing that makes this tractable. Three boundaries hold:

| Component | Lines | Depends on | Verdict |
|---|---|---|---|
| `Drivers/EthDrv/mac_drv.c` | 1080 | CMSIS + HAL only — **no RTOS, no network stack** | **Unchanged** |
| `Drivers/EthDrv/phy_drv/phy_common.c` | 605 | one `osDelay(5)` | **Unchanged** |
| `Src/flexptp_port/ptp_port_stm32h563.c` | 162 | one `vTaskDelay(pdMS_TO_TICKS(10))` | **1 line** |
| `Src/cliutils/cli.c` | 652 | CMSIS-RTOS2 mutex + thread | **Unchanged** |
| `Src/standard_output/` | 381 | CMSIS-RTOS2 event flags, mutex, thread | **Unchanged** |
| `Modules/blocking_io/` | 237 | CMSIS-RTOS2 semaphores + PRIMASK critical sections | **Unchanged** |
| flexPTP core (submodule) | — | selectable OS binding, **custom NSD supported by its CMake** | **No patch needed** |

The single most expensive artefact in the repository — the register-level MAC driver with the
descriptor-level timestamp path, which took Stage 12 plus findings F53, F54, F58, F61 and F71
to get right — is **completely untouched by this port.** It talks to `ETH_TypeDef` and calls
two weak hooks (`ETHHW_ReadCallback`, `ETHHW_EventCallback`). Only the hooks' *implementations*
change.

Likewise, flexPTP's build system already supports a project-supplied network stack driver via
`FLEXPTP_NSD_SRC` + `FLEXPTP_NETWORK_STACK` (`Modules/flexptp/CMakeLists.txt`), exactly as this
project already supplies a custom *hardware* port via `FLEXPTP_HWPORT_SRC`. **A NetX Duo NSD
does not require patching the flexPTP submodule** — unlike the servo fix in `patches/`.

---

## Inventory: what moves, what dies, what is new

### Deleted (~2 200 lines)

| Path | Lines | Note |
|---|---|---|
| `Modules/lwip_port/` | ~830 | `sys_arch.c` is pure FreeRTOS glue; NetX Duo needs no equivalent |
| `Drivers/EthDrv/eth_drv_lwip.c` | 503 | Replaced by a NetX Duo driver entry |
| `Inc/lwipopts.h` | 303 | Replaced by `nx_user.h` |
| `Src/ethernet/ethernet_lwip.c` | 125 | Replaced |
| `Inc/FreeRTOSConfig.h` | 172 | Replaced by `tx_user.h` |
| `Drivers/CMSIS/CMSIS_RTOS_V2/` | ~3 300 | The FreeRTOS-backed shim, swapped for the ThreadX-backed one |
| `Modules/lwip` submodule | — | |
| FreeRTOS `FetchContent` in `Middlewares/` | 48 | |

### New (~1 100–1 400 lines, estimated)

| Path | Est. lines | Role |
|---|---|---|
| `Drivers/EthDrv/eth_drv_netxduo.c` | 400–550 | NetX Duo driver entry: the `NX_LINK_*` command switch, RX thread → `nx_link_ethernet_packet_received()`, TX-timestamp correlation |
| `Src/flexptp_nsd/nsd_netxduo.c` | 200–250 | The four NSD functions |
| `Src/ethernet/ethernet_netxduo.c` | 150–200 | IP instance, packet pool, ARP/ICMP/IGMP/UDP enable, DHCP client |
| `Inc/nx_user.h`, `Inc/tx_user.h` | ~150 | Stack and kernel configuration |
| CMake integration | ~120 | Three middleware trees plus the wrapper |
| Linker script edits | ~20 | Byte-pool region, NetX packet pool placement in SRAM3 |

Net: the tree gets **smaller**, and the deleted code is the fiddliest glue in it.

---

## Part 1 — The RTOS half (ThreadX)

### The wrapper does the heavy lifting

Project code uses these CMSIS-RTOS2 objects: threads, mutexes, semaphores, message queues,
event flags, timers, `osDelay`, `osKernelGetState`. **All are implemented** by ST's wrapper.
The only documented gap is *thread flags*, which this project does not use.

> **The wrapper's README understates it.** The README claims `max_count` is unsupported for
> semaphores. The current source disproves that: `osSemaphoreNew()` stores `max_count` as a
> `ceiling` and `osSemaphoreRelease()` calls `tx_semaphore_ceiling_put()`. That matters,
> because `Modules/blocking_io/blocking_fifo.c` depends on binary-semaphore behaviour.
> **Check the wrapper source, not its README.**

### Six concrete differences to handle

| # | Difference | Impact here |
|---|---|---|
| 1 | **`osThreadNew()` before `osKernelInitialize()` stops working.** The wrapper creates its two ThreadX byte pools *inside* `osKernelInitialize()`, and calls `_tx_initialize_kernel_setup()` there. Before that call there is nowhere to allocate a stack from. | **`Src/main.c` must be reordered.** `serial_io_init()` creates the `"serialio"` thread before `osKernelInitialize()`. `main.c:266–275` already documents this as outside the CMSIS-RTOS2 contract and deliberately left alone; ThreadX turns it into a hard failure. Split `serial_io_init()` into hardware setup and thread creation — the fix the comment already anticipates. |
| 2 | **Priorities invert.** The wrapper maps `tx_priority = osPriorityISR − cmsis_priority`, and `#error`s unless `TX_MAX_PRIORITIES == 64` in `tx_user.h`. | The five threads relying on the `osPriorityNormal` default keep working. The two deliberate choices — `TCP/IP` at 40 and `rx` at 32 ([architecture.md](architecture.md#threads)) — need re-deriving against the new stack's thread set. `osPriorityNormal` (24) becomes ThreadX priority 32. |
| 3 | **No `heap_4`; two byte pools instead.** `configTOTAL_HEAP_SIZE` (30 KB in `.FreeRTOSHeapSection`) is replaced by `RTOS2_BYTE_POOL_HEAP_SIZE` / `RTOS2_BYTE_POOL_STACK_SIZE`, or by `_tx_initialize_unused_memory` under `USE_DYNAMIC_MEMORY_ALLOCATION`. | Linker-script work, and the `osinfo` CLI command in `Src/cmds.c` (48 lines, its only direct FreeRTOS use — `vPortGetHeapStats()`) needs rewriting against `tx_byte_pool_info_get()`. |
| 4 | **Default tick is 100 Hz, not 1000.** `tx_initialize_low_level.S` computes `SYSTICK_CYCLES = (SYSTEM_CLOCK / 100) − 1`, and `SYSTEM_CLOCK` must be supplied to the assembler. | flexPTP's 31 ms heartbeat would quantise to 40 ms. Set the tick to 1000 Hz to match `configTICK_RATE_HZ` and keep `FLEXPTP_MS_TO_TICKS` behaviour identical. |
| 5 | **Critical sections use PRIMASK, not BASEPRI** (`TX_PORT_USE_BASEPRI` / `TX_PORT_BASEPRI` are opt-in). | The `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5` discipline in [architecture.md](architecture.md#the-freertos-priority-constraint) **disappears** — any ISR may call ThreadX APIs. Since all three project interrupts already sit at 7 and 12, nothing regresses. That section of the docs becomes wrong and must be rewritten. |
| 6 | **The failure hooks change shape.** `vApplicationStackOverflowHook`, `vApplicationMallocFailedHook`, `vAssertCalled`, `vApplicationTickHook`, `vApplicationIdleHook` (`Src/main.c`) have no direct ThreadX equivalents. | ThreadX offers `tx_application_stack_error_handler` (with `TX_ENABLE_STACK_CHECKING`) and `TX_THREAD_ENTRY/EXIT_NOTIFY`. Byte-pool exhaustion surfaces as a return code, not a hook. |

### What does **not** regress

**Hardware stack-overflow trapping survives.** [architecture.md](architecture.md#interrupts)
notes that on this part it is `PSPLIM` — not `configCHECK_FOR_STACK_OVERFLOW` — that actually
catches overflows. ThreadX's Cortex-M33 GNU port sets `PSPLIM` on every context switch
(`tx_thread_schedule.S`: `MSR PSPLIM, r12`), so the `UsageFault` handler in `Src/main.c` keeps
reporting `UFSR.STKOF` exactly as it does today.

### flexPTP's OS binding

flexPTP already supports four bindings: `FLEXPTP_FREERTOS` (current),
`FLEXPTP_CMSIS_OS2`, `FLEXPTP_LINUX`, `FLEXPTP_OSLESS`. All OS coupling is confined to
`task_ptp.c`, `ptp_defs.h` and `ptp_types.h`. Three routes:

| Route | Cost | Verdict |
|---|---|---|
| **`FLEXPTP_CMSIS_OS2`** | One line in `flexptp_options.h`, **plus the TX-timestamp fix below** | **Recommended.** Uses a binding flexPTP already ships |
| Native ThreadX binding | ~150 lines across three flexPTP files; a submodule patch or an upstream PR | Cleanest long-term, upstream-worthy, but adds a fourth entry to `patches/` |
| `FLEXPTP_OSLESS` | Requires polling `task_ptp()` from a thread and driving the heartbeat externally | Not worth it when an OS is present |

> `critical.h`'s `FLEXPTP_CMSIS_OS2` branch defines `FLEXPTP_ENTER_CRITICAL()` as
> `__disable_interrupts()`, which is not a CMSIS name (`cmsis_compiler.h` provides
> `__disable_irq()`). It would not compile — but the header is **included by nothing** in
> flexPTP, so it never does. Harmless today; a trap for anyone who starts using it.

---

## Part 2 — The network half (NetX Duo)

### The timestamp path is genuinely better

This is the finding that most changes the shape of the port. Today's arrangement
([architecture.md](architecture.md#the-ptp-timestamp-path)) needs two lwIP extension points
and an unversioned function-pointer contract. NetX Duo provides the equivalents as API:

| Need | Today (lwIP) | NetX Duo |
|---|---|---|
| RX timestamp to the app | 2 injected `struct pbuf` fields via `LWIP_PBUF_CUSTOM_DATA` | `nx_link_ethernet_packet_received(ip, iface, packet, NX_LINK_TIME *)` |
| PTP over 802.3 (EtherType 0x88F7) | `LWIP_HOOK_UNKNOWN_ETH_PROTOCOL` + a manual EtherType/MAC parse in `nsd_lwip.c` | `nx_link_packet_receive_callback_add(..., NX_LINK_ETHERNET_PTP, cb, ctx)` — **the callback receives the timestamp as an argument** |
| 802.3 transmit | `ethernet_output()` with a hand-built destination | `nx_link_ethernet_packet_send(..., NX_LINK_ETHERNET_PTP)` |
| L2 multicast for gPTP | not done explicitly | `nx_link_multicast_join()` |
| "This frame needs a TX timestamp" | 2 more injected `pbuf` fields (`tx_cb`, `tag`) | `nx_packet_interface_capability_flag |= NX_INTERFACE_CAPABILITY_PTP_TIMESTAMP` — an **existing** `NX_PACKET` field |

`NX_LINK_ETHERNET_PTP` and `NX_INTERFACE_CAPABILITY_PTP_TIMESTAMP` are upstream constants, not
inventions. The `tx_cb` signature drift that failed at compile time in F48 has no analogue here.

**Two caveats on that path, both real:**

- **The RX timestamp is delivered by convention, not by a struct field.** `nx_link.c` writes it
  over the front of the packet buffer:
  ```c
  ((ULONG *)packet_ptr -> nx_packet_data_start)[0] = time_ptr -> nano_second;
  ((ULONG *)packet_ptr -> nx_packet_data_start)[1] = time_ptr -> second_low;
  ((ULONG *)packet_ptr -> nx_packet_data_start)[2] = time_ptr -> second_high;
  ```
  It works because those 12 bytes held the Ethernet header, which is already consumed. It
  survives up through IP and UDP to the socket receive, which is how the UDP transport gets its
  timestamp. **It requires ≥12 bytes between `nx_packet_data_start` and the IP header** — true
  for a 14-byte Ethernet header, but it is an undocumented layout dependency of exactly the kind
  that produced F48. Assert it in the driver.

- **NetX Duo does not deliver TX timestamps.** `nx_link_packet_transmitted()` takes an
  `NX_LINK_TIME *` and then does `NX_PARAMETER_NOT_USED(time_ptr)` — verified in both Eclipse
  `master` and ST's 6.4.0 fork. The driver→application TX path is the integrator's problem in
  every NetX Duo design. **This costs nothing here**, because `mac_drv.c` already delivers TX
  timestamps through its own per-descriptor callback (`ETHHW_OptArg_TxTsCap.txTsCbPtr` + `tag`).
  Keep that path; correlate `NX_PACKET *` → flexPTP `uid` in a small side table in the NSD, the
  same technique NetX's own PTP client uses (`nx_ptp_client_delay_req_packet_ptr`).

### NetX Duo's own PTP client is not a substitute for flexPTP

Worth stating plainly, because it is the obvious question. `addons/ptp/nxd_ptp_client.c` is
**slave-only**, has **no BMCA** ("only the first master clock is accepted"), supports **only**
the delay request-response mechanism, and offers **no 802.3 transport or gPTP profile**. This
firmware's whole CLI surface — `ptp master`, `ptp transport 802.3`, `ptp delmech p2p`,
`ptp profile preset gPTP`, master operation via `PTP_ENABLE_MASTER_OPERATION` — has no
counterpart. **flexPTP stays.**

### The NSD is four functions

`Modules/flexptp/src/flexptp/network_stack_driver.h` requires exactly:

| flexPTP function | NetX Duo mapping |
|---|---|
| `ptp_nsd_init(tp, dm)` | `nx_udp_socket_create/bind` ×2 (ports 319/320) + `nx_udp_socket_receive_notify`; or `nx_link_packet_receive_callback_add()` for 802.3 |
| `ptp_nsd_transmit_msg(msg, uid)` | `nx_packet_allocate` + `nx_packet_data_append` + `nx_udp_socket_send` / `nx_link_ethernet_packet_send`, with the capability flag set and the uid recorded |
| `ptp_nsd_get_interface_address(hwa)` | `nx_ip_interface_physical_address_get()` |
| `ptp_nsd_igmp_join_leave(join)` | `nx_igmp_multicast_join/leave()` (224.0.1.129 / 224.0.0.107), `nx_link_multicast_join/leave()` for L2 |

Confirmed safe: `_nx_ip_packet_send()` **ORs** into `nx_packet_interface_capability_flag`
(`|=`) rather than overwriting it, so a flag set by the NSD before send reaches the driver.

### Feature parity for the rest of the firmware

| Feature | Today | NetX Duo |
|---|---|---|
| DHCP client | `lwip/dhcp.h` | `addons/dhcp/nxd_dhcp_client.c` |
| ICMP echo (the `ping` bench test) | built in | `nx_icmp_enable()` |
| IGMP | `LWIP_IGMP` | `nx_igmp_enable()` |
| Link up/down → DHCP start/stop | `netif_set_link_callback` | `NX_LINK_GET_STATUS` polling from the existing `phy` thread |

### The driver entry

`nx_<name>_eth_driver(NX_IP_DRIVER *)` must handle: `NX_LINK_INTERFACE_ATTACH`,
`INITIALIZE`, `ENABLE`, `DISABLE`, `PACKET_SEND`, `RAW_PACKET_SEND`, `ARP_SEND`,
`ARP_RESPONSE_SEND`, `RARP_SEND`, `PACKET_BROADCAST`, `MULTICAST_JOIN`, `MULTICAST_LEAVE`,
`GET_STATUS`, `DEFERRED_PROCESSING`. ST's `nx_stm32_eth_driver.c` (2 880 lines) is a working
reference for the contract — **but do not adopt it.** It is built on `HAL_ETH`, which this
project deliberately bypassed precisely because the HAL abstracts away the DMA descriptors that
hardware timestamping needs; and its PTP mode is hard-wired to `NX_PTP_CLIENT` types and
`#error`s without `NX_ENABLE_VLAN`. The existing `rx_thread` / `low_level_output` structure in
`eth_drv_lwip.c` maps onto the NetX Duo command switch almost one-for-one.

---

## The three things that will actually cost you

### 1. flexPTP's CMSIS-RTOS2 binding drops every TX timestamp (must fix)

The sharpest item in this report, and it is invisible at build time.

`ETHHW_ProcessTx()` runs **in `ETH_IRQHandler`** ([architecture.md](architecture.md#handler-responsibilities))
and invokes the transmit-timestamp callback from there. flexPTP handles that correctly in
FreeRTOS mode — `task_ptp.c:545` branches on `xPortIsInsideInterrupt()` and uses
`xQueueSendFromISR`. The CMSIS-RTOS2 branch does not:

```c
#elif defined(FLEXPTP_CMSIS_OS2)
    osMessageQueuePut(sTxCbFIFO, &ts, 0, osWaitForever);
    osMessageQueuePut(sNotificationFIFO, &notif, 0, osWaitForever);
```

CMSIS-RTOS2 forbids a non-zero timeout from an ISR, and ST's wrapper enforces it:

```c
if ((IS_IRQ_MODE() && (timeout != 0)) || ... ) { status = osErrorParameter; }
```

Every call returns `osErrorParameter` and enqueues nothing. flexPTP ignores the return value.
Consequence: **`t3` never reaches the servo for any `Delay_Req`**, mean path delay is never
computed, and — going by F59's precedent — the visible indicators may well stay green while
the clock free-runs.

The same trap exists in `ptp_transmit_enqueue()`.

Three fixes, cheapest first:

- **Defer in project code (recommended).** Register a `txTsCbPtr` that only stashes the
  timestamp and posts with timeout 0, and call `ptp_transmit_timestamp_cb()` from a thread.
  Entirely inside `eth_drv_netxduo.c`; no submodule patch.
- Patch flexPTP's CMSIS-RTOS2 path to test `__get_IPSR()` and pass timeout 0. Worth reporting
  upstream either way — it is a genuine flexPTP defect, not a porting artefact.
- Write a native ThreadX binding, which sidesteps CMSIS-RTOS2 entirely.

Related: **do not define `TX_TIMER_PROCESS_IN_ISR`.** flexPTP's heartbeat callback calls
`ptp_event_enqueue()`, which does the same `osWaitForever` put. It is safe only while the
callback runs in ThreadX's timer *thread*, which is the default.

### 2. Re-verifying the timestamp path on hardware

The receive and transmit timestamp paths are the parts of this firmware that cost the most to
get right — F42, F48, F53, F58, F61 and F71 all live there, and F42 alone cost days. Replacing
the network stack replaces both halves of that path. The MAC driver is unchanged, so the
hardware side is de-risked, but **the full verification sequence in
[port_review.md](port_review.md) has to be re-run**: RX timestamps non-zero in T1/T2, TX
timestamps 1-for-1 through `ETHHW_ProcessTx()`, no duplicate frame delivery, ping loss at 1400
bytes, and a settled servo against `ptp4l`.

Budget the bench time. This is the item most likely to blow the estimate.

### 3. The `nx_packet_data_start` layout dependency

Covered above. Twelve bytes of undocumented, unasserted head-room convention carrying the
timestamp through three protocol layers. Add a `_Static_assert`-equivalent runtime check in the
driver, in the same spirit as the existing layout assertion in `eth_drv_lwip.c:61`.

---

## Choosing the supply chain

**Decide this before any code is written.** Two upstreams exist for the same middleware, and
they differ in licence, in currency, and — decisively — in patch status.

| Source | Version | Licence |
|---|---|---|
| [eclipse-threadx/threadx](https://github.com/eclipse-threadx/threadx), [netxduo](https://github.com/eclipse-threadx/netxduo) | **6.5.1**, 8 Jun 2026; quarterly train | **MIT** |
| [STMicroelectronics/stm32-mw-threadx](https://github.com/STMicroelectronics/stm32-mw-threadx), [stm32-mw-netxduo](https://github.com/STMicroelectronics/stm32-mw-netxduo) | **6.4.0** (upstream tag dated 2 Jan 2024; vendored by ST 19 Apr 2024) | **Microsoft Azure RTOS licence** + `LICENSED-HARDWARE.txt`; distribution rights inherited only for listed hardware (STM32 is listed) |
| [STMicroelectronics/stm32-mw-cmsis-rtos-tx](https://github.com/STMicroelectronics/stm32-mw-cmsis-rtos-tx) | — | **SLA0044** |

### ST's NetX Duo fork is missing published CVE fixes on the path this firmware runs

This is the finding that settles the choice, and it is not inferred from version numbers —
it was read out of the two trees.

`_nx_ipv4_packet_receive()` in Eclipse upstream carries two bounds checks that **ST's fork does
not have**, each tagged in-source with the advisory that produced it:

| Check, present upstream only | Advisory | What it is |
|---|---|---|
| `nx_packet_length < sizeof(NX_IPV4_HEADER)` at function entry | `GHSA-cf2g-j6vv-m8c5` / CVE-2025-55090 | Out-of-bounds read in the IPv4 receive path |
| `nx_packet_length < sizeof(NX_UDP_HEADER)` in the unicast-DHCP branch | `GHSA-c9pq-93jp-w649` / CVE-2025-55093 | Out-of-bounds read **and write** handling unicast DHCP messages |

**Both sit in code this firmware executes on every received frame**, and the DHCP one is in the
branch reached because the board runs a DHCP client. They were fixed in the 6.4.4 (Oct 2025) and
6.4.5 (Jan 2026) releases — after ST's 6.4.0 baseline. ST's `st_readme.txt` records no security
backports: everything after "Upgrade to Azure RTOS NetXDuo 6.4.0" is a licensed-hardware-list
addition, a doc file, the removal of the Cypress Wi-Fi driver, and one Ethernet-driver
broadcast-filter correction.

Related advisories in the same batch (`CVE-2025-55091`, `-55092`) touch `_nx_ip_packet_receive()`
and `_nx_ipv4_option_process()`. The TLS ones (`CVE-2025-55081`–`55084`) are NetX Secure, which
this firmware would not compile; the IPv6 ones (`CVE-2025-55094`, `-55102`) need IPv6 enabled,
which it would not.

> This is a demo board on a lab network, not an internet-facing product — so treat it as a
> currency signal rather than an alarm. The signal is still clear: **the ST fork is a snapshot,
> not a maintained branch.** Anything built on it inherits a stack that stopped receiving
> upstream fixes two and a half years ago.

### What you actually give up by taking Eclipse upstream

Verified by comparing the trees, not assumed:

| Concern | Reality |
|---|---|
| Kernel differences | **None.** ST's `st_readme.txt` shows the fork is vanilla Azure RTOS plus licensed-hardware-list edits. Its one historical Cortex-M33 port fix (6.1.10) is long since upstream |
| Cortex-M33 GNU port | Same files. `tx_initialize_low_level.S` differs **only** in licence header and release-history comments |
| **`tx_initialize_low_level.S` placement** | The one real difference. ST puts it in `ports/cortex_m33/gnu/src/` (compiled into the port); Eclipse keeps it in `ports/cortex_m33/gnu/example_build/` (**you copy it and adapt it**). You must set `SYSTEM_CLOCK`, the SysTick divisor and `_tx_initialize_unused_memory` against `stm32h563zitx_FLASH.ld` — work this port needs done anyway |
| `nx_stm32_eth_driver.c`, LAN8742 PHY driver | **Not in Eclipse upstream** — `common/drivers/` does not exist there; ST added it. Irrelevant here: it is HAL_ETH-based and unusable for hardware timestamping, and this port writes its own driver against `mac_drv.c`. It stays readable on GitHub as a reference |
| Does ST's CMSIS-RTOS2 wrapper still work? | **Yes.** It touches exactly three ThreadX internals — `_tx_initialize_kernel_setup`, `_tx_initialize_unused_memory`, `_tx_thread_created_ptr` — all standard, all present upstream. It contains no STM32, HAL or ST-fork references |
| CubeMX code generation | Lost. CubeMX generates against ST's tree. This project does not use CubeMX for anything |
| ST support entitlement | Lost. Community support only |
| `nx_link` PTP timestamp code | **Identical in both** — verified line-for-line. Nothing in [Part 2](#part-2--the-network-half-netx-duo) depends on which fork you take |

### The three consistent positions

1. **Eclipse ThreadX + Eclipse NetX Duo (MIT) + ST's CMSIS-RTOS2 wrapper (SLA0044).**
   **Recommended.** Current and patched, MIT for everything that matters, one file under ST
   terms. Costs one adapted `tx_initialize_low_level.S`. Note the mixed licence in the README
   alongside the existing submodule/patch notes.
2. **All-Eclipse, MIT throughout, no CMSIS-RTOS2.** Port the ~50 call sites to native `tx_*` and
   give flexPTP a native ThreadX binding. Perhaps 400 lines of churn in files that would
   otherwise be untouched, and it forfeits finding 1 of this report. Choose it only if a single
   non-MIT file is genuinely unacceptable.
3. **All-ST.** What CubeMX generates and what ST supports — and a stack pinned at 6.4.0 with the
   IPv4/DHCP receive-path fixes above still missing. Choose it only if ST support entitlement is
   worth more than that, and then track the CVEs yourself.

**The awkward asymmetry to be aware of either way:** the CMSIS-RTOS2 wrapper — the single
component that makes the RTOS half cheap — exists only in ST's SLA0044 repository. Eclipse
ThreadX ships compatibility layers for FreeRTOS, OSEK and POSIX, but **not** CMSIS-RTOS2.

---

## Effort and staging

Assumes one developer already familiar with this codebase, and hardware on the bench.

| Stage | Work | Estimate |
|---|---|---|
| T1 | Decide the licensing route; vendor ThreadX (+ wrapper) as submodules; `tx_user.h`; linker-script byte pool | 0.5–1 d |
| T2 | Swap the CMSIS-RTOS2 shim; fix `main.c` ordering (item 1 of the table); rewrite the failure hooks and `cmds.c osinfo`; **boot to CLI with no network** | 1–2 d |
| T3 | Vendor NetX Duo; `nx_user.h`; IP instance, packet pool, ARP/ICMP/IGMP/UDP; **stub driver, no traffic** | 1 d |
| T4 | `eth_drv_netxduo.c` against the existing `mac_drv.c`; **link up, DHCP, ping** | 2–4 d |
| T5 | `nsd_netxduo.c`; the TX-timestamp deferral fix; **flexPTP starts, UDP/IPv4 E2E syncs** | 2–3 d |
| T6 | 802.3 transport via `nx_link_*`; P2P; profile presets | 1–2 d |
| T7 | Re-run the full hardware verification; measure servo settling against `ptp4l`; update `docs/` | 2–4 d |

**Total: 10–17 working days**, i.e. **2–3½ weeks**. T4 and T7 carry the variance. For
calibration, the original lwIP path took twenty stages and a review that produced 70+ findings —
but most of that was MCU bring-up, which this port inherits for free.

A useful property: **T2 is independently valuable and independently testable.** ThreadX with
lwIP still on top is a legitimate intermediate state — lwIP's `sys_arch.c` would need a
CMSIS-RTOS2 or ThreadX rewrite, but that is ~600 lines of well-understood glue. If the network
half stalls, the RTOS half can still land.

---

## Risk register

| Risk | Severity | Likelihood | Mitigation |
|---|---|---|---|
| TX timestamps silently lost via the CMSIS-RTOS2 ISR path | **High** — presents as a subtly wrong clock, not a failure | **Certain** if unaddressed | Fix in T5 *before* bench work; instrument the callback count, as F61 did |
| `nx_packet_data_start` head-room assumption violated | High | Low | Runtime assert in the driver; verify on the first received Sync |
| Re-introducing a duplicate-delivery bug (F61 class) in the new RX path | High | Medium | Port the `inFlight` marker discipline verbatim; re-run the frame-count instrumentation |
| Byte pools sized wrong → thread creation fails at boot | Medium | Medium | ThreadX returns codes rather than calling a hook; check every creation site |
| Tick-rate mismatch skews flexPTP's heartbeat and TTLs | Medium | Medium | Set 1000 Hz explicitly in `tx_user.h`; check `SYSTEM_CLOCK` reaches the port assembly |
| ST wrapper behaviour differs from its README | Low | **Observed** (semaphore `max_count`) | Read the wrapper source for every API used |
| Building on ST's 6.4.0 fork inherits unpatched IPv4/DHCP receive-path CVEs | Medium | **Certain** if that fork is chosen | Take Eclipse upstream — see [Choosing the supply chain](#choosing-the-supply-chain) |
| Eclipse 6.5.1 vs ST 6.4.0 API drift in `nx_link` | Low | Low | The PTP-relevant `nx_link` code is identical across both, verified |
| Eclipse upstream does not compile `tx_initialize_low_level.S` for you | Low | **Certain** | Copy it from `ports/cortex_m33/gnu/example_build/` in stage T1; it needs board adaptation regardless of fork |
| Licensing note omitted from the README | Low | Medium | Fold into the existing submodule/patch section |

---

## Recommendation

**Feasible and reasonable to attempt.** The port is well-suited to this codebase specifically
because of decisions already made here: the HAL-bypassing MAC driver has no stack or RTOS
dependency, the application is CMSIS-RTOS2 throughout, and flexPTP supports both a custom NSD
and an alternative OS binding without patching.

Do it if the goal is any of: fewer bespoke extension points in the timestamp path (NetX Duo's
link layer replaces two lwIP hooks and an unversioned callback contract with supported API);
alignment with what STM32CubeMX generates for H5; or a second reference port proving flexPTP's
portability claims on a third stack.

Do **not** do it expecting better sync accuracy. Timestamps are captured in hardware by an
unchanged driver, so the measured floor will not move. This buys architecture, not nanoseconds.

The single highest-value next step, whether or not the port proceeds: **report the flexPTP
CMSIS-RTOS2 ISR defect upstream.** It is a live bug in flexPTP today for any port using that
binding with an interrupt-context transmit-timestamp callback — which is the normal arrangement.

---

## Provenance

Read directly, at the commits current in August 2026:

- `eclipse-threadx/netxduo` @ `master` — `common/inc/nx_link.h`, `common/src/nx_link.c`,
  `common/inc/nx_api.h` (`NX_PACKET`, `NX_INTERFACE_CAPABILITY_*`),
  `common/src/nx_ip_packet_send.c`, `addons/ptp/nxd_ptp_client.c`
- `STMicroelectronics/stm32-mw-netxduo` @ `main` — 6.4.0; `common/src/nx_link.c`,
  `common/drivers/ethernet/nx_stm32_eth_driver.{c,h}`
- `STMicroelectronics/stm32-mw-threadx` @ `main` — 6.4.0; `ports/cortex_m33/gnu/inc/tx_port.h`,
  `ports/cortex_m33/gnu/src/{tx_thread_schedule.S,tx_initialize_low_level.S}`, `LICENSE.md`,
  `LICENSED-HARDWARE.txt`
- `STMicroelectronics/stm32-mw-cmsis-rtos-tx` @ `main` — `cmsis_os2.c`, `README.md`, `LICENSE.md`
- `STMicroelectronics/STM32CubeH5` @ `main` — `.gitmodules`, `Middlewares/ST/`
- `eclipse-threadx/threadx` @ `master` — `utility/rtos_compatibility_layers/`, `LICENSE.txt`,
  `ports/cortex_m33/gnu/` (tree layout and `example_build/tx_initialize_low_level.S`, diffed
  against ST's copy)
- `eclipse-threadx/netxduo` vs `stm32-mw-netxduo` — `common/src/nx_ipv4_packet_receive.c`,
  diffed; GitHub security advisories for `eclipse-threadx/netxduo`; release tags and dates via
  the GitHub API
- [NetX Duo PTP Client documentation, chapter 1](https://github.com/eclipse-threadx/rtos-docs/blob/main/rtos-docs/netx-duo/netx-duo-ptp-client/chapter1.md)
- This repository at `cpb0dcd`-era `cpp` branch: `Src/`, `Drivers/EthDrv/`, `Modules/lwip_port/`,
  `Modules/flexptp/`, `Inc/`, `docs/`

**Not verified, and worth checking before committing to the plan:** exact byte-pool sizing for
this thread set; whether the `phy` thread's `NX_LINK_GET_STATUS` polling interacts cleanly with
NetX Duo's interface state machine; and NetX Duo's RAM footprint against the current
`PBUF_POOL_SIZE = 8` arrangement in SRAM3.
