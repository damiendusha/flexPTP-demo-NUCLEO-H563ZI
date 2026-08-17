# flexPTP patches

Patches against the pinned flexptp revision (`76aaf67`, `RC1.0-11`), applied **by the Bazel fetch**.

`//bazel/repositories.bzl` lists them in the `patches` attribute of the `new_git_repository`
for `@flexptp`, so they are part of fetching the dependency: the patched tree is what lands in
the repository cache, keyed by content.

That is why these files cannot be deleted even though the submodules and `build.sh` are gone --
they are an input to the build, not a manual step.

## Applying

Nothing to do: `bazel build` applies them. There is no longer a working tree that can be
pristine, patched, or -- the state `build.sh` had to detect and refuse -- neither, which is what
retired the old `--check-patches` gate.

To inspect the patched sources:

```bash
bazel fetch @flexptp
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

> **Why there is no `0001-`.** The numbering is historical, not an index: `0001` was the
> temporary F42/F61 instrumentation patch, dropped once both findings were resolved (see
> [Removed](#removed) below). `0002` is deliberately *not* renumbered — the number is how the
> patch is referred to in commit messages, `docs/port_review.md` and the recovery command
> below, and renaming it would silently invalidate all of them to tidy a gap that costs
> nothing.

### `0002-servo-nan-latch-fix.patch` — **permanent, keep**

Makes the clock servo survive a repeated Sync cycle. Without it a single duplicated Sync
gives `measSyncPeriodNs == 0`, the Kalman filter computes `0.0/0.0`, and NaN latches into
its state permanently — the servo then reports a correction of `0.0000` for ever while the
clock free-runs. Full analysis in
[`docs/port_review.md`](../../projects/flexptp_threadx_netx_demo/docs/port_review.md) under F59.

| File | Change |
|---|---|
| `slave.c` | `ptp_perform_correction()` skips a cycle whose measured Sync period is not positive; `ptp_tune_clock()` ignores a non-finite tuning value |
| `servo/kalman_filter.c` | Skips a non-positive period, and resets the filter if its state or output goes non-finite |

Report upstream: the defect is target-independent.

### `0003-generic-netstack-igmp-addresses.patch` — **permanent, keep**

Adds `PTP_IGMP_PRIMARY_INIT` / `PTP_IGMP_PEER_DELAY_INIT` so a port that supplies its own
network stack can provide the two IGMP group addresses. `ptp_defs.c` already `#error`s telling
the integrator to define `PTP_IGMP_PRIMARY` and `PTP_IGMP_PEER_DELAY`, but both are `const`
objects defined in that file, so there was no way to comply without editing it.

Report upstream: the defect is target-independent.

### `0004-cmsis-os2-event-enqueue-from-timer-thread.patch` — **permanent, keep**

Makes `ptp_event_enqueue()` non-blocking in the CMSIS-RTOS2 binding. It was using
`osWaitForever`, and its highest-frequency caller is `ptp_heartbeat_tmr_cb()`, which
CMSIS-RTOS2 runs in the RTOS's timer thread. ThreadX refuses to suspend that thread:
`_txe_queue_send()` returns `TX_WAIT_ERROR` when `_tx_thread_current_ptr == &_tx_timer_thread`
and the wait option is not `TX_NO_WAIT`, which the CMSIS-RTOS2 wrapper reports as
`osErrorTimeout` — a code `ptp_event_enqueue()` turns into a silent `false`.

So every heartbeat was discarded: `S.ticks` stayed at 0, `ptp_bmca_tick()` never ran, and the
BMCA FSM never left `INITIALIZING` — the one state in which `ptp_handle_announce_msg()` falls
through its `default:` arm and drops the Announce. Receive worked perfectly throughout, which
is what made it look like a receive fault. Full analysis in
[`docs/port_review.md`](../../projects/flexptp_threadx_netx_demo/docs/port_review.md) under F80.

| File | Change |
|---|---|
| `task_ptp.c` | `ptp_event_enqueue()` uses a zero timeout for both the event and its notification in the `FLEXPTP_CMSIS_OS2` branch |

Report upstream: the defect is target-independent, and the FreeRTOS branch has the same shape
— `xQueueSend(..., portMAX_DELAY)` from the timer service task risks deadlocking it rather
than failing cleanly. Only the CMSIS-RTOS2 branch is changed here, because that is the one
this port exercises and the one that was measured.

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
