# PTP Clock Design — STM32H563ZI

Detailed rationale for the PTP timestamp clock configuration of this port.
Summarised in [design_doc.md](design_doc.md#ptp-clock); this document holds the derivation.

**Conclusion up front:** the timestamp unit is clocked from **HCLK at 250 MHz** with a
**5 ns** subsecond increment. This is not a preference — it is the only configuration the
STM32H563 supports. Timestamp granularity is therefore 5 ns, identical to the H743 source
project, and cannot be improved on this part.

## Table of Contents

- [Why there is no clock source to choose](#why-there-is-no-clock-source-to-choose)
- [How the hardware clock works](#how-the-hardware-clock-works)
- [The K > 1 constraint](#the-k--1-constraint)
- [Why a 4 ns increment is unreachable](#why-a-4-ns-increment-is-unreachable)
- [Selected configuration](#selected-configuration)
- [Coupled decisions](#coupled-decisions)
- [History](#history)

---

## Why there is no clock source to choose

Later STM32H5 devices expose an Ethernet PTP kernel-clock multiplexer — `ETHPTPCLKSEL`
in `RCC_CCIPR5`, selecting between HCLK, PLL1R, PLL1Q and PLL3P, with an additional
`ETHPTPDIV` prescaler in `RCC_CCIPR4`. **The STM32H563 has neither field.**

```
$ grep -E 'RCC_CCIPR5_ETHPTPCLKSEL|RCC_CCIPR4_ETHPTPDIV|ETHCLKSEL|ETHREFCLKSEL' \
      Drivers/CMSIS/Device/ST/STM32H5xx/Include/stm32h563xx.h
(no matches)
```

The only Ethernet-related RCC bits on this device are the three clock gates
`RCC_AHB1ENR_ETHEN` / `ETHTXEN` / `ETHRXEN`.

Because the family HAL must serve devices that *do* have the mux, every ETHPTP
clock-selection routine is guarded:

```c
/* Drivers/stm32h5xx-hal-driver/Src/stm32h5xx_hal_rcc_ex.c */
#if defined(RCC_CCIPR5_ETHPTPCLKSEL)
  ...
#endif
```

On an H563 build those blocks compile out entirely, and `RCC_PERIPHCLK_ETHPTP` is never
defined. Reading only `stm32h5xx_hal_rcc_ex.h` gives the misleading impression that the
mux is available — it is a family-wide header, not a device header. **Always confirm
against `stm32h563xx.h`.**

The practical consequence: `ptp_ker_ck` is hardwired to HCLK. Configuring the PTP clock
requires *no RCC work at all*, and conversely, the PTP clock cannot be changed
independently of the CPU clock.

## How the hardware clock works

The MAC timestamp unit runs in **fine correction** mode (`MACTSCR.TSCFUPDT`, set by
`ETHHW_EnablePTPFineCorr()`). The mechanism is:

1. A 32-bit accumulator is incremented by the **addend** (`MACTSAR`) on every PTP
   reference clock edge.
2. When the accumulator overflows, the nanosecond counter advances by the **subsecond
   increment** SSINC (`MACSSIR[23:16]`).

So the rate at which the clock advances is:

```
ns per real second  =  f_ptp × (addend / 2³²) × SSINC
```

For a nominally correct clock this must equal 10⁹, giving

```
addend = 2³² × 10⁹ / (f_ptp × SSINC)
```

which is exactly what flexPTP computes in
`Modules/flexptp/src/flexptp/ptp_defs.h`:

```c
#define PTP_CLOCK_TICK_FREQ_HZ  (1000000000 / PTP_INCREMENT_NSEC)
#define PTP_ADDEND_INIT         (((double)(0x100000000)) / \
                                 (((double)(PTP_MAIN_OSCILLATOR_FREQ_HZ)) / \
                                  ((double)(PTP_CLOCK_TICK_FREQ_HZ))))
```

and passes to the hardware port at `ptp_core.c:54`:

```c
PTP_HW_INIT(PTP_INCREMENT_NSEC, (uint32_t)PTP_ADDEND_INIT);
```

Note the cast to `uint32_t` — it matters, see below.

`ETHHW_SetPTPSubsecondIncrement()` in `Drivers/EthDrv/mac_drv.c` writes
`increment << 16`, which lands correctly in SSINC on the H563
(`ETH_MACMACSSIR_SSINC_Pos = 16`). The sub-nanosecond field SNSINC at `[15:8]` is left at
zero, matching the H743 build.

## The K > 1 constraint

Define the dimensionless quantity

```
K = f_ptp × increment_ns / 10⁹          addend = 2³² / K
```

`MACTSAR` is a 32-bit register and flexPTP casts the computed addend to `uint32_t`.
Therefore:

> **K must be strictly greater than 1.**
>
> Equivalently: **the subsecond increment must be larger than the PTP clock period.**

Physically this is the servo's headroom. `K − 1` is the entire fraction by which the
clock can be sped up; at K = 1 the accumulator overflows on every single clock and there
is no way to run faster. The cost of a larger K is only tuning resolution, which stays
far below 1 ppb for any sane choice.

Candidate configurations:

| Configuration | f_ptp | increment | K | addend | Outcome |
|---|---|---|---|---|---|
| H743 source project | 240 MHz | 5 ns | **1.20** | 3579139413 (`0xD5555555`) | ✅ +20 % headroom |
| Original plan, initial | 200 MHz | 5 ns | **1.00** | 2³² → casts to **0** | ❌ clock never ticks |
| Original plan, "optimised" | 250 MHz | 4 ns | **1.00** | 2³² → casts to **0** | ❌ clock never ticks |
| **Selected** | **250 MHz** | **5 ns** | **1.25** | 3435973836 (`0xCCCCCCCC`) | ✅ +25 % headroom |
| HCLK left at DIV2 | 125 MHz | 5 ns | 0.625 | 2576980377 (`0x99999999`) | ❌ silent, clock at 0.375× |

The addend column shows the value after the `(uint32_t)` cast, as verified by
constant-folding the flexPTP macro under GCC. Note that both failing cases are formally
undefined behaviour — the computed `double` exceeds `UINT32_MAX` — so the precise value is
compiler-dependent and must not be relied upon. GCC folds the K = 1.0 cases to 0 and the
K = 0.625 case to a modular wrap; a saturating conversion would give `0xFFFFFFFF` instead.
The point is not the specific value but that K ≤ 1 has no usable result.

Both configurations in the original plan sit **exactly on the singularity**:
200 × 5 = 1000 and 250 × 4 = 1000. That is not coincidence — both follow from the
intuition *"make the increment equal the clock period"*, which is precisely the one value
that cannot work. The failure mode is a `MACTSAR` write of zero and a timestamp unit that
never advances; it presents as dead PTP hardware rather than as an arithmetic error.

The last row is the reason the AHB prescaler matters here (see
[Coupled decisions](#coupled-decisions)): a K below 1 does not produce zero, it produces a
wrapped addend and a clock running at a wrong-but-plausible rate — considerably harder to
diagnose than a dead clock.

## Why a 4 ns increment is unreachable

Timestamp **granularity equals SSINC**, not the clock frequency: the nanosecond counter
advances in increment-sized steps, so captured timestamps quantise to the increment.
Improving granularity therefore requires a *smaller* increment — which, by the K > 1
constraint, requires a *faster* clock:

```
f_ptp = K × 10⁹ / increment
```

For a 4 ns increment:

| Desired headroom | Required f_ptp |
|---|---|
| K = 1.20 (H743 parity) | 300 MHz |
| K = 1.10 | 275 MHz |
| K = 1.05 (marginal) | 262.5 MHz |

The H563 ceiling is **250 MHz** (HCLK max at VOS0), and HCLK is the only source. **A 4 ns
increment is unavailable at any K.** 5 ns is the floor on this part.

This is counterintuitive enough to be worth restating: moving from a 480 MHz M7 to a
250 MHz M33 does *not* cost timestamp resolution, and the extra 10 MHz of PTP clock
(250 vs 240) does *not* buy any. Granularity is 5 ns on both boards. The only things that
change are tuning range (+25 % vs +20 %) and tuning resolution:

`PTP_ADDEND_CORR_PER_PPB_F = 2³² / (increment × f_ptp)`

| | addend LSB per ppb | resolution |
|---|---|---|
| H743 (240 MHz, 5 ns) | 3.579 | 0.279 ppb |
| H563 (250 MHz, 5 ns) | 3.436 | 0.291 ppb |

A 4 % difference — negligible next to the ST-Link MCO phase noise the project README
already warns about.

An unexplored avenue for *rate* resolution (not granularity) is the SNSINC
sub-nanosecond increment field, currently left at zero. This has not been evaluated and
would need checking against the reference manual before being relied on.

## Selected configuration

```c
/* Inc/flexptp_options.h */
#define PTP_MAIN_OSCILLATOR_FREQ_HZ (250000000)  /* = HCLK; H563 has no ETH PTP clock mux */
#define PTP_INCREMENT_NSEC (5)                   /* must exceed the 4 ns HCLK period      */
```

Derived values:

| Quantity | Value |
|---|---|
| `PTP_CLOCK_TICK_FREQ_HZ` | 200 000 000 |
| `PTP_ADDEND_INIT` | 3435973836 (`0xCCCCCCCC`) |
| K (tuning headroom) | 1.25 → +25 % |
| Timestamp granularity | 5 ns |
| Tuning resolution | ≈ 0.29 ppb |

No `HAL_RCCEx_PeriphCLKConfig()` call is required or possible for the PTP clock. The only
requirement is that HCLK actually be 250 MHz.

## Coupled decisions

Two other settings are load-bearing for the above and must not drift:

**AHB prescaler must be `RCC_HCLK_DIV1`.** The H743 code uses `RCC_HCLK_DIV2` (480 → 240).
On the H563, SYSCLK 250 MHz is already at the HCLK ceiling, so the divider must be 1. If
DIV2 is carried over, HCLK becomes 125 MHz, K drops to 0.625, and the addend wraps —
producing a PTP clock running at roughly 0.375× real time rather than an obvious failure.

**MDIO divider `CR = DIV102`.** The CSR clock for MDIO is HCLK. At 250 MHz,
250/102 = 2.45 MHz, just inside the 2.5 MHz MDIO limit. This is the correct pairing for a
250 MHz HCLK; at 125 MHz, DIV62 would be the appropriate choice instead. Note that
`mac_drv.c` currently contains a shift bug in this register write (it shifts an
already-positioned mask by `ETH_MACMDIOAR_CR_Pos`, landing in the `RDA` field and leaving
CR at DIV42 ≈ 6 MHz); that must be fixed as part of the MAC driver port.

### PLL arithmetic, for reference

Even on a device that *did* have the ETHPTP mux, a 200 MHz PLL1_Q alongside a 250 MHz
SYSCLK is not constructible. `PLLP` forbids odd division factors and the PLL1 wide VCO
range is 192–836 MHz, so for SYSCLK = 250 MHz:

- PLLP = 2 → VCO = 500 MHz ✅
- PLLP = 4 → VCO = 1000 MHz ✗ (above the 836 MHz ceiling)

VCO is pinned at 500 MHz. `PLL1Q` and `PLL1R` are integer dividers of the VCO, yielding
500 / 250 / 166.67 / 125 / 100 MHz — 200 MHz is not attainable without abandoning the
250 MHz SYSCLK target.

## History

The original implementation plan specified PLL1_Q = 200 MHz with a 5 ns increment,
described as "matching the H7 config", and deferred a 250 MHz / 4 ns "optimisation" to a
later stage. Review found:

1. The stated H7 baseline was wrong — the source project uses 240 MHz, not 200 MHz.
2. Both the initial and the "optimised" configuration have K = 1.0 and produce an addend
   of zero.
3. PLL1_Q cannot produce 200 MHz alongside a 250 MHz SYSCLK.
4. The H563 has no ETH PTP clock mux, so PLL1_Q cannot reach the timestamp unit at all.
5. A 4 ns increment is unreachable on this device regardless of clock source.

The configuration was changed to HCLK / 250 MHz / 5 ns, and the planned optimisation
guide was replaced by this document.
