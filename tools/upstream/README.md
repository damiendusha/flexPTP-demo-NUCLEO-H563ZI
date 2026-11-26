# Upstreaming

Fixes for dependencies live in a working clone of the dependency, one branch per fix, and are
filed from there. Until a fix merges, the firmware carries it as a patch in `//patches/flexptp`,
applied by the Bazel fetch; see [`patches/flexptp/README.md`](../../patches/flexptp/README.md).

## The flexPTP fixes

Thirteen, in three batches. U1–U7 are branches off `494e644` (`RC1.0-17`), filed on 2026-09-10.
U8–U10 and U11–U13 are branches off the current pin `fe4b071` (`RC1.0-26`); U8–U10 were filed on
2026-11-26, U11–U13 on 2026-11-27, after the tests below. Each is one commit, independent of the others, so they
can be reviewed, merged or rejected in any combination. Full analysis and evidence in
[`docs/flexptp_upstream_review.md`](../../docs/flexptp_upstream_review.md).

| | Branch | PR | Status (2026-11-27) | What it fixes |
|---|---|---|---|---|
| U1 | `fix/u1-gm-clock-identity-byte-order` | [#20](https://github.com/epagris/flexPTP/pull/20) | **open** | A remote master's clock identity is byte-reversed, and the BMCA compares two byte conventions |
| U2 | `fix/u2-logid-config-bit` | [#21](https://github.com/epagris/flexPTP/pull/21) | merged, comments trimmed | The `logid` flag is never stored, and storing it would make `ptp_load_config()` discard the whole record |
| U3 | `fix/u3-locked-log-prefix` | [#25](https://github.com/epagris/flexPTP/pull/25) | merged, comments trimmed | `[LOG-LCKD]` prints every Sync with no message behind it |
| U4 | `fix/u4-p2p-8023-dest-sticky` | [#24](https://github.com/epagris/flexPTP/pull/24) | merged | A custom P2P 802.3 destination survives a profile change |
| U5 | `fix/u5-stale-pbuf-tx-cb` | [#22](https://github.com/epagris/flexPTP/pull/22) | merged | Recycled pbufs inherit `tx_cb` and get spurious hardware TX timestamps |
| U6 | `fix/u6-pdelay-pcb-comment` | [#26](https://github.com/epagris/flexPTP/pull/26) | **open** | Comment: the PDelay pcbs are receive-only |
| U7 | `fix/u7-transport-type-reset` | [#23](https://github.com/epagris/flexPTP/pull/23) | merged | `ptp transport` does not re-init the driver, so the switch silently does not take effect |
| U8 | `fix/u8-delay-resp-interval` | [#36](https://github.com/epagris/flexPTP/pull/36) | **open** | Fixes [#30](https://github.com/epagris/flexPTP/issues/30): a Delay_Resp echoes the Delay_Req's 0x7F instead of carrying `logMinDelayReqInterval` |
| U9 | `fix/u9-teardown-unlock` | [#34](https://github.com/epagris/flexPTP/pull/34) | **open** | `unreg_task_ptp()`'s lwIP shutdown branch is compiled out on a short-enum ABI, and where it is live it returns holding the core lock |
| U10 | `fix/u10-chained-pbufs` | [#35](https://github.com/epagris/flexPTP/pull/35) | **open** | A chained pbuf reaches flexPTP truncated to its first segment |
| U11 | `fix/u11-teardown-order` | [#39](https://github.com/epagris/flexPTP/pull/39) | **open** | `unreg_task_ptp()` deletes the queues before it stops accepting work, so a late TX timestamp is posted into freed memory; and deletes the PTP task wherever it is, so a lock it holds stays held |
| U12 | `fix/u12-rx-length-version` | [#38](https://github.com/epagris/flexPTP/pull/38) | **open** | A received message is parsed whatever its length and version: a 1-byte datagram is a "Sync" built from the previous message's bytes |
| U13 | `fix/u13-slave-parent-only` | [#37](https://github.com/epagris/flexPTP/pull/37) | **open** | The slave takes Sync, Follow_Up and Delay_Resp from any sender: one foreign Sync per cycle stops synchronisation |

Every one is verified on a NUCLEO-H563ZI, most with a before/after measurement rather than an
argument. U7 was found while testing U4 — and is why U4's negative case was untestable, because
the only way to reach 802.3 without the gPTP preset is the transport switch that U7 repairs.

Five of the first seven are merged in `RC1.0-26` (`fe4b071`); #20 and #26 are open and
mergeable. Those two and U8–U13 are carried as `//patches/flexptp` until they land.

U11–U13 have host tests that run flexPTP's real engine: `//patches/flexptp/test`, described in
[`patches/flexptp/README.md`](../../patches/flexptp/README.md#host-tests). Each fix's tests fail
without it and pass with it; U12 and U13 also have the HIL `fuzz` scenario, and U11 `stopstart_busy`.

U8 came from issue #30, which upstream's `9eefd95` did not fix (it changes the Delay_Req, not the
Delay_Resp). U9 and U10 came from an August branch of this repository that was never merged; it
carried a third driver change, a guard against an 802.3 runt underflowing `p->len - 14`, which was
dropped because lwIP's `ethernet_input()` already discards any frame of 14 bytes or less before the
hook that would see it.

#20 has outside support. Another flexPTP user, Michael (`emb4fun`), reproduced it independently in
[issue #32](https://github.com/epagris/flexPTP/issues/32): with two flexPTP masters the one with
the *larger* clock identity won the election, and with #20 the smaller one correctly does. His
[PR #33](https://github.com/epagris/flexPTP/pull/33) carries #20's change verbatim, credits it, and
adds a separate fix (`ptp_slave_reset()` before `ptp_slave_enable()`).

The PR branches for #20 and #26 on the fork are left at their original `494e644` base -- GitHub
reports both as mergeable, so force-pushing would only churn the review. The patches are generated
instead from the local `series/rc26` branch: all five fixes cherry-picked in order onto `RC1.0-26`,
so each patch's context is exactly the tree it is applied to (see
[`patches/flexptp/README.md`](../../patches/flexptp/README.md#order)).

## Filing them

```bash
tools/upstream/file_flexptp_prs.sh --dry-run     # show exactly what would be filed
tools/upstream/file_flexptp_prs.sh               # ask before each PR, then file it
tools/upstream/file_flexptp_prs.sh --only u9     # just one
```

**One at a time, and nothing happens without a `y`.** No branch is pushed and no PR is opened
until that specific PR is confirmed; `q` stops the run and leaves anything already filed alone.
A branch that already has a PR from the fork is detected and skipped, so the script is safe to
re-run; if GitHub cannot be asked, the run stops rather than risk a duplicate.

The PR body is the commit message body verbatim — it was written to explain the defect, the
mechanism and the measurement, which is what the PR has to say anyway.

Requires `gh` authenticated with `repo` scope and a fork you can push to:

```bash
git -C ~/git/flexPTP-fixes remote add fork https://github.com/<you>/flexPTP.git
```

| Variable | Default |
|---|---|
| `FLEXPTP_CLONE` | `~/git/flexPTP-fixes` |
| `FLEXPTP_UPSTREAM` | `epagris/flexPTP` |
| `FLEXPTP_BASE` | `master` |
| `FLEXPTP_FORK_REMOTE` | `fork` |

## Building the firmware against the working clone

`--override_repository` points the build at the clone without touching the pin in
`//bazel:repositories.bzl`, so the checked-in build stays reproducible while a fix is in
progress:

```bash
bazel build --override_repository=flexptp=$HOME/git/flexPTP-fixes //projects/...
```

The clone needs two files Bazel requires and upstream does not have — a `REPO.bazel` marker and
a copy of `//bazel:flexptp.BUILD` as `BUILD.bazel`. Put both in `.git/info/exclude` in the
clone, where they cannot be committed to a fix branch or show up in `git status`. Watch for
`MODULE.bazel` and `MODULE.bazel.lock` too: Bazel writes them into the overridden directory,
and one of them reached a fix branch before this note existed.
