# flexPTP patches

Five patches against `fe4b071` (`RC1.0-26`), applied **by the Bazel fetch**.

`//bazel/repositories.bzl` lists them in the `patches` attribute of the `new_git_repository`
for `@flexptp`, so they are part of fetching the dependency: the patched tree is what lands in
the repository cache, keyed by content. Nothing has to be applied by hand, and there is no
working tree that can be pristine, patched, or neither.

**Each is, or is ready to be, filed upstream, and is deleted as its pull request merges.** They
are here because the firmware needs them now, not because they belong here.

## Contents

| Patch | PR | File | What it fixes | Verified on RC1.0-26 |
|---|---|---|---|---|
| `0001-u1-gm-clock-identity-byte-order` | [#20](https://github.com/epagris/flexPTP/pull/20), open | `msg_utils.c` | The Announce body is the only place a clock identity is byte-swapped on receive. That inverts the BMCA tie-break between two masters, and a remote master's identity displays reversed | Master clock ID `00a0c9fffe2621e4`, both firmwares |
| `0006-u6-pdelay-pcb-comment` | [#26](https://github.com/epagris/flexPTP/pull/26), open | `nsd_lwip.c` | Comment only: the PDelay pcbs are receive-only | Suite unchanged |
| `0008-u8-delay-resp-interval` | not yet filed; fixes [#30](https://github.com/epagris/flexPTP/issues/30) | `master.c` | A Delay_Resp echoes the Delay_Req's `logMessagePeriod` (0x7F) instead of carrying `logMinDelayReqInterval`, so a ptp4l slave logs `ignore bogus delay request interval 2^127`. `PTP_LOGPER_SYNCMATCHED` (127, the same bit pattern) is mapped to the Sync interval | ptp4l warnings 1 → 0, delreq period 0 and `match` |
| `0009-u9-teardown-unlock` | not yet filed | `nsd_lwip.c` | `unreg_task_ptp()`'s shutdown branch compares a non-negative enum with `-1`, which a short-enum ABI (arm-none-eabi) makes always false, so GCC deletes the branch; where it is live it returns holding the lwIP core lock, which stops the whole stack | two `flexptp stop`/start cycles, 0% ping loss, PTP resyncs |
| `0010-u10-chained-pbufs` | not yet filed | `nsd_lwip.c` | Both receive paths pass `p->len`, the first pbuf's length, so a chained PTP message reaches flexPTP truncated | at `PBUF_POOL_BUFSIZE` 96 and 64: no master → SLAVE within 458 ns / 114 ns |

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

**A stack, applied in the listed order.** Three of the five touch `nsd_lwip.c`, so each patch is
generated against the tree the patches before it leave, not against bare `fe4b071`: every hunk
then applies at its exact line, with no offset and no fuzz. Bazel's patcher, unlike `git apply`,
does not tolerate drift, and an offset that `patch` forgives today is a failure after the next
edit.

The pull requests stay independent. Each fix is also a branch of its own, one commit off
`fe4b071`, and the stack is the same five changes cherry-picked in order onto branch
`series/rc26` in the working clone. `git patch-id --stable` matches each series commit to its PR
branch, so what the firmware builds is what would be filed.

Verified with plain `patch -p1`, since Bazel's applier is closer to that than to `git apply`:

```bash
git clone --shared https://github.com/epagris/flexPTP.git /tmp/fp && git -C /tmp/fp checkout fe4b071
for p in patches/flexptp/*.patch; do patch -d /tmp/fp -p1 --no-backup-if-mismatch < "$p" | grep -E 'offset|fuzz|FAILED' && echo "DRIFT in $p"; done
```

## Regenerating

The patches are `git format-patch` output, so each carries its own commit message — which is
where the evidence lives. They are generated from the `series/rc26` stack in the upstream working
clone, one per commit, oldest first:

```bash
C=~/git/flexPTP-fixes
names=(0001-u1-gm-clock-identity-byte-order 0006-u6-pdelay-pcb-comment 0008-u8-delay-resp-interval
       0009-u9-teardown-unlock 0010-u10-chained-pbufs)
i=0; for c in $(git -C $C rev-list --reverse fe4b071..series/rc26); do
    git -C $C format-patch --stdout --no-signature -1 "$c" > "patches/flexptp/${names[i]}.patch"; i=$((i+1))
done
```

A fix edited on its PR branch is cherry-picked onto the series again, and every patch after it
regenerated: their context depends on it.

Regenerate from the branch, never by hand-editing the patch file, and never with
`git diff -- <file>` from a tree carrying other changes: that silently swallows them, which is
how the old `0002` first shipped with temporary instrumentation folded in.

## When a pull request merges

Delete the patch, drop its line from `//bazel/repositories.bzl` and its `exports_files` entry,
and bump the pin. Rebase `series/rc26` onto the new pin and regenerate the patches after it: they
were generated against a tree that had the merged one applied, and upstream may have trimmed it. Then check the fix really is in the new revision rather than assuming, the way
`RC1.0-17` was checked:

```bash
git -C /tmp/fp apply -R --check patches/flexptp/0001-u1-gm-clock-identity-byte-order.patch  # clean == present
```

A patch that no longer applies but is still listed is worse than no patch at all.
