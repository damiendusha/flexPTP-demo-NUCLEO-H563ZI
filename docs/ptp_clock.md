# PTP Clock Design — STM32H563ZI

Detailed rationale for the PTP timestamp clock configuration of this port.
Summarised in [design_doc.md](design_doc.md#ptp-clock); this document holds the derivation.

**Conclusion up front:** the timestamp unit is clocked from **`pll1_q_ck` at 250 MHz** with
a **5 ns** subsecond increment. The source is fixed in silicon; the frequency is ours to
choose.

> **Corrected 2026-08-09 after hardware bring-up.** This document previously stated the
> PTP clock was HCLK. That was wrong — see [Why the source is `pll1_q_ck`](#why-the-source-is-pll1_q_ck).
> Every derived quantity below is unchanged, because `pll1_q_ck` is also configured to
> 250 MHz, but the *sourcing* and the required PLL setup are different. Timestamp granularity is therefore 5 ns, identical to the H743 source
project, and cannot be improved on this part.

## Table of Contents

- [Why the source is `pll1_q_ck`](#why-the-source-is-pll1_q_ck)
- [How the hardware clock works](#how-the-hardware-clock-works)
- [The K > 1 constraint](#the-k--1-constraint)
- [Why a 4 ns increment is unreachable](#why-a-4-ns-increment-is-unreachable)
- [Selected configuration](#selected-configuration)
- [Coupled decisions](#coupled-decisions)
- [History](#history)

---

## Why the source is `pll1_q_ck`

The STM32H563 has **no ETH PTP clock multiplexer**. `RCC_CCIPR5.ETHPTPCLKSEL` and
`RCC_CCIPR4.ETHPTPDIV` are documented in RM0481, but each carries the note *"only available
on STM32H543/553xx devices"* — which is why `stm32h563xx.h` correctly omits them.

Absence of a mux does **not** mean the clock defaults to HCLK. RM0481 Table 121 *(Kernel
clock distribution overview)* carries two rows for the PTP clock:

| Row | Selector | Source |
|---|---|---|
| `ETH (ptp)` <sup>(2)</sup> | `ETHPTPCLKSEL` | selectable: `rcc_hclk1` / `pll1_r_ck` / `pll1_q_ck` / `pll2_p_ck` |
| `ETH(ptp)` | *none* | **`pll1_q_ck`**, fixed |

Footnote (2) reads *"Available only on STM32H543/553xx devices"*, so the **second row is the
H563 case**: `clk_ptp_ref_i` is hardwired to `pll1_q_ck`.

### Consequences

1. **PLL1Q must be configured for the PTP frequency**, not merely left at whatever suits
   some other peripheral.
2. **The PLL1Q output must be explicitly enabled.** `HAL_RCC_OscConfig()` programmes the
   `PLLQ` divider but leaves the output gate closed; `__HAL_RCC_PLL1_CLKOUT_ENABLE(RCC_PLL1_DIVQ)`
   is required. Omitting it produces a PTP block whose registers accept writes normally
   while the timestamp counter never advances and `TSADDREG` never clears — the symptom
   recorded as finding F53.
3. **HCLK is irrelevant to PTP on this part.** Changing the AHB divider does not detune the
   PTP clock. (An earlier revision of this document claimed the opposite.)

With VCO pinned at 500 MHz (see [PLL arithmetic](#pll-arithmetic-for-reference)),
`PLLQ = 2` gives `pll1_q_ck` = 250 MHz.

### Verified on hardware

| Register | Value | Meaning |
|---|---|---|
| `RCC_PLL1CFGR` | `0x00030417` | `PLL1PEN` and `PLL1QEN` both set |
| `ETH_MACSSIR` | `0x00050000` | subsecond increment = 5 ns |
| `ETH_MACTSAR` | `0xCCCCCCCC` | addend = 3435973836, as derived below |
| `ETH_MACTSCR` | `0x00012e03` | `TSADDREG` clear — addend update completed |
| `ETH_MACSTSR` | 7 → 11 over ~4 s | counter advancing at real-time rate |

---

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
| PLL1Q left at ÷4 (H743 value) | 125 MHz | 5 ns | 0.625 | 2576980377 (`0x99999999`) | ❌ silent, clock at 0.375× |

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

> **Revised after the `pll1_q_ck` correction.** An earlier revision called 4 ns
> *unreachable*, reasoning that the PTP clock was HCLK and therefore capped at 250 MHz.
> That premise was wrong. `pll1_q_ck` = VCO / `PLLQ` with VCO at 500 MHz, so `PLLQ = 1`
> would give **500 MHz and K = 2.0** — a valid 4 ns configuration on paper.

It is **not adopted**, but for engineering reasons rather than impossibility:

- The maximum frequency of `clk_ptp_ref_i` has not been checked against the datasheet;
  500 MHz is far above anything else in this design and must not be assumed valid.
- `PLLQ = 1` pins PLL1Q at 500 MHz, removing it as a source for any other peripheral.
- The gain is 5 ns → 4 ns of quantisation, well below the noise floor of this board's
  ST-Link-sourced clock.

Anyone pursuing it should verify the `clk_ptp_ref_i` maximum first, then measure whether
the board's clock quality justifies it. As configured, granularity is 5 ns — the same as
the H743 — and the differences are tuning range (+25 % vs +20 %) and tuning resolution:

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
#define PTP_MAIN_OSCILLATOR_FREQ_HZ (250000000)  /* = pll1_q_ck (VCO 500 MHz / PLLQ 2) */
#define PTP_INCREMENT_NSEC (5)                   /* must exceed the 4 ns clock period   */
```

Derived values:

| Quantity | Value |
|---|---|
| `PTP_CLOCK_TICK_FREQ_HZ` | 200 000 000 |
| `PTP_ADDEND_INIT` | 3435973836 (`0xCCCCCCCC`) |
| K (tuning headroom) | 1.25 → +25 % |
| Timestamp granularity | 5 ns |
| Tuning resolution | ≈ 0.29 ppb |

No `HAL_RCCEx_PeriphCLKConfig()` call is required or possible — there is no mux to
programme. What *is* required is that PLL1Q be set to 250 MHz **and its output enabled**.

## Coupled decisions

Two other settings are load-bearing for the above and must not drift:

**`PLLQ` must be 2, and the PLL1Q output must be enabled.** `PLLQ` sets the PTP frequency
directly. The H743 used `PLLQ = 4`, which here gives 125 MHz and K = 0.625: not a dead
clock but a wrapped addend running at roughly 0.375× real time. Separately,
`HAL_RCC_OscConfig()` programmes the divider but leaves the output gated, so
`__HAL_RCC_PLL1_CLKOUT_ENABLE(RCC_PLL1_DIVQ)` is mandatory (finding F53).

**AHB prescaler `RCC_SYSCLK_DIV1`** is still correct for performance and for MDIO below,
but it is *not* PTP-critical — an earlier revision wrongly claimed the AHB divider detuned
the PTP clock.

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
3. PLL1_Q cannot produce 200 MHz alongside a 250 MHz SYSCLK (VCO is pinned at 500 MHz and
   `PLLQ` is an integer divider, giving 500 / 250 / 166.7 / 125 …).

The configuration was changed to 250 MHz / 5 ns and the optimisation guide replaced by this
document.

**Correction, 2026-08-09 (hardware bring-up).** The review additionally concluded that the
H563 had no PTP clock mux and therefore used HCLK, and that a 4 ns increment was
unreachable. The first half was right, the inference was not: RM0481 Table 121 shows the
mux-less devices are hardwired to **`pll1_q_ck`**. The original plan's instinct to use
PLL1_Q was correct — only its 200 MHz figure was unattainable. Acting on the HCLK
conclusion left PLL1Q disabled, and the PTP counter never ran (F53). Both the source and
the 4 ns claim are corrected above. Every derived quantity survived unchanged, because
PLL1Q is configured to the same 250 MHz that HCLK happens to run at.
