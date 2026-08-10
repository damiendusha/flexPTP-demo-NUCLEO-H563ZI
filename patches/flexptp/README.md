# flexPTP patches

Patches against the pinned flexPTP submodule (`76aaf67`, `RC1.0-11`).

The submodule is **not** modified in git — the parent repository records only its commit
pointer, so any working-tree change inside `Modules/flexptp` is invisible to a clone and
is destroyed by `git submodule update`. Anything that must survive lives here.

## Applying

**`./build.sh` does this for you** and refuses to build if it cannot — see
`apply_submodule_patches()`. Applying by hand is only needed when building without it:

```bash
git -C Modules/flexptp apply "$PWD/patches/flexptp/<name>.patch"
```

The patches are **independent and order-free**: each is generated against the pinned commit
and touches disjoint regions, so either may be applied alone or both in either order. Verify
after regenerating any of them — a patch extracted with `git diff -- <file>` from a tree that
carries other changes will silently swallow them, which is how `0002` first shipped with the
temporary instrumentation folded in:

```bash
git clone --shared Modules/flexptp /tmp/rt && git -C /tmp/rt checkout $(git -C Modules/flexptp rev-parse HEAD)
git -C /tmp/rt apply "$PWD"/patches/flexptp/*.patch
diff <(git -C Modules/flexptp diff) <(git -C /tmp/rt diff)   # must be empty
```

## Reverting

```bash
git -C Modules/flexptp checkout -- src/
```

## Contents

### `0002-servo-nan-latch-fix.patch` — **permanent, keep**

Makes the clock servo survive a repeated Sync cycle. Without it a single duplicated Sync
gives `measSyncPeriodNs == 0`, the Kalman filter computes `0.0/0.0`, and NaN latches into
its state permanently — the servo then reports a correction of `0.0000` for ever while the
clock free-runs. Full analysis in
[`docs/port_review.md`](../../docs/port_review.md) under F59.

| File | Change |
|---|---|
| `slave.c` | `ptp_perform_correction()` skips a cycle whose measured Sync period is not positive; `ptp_tune_clock()` ignores a non-finite tuning value |
| `servo/kalman_filter.c` | Skips a non-positive period, and resets the filter if its state or output goes non-finite |

Report upstream: the defect is target-independent.

## Removed

`0001-temporary-F42-instrumentation.patch` added volatile debug counters to `task_ptp.c`,
`slave.c`, `ptp_core.c` and `nsd_lwip.c` while F42, and later F61, were being chased. Both
are resolved, so it was dropped rather than left to rot against a future submodule bump — a
patch that no longer applies but is still listed is worse than no patch at all.

Nothing is lost. Recover it with:

```bash
git show 2963813:patches/flexptp/0001-temporary-F42-instrumentation.patch > /tmp/f42.patch
git -C Modules/flexptp apply /tmp/f42.patch
```

It counted messages at four points in the receive path — what lwIP handed the driver, what
reached the core dispatcher, what survived the domain and `transportSpecific` filter, and what
the slave state machine did with it — plus the transmit-timestamp callback path in
`task_ptp.c`. All four structs were `volatile` and read over SWD, so nothing printed from an
ISR. If a future receive-path problem needs the same view, that is the shape to rebuild.
