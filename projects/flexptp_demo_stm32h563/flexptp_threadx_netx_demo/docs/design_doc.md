# flexPTP NUCLEO-H563ZI — Design Document

This document captures design decisions, technical notes, and future porting guides
for the flexPTP demo port from NUCLEO-H743ZI (STM32H743, Cortex-M7) to
NUCLEO-H563ZI (STM32H563, Cortex-M33).

**This project has been ported twice.** The first port moved it between silicon and is what
most of this document describes. The second replaced the RTOS and the network stack —
FreeRTOS/lwIP became Eclipse ThreadX and Eclipse NetX Duo — and everything below reflects the
current, ThreadX state. Where a section describes how something *used to* work, it says so.
The narrative of the second port is in
[threadx_porting_plan.md](threadx_porting_plan.md); the reasoning that preceded it is in
[threadx_netxduo_feasibility.md](threadx_netxduo_feasibility.md). The
[Stage Log](#stage-log) at the end of this document is a record of the *first* port and is
deliberately left as written.

## Table of Contents

- [Port Overview](#port-overview)
- [Dependency Pins](#dependency-pins)
- [System Clock Tree](#system-clock-tree)
- [PTP Clock](#ptp-clock) — full derivation in [ptp_clock.md](ptp_clock.md)
- [TrustZone Migration Guide](#trustzone-migration-guide)
- [RMII Pin Map](#rmii-pin-map)
- [RXER Pin Note](#rxer-pin-note)
- [mac_drv.c Comparison](#mac_drvc-comparison)
- [Caches and DMA Coherency](#caches-and-dma-coherency)
- [Stage Log](#stage-log)

Companion documents: [architecture.md](architecture.md) (module stack, threads, interrupts) ·
[testing_architecture.md](testing_architecture.md) (what is tested, and what is not) ·
[host_ptp_setup.md](host_ptp_setup.md) (running a PTP master on the host) ·
[l4_performance.md](l4_performance.md) (measured layer 4 accuracy, both firmwares) ·
[ptp_clock.md](ptp_clock.md) (PTP clock derivation) ·
[ptp_profiles.md](ptp_profiles.md) (default vs gPTP presets) ·
[port_review.md](port_review.md) (review findings and verified-clean audit) ·
[threadx_porting_plan.md](threadx_porting_plan.md) (the RTOS/stack port, stage by stage) ·
[implementation_plan.md](implementation_plan.md) (the original silicon port — historical)

---

## Port Overview

**Source**: NUCLEO-H743ZI — STM32H743ZI, Cortex-M7 @ 480 MHz, DP-FPU, stm32h7xx HAL  
**Target**: NUCLEO-H563ZI — STM32H563ZI, Cortex-M33 @ 250 MHz, SP-FPU, stm32h5xx HAL

Key decisions:
- PTP clock: **`pll1_q_ck` 250 MHz** / 5 ns increment (fixed source, no mux on this
  device — see [PTP Clock](#ptp-clock))
- HSE: Bypass mode (stock Nucleo, ST-Link MCO at 8 MHz)
- FPU: Keep `double` in Kalman filter, accept software emulation
- RTOS: **Eclipse ThreadX**, reached through ST's CMSIS-RTOS2 wrapper so the application and
  flexPTP stay RTOS-agnostic (was FreeRTOS)
- Network stack: **Eclipse NetX Duo**, whose `nx_link` layer carries PTP hardware timestamps
  natively (was lwIP, now the FreeRTOS firmware's stack; EtherLib was removed, decision D9)
- TrustZone: Disabled (TZEN=0, `TX_SINGLE_MODE_NON_SECURE`)
- mac_drv.c: Register-level port, **unchanged by the RTOS/stack port**

---

## Dependency Pins

Every vendored tree is a git repository fetched by Bazel and pinned by commit in
[`bazel/repositories.bzl`](../../../../bazel/repositories.bzl), which also says why each one comes
from where it does. Since November 2026 every pin is its upstream head, each bump verified on
hardware — see [`docs/flexptp_upstream_review.md`](../../../../docs/flexptp_upstream_review.md), §7.
Two trees are patched, by the fetch: flexPTP (ten fixes, each filed upstream or ready to be) and embfmt (one);
see [`patches/`](../../../../patches/flexptp/README.md).

One workaround from the first port is still live: flexPTP uses `__PRI64_PREFIX`, a glibc-internal
macro newlib lacks, so `bazel/flexptp.bzl` defines it as `__INT64` (finding F47).

The first port pinned its submodules *ahead* of the H743 reference project, deliberately:
flexPTP's `3601a6f` fixed a seconds-rollover race in `ptphw_gettime()` that this project's
hardware port inherits, so pinning back would have reintroduced a timestamp bug. That decision,
its costs (F47, F48) and the submodule and `build.sh` mechanics that went with it were described
here until the Bazel migration retired them; the text is in git history, at
`git show b742e43:projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/design_doc.md`.

---

## System Clock Tree

**SYSCLK = 250 MHz, HCLK = 250 MHz.** This is the H563's maximum, and on this part it is
the CPU/AHB clock. The PTP timestamp clock is separate — `pll1_q_ck`, also 250 MHz.

```
HSE 8 MHz (RCC_HSE_BYPASS — ST-Link MCO, no crystal fitted)
  │
  ├─ PLLM = HSE_VALUE / 2000000 = 4      →  VCI    =   2 MHz   (RCC_PLL1_VCIRANGE_1)
  ├─ PLLN = TARGET_SYSCLK_MHZ  = 250     →  VCO    = 500 MHz   (RCC_PLL1_VCORANGE_WIDE)
  │
  ├─ PLLP = 2                            →  SYSCLK = 250 MHz
  │                                          │
  │    AHBCLKDivider = RCC_SYSCLK_DIV1   →  HCLK   = 250 MHz   (AHB bus, MDIO CSR)
  │    APB1/2/3 Div  = RCC_HCLK_DIV2     →  PCLK   = 125 MHz
  │
  └─ PLLQ = 2, output enabled            →  pll1_q_ck = 250 MHz  → clk_ptp_ref_i
       __HAL_RCC_PLL1_CLKOUT_ENABLE(RCC_PLL1_DIVQ)                 (PTP timestamp unit)

  FLASH_LATENCY_5, voltage scaling VOS0
```

> [!WARNING]
> The divider namespaces are **inverted between the H7 and the H5**, and the constants in
> this diagram are the H5 ones. `AHBCLKDivider` takes `RCC_SYSCLK_DIVn` (it divides SYSCLK);
> the APB dividers take `RCC_HCLK_DIVn` (they divide HCLK).
>
> `RCC_HCLK_DIVn` exists on **both** families with different encodings, so writing the H7
> spelling into `AHBCLKDivider` compiles silently and programs a PPRE-encoded value into the
> HPRE field — the wrong AHB divider, with no diagnostic. That is finding
> [F45](port_review.md), and an earlier revision of this very diagram reproduced it.

Comparison with the source project: the H743 ran SYSCLK 480 MHz with `RCC_HCLK_DIV2`,
giving HCLK of 240 MHz, which on that part *was* also the PTP clock. On the H563 the two
are independent.

### Why these values

**VCO is pinned at 500 MHz.** `PLLP` forbids odd division factors and the PLL1 wide VCO
range is 192–836 MHz. For SYSCLK = 250 MHz, `PLLP = 2` (VCO 500) is the only solution;
`PLLP = 4` would need a 1000 MHz VCO, above the ceiling.

**`AHBCLKDivider` = `RCC_SYSCLK_DIV1`** keeps HCLK at the full 250 MHz. This is a
performance and MDIO-divider decision, *not* a PTP one — on the H563 the PTP clock is
`pll1_q_ck`, independent of the AHB divider.

**`PLLQ` = 2 with the output enabled** is the PTP-critical setting: `pll1_q_ck` is the
timestamp clock. See [ptp_clock.md](ptp_clock.md).

**`PLLN = TARGET_SYSCLK_MHZ` is exact, not approximate.** Because VCI is 2 MHz and
`PLLP` is 2, SYSCLK in MHz equals `PLLN` numerically. The H743 project relied on the same
identity at 480. Setting the target frequency drives the whole chain, and `init_pll()` needs
no arithmetic change.

**Three places state that frequency and none can include the others.** Under FreeRTOS the
single source of truth was `configCPU_CLOCK_HZ` in `FreeRTOSConfig.h`; that header went away
with the kernel in S3. The value now lives in `TARGET_SYSCLK_HZ` in `main.c`, beside the
PLL configuration that produces it, and two other declarations must agree with it by hand:

| Where | Why it cannot just include `main.c` |
|---|---|
| `SYSTEM_CLOCK` in `startup/tx_initialize_low_level.S` | Assembly; it computes the SysTick reload divisor |
| `PTP_MAIN_OSCILLATOR_FREQ_HZ` in `flexptp_options.h` | `pll1_q_ck` feeds the timestamp unit at the same 250 MHz |

Change one, change all three. A mismatch in the first gives a tick rate that is wrong by the
ratio and an RTOS whose timeouts are all skewed; a mismatch in the second gives a PTP clock
that runs at the wrong rate and a servo that cannot converge.

**APB at DIV2 (125 MHz)** matches the H743 structure and is deliberate rather than
inherited. The H563 would permit DIV1, but nothing in this project needs it: USART3's baud
divisor is computed by the HAL from PCLK1, and the Ethernet MDIO `CR` divider keys off
HCLK, not PCLK. DIV2 leaves headroom on the peripheral buses at no cost.

**`FLASH_LATENCY_5` with VOS0** is required for 250 MHz; the H743 used
`FLASH_LATENCY_4` at its own operating point.

---

## PTP Clock

The timestamp unit is clocked from **`pll1_q_ck` at 250 MHz** with a **5 ns** subsecond
increment. It is *not* clocked from HCLK — see point 1 below, and finding F53 for what
believing otherwise cost.

| Parameter | Value |
|---|---|
| `PTP_MAIN_OSCILLATOR_FREQ_HZ` | 250 000 000 (= `pll1_q_ck`) |
| `PTP_INCREMENT_NSEC` | 5 |
| Initial addend | 3435973836 (`0xCCCCCCCC`) |
| Timestamp granularity | 5 ns |
| Frequency tuning range | +25 % |
| Frequency tuning resolution | ≈ 0.29 ppb |

Three things are worth knowing without reading further:

1. **The source is fixed, the frequency is not.** The H563 has no PTP clock mux
   (`RCC_CCIPR5.ETHPTPCLKSEL` exists only on STM32H543/553xx), and RM0481 Table 121 shows
   the mux-less devices are hardwired to **`pll1_q_ck`** — *not* HCLK. So no
   `HAL_RCCEx_PeriphCLKConfig()` call is possible, but PLL1Q must be set to 250 MHz **and
   its output enabled** with `__HAL_RCC_PLL1_CLKOUT_ENABLE(RCC_PLL1_DIVQ)`.
2. **The subsecond increment must exceed the PTP clock period.** With a 4 ns clock period,
   5 ns is the smallest usable increment. Choosing 4 ns produces an addend that does not
   fit in 32 bits and a clock that never ticks.
3. **Timestamp granularity is 5 ns, the same as the H743 source project, and is not
   improved here.** A finer increment needs a *faster* PTP clock (≥ 262 MHz for a marginal
   K = 1.05). That is reachable in principle — `PLLQ = 1` would give `pll1_q_ck` = 500 MHz
   — but it is not adopted, for the engineering reasons set out in
   [ptp_clock.md](ptp_clock.md#why-a-4-ns-increment-is-not-adopted). It is not blocked by
   the 250 MHz HCLK ceiling, which has nothing to do with the PTP clock.

One setting elsewhere is load-bearing for this: **`PLLQ` must be 2 and the PLL1Q output
must be enabled**. The AHB prescaler (`RCC_SYSCLK_DIV1`, the H5 spelling) and the MDIO
divider `CR = DIV102` are paired with the 250 MHz *HCLK*, not with PTP.

> **Full derivation, the `K > 1` constraint, the PLL arithmetic, and the record of why the
> originally planned 200 MHz / PLL1_Q configuration was abandoned:
> [ptp_clock.md](ptp_clock.md).**

---

## TrustZone Migration Guide

This port runs **flat and non-secure**: `TZEN` disabled, one image, no partitioning. That is
a deliberate choice — the demo has nothing to isolate — and it is the configuration every
measurement in these documents was taken on. This section is what a migration would involve.
**None of it has been tried.**

### Current state

| Setting | Value | Where |
|---|---|---|
| `TZEN` option bit | disabled (`0xC3`) | `FLASH_OPTSR2_PRG`, RM0481 §3.6.1 |
| ThreadX port | `ports/cortex_m33/gnu` | `bazel/threadx.BUILD` |
| `TX_SINGLE_MODE_NON_SECURE` | defined | `bazel/threadx.BUILD` |
| Secure-stack sources | **excluded from the build** | `list(FILTER … EXCLUDE REGEX "secure_stack")` |
| Flash / SRAM | `0x08000000` / `0x20000000` — the non-secure aliases | `stm32h563zitx_FLASH.ld` |

ThreadX has one Cortex-M33 port rather than FreeRTOS's three, and it selects its TrustZone
behaviour with macros instead of with the port directory. The predecessor here was
`GCC_ARM_CM33_NTZ_NONSECURE`, the non-TrustZone FreeRTOS variant, so the two configurations
are equivalent.

Both halves of the current setting are needed and they fail differently:
`TX_SINGLE_MODE_NON_SECURE` removes the secure-stack calls from the port's context-switch
paths, and the `EXCLUDE REGEX` drops the five files implementing them. Excluding the files
without defining the macro fails to link; defining the macro without excluding the files
merely emits five `cmse_nonsecure_entry attribute ignored without -mcmse` warnings and links
entry points that can never be reached.

`TZEN` is an option byte, not a runtime setting: it is programmed with the debugger, it
changes which boot-address option bytes the device honours (`SECBOOTADD` instead of
`NSBOOTADD`, RM0481 §3.3.1), and it cannot be flipped from the running application.

### What a partitioned build requires

**Two images, not one.** The secure image boots, configures the partitioning, and branches to
the non-secure image. The address aliases are fixed by the architecture (RM0481 §2, Figure 6):

| Region | Non-secure | Secure |
|---|---|---|
| Flash | `0x08000000` | `0x0C000000` |
| SRAM1/2/3 | `0x20000000` | `0x30000000` |
| Peripherals | `0x40000000` | `0x50000000` |

The linker script and the vector table move accordingly; secure and non-secure have
**separate vector tables**, and SysTick, the MPU and parts of the SCB are banked (RM0481
§3.6.2), so the non-secure kernel gets its own SysTick.

**ThreadX changes.** Stop defining `TX_SINGLE_MODE_NON_SECURE` and stop filtering the
`secure_stack` sources out of `THREADX_PORT_SRC` — both in `bazel/threadx.BUILD`. That
restores `tx_thread_secure_stack.c` and its four assembly companions, which give each thread
a secure stack allocated through `tx_thread_secure_stack_allocate()` so that non-secure
threads can call into the secure world without corrupting one another's secure state.

Two consequences that do not exist under the flat build:

- **Secure stacks are per-thread and must be allocated explicitly.** A thread that calls a
  secure function without one faults. `TX_THREAD_USER_EXTENSION` is already in use here for
  the CMSIS-RTOS2 wrapper's joinable flag, so anything added there needs to coexist with it.
- **The port must be compiled with `-mcmse`** for the entry-point attributes to mean anything,
  which the current firmware does not pass.

The secure image is a separate project with its own ThreadX instance or none at all; ThreadX
does not ship a paired "secure port" the way FreeRTOS's `GCC_ARM_CM33_SECURE` does, because
the secure side is usually a thin, non-RTOS supervisor.

**Partitioning the CPU's view** uses the SAU: eight programmable regions, configurable only
in secure-privileged state, and defaulting to all-secure when TrustZone is enabled (RM0481
§3.6.3). Non-secure code that must call into the secure world needs NSC regions and `SG` entry
points.

### The trap: SAU does not govern the Ethernet DMA

This is the part that would bite, and it is stated plainly in RM0481 §3.6.3:

> *The SAU/IDAU settings are applicable only to the Cortex-M33. The other masters, like DMA,
> are not affected by these policies.*

The Ethernet MAC has its own DMA and is one of those "other masters". Partitioning memory with
the SAU does **nothing** to it. Access for non-CPU masters is governed by the **GTZC**
(RM0481 §3.6.4): `TZSC` gates securable peripherals, and the block-based controller governs
SRAM. So a partitioned build has to:

1. Assign the **ETH peripheral** in TZSC to whichever world runs the driver. A securable
   peripheral made secure-only automatically issues secure transactions when it is a bus
   master — which then have to be permitted at the memory end.
2. Assign the **SRAM3 blocks** holding `.EthDrv_sec` (descriptors and 74 888 B of buffers) so
   the ETH DMA's transactions are allowed. Descriptors and buffers are written by the DMA and
   read by the CPU, so both masters must reach the same blocks with compatible attributes.
3. Route the **ETH interrupt** to the right world. The NVIC has a per-interrupt target-state
   register; the ETH IRQ must land in the world that owns `ETHHW_ISR()`.

Getting any of these wrong does not produce a clean compile error. It produces bus faults, or
DMA writes that are silently dropped — which, on the evidence of this port, would then be
attributed to the driver, the PHY or the switch before anyone suspected the memory controller.

### Recommendation

Do not do this unless the product needs isolation. There is no PTP or networking benefit, the
Ethernet DMA path above is genuinely fiddly, and the flat build is the one with hardware
evidence behind it. If it must be done, bring up the secure/non-secure split with a trivial
application **first** and add Ethernet last, so that when DMA access fails the change that
caused it is obvious.

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

**Board precondition:** UM3115 confirms PB15 as RMII_TXD1 and warns *"Make sure that JP6 is
ON when using Ethernet"* — PB15 is shared with `I2S_A_SD` and routed through that jumper.
The board manual is therefore independent confirmation of the PB13 → PB15 change.

**Consequence for the port:** `ETHHW_InitClocks()` in `lib/eth/mac_drv.c` groups
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

The H743 project configures **PG2** as `ETH_RMII_MII_RXER` in `ETHHW_InitClocks()`. This port
drops it. Three independent facts say it could not have worked here anyway.

**1. RMII has no receive-error signal on this part.** The complete set of `ETH_RMII_*`
signals in the STM32H563ZI datasheet is seven:

```
ETH_RMII_REF_CLK   ETH_RMII_CRS_DV   ETH_RMII_RXD0   ETH_RMII_RXD1
ETH_RMII_TX_EN     ETH_RMII_TXD0     ETH_RMII_TXD1
```

The datasheet lists them as combined entries with their MII counterparts —
`ETH_MII_RX_CLK/ETH_RMII_REF_CLK`, `ETH_MII_RX_DV/ETH_RMII_CRS_DV`,
`ETH_MII_TX_EN/ETH_RMII_TX_EN` — which is worth knowing when grepping the AF tables, because
a naive text extraction splits those names across lines and invents signals that do not exist.
There is no RMII error line. The only receive-error signal the part has is
**`ETH_MII_RX_ER`**, and MII is a different interface — this board is RMII, fixed by the
50 MHz `RMII_REF_CLK` the PHY drives into PA1.

**2. PG2 has no Ethernet alternate function on the H563.** Its full AF list is
`TIM8_BKIN`, `UART12_RX`, `FMC_A12`, `LPTIM6_ETR`, `EVENTOUT` — no `ETH_*` entry at any AF
number. Configuring PG2 as `GPIO_AF11_ETH` on this part selects nothing; AF11 on PG2 is not
Ethernet. On the H563 `ETH_MII_RX_ER` appears on **PB10** and **PI10** only.

**3. MB1404 does not route it.** UM3115 contains **no mention of RXER or RX_ER anywhere**.
Its Ethernet solder-bridge table lists exactly the eight RMII signals above, and PG2 appears
only in the expansion-connector tables — Zio `CN9` pin 42 (`D49`), as a plain GPIO. The pin
goes to a header, not to the LAN8742A.

So the H743 line was vestigial in the same way the PB5 PPS setup was (see
[RMII Pin Map](#rmii-pin-map)): carried forward, configuring a pin the board does not connect.
Dropping it is not a reduction in function.

**To re-enable it** you would need MII rather than RMII — a different PHY interface, a
different pin count, `ETH_MII_RX_ER` on PB10 or PI10, and a board that routes it. That is a
hardware change, not a firmware one. Nothing in this port would need to change first.

**Practical consequence:** none. In RMII the PHY signals receive errors in-band, and the MAC
records them in the error counters. Those counters were read during the F42 investigation —
CRC, alignment and missed-frame counts were all zero — so the diagnostic value RX_ER would
have added was available anyway.

---

## mac_drv.c Comparison

The project uses a **custom register-level MAC driver** and does not call ST's HAL ETH driver
at all, even though `stm32h5xx_hal_eth.c` is compiled into the HAL library. This is inherited
from the H743 project, not a decision taken here — but it is worth understanding, because it
determines who owns the defects.

| | `lib/eth/mac_drv.c` | `stm32h5xx_hal_eth.c` |
|---|---|---|
| Size | 859 lines | 3 524 lines |
| Descriptors | `ETHHW_DescFull` = the 4 hardware DWORDs **plus a software extension** | `ETH_DMADescTypeDef`, hardware layout plus HAL-private backup fields |
| Descriptor memory | Project-allocated, `.EthDrv_sec` in SRAM3 | Application-allocated, handed over in `ETH_InitTypeDef` |
| TX timestamp delivery | Per-descriptor `tsCbPtr` / `tsCbArg` in the extension, invoked from `ETHHW_ProcessTx()` | `HAL_ETH_TxPtpCallback(uint32_t *buff, ETH_TimeStampTypeDef *ts)`, keyed on the buffer pointer |
| RX timestamp | Context descriptor paired in `ETHHW_ProcessRx()`, carried on the event | `HAL_ETH_PTP_GetRxTimestamp()` |
| PTP clock control | `ETHHW_{Init,Set,Get}PTP*` — thin wrappers over `MACTSCR`/`MACSSIR`/`MACTSAR` | `HAL_ETH_PTP_SetConfig()` and friends, with a configured/not-configured state machine |
| State | One `ETHHW_State` struct in the descriptor region | `ETH_HandleTypeDef` with `gState`, `RxState`, error codes, callbacks |

### Why the custom driver exists

The deciding feature is the **descriptor extension**. flexPTP needs a transmit timestamp
matched back to the message that produced it, and `ETHHW_DescExt` carries a callback pointer
and a caller tag alongside each descriptor, so the match is structural. ST's HAL solves the
same problem with `HAL_ETH_TxPtpCallback()` keyed on the buffer address, which works but ties
the identity to a pointer the application must keep alive and unique.

The H743 project made this choice; this port inherited it. Rewriting on top of the HAL was
never in scope, and doing so now would discard the one part of the system that has been
exercised hardest.

### What it cost

Everything the HAL would have provided had to be got right locally, and three defects in this
area were found on hardware during this port:

| | |
|---|---|
| **F57** | `ETHHW_ProcessTx()` kept per-pass scan state outside the loop, so a drained ring could re-dispatch a descriptor |
| **F61** | `ETHHW_ProcessRx()` judged descriptors by the OWN bit alone and re-delivered frames already queued — **57 % of all frames** under ordinary PTP traffic |
| **F54** | Six unbounded hardware waits, any of which turned a clock fault into a silent hang |

None of these would have existed with the HAL driver, which is far more widely exercised. On
the other side, the HAL brings its own weight — a handle state machine, a callback registry —
and its PTP support is newer than the H743 project. The honest summary is that the custom
driver is smaller and fits flexPTP better, and that this port paid for it in debugging time.

### Differences from the H7 implementation

The driver is a register-level port, so the H7 → H5 changes are register and pin changes
rather than structural ones. The substantive ones are recorded in
[Stage 12](#stage-12--ethernet-mac-driver-port) and
[port_review.md](port_review.md): `SYSCFG` → `SBS` for the RMII selection (F11), the MDIO
`CR` divider that double-shifted an already-positioned mask (F8), `RMII_TXD1` moving
PB13 → PB15 (F7), and PG2/PB5 dropped (above, and the RMII pin map). The descriptor layout,
the ring-walking logic and the callback mechanism are unchanged from the H743 version — which
is precisely why F57 and F61 are latent there too.

---

## Caches and DMA Coherency

**Both caches are off, and nothing in this project turns them on.** That is why the Ethernet
DMA regions in SRAM3 need no cache maintenance as built, and it is worth stating explicitly
because the assumption is invisible in the code — there is no maintenance call to notice the
absence of.

| Check | Result |
|---|---|
| ICACHE after reset | Disabled — `EN` in `ICACHE_CR` resets to 0 (RM0481 §8.7) |
| DCACHE after reset | Disabled — `EN` in `DCACHE_CR` resets to 0 (RM0481 §9.7) |
| `HAL_Init()` | Sets NVIC priority grouping and the SysTick source. Does not touch either cache |
| `HAL_ICACHE_MODULE_ENABLED` / `HAL_DCACHE_MODULE_ENABLED` | Both commented out in `lib/runtime/stm32h5xx_hal_conf.h`, so the drivers are not even compiled |
| Application code | No `HAL_ICACHE_Enable()`, `HAL_DCACHE_Enable()`, `SCB_EnableICache()` or `SCB_EnableDCache()` anywhere in the first-party sources (`projects/`) |

The H7 needed care here: the Cortex-M7 has an architectural D-cache, and the H743 project put
the Ethernet buffers in AXI-SRAM (`RAM_D1`) for that reason — the comment carried into
`stm32h563zitx_FLASH.ld` records it as "accessible by the Ethernet DMA without cache coherency
issues (when MPU was configured correctly)". **This project configures no MPU regions at all**
— there is no `HAL_MPU_Enable()` or `ARM_MPU_*` call anywhere in the first-party
sources (`projects/`) — which is consistent, because with both caches off there is nothing to be coherent
with. The H5 has no Cortex-M7 cache; ICACHE and DCACHE are separate peripherals on the bus,
disabled at reset.

**If you enable DCACHE**, the SRAM3 Ethernet region becomes a coherency problem immediately —
descriptors are written by the DMA and read by the CPU, and buffers likewise. Either mark
`.EthDrv_sec` non-cacheable through the MPU, or add clean/invalidate around every descriptor
access. The linker script carries a comment to this effect at the `.EthDrv_sec` placement.
Note that the region is 74 888 B spanning descriptors *and* buffers, so the MPU region needs
to cover both.

---

## Stage Log

> **Historical — the first port only.** What follows records the H743 → H563 silicon port,
> Stages 1–20, as it happened. It describes a FreeRTOS/lwIP firmware, and several statements
> in it are no longer true of the code in this repository: Stage 9 configures a kernel that has
> since been replaced, Stage 12's driver has gained a NetX Duo sibling, and the memory figures
> predate the stack swap.
>
> It is **deliberately not updated**. Rewriting a log to match a later state destroys the one
> thing a log is for — knowing what was believed, and when. For the current state see
> [architecture.md](architecture.md); for the second port's stage-by-stage record see
> [threadx_porting_plan.md](threadx_porting_plan.md).

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
- **`AHBCLKDivider` = `RCC_SYSCLK_DIV1`** (HCLK 250 MHz) and APB
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

- `ETHHW_UpdatePTPTime()` removed entirely (F41): unreachable, and its subtract path
  halved every backward correction. Fixing code that has never run would have produced
  something that looks verified and is not.
- `phy_common.c` needed no change — it reaches the HAL only through `mac_drv.h`
- Verified: `mac_drv.c` and `phy_common.c` both compile clean, zero warnings

### Stage 13 — flexPTP options
- `PTP_MAIN_OSCILLATOR_FREQ_HZ` 240 MHz → **250 MHz** (= `pll1_q_ck`), `PTP_INCREMENT_NSEC` 5.
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

### Hardware bring-up (partial Stage 20)
Flashed via the ST-Link mass-storage volume; console read from the ST-Link VCP on
`/dev/ttyACM0` (USART3 routes there by default on this board, so no external wiring is
needed). Debugging via ST's OpenOCD fork built in Stage 18.

**Confirmed working on silicon:**

| Area | Evidence |
|---|---|
| Startup, vector table | Reaches `main()`; banner prints |
| Clock tree | `RCC_CFGR1` SWS = PLL1, `PLL1DIVR` M=4/N=250/P=2 → SYSCLK 250 MHz, `CFGR2` HPRE=0 → HCLK 250 MHz, PPRE1/2/3=÷2 → 125 MHz |
| Power / flash | `VOSCR` = `0x30` (VOS0), `VOSSR` VOSRDY+ACTVOSRDY set, `FLASH_ACR` latency 5 |
| FreeRTOS on M33 | Scheduler runs; banner prints from a task |
| GPDMA console | Full 1561-byte banner transmits |
| MDIO divider fix (F8) | PHY enumerated: *SMSC LAN8742A (rev. 1) @0* |
| RMII incl. PB13→PB15 | *ETH LINK: UP (100 Mbps, FULL duplex)* |
| Ethernet TX+RX, MAC addr | **DHCP lease acquired: 192.168.1.113** |
| PTP addend derivation | `ETH->MACTSAR` = `0xCCCCCCCC`, matching [ptp_clock.md](ptp_clock.md) exactly |

**Two hardware-only defects found and fixed:** F51 (UID region rejects byte reads) and
F52 (PTP addend/increment ordering). Both compiled and linked cleanly and were invisible
to static review.

**F53 resolved.** The PTP timestamp counter is now running: `PLL1QEN` was never set, so
`pll1_q_ck` — the PTP reference clock on this device — was never generated. Fixed by
`PLLQ` 4 → 2 plus `__HAL_RCC_PLL1_CLKOUT_ENABLE(RCC_PLL1_DIVQ)`. Verified:
`MACSTSR` advances 7 → 11 across ~4 s, `TSADDREG` clears, `MACTSAR` = `0xCCCCCCCC`.

**Open at the time of this stage — since resolved:**
- **F54** — *(resolved)* `ETHHW_SetPTPAddend()` waited on `TSADDREG` with no timeout, eight times over.
  This is what turned F53 from "PTP does not lock" into a silent hang of the `ptp` thread,
  taking the PTP CLI commands down with it. Inherited from the H743 driver, where the wait
  always completed. Deliberately not yet fixed: a silent timeout would trade one invisible
  failure for another, so the right form reports the error rather than swallowing it.

**PTP transport:** the app loads the `default` preset, which is `PTP_TP_IPv4` (UDP/IPv4,
multicast 224.0.1.129) with `PTP_DM_E2E`. A linuxptp master needs no config file to match it:
`ptp4l -i <iface> -S -m --serverOnly=1 --priority1=100`.

> This was `gPTP` (`PTP_TP_802_3`, ethertype 0x88F7, `PTP_DM_P2P`) until the interoperability
> results below settled it. gPTP cannot traverse ordinary bridges by design, so the boot
> preset was changed to the one that works on a general-purpose network — see
> [ptp_profiles.md](ptp_profiles.md). The gPTP route still works and is still documented; it
> just is no longer the default.

### Verification limits

Two of the Stage 20 verification items cannot be completed with the equipment here, and the
distinction matters when reading everything above:

- **The offset figures in these documents measure the host, not the board.** The host NIC
  reports `PTP Hardware Clock: none`, so `ptp4l` runs with software timestamping and the floor
  is tens of microseconds against a 100 ns `LOCKED` threshold. What is demonstrated is that
  the servo engages and holds the offset centred on the master; the board's own accuracy is
  **unmeasured**.
- **1PPS is measured and correct.** PG8 was sampled over SWD at ~1 kHz: `ptp pps 1` gives
  **1.000 Hz** on the pin (eight consecutive periods within 1 ms of nominal), `ptp pps 2`
  gives 2.001 Hz, `ptp pps 4` gives 3.983 Hz, and `ptp pps 0` produces no transitions at all.
  Configuration state is not a waveform — so the waveform was measured. Only **jitter**
  remains unquantified, and that does need an instrument.

Both are detailed in [implementation_plan.md](implementation_plan.md#blocked-on-equipment),
including what would close each.

### Hardware bring-up — PTP interoperability
Tested against `linuxptp` 4.4 on a Linux host.

- **gPTP (802.1AS) over the site switch does not work, by design.** The destination MAC
  `01:80:C2:00:00:0E` is in the IEEE 802.1D reserved range that conformant bridges must not
  forward. Confirmed by `tcpdump`: the host's Pdelay_Req frames leave and nothing returns,
  and ptp4l never progresses past Pdelay_Req because a gPTP port will not emit Sync or
  Announce until the peer-delay exchange proves the link `asCapable`. gPTP needs either a
  direct cable or 802.1AS-capable bridges throughout.
- **Plain IEEE 1588 over UDP/IPv4 reaches the board through the switch — except for
  `Delay_Resp`.** With the `default` preset and ptp4l at its defaults, Announce, Sync,
  Follow_Up and Delay_Req all flow. `Delay_Resp` did **not**: see the F42 entry below, which
  supersedes the earlier form of this bullet. That earlier wording claimed all five messages
  flowed correctly, and it was written before anyone counted `Delay_Resp` arrivals — it is
  the unverified assumption that F42 cost days to falsify.
- **BMCA works**: the board goes `MASTER → UNCALIBRATED → SLAVE` once the host advertises
  a better `priority1`.
- **Receive-side hardware timestamping works** — T1/T2 carry real values.
- **Transmit-side hardware timestamping also works.** Instrumentation counters read over
  SWD show 698 timestamps requested, captured, found in the descriptor and dispatched —
  698 for 698, no losses. The project's MAC driver and lwIP glue are correct.
- **PTP synchronises.** 150 samples against a `linuxptp` grandmaster give a mean time error
  of **−535 ns**, best sample −69 ns, with the addend actively tuned around `0xCCCCCCCC`
  and settling ~41 **ppm** below nominal — the ST-Link MCO's real frequency error, which is
  an unremarkable magnitude for an ordinary crystal. (This read "ppb" until re-measured on
  2026-08-12: the magnitude was right, the unit wrong by 1000. Note that the `corr_ppb`
  column is a *per-cycle increment* and not this quantity — see
  [port_review.md](port_review.md).) The ±83 µs
  spread is host-side software timestamping, not the board.
- **F42 resolved: the switch was discarding Delay_Resp.** Direct cable, nothing else
  changed: 65 Delay_Resp received (≈1/s) and zero failures, versus 0 and continuous failure
  through the switch. Sync, Follow_Up and Announce crossed the switch normally on the same
  multicast group and MAC. Nothing in this port was at fault. See
  [port_review.md](port_review.md).
- **PTP locks on a direct link.** After `ptp reset`, offset converged 5.19 ms → −45 µs with
  the addend actively tuned and 50 of 67 samples applying a correction. The residual
  ±10–50 µs is the host's software-timestamping floor, not the board.
- **F59 resolved: one duplicated Sync cycle latched the servo into NaN.** A repeated Sync
  timestamp makes the measured Sync period zero; the Kalman filter then computes `0.0/0.0`
  and NaN propagates through its state for ever. Nothing in flexPTP tested for it, and two
  Arm conversion behaviours hid it — NaN printed as `0.0000` and tuned the addend by zero,
  so every visible indicator stayed green while the clock free-ran at ~18.7 ppm. Fixed with
  three guards (`patches/flexptp/0002-…`); after the fix, 190 of 191 samples apply a
  correction and the offset is centred on the master instead of drifting (signed mean
  +40 ns, typical magnitude 26 µs — the host's software-timestamping floor).
- **F60 resolved: embfmt could not print `%ll` and rendered NaN as `0.0000`.** The mean path
  delay and measured Sync period columns were blank on every servo line
  (`patches/embfmt/0001-…`).
- **F61 resolved: `ETHHW_ProcessRx()` re-delivered frames already queued.** The duplicated
  Sync of F59 shared T1, T2 *and* offset, so it was one frame delivered twice. The cause was
  the OWN bit being the only "already handled" state, which stays clear until the receive
  thread restores the descriptor — measured at **57% of all frames** under ordinary PTP
  traffic, not the rare event it appeared to be. Fixed with an `inFlight` marker in the
  descriptor extension.

---

### Stage 20 — hardware verification results

Measured on a clean build of `1e446fb`, flashed over the ST-Link mass-storage device, against
`ptp4l -i enp5s0 -S -m --serverOnly=1 --priority1=100` on a directly-connected host.

**Build**

| | |
|---|---|
| Warnings / errors | **0 / 0** (`-Wall -Wextra`, both presets) |
| FLASH | 223 032 B — 10.6% of 2 MB |
| SRAM1 | 133 328 B — 50.9% of 256 KB |
| SRAM2 | 0 B — unused |
| SRAM3 | 74 888 B — 22.9% of 320 KB (ETH descriptors, buffers, lwIP heap) |

**Boot and network**

```
PHY: SMSC LAN8742A (rev. 1) @0
ETH LINK: UP (100 Mbps, FULL duplex)
IP-address: 192.168.2.141
```

No fault, HardFault or driver-timeout report. Ping 0% loss at 56 and 1400 bytes, the latter
with `-p deadbeef` pattern verification, so payload integrity is checked and not merely
delivery.

**PTP clock, against the derivation in [ptp_clock.md](ptp_clock.md)**

| Check | Expected | Measured |
|---|---|---|
| `ptp addend` before tuning | `2³²/1.25` = 3435973836 | **3435973836** — exact |
| Addend under lock | tracks around nominal, does not rail | 3435793666 … 3435978364 |
| Servo variances | 1e-16, 1e-12, 5e-11 | **1.000000e-16, 1.000000e-12, 5.000000e-11** |
| PPS frequency | 1 Hz | reports 1 Hz (configuration only — see [Verification limits](#verification-limits)) |

**BMCA and servo**

```
INITIALIZING -> LISTENING -> UNCALIBRATED -> SLAVE
```

| | |
|---|---|
| Samples | 137 |
| Applying a correction | **136** (the one exception is the servo's own first cycle) |
| Duplicate corrections | **0** — the F59 signature test |
| Driver timeout reports | none |
| Offset, last 100 | signed mean **−716 ns**, magnitude 28.5 µs, peak 77.8 µs |
| Mean path delay, median | 36.6 µs |

**Reading those last two rows correctly.** The signed mean near zero is the result that
matters: it says the servo is locked and centred on the master rather than drifting. The
magnitude and the 36.6 µs path delay are **not** measurements of this board — a direct metre
of cable is nanoseconds, and 36 µs is the host's software timestamping asymmetry. See
[Verification limits](#verification-limits).

**CLI**

All 23 commands swept in their query form; none returned *"Unknown command or bad parameter"*.
The sweep found [F66](port_review.md#f66--embfmt-misreports-small-e-values-and-any-rounding-carry-resolved),
where the three servo variance commands reported `~5.1e-8` for values of 1e-16, 1e-12 and
5e-11; fixed, and correct in the table above.

**Status.** Everything achievable with the equipment here is verified. Two items remain open
and are blocked on instruments, not on the port —
[LOCKED and 1PPS](implementation_plan.md#blocked-on-equipment).
