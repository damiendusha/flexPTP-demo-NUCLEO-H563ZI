# embfmt patches

Patches against the pinned embfmt submodule (`670a349`).

As with flexPTP, the submodule is **not** modified in git — the parent repository records
only its commit pointer, so a working-tree change inside `Modules/embfmt` is invisible to a
clone and is destroyed by `git submodule update`. Anything that must survive lives here.

## Applying

**`./build.sh` does this for you** and refuses to build if it cannot — see
`apply_submodule_patches()`. Applying by hand is only needed when building without it:

```bash
git -C Modules/embfmt apply "$PWD/patches/embfmt/0001-formatting-fixes.patch"
```

## Reverting

```bash
git -C Modules/embfmt checkout -- embformat.c
```

## Contents

### `0001-formatting-fixes.patch` — **permanent, keep**

Two fixes, both target-independent and both worth reporting upstream. Full analysis in
[`docs/port_review.md`](../../docs/port_review.md) under F60.

| Change | Why |
|---|---|
| Accept `ll` as a length modifier | `process_format_word()` consumed only one `l`, so `%lld` fell through as an unknown conversion: the argument was skipped and the trailing `d` printed as literal text. flexPTP uses `__PRI64_PREFIX` (`"ll"` under newlib) throughout, so the mean path delay, the measured Sync period and the coarse-correction message were all unprintable. `LEN_LONG` already reads 64-bit arguments, so nothing downstream needed changing |
| Print `nan` / `inf` / `-inf` | `pfn_double()` had no non-finite path. It splits the value with `(uint64_t)d`, and Arm's `VCVT` returns 0 for NaN — so a NaN printed as `0.0000`, indistinguishable from a genuine zero. That is what disguised F59 as a working servo |
| Normalise before rounding in `%e` | The fixed-point rounding constant (`0.51e-7` at default precision) was added *before* the exponent was normalised, so any value below it was swallowed and printed as the constant itself. `1e-16` came out as `5.100000e-8`, and **zero** printed as `5.100000e-8` too. This is what `ptp servo st\|sg\|stm` were reporting (F66) |
| Fix the rounding carry | When rounding carried into a new integer digit, the fraction had `10` subtracted instead of `10^precision`. `9.9999999` printed as `10.999990` and `1.9999999` as `2.999990` — **in `%f` as much as `%e`**, since the code is shared (F66) |

Verified by compiling the module for the host and formatting `nan("")`, `%lld`, `%llu`, the
three servo variances, zero, and values that straddle a rounding carry — before and after —
and then on the target, where `ptp servo st|sg|stm` now report `1.000000e-16`,
`1.000000e-12` and `5.000000e-11` instead of three variations on `5.1e-8`.

One residual, left alone: `%e` renders a mantissa that rounds up to ten as `10.000000e+0`
rather than re-normalising to `1.000000e+1`, and the exponent is not zero-padded (`e+0`, not
`e+00`). Both are cosmetic, the value is correct, and nothing in this project prints `%e`
outside the three servo commands.
