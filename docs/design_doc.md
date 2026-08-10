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
- [Caches and DMA Coherency](#caches-and-dma-coherency)
- [Stage Log](#stage-log)

Companion documents: [architecture.md](architecture.md) (module stack, threads, interrupts) ·
[host_ptp_setup.md](host_ptp_setup.md) (running a PTP master on the host) ·
[ptp_clock.md](ptp_clock.md) (PTP clock derivation) ·
[port_review.md](port_review.md) (review findings and verified-clean audit) ·
[implementation_plan.md](implementation_plan.md) (staged work plan)

---

## Port Overview

**Source**: NUCLEO-H743ZI — STM32H743ZI, Cortex-M7 @ 480 MHz, DP-FPU, stm32h7xx HAL  
**Target**: NUCLEO-H563ZI — STM32H563ZI, Cortex-M33 @ 250 MHz, SP-FPU, stm32h5xx HAL

Key decisions:
- PTP clock: **`pll1_q_ck` 250 MHz** / 5 ns increment (fixed source, no mux on this
  device — see [PTP Clock](#ptp-clock))
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
the CPU/AHB clock. The PTP timestamp clock is separate — `pll1_q_ck`, also 250 MHz.

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

EtherLib is Epagris' in-house Ethernet stack and an alternative to lwIP. It is present as a
submodule and selectable with `ETH_STACK` in the top-level `CMakeLists.txt`:

```cmake
set(ETH_STACK "LWIP")   # or "ETHERLIB"
```

**It is not compiled in this port and has never been built for the H563.** Only the lwIP path
is exercised. The pieces that would have to be checked are listed below; none is known to be
broken, but none has been tried either.

### What already exists

| Piece | State |
|---|---|
| `Modules/etherlib` submodule | Present, pinned at `ec35be5` |
| `Inc/etherlib_options.h` | Complete — pool size, ARP cache, timers, and the CMSIS-OS2 binding |
| `Src/etherlib_options.c` | Provides `ETHLIB_OS_THREAD_CREATE()` and the recursive-mutex attribute |
| `Drivers/EthDrv/eth_drv_etherlib.c` | The EtherLib-side driver shim, alongside `eth_drv_lwip.c` |
| `Modules/CMakeLists.txt` | Already branches on `ETH_STACK` and wires `FLEXPTP_NSD` to match |

So the wiring is in place. What is missing is a linker placement, and verification.

### The `.ETHLibPool` placement — measure it, do not assume

`Inc/etherlib_options.h` puts EtherLib's 32 KB dynamic pool into a named section:

```c
#define ETHLIB_MEMORY_POOL_TOTAL_SIZE (2 * 16384)
#define ETHLIB_MEMORY_POOL_ATTRIBUTES section(".ETHLibPool")
```

`Modules/etherlib/dynmem.c` then declares `sDynMemPool` with that attribute and hands it to
`mp_init()`. **The linker script has no placement for `.ETHLibPool`** — neither here nor in
the H743 project (finding F29).

The consequence was measured rather than guessed, by compiling a stand-in 32 KB array with
the same section attribute into the current build:

| | Without a live reference | With a live reference |
|---|---|---|
| Outcome | Silently **discarded** — the section appears in the map's *Discarded input sections* list | Placed as an **orphan** output section |
| Placement | — | VMA `0x20000a6c` (SRAM1, immediately after `.data`), **load address `0x080366c0` in FLASH** |
| FLASH | unchanged | **222 904 → 255 680 B** (+32 KB) |
| SRAM1 | unchanged | **133 328 → 166 096 B** (+32 KB) |

The build is compiled with `-fdata-sections -ffunction-sections -Wl,--gc-sections`, which is
why the unreferenced case vanishes without a word. With EtherLib actually compiled the pool
*is* referenced, so the second column is what you get — and it is wrong in two ways:

1. **32 KB of zeros are stored in the image.** The orphan section is `PROGBITS` with a load
   address, so it occupies flash for no purpose.
2. **The pool is never initialised.** The startup copy loop runs `_sdata` … `_edata`, and the
   measured symbols were `_edata = 0x20000a6c` and `sTestPool = 0x20000a6c` — the pool starts
   exactly where the copy stops. `_sbss` was `0x20008a70`, *after* the pool, so the zeroing
   loop skips it too. It is neither copied nor cleared, and holds whatever the SRAM powered
   up with.

The fix is a `NOLOAD` placement next to the other Ethernet regions in
`stm32h563zitx_FLASH.ld`:

```ld
  .ETHLib_sec (NOLOAD) :
  {
    . = ALIGN(4);
    *(.ETHLibPool)
    . = ALIGN(4);
  } >SRAM3
```

SRAM3 has room — the Ethernet buffers and the lwIP heap use 74 888 B of 320 KB, and with lwIP
deselected the 16 KB lwIP heap is not there at all. `NOLOAD` removes the flash cost and makes
the "uninitialised" status explicit rather than accidental; `mp_init()` initialises the pool
header itself, so zeroing is not required.

### Remaining work to enable EtherLib

1. Add the linker placement above.
2. Build with `ETH_STACK=ETHERLIB` and fix what falls out. `Src/etherlib_options.c`,
   `Drivers/EthDrv/eth_drv_etherlib.c` and `Src/ethernet/ethernet_etherlib.c` have **never
   been compiled** in this port — the F51 UID lesson applies, since
   `eth_drv_etherlib.c:97` builds its MAC from the same `ETH_MAC_ADDR*` macros.
3. Check the flexPTP network-stack driver: `FLEXPTP_NSD` follows `ETH_STACK`, so the
   EtherLib NSD gets selected automatically, but the `tx_cb` contract that bit us in F48 is
   lwIP-specific and the EtherLib equivalent has not been checked.
4. `CHECKSUM_BY_HARDWARE` in `Inc/lwipopts.h` is lwIP-only; EtherLib has its own arrangement.
5. Re-run the PTP verification. EtherLib changes the RX/TX path completely, so F58 and F61 —
   both in `eth_drv_lwip.c` and `mac_drv.c` respectively — need re-examining on that path.
   `mac_drv.c` is shared, so the F61 fix carries over; the F58 pbuf-copy fix does not, because
   EtherLib has no pbufs.

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
| FreeRTOS port | `GCC_ARM_CM33_NTZ_NONSECURE` | `Middlewares/CMakeLists.txt` |
| `configENABLE_TRUSTZONE` | `0` | `Inc/FreeRTOSConfig.h` |
| `configRUN_FREERTOS_SECURE_ONLY` | `0` | `Inc/FreeRTOSConfig.h` |
| `configENABLE_MPU` | `0` | `Inc/FreeRTOSConfig.h` |
| Flash / SRAM | `0x08000000` / `0x20000000` — the non-secure aliases | `stm32h563zitx_FLASH.ld` |

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
§3.6.2), so the non-secure FreeRTOS gets its own SysTick.

**FreeRTOS changes.** Swap `FREERTOS_PORT` from `GCC_ARM_CM33_NTZ_NONSECURE` to the
TrustZone-aware **`GCC_ARM_CM33_NONSECURE`** in `Middlewares/CMakeLists.txt`, and set
`configENABLE_TRUSTZONE = 1`. The secure image builds the companion **`GCC_ARM_CM33_SECURE`**
port, which supplies `secure_context.c`, `secure_context_port.c` and `secure_heap.c` — the
context management that lets non-secure tasks call secure functions. Both port names are
already recognised by the fetched kernel (`portable/CMakeLists.txt`), and the sources sit
side by side under `portable/GCC/ARM_CM33/{non_secure,secure}` and `ARM_CM33_NTZ/non_secure`.

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

| | `Drivers/EthDrv/mac_drv.c` | `stm32h5xx_hal_eth.c` |
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
| `HAL_ICACHE_MODULE_ENABLED` / `HAL_DCACHE_MODULE_ENABLED` | Both commented out in `Inc/stm32h5xx_hal_conf.h`, so the drivers are not even compiled |
| Application code | No `HAL_ICACHE_Enable()`, `HAL_DCACHE_Enable()`, `SCB_EnableICache()` or `SCB_EnableDCache()` anywhere in `Src/`, `Drivers/EthDrv/` or `Inc/` |

The H7 needed care here: the Cortex-M7 has an architectural D-cache, and the H743 project put
the Ethernet buffers in AXI-SRAM (`RAM_D1`) for that reason — the comment carried into
`stm32h563zitx_FLASH.ld` records it as "accessible by the Ethernet DMA without cache coherency
issues (when MPU was configured correctly)". **This project configures no MPU regions at all**
— there is no `HAL_MPU_Enable()` or `ARM_MPU_*` call anywhere in `Src/`, `Drivers/EthDrv/` or
`Inc/` — which is consistent, because with both caches off there is nothing to be coherent
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

**Open:**
- **F54** — `ETHHW_SetPTPAddend()` waits on `TSADDREG` with no timeout, eight times over.
  This is what turned F53 from "PTP does not lock" into a silent hang of the `ptp` thread,
  taking the PTP CLI commands down with it. Inherited from the H743 driver, where the wait
  always completed. Deliberately not yet fixed: a silent timeout would trade one invisible
  failure for another, so the right form reports the error rather than swallowing it.

**PTP transport:** the app loads the `gPTP` preset, which is `PTP_TP_802_3` (Layer 2,
ethertype 0x88F7) with `PTP_DM_P2P` — not UDP/IPv4. A linuxptp master must therefore use
the gPTP profile, e.g.
`ptp4l -f /usr/share/doc/linuxptp/configs/gPTP.cfg -i <iface> -S -m --serverOnly=1 --priority1=100`.

### Verification limits

Two of the Stage 20 verification items cannot be completed with the equipment here, and the
distinction matters when reading everything above:

- **The offset figures in these documents measure the host, not the board.** The host NIC
  reports `PTP Hardware Clock: none`, so `ptp4l` runs with software timestamping and the floor
  is tens of microseconds against a 100 ns `LOCKED` threshold. What is demonstrated is that
  the servo engages and holds the offset centred on the master; the board's own accuracy is
  **unmeasured**.
- **1PPS has never been observed.** PG8 is configured and the CLI reports 1 Hz, but no scope
  has been on the pin. Configuration state is not a waveform.

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
- **Plain IEEE 1588 over UDP/IPv4 traverses the switch.** Switching the board to the
  `default` preset (`ptp profile preset default`) and running ptp4l with its defaults gets
  Announce, Sync, Follow_Up, Delay_Req and Delay_Resp all flowing correctly.
- **BMCA works**: the board goes `MASTER → UNCALIBRATED → SLAVE` once the host advertises
  a better `priority1`.
- **Receive-side hardware timestamping works** — T1/T2 carry real values.
- **Transmit-side hardware timestamping also works.** Instrumentation counters read over
  SWD show 698 timestamps requested, captured, found in the descriptor and dispatched —
  698 for 698, no losses. The project's MAC driver and lwIP glue are correct.
- **PTP synchronises.** 150 samples against a `linuxptp` grandmaster give a mean time error
  of **−535 ns**, best sample −69 ns, with the addend actively tuned around `0xCCCCCCCC`
  and settling ~41 ppb below nominal — the ST-Link MCO's real frequency error. The ±83 µs
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
- **F61 open**: the duplicated Sync of F59 shared T1, T2 *and* offset, so one received frame
  was delivered twice. Seen once in ~150 cycles; now harmless, but not root-caused.
