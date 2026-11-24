# flexPTP patches

Two patches against `fe4b071` (`RC1.0-26`), applied **by the Bazel fetch**.

`//bazel/repositories.bzl` lists them in the `patches` attribute of the `new_git_repository`
for `@flexptp`, so they are part of fetching the dependency: the patched tree is what lands in
the repository cache, keyed by content. Nothing has to be applied by hand, and there is no
working tree that can be pristine, patched, or neither.

**Each is filed upstream and is deleted as its pull request merges.** They are here because the
firmware needs them now, not because they belong here.

## Contents

| Patch | PR | File | What it fixes | Verified on RC1.0-26 |
|---|---|---|---|---|
| `0001-u1-gm-clock-identity-byte-order` | [#20](https://github.com/epagris/flexPTP/pull/20), open | `msg_utils.c` | The Announce body is the only place a clock identity is byte-swapped on receive. That inverts the BMCA tie-break between two masters, and a remote master's identity displays reversed | Master clock ID `00a0c9fffe2621e4`, both firmwares |
| `0006-u6-pdelay-pcb-comment` | [#26](https://github.com/epagris/flexPTP/pull/26), open | `nsd_lwip.c` | Comment only: the PDelay pcbs are receive-only | Suite unchanged |

`0001` is no longer only ours: another flexPTP user, Michael (`emb4fun`), reproduced it
independently with two flexPTP masters in [issue #32](https://github.com/epagris/flexPTP/issues/32)
and folded it into his [PR #33](https://github.com/epagris/flexPTP/pull/33). If #33 merges first,
it carries this fix and the patch goes then.

## Merged, and gone from here

The other five of the seven filed after reviewing `RC1.0-17` are in `RC1.0-26`. Each was checked
rather than assumed: `0004`, `0005` and `0007` reverse-apply cleanly, i.e. landed verbatim; `0002`
and `0003` landed with comments trimmed, and the code was read to confirm both functional halves
are there. All five were then re-tested on the board against upstream's code.

| Patch | PR | Fix | Re-verified on RC1.0-26 |
|---|---|---|---|
| `0002-u2-logid-config-bit` | [#21](https://github.com/epagris/flexPTP/pull/21) | logid stored; `CONFIG_LOG_ALL` derived | Round trip `0xbf`, record accepted, flags restored |
| `0003-u3-locked-log-prefix` | [#25](https://github.com/epagris/flexPTP/pull/25) | No orphan `[LOG-LCKD]` | 0 orphans in 25 s |
| `0004-u4-p2p-8023-dest-sticky` | [#24](https://github.com/epagris/flexPTP/pull/24) | Custom 802.3 destinations recomputed | gPTP → defp2p: 233 primary, 47 peer-delay |
| `0005-u5-stale-pbuf-tx-cb` | [#22](https://github.com/epagris/flexPTP/pull/22) | Untag the pbuf | `ptp.drop_txts` 0 under a 120 s flood |
| `0007-u7-transport-type-reset` | [#23](https://github.com/epagris/flexPTP/pull/23) | `ptp transport` resets the engine | Exercised by the U4 test |

The survivors keep their numbers. The number is how a patch is referred to in commits, the
review and the PR tables, so a gap is cheaper than renaming.

**`0002` was two changes on purpose.** Fixing the semicolon alone is worse than leaving the bug:
the `logid` bit then reaches a record that `ptp_load_config()` rejects wholesale, taking the
profile, clock offset and priorities with it and reporting the configuration as corrupt. That
was measured on the board, not reasoned about.

**`0007` was why `0004` was testable.** gPTP is the only preset that is natively 802.3, so the
only route to "802.3 with no custom destination" is the transport switch — which did not work.

## Order

**Independent and order-free.** Both are generated against `fe4b071` and touch different
files. They were rebased from their PR branches onto `RC1.0-26` so the context matches exactly:
Bazel's patcher, unlike `git apply`, does not tolerate drift.

Verified both ways rather than assumed — with `git apply` and with plain `patch -p1`, since
Bazel's applier is closer to the latter:

```bash
git clone --shared https://github.com/epagris/flexPTP.git /tmp/fp && git -C /tmp/fp checkout fe4b071
for p in patches/flexptp/*.patch; do git -C /tmp/fp apply -p1 "$p" || echo "FAILED $p"; done
```

## Regenerating

The patches are `git format-patch` output, so each carries its own commit message — which is
where the evidence lives. They are generated from the branches in the upstream working clone:

```bash
git -C ~/git/flexPTP-fixes format-patch --stdout --no-signature -1 rebase/u1-gm-clock-identity-byte-order \
    > patches/flexptp/0001-u1-gm-clock-identity-byte-order.patch
```

Regenerate from the branch, never by hand-editing the patch file, and never with
`git diff -- <file>` from a tree carrying other changes: that silently swallows them, which is
how the old `0002` first shipped with temporary instrumentation folded in.

## When a pull request merges

Delete the patch, drop its line from `//bazel/repositories.bzl` and its `exports_files` entry,
and bump the pin. Then check the fix really is in the new revision rather than assuming, the way
`RC1.0-17` was checked:

```bash
git -C /tmp/fp apply -R --check patches/flexptp/0001-u1-gm-clock-identity-byte-order.patch  # clean == present
```

A patch that no longer applies but is still listed is worse than no patch at all.
