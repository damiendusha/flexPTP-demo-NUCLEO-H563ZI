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
