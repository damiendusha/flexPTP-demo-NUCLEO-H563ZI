# flexPTP patches

Ten patches against `fe4b071` (`RC1.0-26`), applied **by the Bazel fetch**.

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
| `0008-u8-delay-resp-interval` | [#36](https://github.com/epagris/flexPTP/pull/36), open; fixes [#30](https://github.com/epagris/flexPTP/issues/30) | `master.c` | A Delay_Resp echoes the Delay_Req's `logMessagePeriod` (0x7F) instead of carrying `logMinDelayReqInterval`, so a ptp4l slave logs `ignore bogus delay request interval 2^127`. `PTP_LOGPER_SYNCMATCHED` (127, the same bit pattern) is mapped to the Sync interval | ptp4l warnings 1 → 0, delreq period 0 and `match` |
| `0009-u9-teardown-unlock` | [#34](https://github.com/epagris/flexPTP/pull/34), open | `nsd_lwip.c` | `unreg_task_ptp()`'s shutdown branch compares a non-negative enum with `-1`, which a short-enum ABI (arm-none-eabi) makes always false, so GCC deletes the branch; where it is live it returns holding the lwIP core lock, which stops the whole stack | two `flexptp stop`/start cycles, 0% ping loss, PTP resyncs |
| `0010-u10-chained-pbufs` | [#35](https://github.com/epagris/flexPTP/pull/35), open | `nsd_lwip.c` | Both receive paths pass `p->len`, the first pbuf's length, so a chained PTP message reaches flexPTP truncated | at `PBUF_POOL_BUFSIZE` 96 and 64: no master → SLAVE within 458 ns / 114 ns |
| `0011-u11-teardown-order` | [#39](https://github.com/epagris/flexPTP/pull/39), open | `task_ptp.c` | `unreg_task_ptp()` deleted the message queues before clearing `sPTP_operating`, left the handles dangling, and `ptp_transmit_timestamp_cb()` checked nothing -- a late TX timestamp was posted into freed memory; and it `vTaskDelete()`d the PTP task wherever it was, so a lock it held stayed held. Now: stop accepting work first, have the task stop where it holds nothing, then delete | host: `teardown_test` 3/3 (2 fail without); board: `stopstart_busy`, `stopstart_flood`, `stopstart_load` |
| `0012-u12-rx-length-version` | [#38](https://github.com/epagris/flexPTP/pull/38), open | `msg_utils.c`, `task_ptp.c` | Nothing compared the received length or `versionPTP` with the header: a short datagram was parsed partly from the previous message's bytes, a PTPv1 message as PTPv2. Now dropped in `ptp_receive_enqueue()` | host: `rx_validation_test` U12 5/5 (all fail without); board: `fuzz` |
| `0013-u13-slave-parent-only` | [#37](https://github.com/epagris/flexPTP/pull/37), open | `bmca.c`, `slave.c`, `ptp_types.h` | The slave took Sync, Follow_Up and Delay_Resp from any sender, and ignored a new Sync while waiting for a Follow_Up. Now: the selected master's port only (recorded from its Announce header), and a new Sync restarts the cycle | host: `rx_validation_test` U13 5/5 (all fail without); board: `fuzz` |
| `0014-u14-notification-fifo-length` | [#40](https://github.com/epagris/flexPTP/pull/40), open | `task_ptp.c` | Every producer pushes an item, then a notification, onto a notification FIFO of 16 shared by four FIFOs of 58 between them. A burst that leaves the PTP task behind fills it while an item FIFO has room: the item goes in, its notification is lost, and the task takes every later item late, for good -- a received message, once traffic is slow, past its TTL. A ThreadX board stopped answering Pdelay_Req after a flood and never recovered. Now the notification FIFO is as long as the four | host: `notification_test` 1/1 (fails without); board: `gptp_flood`, `master_flood` on ThreadX, which fail without |
| `0015-u15-timestamp-unsigned-shifts` | not filed yet; drafted for review | `ptp_defs.h`, `msg_utils.c` | Writing and reading a timestamp shifts signed values into their sign bit -- `FLEXPTP_htonl()` on the `int32_t` nanoseconds, `<< 16` on the `int64_t` seconds -- which is undefined behaviour on nearly every timestamp. GCC produces the right bytes anyway, so nothing ever looked wrong; UBSan found it. Now shifted as `uint32_t` and `uint64_t` | host: `byte_order_test` 3/3, which under UBSan fails without it, as do `notification_test` and `rx_validation_test`; in a plain build all pass either way. Board: identical code but for register allocation in the two functions |

`0001` is no longer only ours: another flexPTP user, Michael (`emb4fun`), reproduced it
independently with two flexPTP masters in [issue #32](https://github.com/epagris/flexPTP/issues/32)
and folded it into their [PR #33](https://github.com/epagris/flexPTP/pull/33). If #33 merges first,
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

## Host tests

[`test/`](test) runs flexPTP's real engine on the host -- its own sources, patched exactly as the
firmware fetches them -- with a fake port and network-stack driver (`fake_port.c`):

| Test | Mode | What it proves |
|---|---|---|
| `rx_validation_test` | flexPTP's own OS-less mode, driven synchronously against a simulated ptp4l-like master | U12 and U13: ten interference patterns, each placed before a Sync and between it and its Follow_Up, each checked by completed cycles, by Syncs accepted (exactly the master's), and by no clock step; three positive controls, including 2.0 and 2.1 version bytes and padded frames |
| `notification_test` | OS-less, gPTP profile, a link peer sending Pdelay_Req | U14: the task starved while heartbeats and the peer's frames arrive, then running again: every later Pdelay_Req must be answered at once, with its own sequenceId; and the same with no starvation, as the control |
| `byte_order_test` | OS-less, the timestamp codec alone | U15: nanosecond values either side of 2^23 and a seconds value whose last wire byte is `0xe0`, round-tripped and checked byte for byte. It needs UBSan to fail without the fix (`--config=asan`): a plain build gets the right bytes either way |
| `teardown_test` | flexPTP's FreeRTOS binding on FreeRTOS's POSIX port, with an "electric fence" heap that makes freed memory inaccessible | U11: a TX timestamp after the stop touches no deleted queue; a stop while the PTP task is inside a log line leaves no lock held; three stop/start cycles leak nothing |

```bash
bazel test //patches/flexptp/test:all
```

Each fix's tests fail without it, and only its own: run them against a flexPTP checkout that lacks
a patch with `--override_repository`. From the working clone's branches:

```bash
C=~/git/flexPTP-fixes
git -C $C checkout series/rc26~5      # everything but 0011-0015
bazel test --override_repository=flexptp=$C //patches/flexptp/test:all   # 13 tests fail
git -C $C checkout series/rc26~2      # everything but 0014 and 0015
bazel test --override_repository=flexptp=$C //patches/flexptp/test:all   # 1 fails: U14's
git -C $C checkout series/rc26~1      # everything but 0015 -- UBSan is what fails it
bazel test --features=ubsan --override_repository=flexptp=$C //patches/flexptp/test:all   # 3 targets fail: UBSan stops each at its first report
git -C $C checkout series/rc26        # all of it
bazel test --features=ubsan --override_repository=flexptp=$C //patches/flexptp/test:all   # all pass
```

Each PR branch on its own (`fix/u11-…` to `fix/u15-…`, one commit on bare `fe4b071`) passes its
own tests and fails the others'. It also fails the two slave controls in `rx_validation_test`,
`SlavesToTheMasterAndCompletesEveryCycle` and `FollowsABetterMasterWhenOneAppears`: without `0001`
a remote master's clock identity is byte-reversed on receive (U1, #20), so the BMCA records the
wrong grandmaster. `fe4b071` plus `0001` alone passes both. So the controls are U1's regression
test too.

The clone needs the `REPO.bazel` and `BUILD.bazel` described in
[`tools/upstream/README.md`](../../tools/upstream/README.md).

## Order

**A stack, applied in the listed order.** Several touch the same files (`nsd_lwip.c` three times,
`task_ptp.c` three times), so each patch is generated against the tree the patches before it
leave, not against bare `fe4b071`: every hunk then applies at its exact line, with no offset and
no fuzz. Bazel's patcher, unlike `git apply`,
does not tolerate drift, and an offset that `patch` forgives today is a failure after the next
edit.

The pull requests stay independent. Each fix is also a branch of its own, one commit off
`fe4b071`, and the stack is the same ten changes cherry-picked in order onto branch
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
       0009-u9-teardown-unlock 0010-u10-chained-pbufs 0011-u11-teardown-order
       0012-u12-rx-length-version 0013-u13-slave-parent-only 0014-u14-notification-fifo-length
       0015-u15-timestamp-unsigned-shifts)
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
were generated against a tree that had the merged one applied, and upstream may have trimmed it.
Then check the fix really is in the new revision rather than assuming, the way `RC1.0-17` was
checked:

```bash
git -C /tmp/fp apply -R --check patches/flexptp/0001-u1-gm-clock-identity-byte-order.patch  # clean == present
```

A patch that no longer applies but is still listed is worse than no patch at all.
