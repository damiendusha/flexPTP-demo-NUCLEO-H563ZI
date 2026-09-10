# flexPTP patches

Seven patches against `494e644` (`RC1.0-17`), applied **by the Bazel fetch**.

`//bazel/repositories.bzl` lists them in the `patches` attribute of the `new_git_repository`
for `@flexptp`, so they are part of fetching the dependency: the patched tree is what lands in
the repository cache, keyed by content. Nothing has to be applied by hand, and there is no
working tree that can be pristine, patched, or neither.

**Every one of these is filed upstream as [#20](https://github.com/epagris/flexPTP/pull/20)-[#26](https://github.com/epagris/flexPTP/pull/26), and should be deleted as its pull request merges.** They
are here because the firmware needs them now, not because they belong here. The previous series
(`0002`–`0004`) was deleted exactly that way when `RC1.0-17` absorbed it.

## Contents

Found reviewing `RC1.0-11` → `RC1.0-17`. Three are regressions in the maintainer's own
`7bc544e`. Full analysis, evidence and reproduction in
[`docs/flexptp_upstream_review.md`](../../docs/flexptp_upstream_review.md); the branches and the
PR-filing script are in [`tools/upstream/`](../../tools/upstream/README.md).

| Patch | PR | File(s) | What it fixes | Verified |
|---|---|---|---|---|
| `0001-u1-gm-clock-identity-byte-order` | [#20](https://github.com/epagris/flexPTP/pull/20) | `msg_utils.c` | The Announce body is the only place a clock identity is byte-swapped on receive, so a remote master's identity displays reversed and the BMCA compares two byte conventions | Master clock ID `e42126feffc9a000` → `00a0c9fffe2621e4`, both firmwares |
| `0002-u2-logid-config-bit` | [#21](https://github.com/epagris/flexPTP/pull/21) | `config.c` | A stray `;` drops the `logid` bit; `CONFIG_LOG_ALL` is a stale literal, so storing that bit makes `ptp_load_config()` discard the **whole** record | Config round trip, three states |
| `0003-u3-locked-log-prefix` | [#25](https://github.com/epagris/flexPTP/pull/25) | `stats.c` | `[LOG-LCKD]` prints every Sync with no message behind it | 25 orphan prefixes in 25 s → 0 |
| `0004-u4-p2p-8023-dest-sticky` | [#24](https://github.com/epagris/flexPTP/pull/24) | `nsd_lwip.c` | A custom P2P 802.3 destination survives a profile change, so gPTP → anything keeps transmitting to `01:80:C2:00:00:0E` | Frames on the primary group: 0 of 280 → 233 of 280 |
| `0005-u5-stale-pbuf-tx-cb` | [#22](https://github.com/epagris/flexPTP/pull/22) | `nsd_lwip.c` | `tx_cb` is left on a freed pbuf, so recycled pbufs get spurious hardware TX timestamps | `ptp.drop_txts` 284 392 → 0 under a 120 s flood |
| `0006-u6-pdelay-pcb-comment` | [#26](https://github.com/epagris/flexPTP/pull/26) | `nsd_lwip.c` | Comment only: the PDelay pcbs are receive-only, so the "obvious fix" to the transmit selection is not one | Suite unchanged |
| `0007-u7-transport-type-reset` | [#23](https://github.com/epagris/flexPTP/pull/23) | `settings_interface.c` | `ptp_set_transport_type()` skips the `ptp_reset()` its neighbour calls and calls mandatory, so `ptp transport` silently does not take effect | 802.3 frames after a runtime switch: 0 → 280 |

**`0002` is two changes on purpose.** Fixing the semicolon alone is worse than leaving the bug:
the `logid` bit then reaches a record that `ptp_load_config()` rejects wholesale, taking the
profile, clock offset and priorities with it and reporting the configuration as corrupt. That
was measured on the board, not reasoned about.

**`0007` is why `0004` is testable.** gPTP is the only preset that is natively 802.3, so the
only route to "802.3 with no custom destination" is the transport switch — which did not work.

## Order

**Independent and order-free.** Each is generated against `494e644` and they touch disjoint
regions; three touch `nsd_lwip.c` but not the same hunks. The order in `repositories.bzl` is the
review's numbering, not a dependency.

Verified both ways rather than assumed — with `git apply` and with plain `patch -p1`, since
Bazel's applier is closer to the latter:

```bash
git clone --shared https://github.com/epagris/flexPTP.git /tmp/fp && git -C /tmp/fp checkout 494e644
for p in patches/flexptp/*.patch; do git -C /tmp/fp apply -p1 "$p" || echo "FAILED $p"; done
```

## Regenerating

The patches are `git format-patch` output, so each carries its own commit message — which is
where the evidence lives. They are generated from the branches in the upstream working clone:

```bash
git -C ~/git/flexPTP-fixes format-patch --stdout --no-signature -1 fix/u5-stale-pbuf-tx-cb \
    > patches/flexptp/0005-u5-stale-pbuf-tx-cb.patch
```

Regenerate from the branch, never by hand-editing the patch file, and never with
`git diff -- <file>` from a tree carrying other changes: that silently swallows them, which is
how the old `0002` first shipped with temporary instrumentation folded in.

## When a pull request merges

Delete the patch, drop its line from `//bazel/repositories.bzl` and its `exports_files` entry,
and bump the pin. Then check the fix really is in the new revision rather than assuming, the way
`RC1.0-17` was checked:

```bash
git -C /tmp/fp apply -R --check patches/flexptp/0005-u5-stale-pbuf-tx-cb.patch  # clean == present
```

A patch that no longer applies but is still listed is worse than no patch at all.
