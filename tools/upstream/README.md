# Upstreaming

Fixes for dependencies live in a working clone of the dependency, one branch per fix, and are
filed from there. They are **not** carried as patches in `//patches` unless upstream declines
them — the flexPTP patches this repository used to carry were all accepted, which is why that
directory is gone.

## The flexPTP fixes

Seven, each an independent branch off the pinned upstream revision `494e644` (`RC1.0-17`), each
touching a disjoint set of lines so they can be reviewed, merged or rejected in any
combination. Full analysis and evidence in
[`docs/flexptp_upstream_review.md`](../../docs/flexptp_upstream_review.md).

| | Branch | PR | What it fixes |
|---|---|---|---|
| U1 | `fix/u1-gm-clock-identity-byte-order` | [#20](https://github.com/epagris/flexPTP/pull/20) | A remote master's clock identity is byte-reversed, and the BMCA compares two byte conventions |
| U2 | `fix/u2-logid-config-bit` | [#21](https://github.com/epagris/flexPTP/pull/21) | The `logid` flag is never stored, and storing it would make `ptp_load_config()` discard the whole record |
| U3 | `fix/u3-locked-log-prefix` | [#25](https://github.com/epagris/flexPTP/pull/25) | `[LOG-LCKD]` prints every Sync with no message behind it |
| U4 | `fix/u4-p2p-8023-dest-sticky` | [#24](https://github.com/epagris/flexPTP/pull/24) | A custom P2P 802.3 destination survives a profile change |
| U5 | `fix/u5-stale-pbuf-tx-cb` | [#22](https://github.com/epagris/flexPTP/pull/22) | Recycled pbufs inherit `tx_cb` and get spurious hardware TX timestamps |
| U6 | `fix/u6-pdelay-pcb-comment` | [#26](https://github.com/epagris/flexPTP/pull/26) | Comment: the PDelay pcbs are receive-only |
| U7 | `fix/u7-transport-type-reset` | [#23](https://github.com/epagris/flexPTP/pull/23) | `ptp transport` does not re-init the driver, so the switch silently does not take effect |

Every one is verified on a NUCLEO-H563ZI, most with a before/after measurement rather than an
argument. U7 was found while testing U4 — and is why U4's negative case was untestable, because
the only way to reach 802.3 without the gPTP preset is the transport switch that U7 repairs.

All seven are filed and open as of 2026-09-10.

## Filing them

```bash
tools/upstream/file_flexptp_prs.sh --dry-run     # show exactly what would be filed
tools/upstream/file_flexptp_prs.sh               # ask before each PR, then file it
tools/upstream/file_flexptp_prs.sh --only u5     # just one
```

**One at a time, and nothing happens without a `y`.** No branch is pushed and no PR is opened
until that specific PR is confirmed; `q` stops the run and leaves anything already filed alone.
A branch that already has a PR is detected and skipped, so the script is safe to re-run.

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
