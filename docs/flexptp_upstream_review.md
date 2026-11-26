# Review — flexPTP `RC1.0-11` → `RC1.0-17`

Reviewed at pin `494e644a3218c5867ee7397c1e9c019667b664b1` (`RC1.0-17`, 2026-09-07), against the
previous pin `76aaf67` (`RC1.0-11`). Six commits, 23 files, +600/−237.

Everything this repository carried as a patch is now upstream, so the original
`//patches/flexptp` series is gone. The seven findings below replaced it: they are filed
upstream and, until those pull requests merge, carried as a **new** patch series in
[`//patches/flexptp`](../patches/flexptp/README.md), applied by the fetch. Each is deleted from
there as its PR lands. `//patches/embfmt` is unchanged — embfmt has no new commits.

## 1. What landed

| Commit | Origin | Content |
|---|---|---|
| `2adbe6a` | ours, [PR #15](https://github.com/epagris/flexPTP/pull/15) | Servo NaN latch. Three guards in `slave.c` and `kalman_filter.c` |
| `36ae306` | ours, [PR #17](https://github.com/epagris/flexPTP/pull/17) | `nsd_lwip.c` checks `pbuf_alloc()` before writing through it |
| `2a09cac` | ours, [PR #18](https://github.com/epagris/flexPTP/pull/18) | `PTP_IGMP_PRIMARY_INIT` / `PTP_IGMP_PEER_DELAY_INIT` for a port-supplied stack |
| `e873105` | ours | Non-blocking enqueues, **extended** by upstream to the receive, transmit and TX-timestamp paths and to the FreeRTOS branch, plus drop counters |
| `7bc544e` | maintainer | NSD rework: `NsdInitSettings`, per-message-type PDelay routing, custom P2P 802.3 destinations, GM ClockID byte order, `[LOG-*]` prefixes |
| `494e644` | maintainer | `CMakeLists.txt` and README |

Patches `0002` and `0003` reverse-apply cleanly against `HEAD`, i.e. they are present verbatim.
`0004` does not, because upstream went further than we did — correctly.

**Upstream improved on our patch.** `e873105` fixed the FreeRTOS branch we deliberately left
alone, and added the drop counters we did not propose. `ptp_receive_enqueue()` on FreeRTOS
gained an `xPortIsInsideInterrupt()` split it did not have before.

## 2. Findings

Severity is about consequence, not effort. **U1**, **U2** and **U3** are regressions introduced
by `7bc544e`; **U4**–**U6** are pre-existing or newly exposed.

---

### U1 — A remote master's clock identity is byte-reversed · **confirmed on hardware**

`7bc544e` changed `ptp_print_clock_identity()` to print `p[0..7]` instead of `p[7-i]`, and
removed the `FLEXPTP_ntohll()` from `ptp_construct_binary_announce_message()`. Both changes are
right. The matching change on the **parse** side was not made, so `msg_utils.c:139` still does:

```c
pAnnounce->grandmasterClockIdentity = FLEXPTP_ntohll(pAnnounce->grandmasterClockIdentity);
```

flexPTP's convention everywhere else is that a `uint64_t` clock identity holds the eight octets
in **memory = wire order**: `ptp_create_clock_identity()` builds it that way, and the header's
`clockIdentity` is `memcpy`'d in both directions with no swap. The Announce body's
`grandmasterClockIdentity` is now the only quantity that does not follow it.

Measured on the bench, board slaved to `ptp4l` on a NIC with MAC `00:a0:c9:26:21:e4`:

```
Own clock ID:    56001efffe000451     <- correct (note ff fe in the middle)
Master clock ID: e42126feffc9a000     <- byte-reversed
```

`bytes.fromhex('e42126feffc9a000')[::-1].hex()` is `00a0c9fffe2621e4`, the correct identity.
Before `7bc544e` the *own* ID printed reversed and the master's printed correctly; the commit
swapped which one is wrong rather than removing the inconsistency.

A second consequence is not cosmetic. `bmca.c:80-81` compares the two conventions directly:

```c
bmCI  = S.bmca.masterProps.grandmasterClockIdentity;  // parsed  -> reversed
ourCI = S.capabilities.grandmasterClockIdentity;      // ours    -> wire order
...
if (master_mode_enabled && (bmCI == ourCI)) { state = PTP_BMCA_PRE_MASTER; }
```

These cannot compare equal when they should. It is latent today because `masterProps` is
seeded from `S.capabilities` in `INITIALIZING`, so the common path compares a value against
itself; it bites when the board must recognise its own Announce coming back.

**Fix:** drop the `ntohll` at `msg_utils.c:139`. That makes parse match construct, print, the
header codec and `ptp_create_clock_identity()` in one move. `ptp_select_better_master()`'s
`COMPARE_AND_RETURN` on the identity then becomes a big-endian-ordered comparison, which is
what IEEE 1588 asks for and what it is not today.

---

### U2 — The `logid` config bit is never stored, and storing it would invalidate the config · **two bugs masking each other**

`config.c:40-41`:

```c
                   CONFIG_ADD_LOGGING(S.logging.transmission, CONFIG_LOG_TRANSMISSION);
                   CONFIG_ADD_LOGGING(S.logging.logid, CONFIG_LOG_LOGID);
```

The `;` ends the assignment, so the `logid` line is a discarded expression statement and the
bit is never persisted.

Fixing only that makes things **worse**. `CONFIG_LOG_LOGID` is `0x80`, but `CONFIG_LOG_ALL` was
left at `0x7F`, and `ptp_load_config()` rejects the whole record on any bit outside it:

```c
invalid |= pConfig->logging & (~CONFIG_LOG_ALL);
...
if (invalid) { MSG("The retained flexPTP configuration got corrupted, loading aborted!\n"); return; }
```

So a config saved with `logid` on would be discarded entirely — profile, offset, priorities and
all — with a message blaming corruption.

**Fix:** change `;` to `|` **and** widen `CONFIG_LOG_ALL` to `0xFF`. Either alone is wrong.

---

### U3 — `[LOG-LCKD]` prints on every Sync with no message behind it · **confirmed on hardware**

`stats.c`:

```c
CLILOG(S.logging.logid && S.logging.locked, "[LOG-LCKD] ");
CLILOG(S.logging.locked && (locked != S.stats.locked), "PTP %s!\n", ...);
```

The prefix is missing the `locked != S.stats.locked` term that gates the message it prefixes.
`ptp_collect_stats()` runs once per Sync, so the prefix prints every cycle and the message
almost never does. With `ptp log logid on` and `ptp log locked on`, 25 seconds of console:

```
[LOG-LCKD] [LOG-LCKD] [LOG-LCKD] [LOG-LCKD] [LOG-LCKD] [LOG-LCKD] ... (25 of them, nothing else)
```

**Fix:** give the prefix the same condition as the line — this is the pattern `master.c`'s
`PTP_MASTER_P2P_SLAVE_STATE_LOG` already gets right.

---

### U4 — Custom P2P 802.3 destinations are sticky across a profile change

`nsd_lwip.c`, new in `7bc544e`:

```c
if (memcmp(init->primary_p2p_8023_dest, zero_mac, mac_size)) {
    memcpy(custom_p2p_8023_primary_dest, &init->primary_p2p_8023_dest, mac_size);
    custom_p2p_8023_primary_dest_valid = true;
}
```

The `_valid` flags are only ever set to `true`. An all-zero field means "use the standard
address", but it is expressed by *not* taking the branch, so the previous profile's override
survives. `ptp_nsd_init()` runs on every profile change, so `ptp profile preset gPTP` followed
by `ptp profile preset default` leaves the board transmitting to `01:80:C2:00:00:0E` while
reporting the default profile.

**Fix:** compute both flags unconditionally from the incoming settings, as this repository's
`nsd_netxduo.c` now does.

---

### U5 — `nsd_lwip.c` leaves `tx_cb` on a pbuf it frees, so recycled pbufs get spurious hardware TX timestamps · **confirmed on hardware, 284 392 events**

`nsd_lwip.c:166-167` tags the pbuf and never untags it:

```c
p->tag   = (void *)uid;
p->tx_cb = ptp_transmit_cb;
...
pbuf_free(p);
```

`tx_cb` lives in `LWIP_PBUF_CUSTOM_DATA`, which lwIP does not initialise on allocation — it only
manages its own fields. So a later `pbuf_alloc(..., PBUF_RAM)` landing on the same recycled heap
block inherits a live `tx_cb`, and a port that gates hardware timestamping on `p->tx_cb != NULL`
— which is the only signal `nsd_lwip.c` offers — requests a timestamp for a frame that never
asked for one, then calls `ptp_transmit_timestamp_cb()` with a stale `uid`.

Measured on a NUCLEO-H563ZI, 120 s flood at 76 543 pps / 78.4 Mbit/s:

| | `ptp.drop_txts` |
|---|---|
| FreeRTOS + lwIP (`nsd_lwip.c`) | **284 392** |
| ThreadX + NetX Duo (own NSD, uid through a side channel) | **0** |

A controlled pair isolates it further — same receive load, transmit volume varied:

| 30 s flood | `ptp.drop_txts` |
|---|---|
| multicast only (board replies to nothing) | 0 |
| multicast + unicast (board sends ICMP port-unreachable) | 42 531 |

It tracks **transmit** volume, not receive pressure. Every one of those 284 392 events is a
`sTxCbFIFO` enqueue that `e873105` made non-blocking — on the exact firmware where blocking
meant a circular wait on `LOCK_TCPIP_CORE`. The new code turns them into counted drops, which
is why the board survives; before `e873105` this is the shape that deadlocked it.

**Fix:** clear `p->tx_cb` and `p->tag` in `ptp_transmit_msg()` once the frame is handed to the
stack, or document that a port must zero `LWIP_PBUF_CUSTOM_DATA` on allocation. The first is
cheaper and keeps the contract inside the file that creates it.

---

### U6 — The PDelay pcbs are bound but never transmitted from

`7bc544e` creates four pcbs where there were two, but the transmit path still selects only the
primary pair:

```c
struct udp_pcb *conn = (mc == PTP_MC_EVENT) ? PTP_L4_PRIMARY_EVENT : PTP_L4_PRIMARY_GENERAL;
ip_addr_t ipaddr = isPDel_ ? PTP_IGMP_PEER_DELAY : PTP_IGMP_PRIMARY;
udp_sendto(conn, p, &ipaddr, port);
```

`PTP_L4_PDELAY_EVENT` and `PTP_L4_PDELAY_GENERAL` are receive-only. It works — `udp_sendto()`
overrides the destination and both pairs bind the same local ports — so this is tidiness, not a
defect. Worth a comment saying the PDelay pcbs exist to *receive*, before someone "fixes" the
selection and changes the source binding.

---

### U7 — `ptp transport` does not take effect · **found while testing U4, confirmed on hardware**

`ptp_set_transport_type()` writes the profile field and stops, while its immediate neighbour
does the necessary second half and says why:

```c
void ptp_set_transport_type(PtpTransportType tp) {
    S.profile.transportType = tp;
}

void ptp_set_delay_mechanism(PtpDelayMechanism dm) {
    S.profile.delayMechanism = dm;
    ptp_reset(); // this is mandatory
}
```

It is equally mandatory here. Every network stack driver latches the transport in its own state
at `ptp_nsd_init()` time — `nsd_lwip.c` keeps it in a static `TP` — and nothing else re-runs
that. So the core believes one thing and the driver another: `ptp transport 802.3` prints
`Transport layer: 802.3`, `ptp info` agrees, and the driver carries on with UDP sockets.

Measured on the FreeRTOS + lwIP firmware, raw-802.3 PTP frames transmitted in 100 s after
`ptp profile preset defp2p` followed by `ptp transport 802.3`:

| | Frames |
|---|---|
| before | **0** |
| after | **280** |

Zero from a cold boot as well, so it is not stale state. The gPTP preset hid it: its own
`transportType` is already 802.3 and a profile load resets the engine anyway, so the defect only
appears when the transport is changed on its own.

**This is why U4's negative case was untestable.** gPTP is the only preset that is natively
802.3, so the only route to "802.3 with no custom destination" is the transport switch — which
did not work. Fixing U7 first is what made U4 measurable.

---

### Minor

- **`xQueueSendFromISR(..., NULL)`** in `ptp_receive_enqueue()`, `ptp_transmit_enqueue()` and
  `ptp_transmit_timestamp_cb()` discards `pxHigherPriorityTaskWoken`, so a woken `task_ptp()`
  waits for the next tick instead of preempting. The previous code passed `&hptWoken` and never
  used it, so nothing regressed — but with the enqueues now on the hot path it is a real
  latency floor on the FreeRTOS port.
- **The drop paths `CLILOG` from ISR-reachable code.** `ptp_transmit_timestamp_cb()` is
  explicitly ISR-reachable on FreeRTOS and calls `CLILOG(S.logging.info, "Failed to enqueue
  transmit timestamp...")` on every drop — 284 392 of them in the run above. This port is safe
  only because its `MSG()` refuses ISR output and counts instead (F68); a port without that
  guard gets an unbounded `printf` storm from an interrupt. Worth a note in
  `network_stack_driver.h` that `MSG()` must be ISR-safe or non-blocking.
- **`{}` as an empty initialiser** (`ptp_core.c`, `task_ptp.c`, `ptp_profile_presets.c`) is
  C23; before that it is a GNU extension. Fine under `gnu11`, a warning under `-std=c11
  -pedantic`. `{0}` costs nothing.
- **`enqueueOK = xQueueSend(...)`** in the non-ISR arm of `ptp_transmit_timestamp_cb()` omits
  the `== pdPASS` its sibling arms all have. Correct by the values' definitions, inconsistent
  to read.
- **`bmca.c`'s `SBMC_PRINT_LOG`** now expands to a braced `if`, so it no longer composes with a
  trailing `else`. Only one call site, and it is a statement — but `do { } while (0)` is the
  form that cannot surprise anyone.

## 3. What this repository changed to take the pin

- `bazel/repositories.bzl` — pin to `494e644`, `patches`/`patch_args` removed.
- `//patches/flexptp` deleted; `//patches/embfmt` unchanged.
- `ptp_nsd_init()` takes `const NsdInitSettings *` in `nsd_netxduo.c` and `nsd_stub.c`.
  `nsd_netxduo.c` honours the profile's 802.3 destination overrides, recomputing the validity
  flags on every init rather than latching them — U4 above.
- `ethstat` on both firmwares exposes `ptp.drop_rx`, `ptp.drop_tx` and `ptp.drop_txts`, and
  `check_counters` asserts them zero on a healthy bench.

Neither firmware needed a source change for anything else. Both build clean and all 19 host
tests pass.

## 4. The fixes

Seven branches off `494e644`, each touching a disjoint set of lines so they can be merged,
rejected or reordered in any combination. Every one is verified on a NUCLEO-H563ZI; where a
before/after number exists, it is a measurement and not an argument.

| | Branch | Verification | Before | After |
|---|---|---|---|---|
| U1 | `fix/u1-gm-clock-identity-byte-order` | Master clock ID vs the host NIC's derived identity, both firmwares | `e42126feffc9a000` | `00a0c9fffe2621e4` |
| U2 | `fix/u2-logid-config-bit` | Config round trip through a CLI hook, three states | logid bit missing; semicolon-only fix → **whole record rejected** | round-trips, every flag restored |
| U3 | `fix/u3-locked-log-prefix` | 25 s of console, logid + locked on | 25 orphan prefixes | 0 orphans, prefix on every real transition |
| U4 | `fix/u4-p2p-8023-dest-sticky` | Destination histogram over 280 frames, gPTP → defp2p | 0 frames to the primary group | 233 to primary, 47 to peer-delay |
| U5 | `fix/u5-stale-pbuf-tx-cb` | `ptp.drop_txts` over a 120 s, 78 Mbit/s flood | **284 392** | **0** |
| U6 | `fix/u6-pdelay-pcb-comment` | Comment only; full suite unchanged | — | — |
| U7 | `fix/u7-transport-type-reset` | 802.3 frames after a runtime transport switch | 0 | 280 |

All seven apply together without conflict, and with all seven applied both firmwares pass the
full S8 hardware stage — ThreadX 14/14, FreeRTOS 13/14 with one pre-existing skip — including
the 120-second saturation flood.

File them with [`tools/upstream/file_flexptp_prs.sh`](../tools/upstream/file_flexptp_prs.sh),
one at a time. See [`tools/upstream/README.md`](../tools/upstream/README.md).

## 5. Order to file in

By consequence, not by number — a maintainer who reads only the first two should have read the
two that lose data.

1. **U1** — wrong identity displayed, and a BMCA comparison that cannot match
2. **U2** — a saved configuration is discarded if the obvious one-character fix is applied alone
3. **U5** — spurious hardware timestamps proportional to transmit volume
4. **U7** — a documented control silently does nothing
5. **U4** — a profile change silently does not take effect
6. **U3** — console noise
7. **U6** — tidiness; bundle with the minors

All seven are target-independent.

## 6. Update — `RC1.0-26` (`fe4b071`), 2026-11-24

### Our seven

Five are merged, all re-tested on the board against upstream's own code rather than our patches:

| | PR | How it landed | Re-verified on RC1.0-26 |
|---|---|---|---|
| U2 | #21 | comments trimmed; both functional halves intact | round trip `0xbf`, accepted, flags restored |
| U3 | #25 | comment shortened; logic identical | 0 orphan prefixes in 25 s |
| U4 | #24 | verbatim | gPTP → defp2p: 233 primary / 47 peer-delay, as in September |
| U5 | #22 | verbatim | `ptp.drop_txts` 0 under a 120 s flood |
| U7 | #23 | verbatim | exercised by the U4 test |
| **U1** | **#20** | **open**, mergeable | carried as `0001`; master clock ID correct on both firmwares |
| **U6** | **#26** | **open**, mergeable | carried as `0006` |

### U5 now has a regression gate

Upstream's #22 fixed U5 at its source, but nothing here would have noticed it coming back: the
flood check reported `ptp.drop_txts` and passed whatever it read, which is exactly how U5 hid in
the first place. Two things are ported from a September branch that was never merged:

- `check_ptp_flood` **fails** if `ptp.drop_txts` moves by more than 16 during the flood, and says
  so explicitly when the counter could not be read rather than passing as if it were 0.
- the lwIP driver clears `tx_cb`/`tag` once it has read them, a second line of defence that
  stops a stale tag propagating through recycled buffers whatever produced it.

Each was proven against the defect rather than assumed, on the FreeRTOS + lwIP firmware with the
U5 bug put back into `nsd_lwip.c`: driver clear present → `drop_txts` 0, check passes; driver
clear also removed → `drop_txts` 194 237, check **fails**.

### U1 is not latent — correction

§2 called U1's BMCA consequence "latent". That understated it. `ptp_select_better_master()` compares
two *parsed* identities, and byte reversal does not preserve numeric order — so whenever two masters
compete, the tie-break on identity picks the wrong one. That is live, not latent.

It was shown by someone else. In [issue #32](https://github.com/epagris/flexPTP/issues/32), Michael
(`emb4fun`) ran two flexPTP masters with MACs `06:CA:FE:33:44:55` and `02:CA:FE:33:44:55`. Without
#20 the device with the *larger* identity stayed master; with it the smaller one correctly won. He
notes he did not at first see why #20 fixed anything, and tested it to find out.

### PR #33 (emb4fun)

[#33](https://github.com/epagris/flexPTP/pull/33) carries #20's change verbatim, credits it, and adds
`ptp_slave_reset()` before `ptp_slave_enable()` on the transition into SLAVE. His symptom: after
SLAVE → MASTER → SLAVE the servo printed `26810515154010112000.0000` and the offset walked away.

Tested on this board, FreeRTOS + lwIP firmware, host master stopped so the board takes over, ~65 s
as master, host master restarted. Two runs each:

| | First offsets after return | Servo values | Converged (last 15 s) |
|---|---|---|---|
| RC1.0-26 as pinned (no reset) | 5.9 µs, −9.4 µs | normal from the first sample | ≤ 132 ns, ≤ 477 ns |
| with #33 | 63 µs, 59 µs | first correction 0.0, then tens of thousands of ppb | ≤ 308 ns, ≤ 312 ns |

So **Michael's failure does not reproduce here**, and #33 costs this configuration a re-acquisition
transient roughly ten times larger by discarding servo state that was not stale. It is harmless in
steady state. The likely difference is configuration: his log has two servo columns, i.e. flexPTP's
HLT clock interface; this board uses the addend interface with the Kalman servo. Worth telling him
— a reset scoped to the state that actually goes stale might fix his case without costing ours —
but that is a hypothesis; nothing here identifies which state that is.

A note on method, because the first attempt at this test was wrong. The host master unit then
copied system time into the PHC on every start, which stepped the master by the PHC's accumulated
drift — 17.2 ms on that run, under flexPTP's 20 ms coarse threshold, so the board had to slew it and
produced servo values above 10⁶ ppb that looked like Michael's bug. The units now reset only the
PHC's rate on start (see `tools/hil/privileged/`), and the figures above are from step-free runs.

### Issue #30 is still open

ptp4l slaves log `ignore bogus delay request interval 2^127` against a flexPTP master. `9eefd95`
("Delay_Req logMsgPeriod was set to standard 0x7F value") looks like the fix but is not:

- it changes **Delay_Req**, which IEEE 1588 does set to 0x7F — correct, but not what #30 is about;
- #30 is about **Delay_Resp**, which must carry the master's `logMinDelayReqInterval`.
  `ptp_send_delay_resp_message()` copies the incoming Delay_Req header and never sets
  `logMessagePeriod`, so it echoes whatever the slave sent. ptp4l sends 0x7F; so, since `9eefd95`,
  does a flexPTP slave.

Measured with the board as master and ptp4l as slave (`flexptp-ptp4l-slave` unit), fresh ptp4l
process each time (it rate-limits the warning to once per 300 s):

| Board firmware | ptp4l reaches SLAVE | warnings |
|---|---|---|
| RC1.0-26 as pinned | yes | **1**, on the first Delay_Resp |
| + `header.logMessagePeriod = S.profile.logDelayReqPeriod;` | yes, path delay 558 ns | **0** |

ptp4l still uses the Delay_Resp for the path delay either way; what it cannot do is learn the
master's interval. A real fix must also map `PTP_LOGPER_SYNCMATCHED` — which is **127**, the same bit
pattern as 0x7F — to something meaningful such as `logSyncPeriod`, rather than copy it through. Not
filed; the experiment was on a scratch branch. **Since done properly as U8 — see §7.**

The board as master, incidentally, holds a ptp4l slave to about ±30 ns.

### New upstream code, tested

| Commit | Change | Test | Result |
|---|---|---|---|
| `0800cbf` (#28) | IEC/IEEE 61850-9-3 Power Utility Profile preset | `ptp profile preset PUP` on lwIP | 802.3, P2P, domain 93 |
| `3b33b89` (#29) | PUP primary MAC typo `01:18:19` → `01:1B:19`; lwIP L2 receive filter uses the profile's destinations | 802.3 destination histogram under PUP, 100 s | 295 → `01:1B:19:00:00:00`, 50 → `01:80:C2:00:00:0E`, none to `01:18:19` |
| `3b33b89` (#29) | same receive filter, gPTP | gPTP against a ptp4l gPTP grandmaster, lwIP | SLAVE, 240 samples / 30 s, ≤ 96 ns |
| `9eefd95` | Delay_Req `logMessagePeriod` 0x7F | full S8 against ptp4l, both firmwares | unaffected; see #30 above |

### Full results at RC1.0-26 + `0001` + `0006`

Both firmwares pass the full S8 stage including the 120 s flood (ThreadX 14/14, FreeRTOS 13/14 with
the long-standing `threads` skip), and gPTP reaches SLAVE on both, 8 Sync/s: steady state 52–85 ns (ThreadX) and 60–96 ns
(FreeRTOS) across two runs. `ptp_sync` in the final S8 runs: 206 ns (FreeRTOS), 350 ns (ThreadX).

One harness fix came out of this. `ptp_sync` sampled from the moment the board selected a master, so
its figure depended on which checks ran before it: ~800 ns in S8 order, 7–21 µs as `--checks
dhcp,ptp_sync`, both PASS because nothing asserts on the offset. It now waits until the board has
been slaved for 60 s, and reads ~100–150 ns either way.

## 7. Update — three more fixes, dependencies at head, final campaign (2026-11-26)

### Three more fixes, carried as `0008`–`0010`

Each is a branch of its own off `fe4b071` in the working clone, ready to file and **not filed**.
Each was measured on the board with and without the change.

| | Branch | Defect | Without | With |
|---|---|---|---|---|
| U8 | `fix/u8-delay-resp-interval` | Issue #30. A Delay_Resp is a copy of the Delay_Req's header, so it echoes `logMessagePeriod` 0x7F instead of carrying `logMinDelayReqInterval`. `PTP_LOGPER_SYNCMATCHED` is 127 — the same bit pattern — so it is mapped to the Sync interval it means | ptp4l slave warns once per process (`bogus delay request interval 2^127`), for delreq period 0 and for SYNCMATCHED; copying the period naively still warns for SYNCMATCHED | 0 warnings in both cases; ptp4l holds the board within ~±20 ns |
| U9 | `fix/u9-teardown-unlock` | `unreg_task_ptp()` shuts the lwIP NSD down with `{-1, -1}` in two non-negative enums. arm-none-eabi uses short enums (`Tag_ABI_enum_size: small`), so they hold 255, `== -1` is always false and GCC deletes the branch. Where the branch is live it returns holding `LOCK_TCPIP_CORE()` | upstream: branch compiled out, stack survives by accident; comparison fixed alone: stack dead after one stop (100% ping loss) | two stop/start cycles, 0% loss, PTP resyncs |
| U10 | `fix/u10-chained-pbufs` | Both lwIP receive paths pass `p->len`, the first pbuf's length, so a chained message reaches flexPTP truncated | `PBUF_POOL_BUFSIZE` 96 (IPv4) and 64 (802.3): master ID `0000000000000000`, no samples | SLAVE within 458 ns and 114 ns |

`nsd_netxduo.c`, ours, made the same `-1` comparison; on this ABI it was always *true* and reached the
right state only because 255 matches neither transport. It is now type-correct. flexPTP's
`nsd_etherlib.c` and `nsd_linux.c` compare the same way and are untouched — nothing here can test them.

U9 needed a way in: nothing in either firmware called `unreg_task_ptp()`. Both now have
`flexptp stop`.

The August branch that U9 and U10 came from carried a third driver change, a guard against an
802.3 runt underflowing `p->len - 14`. **Dropped as unreachable**: lwIP's `ethernet_input()`
discards every frame of 14 bytes or less before `LWIP_HOOK_UNKNOWN_ETH_PROTOCOL` runs — still true
at lwIP `3aa9557`.

### Dependencies at upstream head

| Dependency | Was | Now | Board result |
|---|---|---|---|
| STM32H5 HAL | `v1.7.0-8-g0fff6e2` | `v1.7.0-13-gdeaf9d1` | both images byte-identical across the bump |
| FreeRTOS-Kernel | V11.3.0 (`9b777ae`, a release branch) | `main` head `83a45bf` — reports itself as `V11.1.0+` | S8 13/14, gPTP 240/30 s |
| lwIP | `STABLE-2_2_1_RELEASE-34` | `-104` (`3aa9557`) | S8 13/14, flood `drop_txts` 0 |
| ThreadX | `44d7c95` | `v6.5.2.202603b_rel` (`b2868c3`) | S8 14/14 |
| NetX Duo | `473d192` | `v6.5.2.202603b_rel` (`c690f59`) | S8 14/14 |
| flexPTP | — | `fe4b071` (`RC1.0-26`), master head, plus `0001` `0006` `0008` `0009` `0010` | below |

embfmt and ST's CMSIS-RTOS2 wrapper were already at head.

### The final campaign: everything together, both firmwares

The final images — this section's three firmware fixes included (FreeRTOS build ID `60bd3664…`,
ThreadX `1c3b041c…`) — verified on the board by build ID before every run.

| | FreeRTOS + lwIP | ThreadX + NetX Duo |
|---|---|---|
| Full S8 | 13/14 (the long-standing `threads` skip); `ptp_sync` ≤ 157 ns | 14/14; ≤ 464 ns |
| U1–U4, PUP probes | pass (U4 and PUP are lwIP-only) | U1–U3 pass |
| gPTP, 60–120 s after slaving to a ptp4l gPTP grandmaster | 480 Sync / 60 s; two runs: median 135 and 32 ns, max 225 and 100 ns | median 32 ns, max 119 ns |
| Host master stopped → board MASTER → back to SLAVE | no non-finite servo output; last 15 within 439 ns | within 208 ns |
| `flexptp stop` / start, two cycles (U9) | stack alive after each stop, PTP resyncs | same |
| Board as master, ptp4l slave (U8), delreq 0 and SYNCMATCHED | 0 warnings, offsets ≤ 13 ns | 0 warnings, ≤ 17 ns |
| 120 s flood | 76 Mbit/s, 0% loss, `drop_txts` 0 | 78 Mbit/s, `drop_txts` 0 |
| 600 s flood | offered 106 Mbit/s (above line rate), 0% loss, `drop_txts` 0 | 93 Mbit/s, `drop_txts` 0 |
| Flood during gPTP | 0 samples during; 240/30 s, ≤ 197 ns after | same; ≤ 184 ns after |
| Flood with the board as master | ptp4l loses it during; back in SLAVE, last 10 ≤ 8 ns | same; last 10 ≤ 20 ns |
| Stop/start during a saturation flood | commands run as the flood ends; stack alive, PTP resyncs | same |
| Stop/start at 20 kpps | CLI starved (below); stack alive, PTP resyncs after | 4/4 commands in 0.2 s, stack alive, PTP resyncs |
| Console after three silent 120 s floods | intact 3/3 | intact 3/3 |
| Malformed and foreign PTP (below) | survives, recovers | survives, recovers |
| FreeRTOS heap walk at the end | consistent | — |

The converged gPTP figure moves by a factor of four from run to run on the same image, so it
says nothing about the fixes: in the same session the image before them measured median 55 ns
(max 129) and the pre-session image 114 ns (max 180). The earlier "≤ 45–96 ns" figures were
taken over the first 30 s after slaving, inside the transient a profile switch causes, and are not
comparable.

### What saturation does, and whether it is new

The same probes were run on the pre-session image (`9aef180`: RC1.0-26 with `0001` and `0006`,
old dependency pins).

- **PTP stops during a saturation flood, on both firmwares and both images, and recovers after.**
  As slave: no Sync cycles complete during the flood. As master: the ptp4l slave watching it drops
  to LISTENING. Both resynchronise once the flood ends. Not new.
- **The CLI is starved during a saturation flood, on every image.** FreeRTOS answers nothing until
  the flood ends; ThreadX answered one request in two, after 10–34 s. On FreeRTOS it extends down
  to 20 kpps (20 Mbit/s): no `osinfo` answered in 120 s on the current image, the image with
  FreeRTOS put back to V11.3.0, the image with lwIP put back to its old pin, and the baseline
  alike. SWD sampling puts 30–38 of 40 samples in the TCP/IP and rx threads. ThreadX at 20 kpps
  answers in 0.2 s. A property of this lwIP port, not of the bumps.
- **The baseline ThreadX image, not the current one, lost PTP after a gPTP flood:** it made
  itself master (`56001efffe000451`) and the following master-mode probe got no offsets. Seen
  once; the current ThreadX image passed the same probes.

### The FreeRTOS wedge: three bugs of ours, all older than this session

Late in long runs, three times, the FreeRTOS board stopped: console garbled then silent, ping
dead, until a reset. Shorter replays mostly did not reproduce it. Caught live over SWD, it was two
different crashes and a console fault, all in this repository's firmware rather than in flexPTP,
and **the baseline image has them**: after a single 120 s flood with nothing sent to the console, the pre-session
image lost its console and its network in one of two runs.

**1. The CLI task overflows its stack in `osinfo`.** PC in `vApplicationStackOverflowHook`,
entered from PendSV, current task `cli`, tick stopped; the task's saved stack pointer 16 bytes
below its stack, and a heap walk stopping at the `heap_4` header of the stack's own allocation,
overwritten with stack contents. The dump gives the chain `task_cli` → `process_cli_line` →
`os_info` → `MSG` → `vembfmt` → `pfn_integer` → `print_number`. `os_info()` kept a 16-entry
`TaskStatus_t` array — 704 of its 776-byte frame — on the CLI's 2048-byte stack, and `osinfo`
left 80–136 bytes of it untouched when idle; every other command stops well short. Under load
the CLI is preempted constantly, each preemption saving its context on the task's own stack, and
at osinfo's deepest point that is enough. The same 34 words free after `osinfo` on the baseline,
on the image with FreeRTOS put back to V11.3.0, and with lwIP put back to its old pin.

*Fixed:* the array is `static` — only the CLI thread runs `osinfo`. The CLI's lowest point over
every command is now 640 bytes untouched, the deepest command now `flexptp` start/stop. With
`osinfo` and `ethstat` every 2 s through a 90 s flood at 2 and at 5 kpps (80 of 80 answered each
time):

| Image | CLI stack never touched, after |
|---|---|
| before the fix | **56 bytes** — 24–80 deeper than idle, from preemption |
| fixed | 760 bytes |

ThreadX shares the 2048-byte CLI stack from `lib/cli` but has no such array; measured from the
stack fill after the same deep commands, its CLI thread keeps 648 bytes and no thread fewer
than 436.

**2. The Ethernet ISR calls a garbage `tx_cb`.** HardFault (`CFSR` = IACCVIOL, forced) taken
in the ETH interrupt (xPSR exception 122), stacked PC `0xfffffe00`, return address just after
the `blx` in `ETHHW_ProcessTx()` that calls a TX descriptor's timestamp callback. That pointer
comes from `pbuf->tx_cb`, one of the `LWIP_PBUF_CUSTOM_DATA` fields — which lwIP does not
initialise. lwIP has a hook for it, `LWIP_PBUF_CUSTOM_DATA_INIT`, called from
`pbuf_init_alloced_pbuf()` at both lwIP pins; `lwipopts.h` never defined it, and the driver's
comment said no hook existed. So a pbuf lwIP allocates for itself — an ICMP reply to the flood,
ARP, IGMP — can carry stale bytes there, and the driver reads any non-NULL value as "timestamp
this frame and call this". The clear after the read, ported in September for U5, cannot catch
it: the gate trusts the field. Rare: an instrumented build that counted, instead of calling,
non-PTP frames with `tx_cb` set saw none in one 120 s flood.

*Fixed:* `lwipopts.h` defines the hook, zeroing all four custom fields on every allocation; the
driver's clear stays as the second line. Silent 120 s floods, then three full `osinfo` reads and
a ping: fixed image intact 7 of 7 (five of them back to back on one boot), baseline broken in 1
of 2.

**3. The console thread falls one completion behind and stays there.** Not a crash, but it is
what made both crashes look the way they did: after a saturation flood the board's replies come
back garbled — characters missing, lines out of order — while the network and PTP run normally.
Caught on the image with fixes 1 and 2: FIFO empty, DMA channel idle and disabled, and the console thread's
TX-complete flag *set* with nobody waiting (clear on a healthy board). `serial_io_thread()` waits
up to 500 ms for each transfer's completion and aborts on timeout; a completion arriving after
that timeout stayed set, the next wait returned at once, the next chunk was read into the buffer
the DMA was still sending, and `HAL_UART_Transmit_DMA()` then failed busy and dropped the chunk
after it. Once a flag ahead, the thread stays a flag ahead. `lib/io/serial_io.c`, shared by both
firmwares.

*Fixed:* the flag is cleared before each transfer, so a wait can be satisfied only by its own
transfer's completion.

Found in the code while chasing it, and unrelated to it in the end:

- **U11 — `unreg_task_ptp()` teardown order** (fixed by `0011`, see the next section). It deletes the message queues before
  clearing `sPTP_operating`, leaves the handles dangling, and `ptp_transmit_timestamp_cb()` has no
  `sPTP_operating` guard. A hardware TX timestamp for a frame queued before the stop and completed
  after it posts into freed queue memory. It also `vTaskDelete()`s the PTP task from the caller's
  thread, so a mutex that task holds at that moment — lwIP's core lock inside a transmit, or an
  application's console lock — stays held for ever. Reachable by any application that stops PTP;
  here only through `flexptp stop`, which the baseline does not have. The heap walks show queues
  re-allocated at the same addresses on restart, which makes most late callbacks harmless, so the
  window that actually corrupts is narrow. Not demonstrated on hardware.

### Malformed and foreign PTP traffic

Nine traffic classes, 30 s each at 10 packets/s to 224.0.1.129, with the board slaved to the
host; Sync cycles counted against a quiet 30 s reference (30 cycles). The same on both firmwares, in
every run; where a class blocks, a run occasionally let one cycle through:

| Class | Effect |
|---|---|
| 1-byte datagram to port 319 | **0–1 of 30 cycles** |
| foreign two-step Sync cut to 33 B | **0–1** |
| foreign two-step Sync, `versionPTP` 1 | **0–1** |
| foreign two-step Sync padded to 1400 B | **0–1** |
| well-formed foreign two-step Sync, domain 0 | **0–1** |
| reserved messageType 0x5 | 30 — ignored |
| foreign Sync, domain 1 | 30 — ignored |
| foreign Announce, priority1 255 | 30 — ignored, host stays master |
| random 1–200 B to 319 and 320 | 30 — no effect |

In every case the board stayed with the host as master, never reset, kept its stack up, and
resynchronised once the traffic stopped. Two gaps in flexPTP's receive path, both in the core and
so in every port:

- **U12 — no length or version check** (fixed by `0012`). `ptp_process_packet()` parses a 34-byte header
  from the receive slot whatever was received; a short datagram is parsed partly from the
  previous message's bytes, and `versionPTP` is never compared.
- **U13 — the slave takes Sync and Follow_Up from anyone** (fixed by `0013`). `ptp_slave_process_message()`
  never compares the sender with the selected master, and a Sync arriving while it waits for a
  Follow_Up is ignored rather than restarting the cycle. So one foreign Sync between two of the
  master's cycles parks the slave on a sequence ID that never comes, and the master's own Sync is
  then discarded. A second, misconfigured master on the domain stops synchronisation outright
  while BMCA still reports the right master. Now and then the mixture completes a cycle instead,
  from the wrong messages: one such Sync cycle on ThreadX reported a 417 µs offset to the servo.

### U11–U13: fixed, and tested on the host and the board

All three are now patches (`0011`–`0013`) and branches off `fe4b071`, each with a test of flexPTP's
real engine on the host -- its own sources, patched as the firmware fetches them -- in
[`//patches/flexptp/test`](../patches/flexptp/test). Each fix's tests fail without it, and only its
own, whichever of the others is present.

**U13**, slave filters by parent port. The BMCA records the `sourcePortIdentity` from the header
of the Announce that won -- the parent port, not the grandmaster, so it is right behind a boundary
clock or a gPTP bridge too -- and the slave takes Sync, Follow_Up and Delay_Resp from that port
only. A new Sync from it while a Follow_Up is awaited restarts the cycle instead of being dropped.

**U12**, `ptp_receive_enqueue()` drops anything shorter than a common header, not PTPv2 (low
nibble: ptp4l 4.x sends 2.1), shorter than its own `messageLength`, or shorter than its type's
fixed part. Longer is padding, and fine.

| Host test (`rx_validation_test`, OS-less engine vs a simulated master) | without | with |
|---|---|---|
| 1-byte datagram, before and between each Sync/Follow_Up | 20 Syncs accepted for the master's 10; 10 clock steps | 10; 0 |
| Sync cut to 33 bytes | 0/10 cycles | 10/10 |
| Follow_Up cut to its 34-byte header | 10 clock steps | 0 |
| `versionPTP` 1 | 0/10 | 10/10 |
| `messageLength` 34 on a Sync | 0/10 | 10/10 |
| foreign two-step Sync | 0/10 | 10/10 |
| foreign Follow_Up, master's sequenceId, a second off | 11 clock steps | 0 |
| Sync from the master's port 2 | 0/10 | 10/10 |
| Sync(n), Sync(n+1), Follow_Up(n+1) | 0/10 | 10/10 |
| foreign Delay_Resp before the master's | 1 clock step | 0 |
| controls: settled slave; 2.0/2.1 version bytes and padded frames; a better master appears | pass | pass |

**U11**, `unreg_task_ptp()` in a safe order: stop accepting work (`sPTP_operating` first, and
`ptp_transmit_timestamp_cb()` now checks it), remove the heartbeat, ask the task to stop where it
holds nothing (`PTP_CEV_TERMINATE`, answered at the end of its loop iteration, after which it
parks) and only then delete it, then close the network stack driver, then delete the queues and
NULL their handles. The answer comes on a one-shot semaphore; after `PTP_TASK_STOP_TIMEOUT_MS`
(2 s) the task is deleted where it is, as before. Self-exit was not an option: in ST's ThreadX
CMSIS wrapper only `osThreadTerminate()` frees a thread's stack.

| Host test (`teardown_test`: FreeRTOS binding, POSIX port, heap that makes freed memory inaccessible) | without | with |
|---|---|---|
| TX timestamp reported after the stop | SIGSEGV in `xQueueGenericSend()` on the deleted `sTxCbFIFO` | dropped |
| stop while the PTP task is inside `MSG()`, the console draining 300 ms later | console mutex still held by the deleted task | released; the stop waits for it |
| three stop/start cycles (control) | no leak | no leak |

The board results are in the next section. Filed as #37 (U13), #38 (U12) and #39 (U11).

One latent oddity found on the way, not filed: in `FLEXPTP_OSLESS` mode the event FIFO is created
with an element size of `sizeof(uint32_t)` while its pool and its items are `PtpCoreEvent`s (8
bytes), so only `code` and `w` survive a push. Nothing reads `dw` today.

The `ptp period` help text advertises `matched`, but the parser compares against `match`;
`matched` falls through to `atoi()` and silently sets the period to 0. Noted in U8's message, not
changed.

### U11–U13 on the board

The same images before and after: the final-campaign images, then the same tree plus `0011`–`0013`
(build IDs `2808cbc5` FreeRTOS, `0850473d` ThreadX; rebuilding after the commit messages were
finalised gives the same IDs). The scenarios are `tools/hil/scenario.py`'s, now permanent.

| | without (final images) | with `0011`–`0013` |
|---|---|---|
| `fuzz`, both firmwares | 1-byte, cut-to-33, `versionPTP` 1, padded-foreign and foreign Sync: **0/30** cycles each | all nine classes **30/30**, host stays master, resync < 1 µs |
| `stopstart_busy` (ten stops while the PTP task logs everything) | pass, slowest stop 0.20 s | pass, slowest stop 0.20 s -- the wait for the task costs nothing measurable |
| S8, FreeRTOS | -- | 13 passed, 1 not applicable (`threads`) |
| S8, ThreadX | -- | 14/14 |
| all 20 scenarios, FreeRTOS | -- | **111 checks passed, 0 failed** (58 min) |
| all 18 scenarios, ThreadX | -- | 98 passed, **5 failed**: `gptp_flood` (3) and `master_flood` (2) |

The two ThreadX failures are not U11–U13's. They fail the same way on the final images without
them: a second round of the baseline campaign showed the same pair, five of five attempts
reproduce it with them, and every attempt without. After a saturation flood in gPTP, the board
stays its own master and the host never serves it again; after one with the board as an E2E
master, ptp4l sees its Announces but never reports an offset. The next section has the cause.
