# embfmt patches

Patches against the pinned embfmt submodule (`670a349`).

As with flexPTP, the submodule is **not** modified in git — the parent repository records
only its commit pointer, so a working-tree change inside `Modules/embfmt` is invisible to a
clone and is destroyed by `git submodule update`. Anything that must survive lives here.

## Applying

```bash
git -C Modules/embfmt apply "$PWD/patches/embfmt/0001-ll-length-modifier-and-non-finite-doubles.patch"
```

## Reverting

```bash
git -C Modules/embfmt checkout -- embformat.c
```

## Contents

### `0001-ll-length-modifier-and-non-finite-doubles.patch` — **permanent, keep**

Two fixes, both target-independent and both worth reporting upstream. Full analysis in
[`docs/port_review.md`](../../docs/port_review.md) under F60.

| Change | Why |
|---|---|
| Accept `ll` as a length modifier | `process_format_word()` consumed only one `l`, so `%lld` fell through as an unknown conversion: the argument was skipped and the trailing `d` printed as literal text. flexPTP uses `__PRI64_PREFIX` (`"ll"` under newlib) throughout, so the mean path delay, the measured Sync period and the coarse-correction message were all unprintable. `LEN_LONG` already reads 64-bit arguments, so nothing downstream needed changing |
| Print `nan` / `inf` / `-inf` | `pfn_double()` had no non-finite path. It splits the value with `(uint64_t)d`, and Arm's `VCVT` returns 0 for NaN — so a NaN printed as `0.0000`, indistinguishable from a genuine zero. That is what disguised F59 as a working servo |

Verified by compiling the module for the host and formatting `nan("")`, `%lld` and `%llu`,
before and after.
