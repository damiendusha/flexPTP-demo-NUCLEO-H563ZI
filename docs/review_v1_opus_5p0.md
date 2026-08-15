# Comprehensive review — ThreadX / NetX Duo port

**Reviewer:** Claude Opus 5.0 · **Date:** 2026-08-15 · **Tree:** `threadx-port` @ `581c25a`
· **Document:** `docs/review_v1_opus_5p0.md`

Scope, as requested: the quality of the ThreadX/NetX Duo port, architectural considerations,
testing opportunities (especially unit tests), opportunities for clean C++ to assist testing,
documentation consistency with the implementation, documentation self-consistency, and general
improvements.

Every finding carries an identifier. The [Summary table](#summary-table) at the end lists all of
them with priority.

---

## Table of Contents

- [Method, and what was measured rather than read](#method-and-what-was-measured-rather-than-read)
- [Verdict](#verdict)
- [1 — Port quality: ThreadX and NetX Duo](#1--port-quality-threadx-and-netx-duo)
- [2 — Architecture](#2--architecture)
- [3 — Testing opportunities](#3--testing-opportunities)
- [4 — C++ extraction opportunities](#4--c-extraction-opportunities)
- [5 — Documentation vs implementation](#5--documentation-vs-implementation)
- [6 — Documentation self-consistency](#6--documentation-self-consistency)
- [7 — General improvements](#7--general-improvements)
- [Summary table](#summary-table)

---

## Method, and what was measured rather than read

This project draws a hard line between what was reasoned and what was measured, and this review
keeps to it.

**Executed:**

| Action | Result |
|---|---|
| `cmake --build build/test && ctest` | 55/55 pass, 0.16 s wall |
| `ctest -N` + `--gtest_list_tests` | 55 ctest entries, 59 GoogleTest cases — matches the README exactly |
| `python3 tools/ci/check_docs_links.py` | 32 files, 0 broken links |
| `arm-none-eabi-size -A build/flexptp-demo.elf` | FLASH 206 344 B · SRAM1 80 864 B · SRAM3 89 704 B — all three match `architecture.md` to the byte |
| `arm-none-eabi-nm` on the linked image | confirms `atof`/`strtod`/`_Balloc`/`_malloc_r`/`_sbrk` are all linked (see [NX-03](#nx-03--the-newlib-heap-and-the-threadx-byte-pools-are-not-disjoint)) |

**Read against the vendored sources rather than from memory:** NetX Duo's
`nxd_udp_socket_send.c`, `nx_ip_packet_send.c`, `nx_link.c`, `nx_udp_socket_create.c` and
`nxe_udp_socket_create.c`; ThreadX's `cmsis_os2.c`; ST's `stm32h5xx_hal_eth.c`.

**Not done:** nothing was run on hardware. Every finding below that concerns runtime behaviour
is either derived from vendored source or explicitly marked as needing bench confirmation.

Two things I set out to report and then withdrew after checking, because the code was right and
my first reading was not — recorded because a review that only lists hits is not calibrated:

- `tx_concat_buf` in `eth_drv_netxduo.c:460` is a shared static, and the comment claims NetX Duo
  serialises the send path. **It does.** `_nxd_udp_socket_send` takes `nx_ip_protection` at
  `nxd_udp_socket_send.c:578` and holds it across `_nx_ip_packet_send`;
  `_nx_link_ethernet_packet_send` does the same at `nx_link.c:275`. The claim holds.
- `ptp_l2_receive`'s unchecked length subtraction looked like a remote buffer overflow. It is
  not: `ptp_receive_enqueue` clamps with `MIN(len, MAX_PTP_MSG_SIZE)` at
  `task_ptp.c:486`. Downgraded to [NX-05](#nx-05--unchecked-length-arithmetic-on-the-8023-receive-path), Low.

---

## Verdict

**This is the best-documented embedded port I have reviewed, and the engineering discipline
behind it is unusual.** The specific things that stand out:

- **Failure modes are ranked by silence, not by severity.** The recurring theme — F59's NaN, F53's
  dead timestamp counter, F80's stranded BMCA — is "wrong while every indicator stays green", and
  the code is built to make that class loud. `ethinfo`'s counters, the `SKIP`-is-not-a-`PASS` rule
  in the HIL runner, and `build.sh` refusing to build unpatched submodules are all the same idea.
- **Mutation testing as the entry fee for a characterisation test.** Ten mutations are recorded
  with their outcomes, and one test was found *vacuous* by the exercise and rewritten. Very few
  projects do this at all; fewer publish the one that failed.
- **The comments carry the reasoning, not the code's restatement.** `NX_PHYSICAL_HEADER` being 16,
  `nx_interface_address_mapping_needed`, the checksum-offload double-computation — each is a
  paragraph explaining a measurement, not a paraphrase of the line below it.

The findings below are, with three exceptions, refinements rather than corrections. The three
that are more than that — [NX-01](#nx-01--the-receive-timestamp-is-read-from-a-context-descriptor-that-may-not-have-been-written),
[NX-02](#nx-02--partial-failure-in-ptp_nsd_init-leaks-created-sockets) and
[NX-03](#nx-03--the-newlib-heap-and-the-threadx-byte-pools-are-not-disjoint) — are all latent
rather than currently-firing, and all three are in exactly the "silently wrong" class the rest of
the project is organised against.

---

## 1 — Port quality: ThreadX and NetX Duo

### NX-01 — the receive timestamp is read from a context descriptor that may not have been written

**Priority: High** · `Drivers/EthDrv/mac_drv.c:471-483`

```c
bool tsFound = bd->desc.DES1 & ETH_DMARXNDESCWBF_TSA;
ETHHW_DescFull *ctx_bd = NULL;
if (tsFound) {
    ctx_bd = bd_next;
    evt.data.rx.ts_s  = ctx_bd->desc.DES1;
    evt.data.rx.ts_ns = ctx_bd->desc.DES0;
    bd_next = ETHHW_DESC_NEXT(ring, ringLen, bd_next);
}
```

`ctx_bd` is assumed to be a written-back context descriptor purely because `TSA` was set on the
data descriptor. Neither of the two things that make that true is checked:

1. **that the DMA has released it** — `!(ctx_bd->desc.DES3 & OWN)`, and
2. **that it is a context descriptor at all** — `ctx_bd->desc.DES3 & ETH_DMARXNDESCWBF_CTXT`.

ST's own HAL checks both, in that order, and **breaks out of the drain loop to retry later** when
the context descriptor is not yet ready — `stm32h5xx_hal_eth.c:1130-1148`:

```c
if (READ_BIT(dmarxdesc_next->DESC3, ETH_DMARXNDESCWBF_OWN) == (uint32_t)RESET) {
    if (READ_BIT(dmarxdesc_next->DESC3, ETH_DMARXNDESCWBF_CTXT) != (uint32_t)RESET) { ... }
} else {
    /* timestamp context descriptor is not ready, exit and retry later */
    break;
}
```

Two consequences, and the second is worse than the first:

- **A wrong `t2`.** Whatever is in that descriptor's DES0/DES1 becomes the receive timestamp — on
  a freshly-armed descriptor that is the buffer address (`DES0 = bufAddr`), which is a large,
  plausible, non-zero nanosecond value. The servo is fed a real-looking lie. This is the F59
  shape, and it is the shape the project's own `RxRing.ZeroesTheTimestampWhenTheFrameHasNone`
  test exists to prevent on the neighbouring path.
- **A phantom frame.** The code then marks that descriptor `inFlight` and later restores it. If
  the DMA was mid-write, the sequence races; and if the descriptor is subsequently written as a
  genuine context descriptor, the main loop has no `CTXT` test either, so it is delivered upward
  as a frame whose length is `DES3 & 0x3FFF` — garbage bytes handed to NetX Duo.

**Why the host suite cannot see it:** `test/eth_model.cpp:100-119` always writes the context
descriptor completely and immediately. The model is right about the happy path and silent about
this one — the exact hazard `test/README.md` names for F71 ("a hardware model that is wrong is a
test that passes for the wrong reason").

**Recommended fix:** mirror the HAL. Check `OWN` first and `break` if still owned; check `CTXT`
and treat its absence as "no timestamp" (zeroed, as the untimestamped path already does). Add a
`CTXT` guard to the main loop so a stray context descriptor is skipped rather than delivered.
Pair it with [TS-03](#ts-03--the-dma-model-cannot-express-a-late-context-descriptor).

> **✅ ADDRESSED** — `mac_drv.c` (commit following this review).
>
> The pairing is now three tests rather than an assumption, in the order ST's HAL uses:
>
> | Context slot | Action |
> |---|---|
> | still DMA-owned | `break` — defer the frame, retried on the next receive interrupt; counted |
> | ours, `CTXT` set | pair them (the ordinary case) |
> | ours, `CTXT` clear | deliver **unstamped**; counted |
>
> A stray context descriptor reaching the main loop is skipped **and restored**, so the ring does
> not lose a descriptor (the F71 lesson). Deferring is safe because the descriptor stays
> application-owned and unmarked — the backward scan picks it up next interrupt — and the DMA
> cannot advance past the context slot without writing it, so nothing newer can overtake it.
>
> Two counters (`ETHHW_GetRxCtxCounters`) are reported by `ethinfo` as
> `rx ts: N deferred … N stray`.
>
> **Evidence.** The bug was reproduced before it was fixed: the driver delivered a frame stamped
> `ts_ns = 1 080 035 208` — the buffer address, read out of an armed descriptor. Four new tests
> in `test_rx_ring` (13 cases, up from 9), all three branches mutation-verified; the `CTXT`
> mutation **survived the first round** and `DoesNotReadTheNextFrameAsATimestamp` was written to
> catch it. Host suite 59/59. Hardware `S8` **10/10** with `|offset| ≤ 10 166 ns` (baseline
> 11 010 ns), and `ethinfo` after a full run: `rx: 603 delivered … rx ts: 0 deferred, 0 stray` —
> the race does not fire in normal operation, which is expected: exception-entry latency alone
> closes the window.

### NX-02 — partial failure in `ptp_nsd_init()` leaks created sockets

**Priority: High** · `Src/flexptp_nsd/nsd_netxduo.c:265-293`

Three early returns leave the NSD in a state the next call cannot recover from:

```c
if ((nx_udp_socket_create(...event...) != NX_SUCCESS) ||
    (nx_udp_socket_create(...general...) != NX_SUCCESS)) {
    MSG("PTP: could not create the UDP sockets\n");
    return;                        /* (a) event socket may already be created */
}
if ((nx_udp_socket_bind(...) != NX_SUCCESS) || (...)) {
    MSG("PTP: could not bind the UDP sockets\n");
    return;                        /* (b) BOTH sockets created, never deleted */
}
```

`s_sockets_open` is only set to `true` on the success path, so `close_sockets()` returns early
next time and **never deletes them**. The next `ptp_nsd_init(PTP_TP_IPv4, …)` — reachable from
`ptp profile preset …`, `ptp transport …`, `ptp reset`, or flexPTP's own re-init — calls
`nx_udp_socket_create()` on a socket that is still on the IP instance's created list. What
happens then depends on the build configuration:

| Build | `NX_DISABLE_ERROR_CHECKING` | Behaviour |
|---|---|---|
| **Debug** | not defined | `_nxe_udp_socket_create` walks the created list (`nxe_udp_socket_create.c:113`) and returns `NX_PTR_ERROR`. PTP is then **permanently dead** — every subsequent init fails, long after the transient cause has gone |
| **Release** | defined (`Inc/nx_user.h:102`) | the check is gone. `_nx_udp_socket_create` unconditionally re-links the socket into `nx_ip_udp_created_sockets_ptr` (`nx_udp_socket_create.c:139-170`) and increments the count. **The doubly-linked list is corrupted**: one node appears twice with self-referential `previous`/`next` |

A related, milder problem on the same paths: `s_tp` and `s_dm` keep their *previous* values on
every failure return, so the NSD believes a transport is configured whose sockets no longer
exist. `ptp_nsd_transmit_msg()` will then take the IPv4 branch and send on a deleted socket.

**Recommended fix:** make the function transactional. Set `s_sockets_open = true` immediately
after the first successful create, so `close_sockets()` always has something to undo; delete
what was created on every failure path; and set `s_tp = s_dm = -1` before every early return so
the transmit path cannot act on a half-configured NSD. This is also the single best argument for
[CX-04](#cx-04--nsdlifecycle-over-an-injected-stack-interface).

> **✅ ADDRESSED** — with [CX-04](#cx-04--nsdlifecycle-over-an-injected-stack-interface) and
> [TS-07](#ts-07--no-test-of-the-nsd-lifecycle), because the fix is only worth anything if it is
> verified and it was not verifiable in place.
>
> The state machine moved to `Src/flexptp_nsd/ptp_nsd_lifecycle.{h,cpp}` and
> `ptp_nsd_init()` became a mapping onto it. `nsd_netxduo.c` supplies the seven stack calls;
> the decisions are the unit's.
>
> - **Per-object flags.** Each socket is marked created the instant it exists, not once the
>   pair is complete, so cleanup is exact rather than approximate.
> - **Transactional.** Any failure destroys everything that call built.
> - **`s_tp`/`s_dm` are gone.** The transmit and receive paths read the lifecycle's own
>   transport, which stays `PTP_EP_TRANSPORT_NONE` until the build has succeeded — so a failed
>   configure cannot leave the transmit path pointing at deleted sockets. That also fixed an
>   ordering trap the refactor exposed: the multicast join needs the *new* delay mechanism and
>   the *new* transport, and a copy updated after `configure()` returns is one step behind.
> - **`nx_udp_socket_bind()` is now bounded** (1 s) rather than `NX_WAIT_FOREVER`. It runs on
>   flexPTP's own thread, so an occupied port used to park the PTP stack for good — the shape
>   F54 swept up — and it also made the failure path this finding is about unreachable in
>   practice. Bounded, it is reportable *and* recoverable.
>
> **Evidence.** 14 new tests against a fake stack that counts creates and deletes and can be
> told to fail any single call. Two mutations, both caught: marking the sockets created only
> once the whole sequence succeeded (3 tests fail, reporting a socket the stack still holds),
> and leaving the transport set across a teardown (4 tests fail). Host suite 73/73.
> Hardware `S8` **10/10**, `|offset| ≤ 10 971 ns`. Then the path S8 does *not* cover, driven
> over the CLI: **15 profile loads** cycling `gPTP` → `defp2p` → `default` plus a `ptp reset`
> — 18 NSD re-initialisations — after which the board returned to `SLAVE` with a master
> selected and `ptp ts: posted 95, delivered 95, dropped 0`.

### NX-03 — the newlib heap and the ThreadX byte pools are not disjoint

**Priority: High** · `Src/sysmem.c:56-72`, `stm32h563zitx_FLASH.ld:200-211`

The linker script states the invariant explicitly:

> *"It must come AFTER `._user_heap_stack` … `_sbrk()` hands out everything between `_end` and
> `_estack - _Min_Stack_Size`, so a byte pool placed inside that window would be handed to
> `malloc()` as well. **Sitting above the reservation keeps the two allocators disjoint.**"*

The conclusion does not follow from the premise. `_sbrk()` starts at `_end` and its **only**
ceiling is `_estack - _Min_Stack_Size`. `__RAM_segment_used_end__` sits `_Min_Heap_Size +
_Min_Stack_Size` = **2 560 bytes** above `_end` — far below that ceiling. Measured from the
linked image:

| Symbol | Address |
|---|---|
| `_end` (start of `._user_heap_stack`) | `0x20025FE0` |
| `__RAM_segment_used_end__` ≈ byte-pool base | `0x200269E0` |
| `_sbrk` ceiling (`_estack - _Min_Stack_Size`) | `0x2003F800` |

So the first 2 560 bytes of cumulative `malloc()` are safe, and **everything after that is inside
the 32 KB of ThreadX byte pools** — the thread stacks and every kernel object. Reordering the
sections moved the overlap from "immediate" (the FreeRTOS `ucHeap` case the comment describes) to
"after 2 560 bytes"; it did not remove it.

**Is `malloc` reachable?** Yes. `arm-none-eabi-nm` on the linked image shows `atof`, `strtod`,
`_Balloc`, `_malloc_r` and `_sbrk` all present, and `atof()` is called from `CB_pps` in
`Src/flexptp_port/ptp_port_stm32h563.c:84` — the `ptp pps <freq>` CLI command — as well as from
flexPTP's own servo-parameter commands. newlib's `strtod` reaches `_Balloc` on precision-heavy
inputs, which is exactly what `ptp servo st 1e-18` is. Allocation is small and mostly recycled
through the reent freelist, so this is unlikely to fire in practice — but "unlikely, silent, and
corrupts thread stacks" is the profile of every finding in `port_review.md` that cost days.

**Recommended fix (one line):** bound `_sbrk` against the real boundary.

```c
extern uint8_t __RAM_segment_used_end__;
const uint8_t *max_heap = (const uint8_t *)&__RAM_segment_used_end__;
```

That makes the linker script's claim true rather than aspirational, and turns a silent overlap
into `ENOMEM`. It also means `_Min_Heap_Size` finally does something, closing the loose end the
script itself flags as finding H-5.

> **✅ ADDRESSED** — `Src/sysmem.c`, `Src/sysmem_bounds.h`, `stm32h563zitx_FLASH.ld`,
> `Src/cmds.c`.
>
> **The overlap was not latent — it was in use.** Applying the bound alone, before sizing the
> heap, wedged the console thread after seven CLI commands while the rest of the firmware ran
> on. That is the proof the old code was allocating past 2 560 bytes in normal operation: those
> bytes were coming out of the ThreadX byte pools. Confirmed by A/B — the pre-fix firmware
> survives 42 commands, the bounded-but-unsized one dies after 7.
>
> Three changes, in the order the evidence demanded:
>
> 1. **The bound** is `__RAM_segment_used_end__`, extracted into `sysmem_bounds_next()` in a
>    header so it can be tested without a target. Corrected figures: `_end` = `0x200131F0`,
>    the limit = `0x20013BF0`, the old ceiling = `0x2003F800` — an overlap of **179 216 bytes**,
>    not the ~103 KB estimated in the finding above.
> 2. **`_Min_Heap_Size` 0x200 → 0x2000**, sized from a measurement rather than a guess. With a
>    deliberately oversized heap and `osinfo` reporting the high-water mark: 0 bytes at boot,
>    0 after seven servo/PPS commands with flexPTP stopped, **3 600 bytes** once flexPTP is
>    running, and still 3 600 after 130 commands. The window is now 10 240 B — 2.8× the peak.
>    *(My guess in the finding above was that `atof()` was the allocator. It is not: those
>    commands allocate nothing with flexPTP stopped. Recorded because it was a plausible
>    wrong answer.)*
> 3. **Exhaustion reports and halts** instead of returning `ENOMEM`. newlib's `_Balloc()` does
>    not check its allocation, so the observed failure is a console that stops answering while
>    everything else runs — the exact "board goes quiet" mode F51/F53/F68 are about. A halt
>    that names the constant to raise is worth more than a silent wedge.
>
> **Evidence.** 11 host tests (`SysmemBounds`), two mutations: widening the limit fails 3, and
> writing the growth check the natural `heap_end + incr > limit` way fails 1 — **that second
> test had to be added, because the first version of the mutation survived.** On this target
> `ptrdiff_t` is 32-bit and the heap sits low, so the two formulations agree everywhere the
> board can reach; the discriminating case places the window near the top of the address space.
> The comment now says which of the two facts it is claiming. Host suite 84/84. Hardware `S8`
> **10/10**, `|offset| ≤ 11 358 ns`, the wedge repro passes 42 commands, and `osinfo` reports
> `C heap: 3600 of 10240 bytes used (peak)`.

### NX-04 — SysTick and SVCall run at priorities the documentation does not state

**Priority: Medium** · `Startup/tx_initialize_low_level.S:119-128`

```asm
LDR     r1, =0x00000000     // Rsrv, UsgF, BusF, MemM     -> SHPR1
LDR     r1, =0xFF000000     // SVCl, Rsrv, Rsrv, Rsrv     -> SHPR2
LDR     r1, =0x40FF0000     // SysT, PnSV, Rsrv, DbgM     -> SHPR3
```

Decoding SHPR3: PendSV = `0xFF` → 15 ✔, **SysTick = `0x40` → priority 4**. SHPR2: **SVCall =
`0xFF` → 15**. SHPR1: MemManage / BusFault / UsageFault = 0.

This runs inside `tx_kernel_enter()`, i.e. *after* `HAL_Init()` → `HAL_InitTick(TICK_INT_PRIORITY)`
and after `HAL_RCC_ClockConfig()`'s re-init, so it is the last writer and 4 is the value that
sticks. `TICK_INT_PRIORITY` is 15 (`Inc/stm32h5xx_hal_conf.h:185`) and is overridden.

Three consequences:

- `docs/architecture.md:352,354` says SVC = 0 and SysTick = 15 (`TICK_INT_PRIORITY`). Both are
  wrong. See [DC-01](#dc-01--the-interrupt-priority-table-is-wrong-in-two-rows).
- The file's own comment two lines above says *"SVCall and PendSV must both sit at the lowest
  priority (0xFF) … This mirrors what the FreeRTOS port did with PendSV/SysTick at 15"* — while
  the instruction below it puts SysTick at 4.
- It is a **behavioural change from the FreeRTOS build** that nothing records: SysTick (4) now
  preempts ETH (7), where previously it could not. Nothing here is unsafe — ThreadX's critical
  sections are PRIMASK-based, and the SysTick handler is short — but the ETH ISR's worst-case
  latency changed and the interrupt table is the document someone will consult when adding a
  fourth interrupt.

**Recommended fix:** decide deliberately (keeping 4 is defensible; so is 15), then make the
constant, the comment and the table agree. If 4 is kept, note in `architecture.md` why the tick
sits above Ethernet.

> **✅ ADDRESSED** — `Startup/tx_initialize_low_level.S`, `Src/cmds.c`, `tools/hil/checks.py`,
> `docs/architecture.md`. Together with [DC-01](#dc-01--the-interrupt-priority-table-is-wrong-in-two-rows).
>
> **Kept at 4, deliberately.** The tick drives the timer thread, which is where flexPTP's
> heartbeat runs — and F80 is the finding about that heartbeat not arriving. Keeping it ahead
> of a long receive burst bounds that latency. Nothing depended on the old ordering: ThreadX
> masks with PRIMASK rather than BASEPRI so there is no ceiling for kernel calls, and PendSV at
> 15 cannot preempt the ETH ISR whatever the tick does.
>
> The three SHPR words are now named constants with the reasoning per exception, and the
> file's self-contradicting comment ("mirrors … PendSV/SysTick at 15", above an instruction
> setting SysTick to 4) is gone.
>
> **The real fix is that it is now checked, not documented.** `osinfo` reads all nine
> priorities back out of the registers via `NVIC_GetPriority()`, and a new `irq_prio` HIL check
> asserts them against the table. A priority set in three places by three components — ST's
> HAL, this project's assembly, the drivers — with the last writer silently winning is exactly
> what drifts.
>
> **Evidence.** The board reports `MemManage 0, BusFault 0, UsageFault 0, SVCall 15, PendSV 15,
> SysTick 4, ETH 7, USART3 12, GPDMA1_Ch0 12` — confirming the assembly reading and the finding.
> Hardware `S8` **11/11** with the new check. The check was itself mutation-tested: asserting
> the documented-but-wrong `SysTick: 15` makes it fail with `SysTick is 4, expected 15`.

### NX-05 — unchecked length arithmetic on the 802.3 receive path

**Priority: Low** · `Src/flexptp_nsd/nsd_netxduo.c:197-206`

```c
UCHAR *ptp  = packet_ptr->nx_packet_prepend_ptr + header_size;
ULONG length = packet_ptr->nx_packet_length - header_size;
```

`_nx_link_ethernet_packet_received` does **not** validate `nx_packet_length >= header_size`
before dispatching to a registered callback (verified in `nx_link.c:1036-1230`), so on a runt the
subtraction underflows to ~4 G. The blast radius is contained — `ptp_receive_enqueue` clamps with
`MIN(len, MAX_PTP_MSG_SIZE)` (128) and the source stays inside a 1 528-byte buffer — so the
observable effect is a garbage 128-byte "message" that flexPTP rejects on version/type. The MAC's
store-and-forward configuration also drops error frames, which makes a sub-14-byte delivery
unlikely.

Still worth a guard, for two reasons: the UDP path in the same file *does* check its
preconditions and says why, and the clamp that saves this is inside an unpinned submodule.

Two smaller asymmetries on the same function, worth fixing together:

- a chained packet is silently discarded here, while the UDP path prints
  `"PTP: chained receive packet (%u bytes) discarded"` (`nsd_netxduo.c:153`);
- `s_rx_notify`/`s_rx_packets` are incremented before the chain test, so `ethinfo`'s
  `packets` and `enqueued` diverge with no counter explaining the gap.

> **✅ ADDRESSED** — the length is checked before the subtraction (NetX Duo's
> `_nx_link_ethernet_packet_received()` genuinely does not check it), a chained 802.3 packet is
> now reported like the UDP path's, and both refusals are counted and printed by `ethinfo` as
> `nsd rejected: N runt, N chained`. Both read zero on hardware.

### NX-06 — the receive-queue drop path is the only uninstrumented one

**Priority: Medium** · `Drivers/EthDrv/eth_drv_netxduo.c:687-696`

```c
if (osMessageQueuePut(rx_msg_q, evt, 0, 0) != osOK) {
    return ETHHW_RET_RX_PROCESSED;
}
```

`docs/architecture.md:335-339` already identifies this as the drop path that actually bites under
load — the packet pool measurably does not starve, and cutting it from 8 to 2 changed nothing —
and states plainly that it drops "silently, and without a counter". Documenting a gap is better
than not, but this one is a two-line fix (`static ULONG drv_rx_dropped_queue;` plus a line in
`eth_drv_report()`) in a driver whose entire diagnostic philosophy is that counters are how the
port's defects were found. Leaving it is inconsistent with the rest of the file.

> **✅ ADDRESSED** — `eth_drv_netxduo.c`; `ethinfo` now reads
> `rx: N delivered, N dropped (no packet), N dropped (size), N dropped (queue full)`.
>
> **And it still does not fire.** 70 600 UDP frames of 1 400 B in 8 s, 69 595 delivered by the
> driver, 0 queue drops — `rx_thread` keeps up with the DMA at line rate. (The ~1 000 missing
> were lost host-side; NetX Duo's own counters agree: `udp: rx 69 240, dropped 68 943` for a
> closed port.)
>
> So the honest position has changed from "one silent drop path" to "three instrumented drop
> paths, none of which has ever incremented" — which is better, but is not the same as proven,
> and `testing_architecture.md`'s known-gaps section now says so.

### NX-07 — RX packet length is masked to 14 bits, but the field is 15

**Priority: Low** · `Drivers/EthDrv/mac_drv.c:466`

```c
evt.data.rx.size = (bd->desc.DES3) & 0x3FFF;
```

`ETH_DMARXNDESCWBF_PL` is `0x00007FFF` (`stm32h5xx_hal_eth.h:990`). Unreachable today — the MTU is
1 500 and the buffer 1 528 — but it is a latent trap of exactly the kind this codebase catalogues,
and it is easy to mistake for the *transmit* case immediately below, where `& 0x3FFF` on TDES2's
`B1L` **is** correct. Use the named mask on both sides so the difference is visible.

> **✅ ADDRESSED** — the receive side uses `ETH_DMARXNDESCWBF_PL`. The transmit side keeps
> `0x3FFF` because TDES2's `B1L` really is 14 bits; the comment now says so, which is the part
> that makes the difference visible rather than invisible.

### NX-08 — receive queue leaked if the receive thread fails to start

**Priority: Low** · `Drivers/EthDrv/eth_drv_netxduo.c:356-373`

`rx_msg_q = osMessageQueueNew(...)` runs before `osThreadNew()`, and `rx_started` is only set when
both succeed. A repeat `NX_LINK_INITIALIZE` therefore allocates a second queue from the byte pool
and orphans the first — the leak the surrounding `static bool rx_started` guard exists to prevent.
Guard the queue creation on `rx_msg_q == NULL` as well.

> **✅ ADDRESSED** — the queue creation is guarded on `rx_msg_q == NULL` as well as on
> `rx_started`.

### NX-09 — `ptp pps 0` reports "off" but may leave the output running at 1 Hz

**Priority: Low** · `Src/flexptp_port/ptp_port_stm32h563.c:35-40,88-92`, `Drivers/EthDrv/mac_drv.c:1005-1011`

`freq <= 0` sets `fc_exp = 0` and writes `PPSCTRL = 0b0000`. In the DWC EQOS `MACPPSCR`, the
all-zero `PPSCTRL` encoding is *"PPS output 1 Hz"*, not *"disabled"* — disabling the output needs
`PPSEN0`/pulse-train control, not code 0. Meanwhile the CLI prints *"PPS output is turned off."*

I have not confirmed this against RM0481 (no copy available here) and the enum's non-zero codes
are clearly correct, so this is flagged for **verification on the bench with a scope on PG8**
rather than asserted. If confirmed it is a one-line fix and a one-line message change.

> **✅ ADDRESSED — and it was real.** Verified by reading `MACPPSCR` over SWD rather than with a
> scope, which is weaker evidence but sufficient here because the register tells the whole story:
>
> | Command | CLI said | `MACPPSCR` |
> |---|---|---|
> | `ptp pps 1` | `PPS frequency: 1 Hz` | `0x61` — fixed-frequency mode, `PPSCTRL` = 1 |
> | `ptp pps 0` *(before)* | `PPS output is turned off.` | `0x60` — **the same mode, divider 0**, which is the 1 Hz code |
>
> There is no "off" encoding in `PPSCTRL`: `0b0000` is 1 Hz. The pin was still toggling.
>
> The fix is not the one-liner I guessed. Stopping the output means *leaving* fixed-frequency
> mode — setting `PPSEN0` and issuing the pulse-train STOP command — which
> `ETHHW_StopPTPPPSPulseTrain()` already did for the other PPS mode, so
> `ptphw_set_pps_freq(0)` now calls it. Reversible, because writing a frequency clears `PPSEN0`
> again. After:
>
> | Command | `MACPPSCR` |
> |---|---|
> | `ptp pps 1` | `0x61` — 1 Hz |
> | `ptp pps 0` | `0x70` — **command mode, STOP consumed**: the output is stopped |
> | `ptp pps 8` | `0x64` — back in fixed-frequency mode, `PPSCTRL` = 4 → 2³ = 8 Hz |
>
> **Not scoped.** The evidence is the register transition, not a trace of PG8. A scope would
> settle it beyond doubt and is the right next step if anyone has one on the bench.

### NX-10 — `NX_LINK_DISABLE` does not stop the MAC

**Priority: Low** · `Drivers/EthDrv/eth_drv_netxduo.c:403-406`

`drv_disable()` clears `drv_enabled` and returns. The MAC's `RE`/`TE` and the DMA's `SR`/`ST` stay
set, so frames continue to land in the ring and `rx_thread` keeps handing them to a stack that
believes the interface is down. Harmless in this firmware — nothing ever disables the interface —
but it is the kind of asymmetry that becomes a bug the first time somebody adds a link-flap
recovery path. Either stop the receiver or say in the comment that disable is deliberately
transmit-only.

> **✅ ADDRESSED** — documented rather than changed, which is the option the finding offered and
> the right one here: stopping the engines means draining the rings and handling a restart, and
> NetX Duo only issues `NX_LINK_DISABLE` from `nx_ip_interface_detach()` and `nx_ip_delete()`,
> neither of which this application calls. The comment now says it is transmit-only, why, and
> that it is the function to grow if a link-flap recovery path is ever added.

---

## 2 — Architecture

### AR-01 — every frame is copied twice on transmit and once on receive

**Priority: Opportunity (Medium)** · `eth_drv_netxduo.c:460-568`, `mac_drv.c:762`

Transmit: NX_PACKET chain → `tx_concat_buf` → `bd->ext.bufAddr`. Receive: DMA buffer →
`nx_packet_prepend_ptr + 2`. `mac_drv.c` owns 48 fixed 1 528-byte buffers and the descriptors
always point at them, so NetX Duo's zero-copy design is entirely given up.

At PTP rates this is free, and at the measured 53 000 frames in 6 s it evidently still is. But it
costs 73 KB of SRAM3 that duplicates the packet pool, and the DWC EQOS can point descriptors
directly at `NX_PACKET` payloads. If throughput ever matters, this is the change to make — and it
is a substantial one, because it moves buffer ownership across the `mac_drv.c` boundary that this
port deliberately kept unchanged. Recording it as a known trade rather than a defect.

### AR-02 — the pending-timestamp registry could key on the message instead of the pointer

**Priority: Opportunity (Medium)** · `Src/flexptp_nsd/ptp_tx_registry.cpp`, `eth_drv_netxduo.c:413-453`

The `NX_PACKET *` key is a genuinely elegant answer to "NX_PACKET has no user field", and the ABA
hazard is handled correctly by three independent mechanisms with twelve tests behind them. But the
hazard exists only because the key is an address that NetX Duo recycles.

An alternative removes it structurally: key on the PTP header's `(messageType, sequenceId,
sourcePortIdentity)` — the tuple the protocol already guarantees unique for an outstanding
exchange — read out of the frame in `drv_send()` just before handing it to the MAC. A recycled
packet then cannot collide, because the key is the message, not the memory. The cost is parsing
14+30 bytes into the frame on the transmit path, and a dependency on frame layout that the current
design avoids.

Not a recommendation to change working, well-tested code. Recorded because the three-mechanism
invariant is the most fragile thing in the port — it is one forgotten `cancel()` on a future drop
path away from a wrong `t3` — and a future maintainer should know the alternative was considered.

> **✅ ADDRESSED — the hazard is bounded and counted, and the design is unchanged.**
>
> Rekeying on `(messageType, sequenceId, sourcePortIdentity)` was not done. It would trade a
> correct, twelve-test design for a frame-layout dependency the current one deliberately avoids,
> to fix a hazard that has never fired. My own advice on the CX items applies here: pulled by a
> need, not pushed.
>
> What *was* done is smaller and answers the actual worry. The finding's own words are that the
> invariant is "one forgotten `cancel()` on a future drop path away from a wrong `t3`" —
> i.e. the risk is that a violation is **invisible**. So the registry now ages entries: one that
> has watched `PTP_TX_REGISTRY_MAX_AGE` (16) later registrations go past cannot still be waiting
> for its own transmit — flexPTP has at most two messages outstanding — so it is evicted and
> counted in `stale`. `ethinfo` and `ethstat` report it, and the HIL `counters` check asserts it
> is zero.
>
> **This is not a correctness mechanism and the header says so**: the three rules still are. It
> converts "a future `return` strands an entry and the servo is silently misfed" into "a future
> `return` strands an entry and the next hardware run fails on a non-zero counter".
>
> **Evidence.** Two new tests, and the mutation had to fail in *both* directions to be worth
> having: removing the eviction fails 1, and evicting at `MAX_AGE` 1 fails 3 — so the bound is
> pinned as neither absent nor over-eager. Host suite 97/97; hardware `S8` **13/13** with
> `counters` now checking 12 values.

### AR-03 — diagnostics reach across layers through function-local `extern` declarations

**Priority: Low** · `Drivers/EthDrv/eth_drv_netxduo.c:729-759`

```c
extern void ptp_nsd_stats(uint32_t *, uint32_t *, uint32_t *, uint32_t *);
extern void ptp_tx_timestamp_stats(uint32_t *, uint32_t *, uint32_t *);
extern void ptp_nsd_rx_types(uint32_t *);
extern void ptp_nsd_last_rx(uint8_t *, uint32_t *);
```

The comment is honest about why ("a permanent dependency the other way would be wrong"), and the
instinct is right — the driver should not depend on the NSD. But four hand-written prototypes with
no header are four opportunities for a silent signature drift, which is precisely finding F48.

A small registration interface fixes it without inverting the dependency: `eth_drv_report()` walks
a table of `{const char *name, void (*print)(void)}` that each module registers into from its own
initialiser. The driver then depends on nothing above it, and every prototype is checked by its
own translation unit.

> **✅ ADDRESSED — more simply than the sketch above.** No registration table was needed: the
> dependency did not want inverting, it wanted moving to the layer that legitimately has both.
>
> `ethinfo` is an application command, and `Src/cmds.c` may depend on the driver *and* the NSD
> *and* flexPTP. So `eth_drv_report()` now reports only the driver and NetX Duo's own counters,
> a new `ptp_nsd_report()` (declared in `Src/flexptp_nsd/nsd_netxduo.h`) reports the NSD's, and
> `cmds.c` calls them in order. Every prototype is now checked against its definition by the
> compiler.
>
> A registration table would have bought late binding this firmware has no use for — there is
> one driver, one NSD and one command — at the cost of a function-pointer array and an
> initialisation order to get wrong.
>
> **Evidence.** Hardware `S8` **12/12**; `ethinfo` output verified unchanged in content and
> order across the move (driver counters → `nsd:` → `nsd rejected:` → rx types → `ptp ts:` →
> `flexPTP core:`).

### AR-04 — Debug and Release differ in more than optimisation level

**Priority: Medium** · `Inc/nx_user.h:91-103`, `Inc/tx_user.h:73-75`

Three configuration switches are conditional on `DEBUG`:

| Switch | Debug | Release |
|---|---|---|
| `NX_ENABLE_PACKET_DEBUG_INFO` | on | off |
| `NX_DISABLE_ERROR_CHECKING` | off | **on** |
| `TX_ENABLE_STACK_CHECKING` | on | **off** |

Each is individually well-reasoned in place. Collectively they mean the Release image runs
**different NetX Duo API semantics** — this is what turns [NX-02](#nx-02--partial-failure-in-ptp_nsd_init-leaks-created-sockets)
from a diagnosable error into list corruption — and has no software stack check, while
`main.c:312` still registers `app_stack_error_handler()` unconditionally as though it did.

Both configurations are hardware-tested, which is exactly right. What is missing is a single place
that says "these are the semantic differences between the two images" — `architecture.md`'s module
inventory mentions neither. See [DC-02](#dc-02--release-drops-two-checks-the-architecture-document-presents-as-unconditional).

### AR-05 — the `ETH_STACK` switch is advertised as easier than it is

**Priority: Low** · `README.md:263-271` vs `docs/design_doc.md:377-402`

The README's TIP says the project "can either use NetX Duo or EtherLib" and that this "can be
easily changed by setting the `ETH_STACK` CMake variable". `design_doc.md` is candid that
`etherlib_options.c`, `eth_drv_etherlib.c` and `ethernet_etherlib.c` have **never been compiled**
in this port, and lists six items of remaining work including a missing linker placement and an
unverified NSD interface.

The README does add "Only the NetX Duo path is exercised on this port", which softens it, but "can
easily be changed" and "never been compiled, fix what falls out" are different claims about the
same switch. Align the README with the design doc — and consider [TS-08](#ts-08--no-compile-only-gate-for-the-non-default-eth_stack-paths).

> **✅ ADDRESSED — and the switch was worse than either document said.** Neither alternative
> *configured*: `ETH_STACK=NONE` and `ETH_STACK=ETHERLIB` both died on
> `fatal error: nx_api.h: No such file or directory`, because `Src/main.c` and `Src/cmds.c`
> included NetX Duo headers unconditionally. The design doc blamed the EtherLib files, which
> were not the first blocker.
>
> Rather than downgrade the claim, the switch was made real: one `ETH_*` compile definition per
> stack, a header choice in `main.c`, and `ethinfo`'s driver dump behind `ETH_NETXDUO`.
> **Both alternatives now build**, and CI compiles them ([TS-08](#ts-08--no-compile-only-gate-for-the-non-default-eth_stack-paths)).
>
> The claim is now sized to the evidence in both documents: `NETXDUO` is run and verified;
> `ETHERLIB` and `NONE` are *compiled and nothing more*. EtherLib has still never been run, and
> `design_doc.md`'s remaining-work list keeps every item that is about running it.

### AR-06 — no watchdog, and every failure path halts forever

**Priority: Low (decision to record)**

`Error_Handler()`, `app_stack_error_handler()`, `fault_report()` and the two `serial_io_start()`
failure paths all end in `for (;;)`. There is no IWDG anywhere in the tree.

For a bench demo whose whole thesis is "make failure loud", halting with a decoded CFSR on the
console is arguably the *right* behaviour — a silent auto-reboot loop would be worse. But it is a
decision, and it is not written down anywhere. One paragraph in `architecture.md` (and, if a
watchdog is ever added, a `HAL_IWDG` refresh in the `serialio` thread plus a reset-cause report at
boot) would settle it.

> **✅ ADDRESSED** — recorded in `architecture.md` as a decision, with the argument (an
> auto-rebooting board destroys the console output that says what happened, and presents as an
> intermittent fault) and with what would change for a product: IWDG enabled in `main()`,
> refreshed from the `serialio` thread — the lowest-priority thread that must keep running for
> the console to work, so a refresh there proves more than a timer callback would — plus a
> reset-cause report from `RCC_RSR` at boot.

### AR-07 — equal-priority scheduling semantics changed with the kernel

**Priority: Low** · `Modules/cmsis_rtos_threadx/cmsis_os2.c:116,923`

Four threads sit at ThreadX 32 (`init`, `cli`, `serialio`, `ptp`). Under FreeRTOS with
`configUSE_TIME_SLICING` they round-robined every tick (1 ms); ST's wrapper passes
`RTOS2_DEFAULT_TIME_SLICE = 4`, so they now round-robin every **4 ms**.

I checked this expecting to find `TX_NO_TIME_SLICE` and a starvation risk; there is none, and no
thread in this firmware spins without blocking anyway. Recorded only because `architecture.md`'s
threads section documents the *priority* mapping between the two kernels in detail and says
nothing about the quantum, and because `RTOS2_DEFAULT_TIME_SLICE` is overridable from
`Middlewares/threadx.cmake` if it ever needs to be.

> **✅ ADDRESSED** — `architecture.md` gains a *Scheduling* subsection next to the thread table:
> the quantum, that it changed from 1 tick to 4 with the kernel swap, that nothing depends on it
> because all four equal-priority threads block, and where to override it.

---

## 3 — Testing opportunities

The existing suite is strong: 55 entries, 0.16 s, mutation-verified, honest about its reach. The
gaps below are ordered by value.

### TS-01 — `blocking_fifo.c` has no automated coverage at all

**Priority: High** · `Modules/blocking_io/blocking_fifo.c` (237 lines)

This file carried **finding F67 (N1, N7)** — the FIFO could not distinguish full from empty, and
`bfifo_pop()` released the writer token from the consumer thread mid-push — and the fix introduced
free-running `pushedTotal`/`poppedTotal` totals whose correctness rests on **modular arithmetic
across the 2³² wrap** (`bfifo_get_used()` is a bare subtraction). Nothing tests any of it.

It is also nearly free to test: the only RTOS dependencies are two semaphores and a
`__disable_irq()` critical section, and `test/` already links a **real ThreadX kernel and the real
CMSIS-RTOS2 wrapper** for `test_rtos_context`. Dropping `blocking_fifo.c` into that target costs
one `add_executable`.

Cases worth pinning, each mapping to something that was once wrong: full vs empty at equal
indices; `get_used()` across the `pushedTotal` wrap; a push that wraps the ring buffer; a
blocking push that unblocks on a pop; `bfifo_push_all` with a payload larger than the FIFO; and
the interleaving where a second writer enters while the first is mid-push.

> **✅ ADDRESSED** — `test/test_blocking_fifo.cpp`, 13 cases, linked against the real ThreadX
> kernel and ST's real CMSIS-RTOS2 wrapper. Every case above except the last: the second-writer
> interleaving needs the ISR producer, and `__get_IPSR()` is 0 on the host, so that half stays
> on the bench and the file says so.
>
> Two shortcuts worth naming. The 2³² wrap test sets `pushedTotal`/`poppedTotal` directly —
> reaching it honestly would take 4 GB through a console FIFO — which is white-box, and the
> alternative was not testing the property at all. And `TheCriticalSectionRestoresPrimaskRather
> ThanEnablingIt` uses the host shim's `host_primask`, so it pins the save/restore discipline
> rather than real interrupt masking.
>
> **Evidence.** Mutation: reverting `bfifo_get_used()` to the index comparison (F67/N1) fails
> **4** tests — and one of them fails *by timing out*, because a full FIFO reporting empty
> stalls `bfifo_push_all()` against its consumer. That is the failure the board actually
> showed. Host suite 85/85.

### TS-02 — the CLI tokenizer has no automated coverage

**Priority: High** · `Src/cliutils/cli.c:82-200`

**Finding F74 (N8, N12)** was two defects here: a one-byte `.bss` overrun from a `static char
pLineCpy[CLI_LINEBUF_LENGTH]` holding a 191-character line plus terminator, and `strtok`'s shared
state used concurrently from the `cli` and `ptp` threads. Both are fixed; neither is tested.

The tokenizer is a pure function over a line buffer. Cases: a token of exactly `MAX_TOK_LEN`
characters (the F74/N12 case — `copy_token` must terminate); a line of exactly
`CLI_LINEBUF_LENGTH` characters; more than `TOK_ARR_LEN` tokens; empty and whitespace-only input;
and command matching where one command is a prefix of another (`ptp servo st` vs `ptp servo stm`
— the table has both, and a wrong `tokCnt` comparison would silently dispatch the wrong one).

The dependency to break is the `cmsis_os2.h` mutex and `MSG()`; see
[CX-03](#cx-03--tokenizer-and-commandtable).

> **✅ ADDRESSED** — `test/test_cli.cpp` + `test/cli_uut.c`, 15 cases. Every case listed above.
>
> **No extraction was needed.** `cli_uut.c` includes `cli.c` verbatim — the `mac_drv_uut.c`
> idiom this repo already uses — so the mutex, the FIFO and the thread API stay real and only
> the console is faked. One forwarding wrapper reaches `tokenize_cli_line()`, which is a
> file-static; everything else the tests drive is already public interface, which is where a
> test should be looking anyway. That makes [CX-03](#cx-03--tokenizer-and-commandtable)
> optional rather than a prerequisite.
>
> Two limitations stated in the file: the formatter is `vsnprintf`, not embfmt, so an embfmt
> defect cannot masquerade as a CLI failure; and a one-byte `.bss` overrun does not reliably
> crash, so the buffer test asserts the observable consequence rather than the overrun.
>
> **Evidence.** Three mutations, all caught: `strncpy` without the terminator (N12) fails 1,
> shrinking the line buffer by one (N8) fails 1, and prefix-matching instead of whole-token
> matching fails 1. **One of my own tests was wrong first** — the full-length-line case built
> its line from 47 short tokens, so tokenising stopped at `MAX_TOKEN_N` long before the buffer
> was full and it asserted nothing about the buffer. It now hits both limits at once: 8 tokens
> × 23 characters + 7 separators = exactly 191. Host suite 86/86.

### TS-03 — the DMA model cannot express a late context descriptor

**Priority: High** · `test/eth_model.cpp:100-119`

`deliver_frame_ts()` writes the data descriptor and the context descriptor in one call, both
fully populated. There is no way to construct the states [NX-01](#nx-01--the-receive-timestamp-is-read-from-a-context-descriptor-that-may-not-have-been-written)
describes.

Two model additions, both a few lines:

```cpp
void deliver_frame_ts_pending(...);   // TSA set; the next descriptor still OWNed by the DMA
void deliver_frame_ts_no_ctxt(...);   // TSA set; the next descriptor is a normal one
```

and two tests asserting that neither produces a delivered timestamp. Do them **before** fixing
NX-01, so the fix is mutation-verified the way the rest of this suite is: today both tests fail,
after the fix both pass, and reverting the fix turns them red again.

The model header's "known omissions" list should gain a line either way — it is the document that
told me the model was trustworthy, so it should be the document that says where it is not.

> **✅ ADDRESSED** — `test/eth_model.{hpp,cpp}`, `test/test_rx_ring.cpp`.
>
> Write-back ordering is now the **test's** choice rather than something the model decides on the
> hardware's behalf: `deliver_frame_ts()` writes both descriptors (ordinary case),
> `deliver_frame_ts_pending()` + `complete_pending_ts()` writes them separately, and
> `deliver_stray_context_descriptor()` produces an orphan. The header's omissions list says so.
>
> Four tests were written **before** the NX-01 fix and all four failed against the old driver,
> which is what makes the fix mutation-verified rather than merely accompanied by tests. A fifth
> scenario — a TSA frame followed by an ordinary frame — needed no new model method: pending
> a frame and then delivering a normal one puts exactly that in the context slot.

### TS-04 — `drv_send()`'s framing logic is untested

**Priority: Medium** · `Drivers/EthDrv/eth_drv_netxduo.c:469-616`

This is the function with the highest defect density in the whole port — three of the five
bring-up defects (`nx_interface_address_mapping_needed`, the checksum-offload declaration, the
2-byte alignment shim) live in or immediately around it — and it has zero automated coverage,
because it is entangled with `NX_PACKET`, `NX_IP_DRIVER` and `ETHHW_Transmit`.

Its decision content is pure: which EtherType for which command; `RAW_PACKET_SEND` must **not**
get a second header; the headroom check before `prepend_ptr -= 14`; the big-endian byte order of
the msw/lsw split; chain flattening with the `ETH_BUFFER_SIZE` cap; and — the one that matters
most — that **every** drop path calls `eth_drv_cancel_tx_timestamp()`. That last invariant is what
the whole registry design rests on, and nothing checks that a newly-added `return` honours it.
See [CX-05](#cx-05--ethernetframer).

> **✅ ADDRESSED, in part, and the part matters** — `Drivers/EthDrv/eth_framer.{h,cpp}` +
> `test/test_eth_framer.cpp`, 9 cases, with [CX-05](#cx-05--ethernetframer).
>
> Three of the six things listed above were liftable and are now tested: the EtherType mapping,
> the msw/lsw split (one expected byte array), and the saturating chain append.
>
> **Three were not, and pretending otherwise would have produced an abstraction harder to read
> than the code it replaced:** `RAW_PACKET_SEND` not getting a second header, the headroom
> check, and the cancel-on-every-drop-path invariant are all statements about `NX_PACKET`
> ownership. They stay in the driver and stay on the bench. The last one — that every `return`
> in `drv_send()` cancels the pending timestamp — remains the most fragile thing in the port,
> and this finding does not close it; see [AR-02](#ar-02--the-pending-timestamp-registry-could-key-on-the-message-instead-of-the-pointer)
> for the change that would make it unnecessary.
>
> **Evidence.** Three mutations, all caught: swapping the destination msw/lsw halves (1),
> off-by-one in the append (2), transposing ARP and RARP (1). Host suite 95/95. Hardware `S8`
> **12/12** — which is the real check on the refactor, since ARP, DHCP, ICMP and PTP-over-UDP
> all go through the rewritten path.

### TS-05 — the HIL `threads` check does not assert the priorities that matter

**Priority: Medium** · `tools/hil/checks.py:280-295`

The check asserts presence of `System Timer Thread`, `serialio`, `init`, `cli` (+ `eth ip` when a
stack exists), and priority 32 for three of them. It does **not** assert:

- `eth ip` at ThreadX **16** — "the stack stays ahead of everything that feeds it";
- `rx` at **24** — the priority `architecture.md` calls load-bearing for the F61 window;
- `ptpts` at **24** — latency to the servo;
- the presence of `rx`, `ptpts` or `phy` at all.

`architecture.md:277-288` presents that table as the specification, and `check_threads`'s own
docstring says it exists because the table "was previously checkable only with a debugger". Four
more dictionary entries close the gap.

> **✅ ADDRESSED** — `tools/hil/checks.py`. The check now asserts **presence** of `rx` and `phy`
> (gated on `expect_phy`, so the S3/S4 stages still work) and **every** documented priority:
> timer thread 0, `eth ip` 16, `rx` 24, `phy`/`serialio`/`init`/`cli` 32, plus `ptpts` 24 and
> `ptp` 32 when they exist. Reports `10 threads, 9 priorities as documented`.
>
> **Evidence.** Two mutations, both caught: claiming `eth ip` sits at 32 fails with
> `eth ip at ThreadX 16, expected 32`; claiming `rx` sits at the console's priority fails with
> `rx at ThreadX 24, expected 32`. Those are exactly the two relationships `architecture.md`
> calls load-bearing, and neither was checked before.

### TS-06 — CI verifies two of the four submodule patches

**Priority: Medium** · `.github/workflows/ci.yml:59-62`

```yaml
git -C Modules/embfmt  apply --check --reverse ".../0001-formatting-fixes.patch"
git -C Modules/flexptp apply --check --reverse ".../0002-servo-nan-latch-fix.patch"
```

`0003` and `0004` are not checked. `0003`'s absence is self-announcing (flexPTP `#error`s), so
the build covers it. **`0004` is the one that is silent** — without it every heartbeat is
rejected, the BMCA never leaves `INITIALIZING`, and no master is ever selected (F80). It is
precisely the patch whose omission the README's table describes as producing a firmware that
"simply never syncs", and it is the one CI does not verify.

Loop over the same `SUBMODULE_PATCHES` list `build.sh:205-210` already defines, so the two cannot
drift.

> **✅ ADDRESSED** — `build.sh --check-patches`, `.github/workflows/ci.yml`.
>
> A verify-only mode driven from the same `SUBMODULE_PATCHES` array as the apply path: it
> updates nothing, applies nothing and downloads nothing, so it stays an *independent* check —
> a build that applies a patch cannot also be the evidence that it was applied. CI's step is
> now one line.
>
> **Evidence.** Positive: all four reported applied, exit 0. Negative, which is the case that
> matters: reversing **only** `0004` — the silent one CI used to skip — makes it report
> `0004-… is NOT applied` and exit 1, with the other three still green. Restored and
> re-verified.

### TS-07 — no test of the NSD lifecycle

**Priority: Medium**

`ptp_nsd_init()` is a state machine — teardown, transport switch, IGMP join/leave ordering,
re-init, `(-1, -1)` shutdown — and it holds [NX-02](#nx-02--partial-failure-in-ptp_nsd_init-leaks-created-sockets).
None of it is tested, because it needs `NX_IP`.

It does not, though, need a *real* `NX_IP`. Behind a five-function interface (create, bind,
receive-notify, delete, multicast join/leave) the lifecycle becomes testable against a fake that
counts creates and deletes, and the properties are the interesting ones: every created socket is
eventually deleted; a create failure leaves no socket behind; a bind failure leaves no socket
behind; a failed init leaves the transport unset; IPv4 → 802.3 → IPv4 leaves exactly two sockets
and one L2 registration. See [CX-04](#cx-04--nsdlifecycle-over-an-injected-stack-interface).

> **✅ ADDRESSED** — `test/test_ptp_nsd_lifecycle.cpp`, 14 cases, all of the properties above
> plus teardown idempotence, shutdown-by-`(-1,-1)`, and five IPv4↔802.3 round trips leaking
> nothing. The fake is a ledger, not a simulator: it records every call in order (so the
> transport-switch test asserts the exact sequence `mcast:leave, delete:event, delete:general,
> l2:register, mcast:join`) and flags a second create of a live socket, which is the Release
> list corruption made loud.

### TS-08 — no compile-only gate for the non-default `ETH_STACK` paths

**Priority: Low**

`ETH_STACK=ETHERLIB` and `ETH_STACK=NONE` are both wired into the CMake and both are unbuildable
in CI today. `NONE` was the working configuration for stages S3–S5 and is the fallback if NetX Duo
ever needs to be bisected out; `ETHERLIB` is advertised in the README. A `configure + build`
matrix entry for each (allowed to fail loudly, not silently) would keep them from rotting, and
would immediately tell you whether [AR-05](#ar-05--the-eth_stack-switch-is-advertised-as-easier-than-it-is)'s
"easily changed" is true.

> **✅ ADDRESSED** — a second CI job, `Build (ETH_STACK=…)`, over a `[NONE, ETHERLIB]` matrix.
>
> It answered its own question immediately: **neither configuration built.** Both failed at
> `main.c` and `cmds.c`, which included NetX Duo headers unconditionally — so "easily changed"
> was not merely optimistic, it was false, and had been since S6. Fixed under
> [AR-05](#ar-05--the-eth_stack-switch-is-advertised-as-easier-than-it-is); both now compile.
>
> **Compile-only is the honest scope**, and the job comment says so: it proves the switch still
> means something, not that either configuration works.

### TS-09 — the ISR half of the context rule is reachable on the host after all

**Priority: Low** · `test/host/cmsis_compiler.h`, `test/test_rtos_context.cpp`

Both `testing_architecture.md` and `test/README.md` state that the ISR half of the
"blocking call from a context that may not block" rule cannot be covered on the host, because
`__get_IPSR()` is hard-coded to 0.

That is true of the *current* shim, not of the host. Make the shim read a settable variable:

```c
extern uint32_t host_fake_ipsr;
__STATIC_INLINE uint32_t __get_IPSR(void) { return host_fake_ipsr; }
```

`IS_IRQ_MODE()` inside ST's wrapper then evaluates true, and the wrapper's
`if ((IS_IRQ_MODE() && (timeout != 0)) || ...) status = osErrorParameter;` — the exact line quoted
at the top of `ptp_tx_timestamp.c` — becomes assertable. It does not simulate a real exception
context (ThreadX's own kernel-entry paths still think they are in a thread), so it pins the
*wrapper's* rule rather than the silicon's. That is a smaller claim than the timer-thread test
makes, and it should be documented as such — but it is the difference between "guarded by a
comment" and "guarded by a test" for the callback `ptp_tx_timestamp.c` exists to defer.

> **✅ ADDRESSED** — `test/host/cmsis_compiler.h` reads a settable `host_fake_ipsr`, and
> `test_rtos_context.cpp` gains two cases behind a `PretendInterruptContext` RAII guard: a
> blocking put from IRQ mode must be **refused**, and the timeout-0 form
> `ptp_tx_timestamp_from_isr()` actually uses must **succeed and deliver**.
>
> The narrower claim is stated in the test and in both documents that used to call this
> unreachable, with a table separating the two halves by *who enforces them*: the timer-thread
> rule is ThreadX's own (`txe_queue_send.c`), the interrupt rule is ST's wrapper checking
> `IS_IRQ_MODE()`. Only the first is a kernel restriction.
>
> **Evidence.** Mutation: asserting the blocking put *succeeds* fails with **−4**
> (`osErrorParameter`) against 0 — so the wrapper really does refuse it, and the test is not
> tautological. `RtosContext` is now 7 cases; host suite 95/95.

### TS-10 — the 2-byte alignment contract has no static assert

**Priority: Low** · `Drivers/EthDrv/eth_drv_netxduo.c:148-167`

The whole receive path depends on `NX_PHYSICAL_HEADER` being ≥ 16, so that a 14-byte Ethernet
header copied at `prepend_ptr + 2` leaves the IP header 4-byte aligned *and* leaves 12 bytes for
the timestamp NetX Duo writes over `data_start`. The comment explains this beautifully and cost a
bench session to learn. Nothing enforces it: a NetX Duo bump or an `nx_user.h` edit that changed
the constant would silently reintroduce "every IPv4 packet fails its checksum while ARP works
perfectly".

The file already uses `_Static_assert` for the state-block layout contract 80 lines above. One
more:

```c
_Static_assert(NX_PHYSICAL_HEADER >= 16,
               "the 2-byte alignment shim needs 16 bytes of headroom: 2 + 14, so that the IP "
               "header lands 4-byte aligned and NetX Duo's 12-byte timestamp fits below it");
```

> **✅ ADDRESSED** — added, with the failure mode in the message so the person who hits it does
> not have to rediscover it.
>
> **Evidence.** Mutation: defining `NX_PHYSICAL_HEADER` as 14 in `Inc/nx_user.h` — the realistic
> way it would drop — now fails the build with
> *"static assertion failed: the 2-byte alignment shim needs 16 bytes of headroom…"*. Before,
> that edit compiled cleanly and broke every IPv4 checksum while ARP kept working.

---

## 4 — C++ extraction opportunities

The three units already extracted (`ptp_tx_registry`, `ptp_ts_codec`, `ptp_endpoint`) are the
model to follow: `extern "C"` boundary, caller-owned storage, no allocation, and — the property
that makes them work — **no dependency on anything, not even the host fakes**. The candidates
below are ranked by (defect history × ease of extraction).

### CX-01 — `RxRing` / `TxRing` as typed C++ over an injected descriptor view

**Priority: Opportunity (High)** · would replace the `#include "mac_drv.c"` + `MAP_32BIT` + `-no-pie` scaffolding

Today the ring state machines are tested by textually including the driver's `.c` and then
arranging for every pointer in the process to fit in 32 bits — a `MAP_32BIT` arena, `-no-pie`, and
two harness tests that exist solely to detect that scaffolding breaking. `test/README.md` is
explicit that `-m32` "would be strictly better".

A C++ extraction removes the need for either. The 32-bit constraint exists only because
`ETHHW_DescExt` stores `bufAddr` and `tsCbPtr` as `uint32_t` for the target's benefit. Behind a
small `DescriptorView` abstraction — `own()`, `set_own()`, `length()`, `timestamp()`,
`callback()` — the *logic* (oldest-first modular selection, the `inFlight` window, the tail-pointer
restart) becomes pointer-width agnostic and testable natively:

```cpp
class TxRing {
  public:
    explicit TxRing(DescriptorView ring, State &state);
    std::optional<Completion> reap();       // the ETHHW_ProcessTx selection, as a value
};
```

The existing 19 ring tests port over largely unchanged and gain the ability to run under ASan and
UBSan, which the current arena arrangement precludes. This is the largest of the suggestions here
and the one with the best return: it retires a whole class of harness fragility.

**Caveat worth stating:** `mac_drv.c` is shared with the EtherLib path and was deliberately left
byte-unchanged by this port. Extraction breaks that property. It is the right change only if
`mac_drv.c` is accepted as this project's file rather than a carried-over one.

### CX-02 — `ByteFifo` with an injected critical-section policy

**Priority: Opportunity (High)** · pairs with [TS-01](#ts-01--blocking_fifoc-has-no-automated-coverage-at-all)

```cpp
template <class Crit>
class ByteFifo {
  public:
    ByteFifo(std::uint8_t *mem, std::uint32_t size);
    std::uint32_t used() const;    // modular, wrap-correct — the F67 property
    std::uint32_t push(std::span<const std::uint8_t> in);
    std::uint32_t pop(std::span<std::uint8_t> out);
};
```

`Crit` is `IrqMask` on the target and a no-op (or a `std::mutex`, or an *instrumented* one that
injects a simulated ISR entry at a chosen point) on the host. That last variant is what makes
the F67/N7 interleaving — a second writer entering mid-push — an actual test rather than a
code-reading exercise. The blocking wait stays in a thin C wrapper over `osSemaphore`, outside the
tested core.

### CX-03 — `Tokenizer` and `CommandTable`

**Priority: Opportunity (Medium)** · pairs with [TS-02](#ts-02--the-cli-tokenizer-has-no-automated-coverage)

`tokenize_cli_line()` is already pure apart from its buffer sizes; lifting it into
`Tokenizer::split(std::string_view line) -> FixedVector<std::string_view, 16>` removes the
`strtok_r` state question entirely (the F74/N8 defect becomes unrepresentable) and the
`copy_token` termination question with it (`string_view` does not terminate).

`CommandTable` — registration, prefix matching, argument counting, help rendering — is the other
half, and it is where the `ptp servo st` / `ptp servo stm` prefix ambiguity lives. Both are pure
functions over fixed-capacity storage; neither needs an allocator.

### CX-04 — `NsdLifecycle` over an injected stack interface

**Priority: Opportunity (Medium)** · pairs with [NX-02](#nx-02--partial-failure-in-ptp_nsd_init-leaks-created-sockets) and [TS-07](#ts-07--no-test-of-the-nsd-lifecycle)

```cpp
struct INetStack {                       // five calls; NetX Duo on target, a counting fake on host
    virtual bool socket_create(SocketId) = 0;
    virtual bool socket_bind(SocketId, std::uint16_t) = 0;
    virtual void socket_delete(SocketId) = 0;
    virtual bool l2_register() = 0;
    virtual void multicast(bool join, Group) = 0;
};

class NsdLifecycle {
    bool configure(Transport, DelayMechanism);   // transactional: all-or-nothing
    void teardown();
};
```

The value is not abstraction for its own sake — it is that "every created socket is eventually
deleted" and "a failure leaves nothing behind" become assertions a fake can check, and NX-02
becomes a red test before it becomes a fix. Note the virtual calls are on a cold path (profile
changes), so the vtable cost is irrelevant here; a template parameter would work equally well if
the C++ rules in `cpp_support.cpp` are read as excluding virtuals.

> **✅ ADDRESSED** — `Src/flexptp_nsd/ptp_nsd_lifecycle.{h,cpp}`, 14 tests.
>
> Built with a **C function-pointer struct** (`PtpNsdOps`) rather than an abstract class: the
> caller is C, the boundary is `extern "C"` like the other three units, and it sidesteps the
> question of whether `cpp_support.cpp`'s rules admit virtuals. Seven calls —
> create/bind/notify/delete, L2 register/unregister, multicast — and the storage is the
> caller's struct, so it adds nothing to any byte pool.
>
> The refactor paid for itself immediately beyond NX-02: it exposed that the multicast join
> reads the delay mechanism and transport, and that publishing them *after* `configure()`
> returns would be one step too late. `TheDelayMechanismIsPublishedBeforeTheGroupIsJoined`
> pins the ordering.

### CX-05 — `EthernetFramer`

**Priority: Opportunity (Medium)** · pairs with [TS-04](#ts-04--drv_sends-framing-logic-is-untested)

Two pure functions with no NetX Duo in sight:

```cpp
std::uint16_t ethertype_for(DriverCommand cmd, bool is_ipv6);
bool write_header(std::span<std::uint8_t> out, Mac dst, Mac src, std::uint16_t ethertype);
std::uint32_t flatten(const ChainView &chain, std::span<std::uint8_t> out);  // returns bytes, saturating
```

The msw/lsw byte-order split — twelve hand-written shifts at `eth_drv_netxduo.c:527-538`, written
out longhand precisely *because* getting it wrong "produces a MAC that looks plausible in a
debugger and is wrong on the wire" — becomes one test with one expected byte array.

> **✅ ADDRESSED** — built as three `extern "C"` functions rather than the sketch above:
> `eth_framer_ethertype()`, `eth_framer_write_header()` and `eth_framer_append()`. No span, no
> chain view — the driver keeps its `NX_PACKET` walk and calls the saturating append per chunk,
> which is a smaller change to the file with the finding history and leaves the ownership rules
> where they are. The unit depends on nothing, not even `nx_link.h`: the EtherType constants are
> its own, so a test of the framing is not a test of NetX Duo's headers.

### CX-06 — `LinkState` as a pure transition function

**Priority: Opportunity (Low)** · `eth_drv_netxduo.c:220-269`

`phy_thread`'s decision logic — suppress `DOWN` on the first poll, report every genuine transition
after it, push speed/duplex into the MAC only on `UP` — is four lines of policy tangled with
`osDelay`, `MSG()` and register writes. As `LinkDecision next(LinkState prev, PhyStatus now)`
returning `{new_state, announce, program_mac}`, the first-poll suppression rule (which exists so a
normal boot does not look like a cable fault) becomes a three-case test.

> **✅ ADDRESSED** — `Drivers/EthDrv/eth_link_state.{h,cpp}` + 7 tests, built almost exactly as
> sketched: `eth_link_state_update(EthLinkState *, EthPhyStatus) -> EthLinkAction` with
> `{announce_up, announce_down, program_mac, publish_to_stack}`. `phy_thread()` now only carries
> the decision out.
>
> `publish_to_stack` is the fourth flag the sketch did not have, and it earns its place: it is
> the `nx_interface_link_up` write, and setting that only on `UP` is the bring-up defect where
> the board reported "ETH LINK: UP", received frames happily and transmitted absolutely nothing.
>
> **Evidence.** Two mutations: announcing `DOWN` on the first poll fails 1, and publishing to
> the stack only on `UP` fails 2. Host suite 104/104; hardware `S8` **13/13**.

### CX-07 — make the C++ rules enforced rather than documented

**Priority: Opportunity (Low)** · `Src/cpp_support.cpp:22-30`

The rules — no dynamic allocation, no exceptions, no RTTI, `extern "C"` at every boundary — are a
comment. Two of the four are already enforced by compiler flags; the allocation rule is not.

A CI step asserting the linked image contains no `operator new`, no `__cxa_throw` and no
`std::__throw_*` would make the most important rule as observable as `cpp_ctor` makes static
initialisation. It is one `nm | grep -q` away, and it fits this project's existing habit of
checking configuration claims rather than trusting them (F78). Note it cannot assert "no
`malloc`" — newlib's `strtod` pulls it in, which is [NX-03](#nx-03--the-newlib-heap-and-the-threadx-byte-pools-are-not-disjoint).

> **✅ ADDRESSED** — `tools/ci/check_cxx_rules.sh`, run by CI on both configurations. It greps
> the linked image for the mangled names of `operator new`/`new[]`/`delete`/`delete[]`,
> `__cxa_throw`, `__cxa_allocate_exception` and the `__cxxabiv1` RTTI vtables.
>
> Checked against the **image**, not the sources — the same argument `cpp_ctor` makes for
> asking the running board about static constructors.
>
> **Evidence.** Clean today; and **not vacuous**: adding a `std::vector` to `ptp_endpoint.cpp`
> makes it fail with `08028de0 T _ZdlPv`. As the finding notes, it deliberately does not assert
> "no `malloc`" — newlib's `strtod` pulls that in, which is
> [NX-03](#nx-03--the-newlib-heap-and-the-threadx-byte-pools-are-not-disjoint)'s territory and
> is bounded there instead.

---

## 5 — Documentation vs implementation

The published documents are, with the exceptions below, remarkably accurate — the memory figures
match the linked image to the byte, and the test counts match `ctest`. Most of what follows is
**in-source comments** that the port moved past.

A distinction worth making first, because it affects how these should be read: this codebase uses
deliberate "what lwIP needed, and what replaced it" comparisons throughout, and those are
valuable. The findings below are not those. They are statements that describe the **present tense
of a system that no longer exists**.

### DC-01 — the interrupt priority table is wrong in two rows

**Priority: Medium** · `docs/architecture.md:352,354`

| Row | Documented | Actual (`tx_initialize_low_level.S:125-128`) |
|---|---|---|
| SVC | 0 | **15** (`0xFF` in SHPR2) |
| SysTick | 15 (`TICK_INT_PRIORITY`) | **4** (`0x40` in SHPR3) |

`TICK_INT_PRIORITY` *is* 15, but `HAL_InitTick` runs before `tx_kernel_enter()` and the assembly
overwrites it. See [NX-04](#nx-04--systick-and-svcall-run-at-priorities-the-documentation-does-not-state).
The same section's neighbouring claim — "ETH (7) preempts the console (12)" — remains true.

> **✅ ADDRESSED** — the table now has all nine rows with the measured values, says why SysTick
> sits above Ethernet, and warns that `TICK_INT_PRIORITY` is not the setting. Verified by the
> `irq_prio` check rather than by re-reading, which is the part that keeps it true.

### DC-02 — Release drops two checks the architecture document presents as unconditional

**Priority: Medium** · `docs/architecture.md:369-372`

> *"`TX_ENABLE_STACK_CHECKING` is defined in `Inc/tx_user.h` and `app_stack_error_handler()` is
> registered with `tx_thread_stack_error_notify()` in `main()`"*

`Inc/tx_user.h:73-75` wraps it in `#ifdef DEBUG`. In Release there is no software stack check at
all — only the PSPLIM hardware trap, which the same paragraph correctly identifies as the primary
mechanism, so nothing is *broken*. But the sentence reads as unconditional and it is not, and the
registration call in `main.c:312` is unconditional, which reinforces the wrong impression.

The same section should mention `NX_DISABLE_ERROR_CHECKING` in Release
([AR-04](#ar-04--debug-and-release-differ-in-more-than-optimisation-level)), which is a larger
behavioural difference than the stack check.

> **✅ ADDRESSED** — the sentence now says "**in Debug only**", and notes that the registration
> in `main()` is unconditional while the checking is not. The larger differences are collected
> in the new *Debug and Release differ in more than optimisation level* section above it.

### DC-03 — `ethernet_netxduo.c` still describes the S4 stub driver, and contradicts itself 55 lines later

**Priority: Low** · `Src/ethernet/ethernet_netxduo.c:8-9` vs `:62-63`

```c
 * Stage S4 wires everything except the driver, which is a stub -- see nx_driver_stub() at the
 * bottom. The IP thread runs, the pool exists, and nothing reaches the wire.
```

There is no `nx_driver_stub()` in the file, and the driver has not been a stub since S5a — which
line 63 of the same file says correctly. A reader who stops at the header comes away believing
this firmware has no network.

> **✅ ADDRESSED** — the header now names the real driver and says where the stack was wired in,
> while keeping the fact that S4 had a stub (it explains why parts of the file are phrased as
> though the wire were unreachable).

### DC-04 — `main.c` says the Ethernet driver is still a stub

**Priority: Low** · `Src/main.c:187-190`

> *"NetX Duo from S4. The Ethernet driver under it is still a stub until S5a…"*

Same issue, in the file most likely to be read first.

> **✅ ADDRESSED** — replaced with what `init_ethernet()` actually brings up, keeping the
> "report and continue" rationale, which is still the right call.

### DC-05 — the driver's default case describes features it now handles explicitly

**Priority: Low** · `Drivers/EthDrv/eth_drv_netxduo.c:667-671`

> *"Multicast join/leave and deferred processing arrive in S5c. Reported as handled so the stack
> does not mark the interface broken in the meantime."*

Both arrived. Both have their own `case` labels 15 lines above. The `default` is now a genuine
catch-all and should say what it is catching and why `NX_SUCCESS` is the right answer for an
unknown command.

> **✅ ADDRESSED** — the comment now says what falls through (`GET_INTERFACE_TYPE`, the IPv6 and
> PPP commands, anything a future NetX Duo adds) and why `NX_SUCCESS` is right: the stack marks
> an interface broken on a driver error, and refusing a command this firmware does not need is
> not a reason to take the link down.

### DC-06 — three `mac_drv.c` comments are written in terms of a deleted stack

**Priority: Medium** · `Drivers/EthDrv/mac_drv.c:459, 586, 723-728`

| Line | Says | Reality |
|---|---|---|
| 459 | *"rx_thread copies them into `pbuf->time_s`/`time_ns` unconditionally"* | it fills an `NX_LINK_TIME` |
| 586 | *"Signature must match `lwipopts.h`'s `tx_cb`"* | `Inc/lwipopts.h` was deleted in S3 |
| 723-728 | the entire rationale for the bounded transmit wait — *"`low_level_output()` calls it on `tcpip_thread` at priority 40 while holding the lwIP core lock, so a stalled TX DMA would … deadlock every lwIP user behind the lock"* | no lwIP, no core lock |

The 723-728 one matters most, because the *hazard is still real and still exactly as described* —
NetX Duo holds `nx_ip_protection` across the driver call (`nxd_udp_socket_send.c:578`), so a
stalled TX DMA would block every IP user behind that mutex instead. The comment argues correctly
for a bound that is still needed, using a system that is gone. Rewriting it in NetX Duo terms
preserves the argument and makes it checkable.

> **✅ ADDRESSED** — all three rewritten in current terms rather than deleted, because two of the
> three arguments survive the stack swap intact. The bounded-wait rationale now names
> `nx_ip_protection` and the call sites that hold it, keeping the lwIP sentence in parentheses as
> history. The `tx_cb` comment states the signature outright and notes that the only thing
> enforcing it is the cast — which is why F48 went unnoticed until it stopped compiling.

### DC-07 — three references to a CLI command that does not exist

**Priority: Low** · `Src/flexptp_nsd/ptp_tx_timestamp.c:54`, `ptp_tx_timestamp.h:17`,
`docs/threadx_porting_plan.md:176`

All three say the transmit-timestamp counters are *"Surfaced by `ptp diag ts`"*. There is no
`ptp diag` command — `cmd_init()` registers `osinfo`, `flexptp` and `ethinfo`, and the counters
are printed by `ethinfo` via `eth_drv_report()`. The porting plan's instance is the one that
matters, because it describes the mechanism a HIL check depends on.

> **✅ ADDRESSED** — all three now say `ethinfo`.

### DC-08 — `flexptp_options.h` describes the transmit-timestamp deferral as future work

**Priority: Low** · `Inc/flexptp_options.h:42-44`

> *"That does not bite yet: the S3 stub driver transmits nothing. S6 must defer the callback out
> of interrupt context before wiring up a real driver."*

S6 is done and `Src/flexptp_nsd/ptp_tx_timestamp.c` is the deferral. The warning above it is
excellent and should stay — it is the sharpest hazard in the port — but it should point at the
solution rather than schedule it. (Cosmetic, same file: the include guard is still
`SRC_FLEXPTP_OPTIONS_ETHERLIB`.)

> **✅ ADDRESSED** — the warning stays and now points at `ptp_tx_timestamp.c`, with the part that
> matters spelled out: delete that file and every transmit timestamp goes silently missing again.
> The guard name is left alone and explained — renaming it is a no-op with a small chance of a
> collision.

### DC-09 — the IGMP comment keeps a superseded first paragraph

**Priority: Low** · `Src/flexptp_nsd/nsd_netxduo.c:328-336`

Two consecutive comment blocks, the first describing `PTP_IGMP_*` constants from
`flexptp_options.h` and byte order, the second correcting it to say the endpoint unit returns host
order. The code uses only the second. The first reads as current until you reach the second.

> **✅ ADDRESSED** — merged into one block that states the current behaviour first and keeps the
> `PTP_IGMP_*` constants as the contrast they were meant to be.

### DC-10 — `README.md` overstates the `ETH_STACK` switch

**Priority: Low** — see [AR-05](#ar-05--the-eth_stack-switch-is-advertised-as-easier-than-it-is).

---

## 6 — Documentation self-consistency

### DS-01 — `port_review.md`'s table of contents omits its largest section

**Priority: Medium** · `docs/port_review.md:22-34` vs `:1656`

The TOC lists ten sections. **"Second-Review Findings"** — 620 lines, findings F67 through F80,
including F80, the defect that cost a bench session and required the fourth submodule patch — is
not among them. A reader navigating by TOC never learns those findings exist.

> **✅ ADDRESSED** — the section is in the TOC, renamed *Second-Review and ThreadX-Port
> Findings* so the heading stops claiming a single source.

### DS-02 — the Disposition Summary claims completeness it does not have

**Priority: Medium** · `docs/port_review.md:119-195`

The table runs F1 → F66 and closes with **"Every finding is tracked."** F67–F80 are absent. So are
they from both phase tables:

| Location | Coverage |
|---|---|
| `:78-84` "Phase / Findings / Evidence" | F1–F43, F44–F50, F51–F66 |
| `:89-95` "Findings by phase" | F1–F43 (43), F44–F50 (7), F51–F66 (16) |

Fourteen findings — a fifth of the log, and the fifth containing most of the ThreadX/NetX Duo work
— have no disposition row and no phase attribution. Given that this document is explicitly the
living record that supersedes `docs/reviews/` where they disagree, and given how carefully the
phase split is justified ("check which phase a finding came from before trusting how it was
established"), the omission undercuts the document's central discipline.

Adding a fourth phase row ("Second review and ThreadX port · F67–F80 · independent review of the
tree, plus the ThreadX/NetX Duo bench sessions") and extending the disposition table would fix
both DS-01 and DS-02.

> **✅ ADDRESSED** — 14 disposition rows added, checked programmatically: every `### Fnn` section
> now has a row. And **two** phase rows rather than one, because the sources really are
> different — *Independent review · F67–F79* and *ThreadX/NetX Duo port · F80*. The completeness
> claim now carries a note recording that it was wrong and for how long.

### DS-03 — F80 is attributed to the second review pass, which did not find it

**Priority: Low** · `docs/port_review.md:1656-1660`

> *"Raised by the second review pass (`final_review_v2_opus_5p0.md`, `consolidated_review_v2.md`)
> and numbered F67 onward."*

F80 is under that heading, but it was found during the ThreadX port's S6 bench session — as its
own entry, `Inc/tx_user.h:47-53` ("CORRECTION, S6 … it cost the S6 bench session") and
`testing_architecture.md:121-132` all say. Every other finding in the section has an `[Nnn]`
bracket tying it to a second-review identifier; F80 has none, which is the visible symptom.

> **✅ ADDRESSED** — the section body separates F67–F79 (second review) from F80 (the S6 bench
> session), and points out that the missing N-number is the visible sign of the difference
> rather than an omission.

### DS-04 — "Two options" is followed by three

**Priority: Low** · `docs/threadx_s5e_plan.md:80`

`## Two options` → `### Option A`, `### Option B`, `### Option C`, `## How the three compose`,
and a recommendation that sequences all three. The status banner at the top is accurate ("C and B
are built and passing"); only the heading was left behind when C was added.

> **✅ ADDRESSED** — the heading is now *The three options*.

### DS-05 — the host suite's runtime is quoted as three different numbers

**Priority: Low**

| Source | Claim |
|---|---|
| `README.md:110` | "about 0.2 seconds" |
| `docs/testing_architecture.md:29-30` | 0.2 s (tier 1) and 0.04 s (tier 2) |
| `docs/testing_architecture.md:228`, `test/README.md:157` | "the 0.04 s the whole suite takes" |
| `docs/threadx_s5e_plan.md:70` | "runs in 0.06 s" |
| Measured here | **0.16 s** for all 55 |

Trivial individually. Worth one pass because the 0.04 s figure is used as the justification for
the `TIMEOUT 10` headroom calculation ("250× headroom"), which against the real figure is ~60×
— still ample, but the stated arithmetic no longer follows.

> **✅ ADDRESSED** — re-measured (three runs: 0.25, 0.25, 0.26 s for 86 entries; 0.20 s for the
> tier-1 subset) and made consistent across five documents. The `TIMEOUT 10` justification no
> longer quotes a multiplier that has to be maintained — the suite grew by 31 entries during
> this review alone, which is exactly why that number kept going stale.

### DS-06 — the review archive's README stops at "F7x"

**Priority: Low** · `docs/reviews/README.md:8`

> *"every finding (F1–F7x)"*

The log reaches F80. A small thing, but it is in the sentence that tells the reader `port_review.md`
is the complete record.

> **✅ ADDRESSED** — now `F1–F80`.

---

## 7 — General improvements

### GI-01 — let the HIL runner prove which firmware it is testing

**Priority: Medium**

Both the README and `testing_architecture.md` carry a warning that without `--flash` the runner
"will report a confident pass for code you never loaded", and note it has already caught someone
out. A warning is the weakest available control for a hazard this cheap to eliminate.

Have the firmware print a build identity at boot — a short hash over the `.text`/`.rodata` image,
or the git describe injected as a compile definition — and add it to `osinfo`. `run.py` then reads
the same identity out of the supplied ELF and **fails with exit 1 on a mismatch**, whether or not
`--flash` was passed. That converts "remember to pass a flag" into "cannot report a pass for the
wrong image", which is the same transformation `build.sh`'s patch check already applied to the
submodule hazard.

> **✅ ADDRESSED** — `-Wl,--build-id=sha1` + the linker script + `osinfo` + a `build_id` check
> that runs **first in every stage**.
>
> The identity is the linker's own SHA-1 over the linked output, not a home-made hash or a
> `__DATE__` string: it changes when and only when the image does. `osinfo` parses the ELF note
> out of FLASH; the runner parses the same note out of the ELF **in pure Python**, so a missing
> `readelf` cannot make the check silently skip — which would be the exact failure mode it
> exists to remove.
>
> **Evidence, all three paths:**
>
> | Case | Result |
> |---|---|
> | board flashed from the given ELF | `PASS build_id 6fe33dcff5c8d8ec… matches flexptp-demo.elf` |
> | board running a different image (built a variant to force it) | `FAIL … the board is running 6fe33dcff5c8d8ec… but flexptp-demo.elf is 16044a068c71263a… -- every other result in this run is about the wrong firmware` |
> | no ELF given | `SKIP … no ELF to compare against`, and the run reports **incomplete** — no claim was made, so none is checked |
>
> Full `S8` **12/12**, `|offset| ≤ 3 817 ns`. The README's `[!WARNING]` about `--flash` is now
> a `[!NOTE]` describing a control rather than a hazard.

### GI-02 — give `ethinfo` and `osinfo` a machine-readable mode

**Priority: Low**

`tools/hil/checks.py` regex-parses prose intended for humans, and the code says what that costs:
*"An earlier version used `\S+` and silently miscounted"* (`checks.py:256`). Every future edit to
a `MSG()` format string in `eth_drv_report()` is a potential silent HIL breakage — and a HIL check
that silently stops asserting is the failure mode this project cares most about.

A `--json` (or a second, terse `ethinfo raw`) emitting `key=value` lines would let the checks parse
something with a contract. The prose output stays for humans.

> **✅ ADDRESSED** — a new `ethstat` command emitting `key=value` between `BEGIN ethstat` and
> `END ethstat`, and a `read_stats()` helper in `checks.py` that parses it. The prose `ethinfo`
> stays for humans and its layout is explicitly free to change; the key names are the contract.
> The driver's `key=value` dump is deliberately a *separate function* from its prose one, for
> the same reason.
>
> It paid for itself immediately: a new **`counters`** check asserts that all ten drop and
> refusal counters read zero — `rx.ts_deferred`, `rx.ts_stray`, `nsd.runt`, `nsd.chained`,
> `ptpts.dropped`, and the driver's five. Every one of them exists because its absence is
> invisible, and until now none was ever *asserted*, only printed.
>
> **And the change caught itself.** Adding a command made `cli` fail with
> `expected 24 registered commands, got 25` — that check asserts the exact count on purpose,
> because a command silently failing to register is a real failure mode. Updated to 25, along
> with the README's captured listing.
>
> Hardware `S8` is now **13/13**.

### GI-03 — add static analysis to CI

**Priority: Low**

CI gates on first-party compiler warnings, which is good and unusual. `cppcheck --enable=warning`
or `clang-tidy` scoped to `Src/`, `Inc/` and `Drivers/EthDrv/` would add the classes GCC does not
reach — and several findings in this review ([NX-05](#nx-05--unchecked-length-arithmetic-on-the-8023-receive-path)'s
unsigned underflow, [NX-08](#nx-08--receive-queue-leaked-if-the-receive-thread-fails-to-start)'s
leak-on-error) are in the categories those tools are good at. A `compile_commands.json` already
falls out of the CMake build.

> **✅ ADDRESSED, with one part verified and one part not — and the difference is recorded.**
>
> **GCC `-fanalyzer`, verified.** Enabled on the first-party *host* targets in
> `test/CMakeLists.txt`, so it runs in the existing host-tests job. Clean today, and **not
> vacuous**: introducing a NULL dereference into `ptp_tx_registry.cpp` is reported as
> `-Wanalyzer-null-dereference` with the path that reaches it. Not `-Werror` — a checker that
> blocks the suite on a false positive is a checker somebody disables.
>
> **cppcheck, not verified here.** It covers what `-fanalyzer` cannot reach: the files that only
> exist cross-compiled (`eth_drv_netxduo.c`, `nsd_netxduo.c`, `main.c`, `cmds.c`,
> `serial_io.c`). I could not run it — the Debian binary hard-codes `FILESDIR` to a path outside
> a non-root prefix and refuses to start without an install, and my first apparent "clean run"
> was in fact cppcheck failing to start. The job comment says so. `--error-exitcode=1` is kept
> deliberately: if the first CI run finds something, a red job is the correct outcome.

### GI-04 — two `.pyc` files are committed

**Priority: Low** · `tools/hil/__pycache__/checks.cpython-314.pyc`, `hil.cpython-314.pyc`

`git ls-files tools/hil` lists both. `.gitignore` has two entries (`build/`, `toolchains/`) and
lacks `__pycache__/` and a trailing newline. `git rm --cached` the two files and add the pattern.

> **✅ ADDRESSED** — both files untracked; `.gitignore` gained `__pycache__/`, `*.pyc` and the
> missing trailing newline. Done early and out of priority order because the stale bytecode was
> turning up as a modification in every commit of this review's work.

### GI-05 — the release optimisation-level analysis deserves a re-run

**Priority: Low** · `CMakeLists.txt:150-180`

The `-Og` vs `-O2` vs `-Os` comparison is excellent work — three levels built *and run on
hardware*, with the F62 disassembly regression checked at each. But the absolute figures quoted
(167 404 / 167 708 / 165 140 B) predate the ThreadX/NetX Duo port; the current image is 206 344 B.
The comment even warns to "compare deltas rather than absolutes". The conclusion is very likely
unchanged, but the numbers now describe a different firmware, and this is a project that measures.

> **✅ ADDRESSED — re-measured and re-run, not just re-numbered.**
>
> | Level | FLASH | vs `-Og` | Hardware |
> |---|---|---|---|
> | `-Og` *(ships)* | 149 400 B | — | `S8` 12/12, offset ≤ 11 036 ns |
> | `-O2` | 149 456 B | **+56 B** | `S8` 12/12, ≤ 14 895 ns |
> | `-Os` | 147 400 B | −2 000 B | `S8` 12/12, ≤ 9 142 ns |
>
> *(The 206 344 B in the finding above is the **Debug** image — `-O0 -g3`. Release was never
> that size; the two were being compared across configurations.)*
>
> The absolutes are ~18 KB below the pre-port figures — lwIP and FreeRTOS left, NetX Duo and
> ThreadX arrived — but the *relationship* the decision rests on is unchanged, including the
> counter-intuitive part: `-O2` is still **larger** than `-Og`.
>
> **One claim was narrowed rather than refreshed.** The comment said the F62 regression "was
> checked at each level: no byte-wide loads of the UID region appear in any of the three
> disassemblies". I could only reproduce that at `-Og`, where the UID is read with `ldr.w` and
> the bytes extracted with `ubfx`. At `-O2` and `-Os` `drv_initialize()` is inlined away and I
> did not track the base register through the inlined code. The comment now says exactly that,
> and notes the hardware runs as the weaker evidence standing in for it — F62 was a HardFault,
> so a pass does mean the byte access is absent, but only on the paths `S8` exercises.

### GI-06 — consider persisting PTP settings

**Priority: Low**

`architecture.md:262-265` and `ptp_profiles.md` both note that a runtime `ptp profile preset …` is
lost on reset because there is no settings store. On a demo board that is arguably correct
behaviour — every reset returns to a known state. Recording it as an explicit decision (with
"where you would put it if you wanted it": one flash sector, written on an explicit `ptp save`)
rather than as an absence would close the question.

> **✅ ADDRESSED** — recorded as a decision in `architecture.md`, with the reason (nothing
> accumulates invisibly across sessions, so "it behaves differently from your board" is never
> explained by settings someone typed a month ago) and the shape it would take: one flash
> sector, magic-and-version header, fallback to the compile-time preset on mismatch, and written
> **only** on an explicit `ptp save` — never implicitly by `ptp profile preset`, which would
> reintroduce exactly the invisible state this avoids.

---

## Summary table

A leading ✅ marks a finding that has been addressed — see the box at the end of its section for
how, and for the evidence.

**Priority key** — **P1**: fix before the next release; latent correctness or silent-failure risk.
**P2**: fix soon; wrong documentation on a load-bearing detail, or a meaningful test gap.
**P3**: nit, hygiene, or a recorded decision. **Opp**: opportunity/suggestion, no defect implied.

| ID | Pri | Type | Finding | Location | Notes |
|---|---|---|---|---|---|
| [NX-01](#nx-01--the-receive-timestamp-is-read-from-a-context-descriptor-that-may-not-have-been-written) | ✅ **P1** | Defect | RX timestamp read from a context descriptor without checking `OWN` or `CTXT` | `mac_drv.c:471-483` | ST's HAL checks both and retries (`stm32h5xx_hal_eth.c:1130-1148`). Yields a plausible wrong `t2`, and can deliver a context descriptor upward as a frame. Invisible to the host suite — see TS-03 |
| [NX-02](#nx-02--partial-failure-in-ptp_nsd_init-leaks-created-sockets) | ✅ **P1** | Defect | Partial failure in `ptp_nsd_init()` leaks created UDP sockets; `s_tp`/`s_dm` left stale | `nsd_netxduo.c:265-293` | Debug → PTP permanently dead (`NX_PTR_ERROR`). Release → `nx_ip_udp_created_sockets_ptr` list corruption, because `NX_DISABLE_ERROR_CHECKING` removes the guard |
| [NX-03](#nx-03--the-newlib-heap-and-the-threadx-byte-pools-are-not-disjoint) | ✅ **P1** | Defect | `_sbrk` can allocate into the ThreadX byte pools after 2 560 bytes; the linker script claims the two are disjoint | `sysmem.c:56-72`, `.ld:200-211` | `atof`→`strtod`→`_Balloc`→`_malloc_r` all linked; reachable from `ptp pps` and the servo CLI. One-line fix: bound against `__RAM_segment_used_end__` |
| [NX-04](#nx-04--systick-and-svcall-run-at-priorities-the-documentation-does-not-state) | ✅ P2 | Defect/Doc | SysTick runs at priority 4 (not 15) and SVCall at 15 (not 0); changed silently from the FreeRTOS build | `tx_initialize_low_level.S:119-128` | SysTick now preempts ETH. Not unsafe (PRIMASK critical sections) but undocumented; the file's own comment contradicts its code |
| [NX-05](#nx-05--unchecked-length-arithmetic-on-the-8023-receive-path) | ✅ P3 | Defect | `nx_packet_length - header_size` can underflow; chained L2 packet dropped silently | `nsd_netxduo.c:197-206` | Contained by flexPTP's `MIN(len, 128)` clamp — verified in `task_ptp.c:486`. NetX Duo does **not** validate the length itself |
| [NX-06](#nx-06--the-receive-queue-drop-path-is-the-only-uninstrumented-one) | ✅ P2 | Gap | `ETHHW_ReadCallback` drops events on a full queue with no counter | `eth_drv_netxduo.c:687-696` | Already documented as the drop path that bites under load (`architecture.md:335`). Two-line fix in a driver built around counters |
| [NX-07](#nx-07--rx-packet-length-is-masked-to-14-bits-but-the-field-is-15) | ✅ P3 | Latent | RDES3 length masked `0x3FFF`; `ETH_DMARXNDESCWBF_PL` is `0x7FFF` | `mac_drv.c:466` | Unreachable at MTU 1500. The adjacent TX `& 0x3FFF` **is** correct, which makes the difference easy to miss |
| [NX-08](#nx-08--receive-queue-leaked-if-the-receive-thread-fails-to-start) | ✅ P3 | Defect | RX queue re-created and leaked if `osThreadNew` failed on a previous `NX_LINK_INITIALIZE` | `eth_drv_netxduo.c:356-373` | Defeats the `rx_started` guard it sits inside |
| [NX-09](#nx-09--ptp-pps-0-reports-off-but-may-leave-the-output-running-at-1-hz) | ✅ P3 | **Confirmed** | `ptp pps 0` writes `PPSCTRL=0`, likely 1 Hz rather than off, while reporting "turned off" | `ptp_port_stm32h563.c:35-40,88-92` | **Needs bench confirmation** — scope on PG8. Not asserted; RM0481 not available to this review |
| [NX-10](#nx-10--nx_link_disable-does-not-stop-the-mac) | ✅ P3 | Gap | `NX_LINK_DISABLE` leaves `RE`/`TE`/`SR`/`ST` set | `eth_drv_netxduo.c:403-406` | Unreachable today; becomes a bug with any link-flap recovery |
| [AR-01](#ar-01--every-frame-is-copied-twice-on-transmit-and-once-on-receive) | Opp (M) | Arch | Two copies per TX frame, one per RX; NetX Duo's zero-copy is unused | driver + `mac_drv.c` | 48 × 1 528 B = 73 KB of SRAM3 exists only to be copied out of and into. Crosses the `mac_drv.c` boundary this port kept unchanged |
| [AR-02](#ar-02--the-pending-timestamp-registry-could-key-on-the-message-instead-of-the-pointer) | ✅ Opp (M) | Arch | TX-timestamp registry keys on a recycled pointer; keying on `(msgType, sequenceId, portIdentity)` removes the ABA hazard structurally | `ptp_tx_registry.cpp` | Current design is correct and well-tested; recorded because it rests on three invariants a future drop path could break |
| [AR-03](#ar-03--diagnostics-reach-across-layers-through-function-local-extern-declarations) | ✅ P3 | Arch | Four unheadered `extern` prototypes inside `eth_drv_report()` | `eth_drv_netxduo.c:729-759` | Signature-drift exposure — the F48 class. A small registration table fixes it without inverting the dependency |
| [AR-04](#ar-04--debug-and-release-differ-in-more-than-optimisation-level) | ✅ P2 | Arch/Doc | Release changes NetX Duo API semantics and drops the software stack check | `nx_user.h:91-103`, `tx_user.h:73-75` | Turns NX-02 from an error code into list corruption. No document states the delta |
| [AR-05](#ar-05--the-eth_stack-switch-is-advertised-as-easier-than-it-is) | ✅ P3 | Doc/Arch | README calls the EtherLib switch easy; design_doc says the files have never been compiled | `README.md:263-271` | Align the two; see TS-08 |
| [AR-06](#ar-06--no-watchdog-and-every-failure-path-halts-forever) | ✅ P3 | Decision | No IWDG; all failure paths halt | `main.c`, `serial_io.c` | Defensible for a bench demo — but undocumented as a choice |
| [AR-07](#ar-07--equal-priority-scheduling-semantics-changed-with-the-kernel) | ✅ P3 | Doc | Equal-priority round-robin quantum changed 1 ms → 4 ms with the kernel swap | `cmsis_os2.c:116,923` | Checked expecting starvation; there is none. `RTOS2_DEFAULT_TIME_SLICE` is overridable |
| [TS-01](#ts-01--blocking_fifoc-has-no-automated-coverage-at-all) | ✅ **P1** | Test gap | `blocking_fifo.c` untested despite carrying F67 (N1, N7) and wrap-dependent arithmetic | `blocking_fifo.c` | Cheapest high-value test in the tree — the real-ThreadX host target already exists |
| [TS-02](#ts-02--the-cli-tokenizer-has-no-automated-coverage) | ✅ **P1** | Test gap | CLI tokenizer untested despite F74 (N8, N12: one-byte overflow, shared `strtok` state) | `cli.c:82-200` | Include the `ptp servo st` / `ptp servo stm` prefix-matching case |
| [TS-03](#ts-03--the-dma-model-cannot-express-a-late-context-descriptor) | ✅ **P1** | Test gap | DMA model always writes a complete context descriptor, so NX-01 is unreachable | `eth_model.cpp:100-119` | Add `deliver_frame_ts_pending()` / `_no_ctxt()`. Write the tests **before** the NX-01 fix so it is mutation-verified |
| [TS-04](#ts-04--drv_sends-framing-logic-is-untested) | ✅ P2 | Test gap | `drv_send()` untested — the function with the port's highest defect density | `eth_drv_netxduo.c:469-616` | Most important property: **every** drop path cancels the pending timestamp |
| [TS-05](#ts-05--the-hil-threads-check-does-not-assert-the-priorities-that-matter) | ✅ P2 | Test gap | HIL `threads` check omits `eth ip` (16), `rx` (24), `ptpts` (24) and their presence | `checks.py:280-295` | `architecture.md` presents that table as the spec; four dictionary entries close it |
| [TS-06](#ts-06--ci-verifies-two-of-the-four-submodule-patches) | ✅ P2 | Test gap | CI verifies patches 0001 and 0002 only; **0004 is the silent one** | `ci.yml:59-62` | Without 0004 no master is ever selected. Loop over `build.sh`'s existing `SUBMODULE_PATCHES` list |
| [TS-07](#ts-07--no-test-of-the-nsd-lifecycle) | ✅ P2 | Test gap | No lifecycle test for `ptp_nsd_init` teardown / transport switch / re-init | `nsd_netxduo.c` | The state machine holding NX-02. Needs a fake, not a real `NX_IP` |
| [TS-08](#ts-08--no-compile-only-gate-for-the-non-default-eth_stack-paths) | ✅ P3 | Test gap | `ETH_STACK=ETHERLIB` and `=NONE` are not built anywhere | `ci.yml` | Two matrix entries; also settles AR-05 |
| [TS-09](#ts-09--the-isr-half-of-the-context-rule-is-reachable-on-the-host-after-all) | ✅ P3 | Opportunity | A settable host `__get_IPSR()` makes the ISR half of the context rule testable | `test/host/cmsis_compiler.h` | Pins the *wrapper's* rule, not the silicon's — document the narrower claim. Contradicts a "cannot" in two docs |
| [TS-10](#ts-10--the-2-byte-alignment-contract-has-no-static-assert) | ✅ P3 | Test gap | No `_Static_assert` on `NX_PHYSICAL_HEADER >= 16` | `eth_drv_netxduo.c:148-167` | A NetX Duo bump would silently reintroduce "every IPv4 checksum fails while ARP works" |
| [CX-01](#cx-01--rxring--txring-as-typed-c-over-an-injected-descriptor-view) | Opp (H) | C++ | Extract the ring state machines; retires the `MAP_32BIT` arena, `-no-pie` and two harness tests | `mac_drv.c` → C++ | Enables ASan/UBSan. Breaks the "`mac_drv.c` unchanged" property — a real trade |
| [CX-02](#cx-02--bytefifo-with-an-injected-critical-section-policy) | Opp (H) | C++ | `ByteFifo<Crit>` with an injectable critical section | `blocking_fifo.c` → C++ | An instrumented `Crit` makes the F67/N7 mid-push interleaving an actual test. Pairs with TS-01 |
| [CX-03](#cx-03--tokenizer-and-commandtable) | Opp (M) | C++ | `Tokenizer` over `string_view` + `CommandTable` | `cli.c` → C++ | Makes the F74/N8 shared-state defect unrepresentable rather than fixed. Pairs with TS-02 |
| [CX-04](#cx-04--nsdlifecycle-over-an-injected-stack-interface) | ✅ Opp (M) | C++ | `NsdLifecycle` over a five-call stack interface | `nsd_netxduo.c` → C++ | Makes NX-02 a red test before a fix. Cold path, so dispatch cost is irrelevant |
| [CX-05](#cx-05--ethernetframer) | ✅ Opp (M) | C++ | `EthernetFramer`: ethertype selection, header write, chain flatten | `eth_drv_netxduo.c` → C++ | Twelve hand-written byte shifts become one expected-array test. Pairs with TS-04 |
| [CX-06](#cx-06--linkstate-as-a-pure-transition-function) | ✅ Opp (L) | C++ | `LinkDecision next(prev, phy_status)` | `eth_drv_netxduo.c:220-269` | Pins the first-poll `DOWN` suppression rule |
| [CX-07](#cx-07--make-the-c-rules-enforced-rather-than-documented) | ✅ Opp (L) | C++ | Assert in CI that the image has no `operator new` / `__cxa_throw` | `cpp_support.cpp:22-30` | Makes the no-allocation rule observable, as `cpp_ctor` does for static init. Cannot assert "no `malloc`" — see NX-03 |
| [DC-01](#dc-01--the-interrupt-priority-table-is-wrong-in-two-rows) | ✅ P2 | Doc | Interrupt table: SVC and SysTick priorities both wrong | `architecture.md:352,354` | Consequence of NX-04 |
| [DC-02](#dc-02--release-drops-two-checks-the-architecture-document-presents-as-unconditional) | ✅ P2 | Doc | `TX_ENABLE_STACK_CHECKING` presented as unconditional; it is Debug-only | `architecture.md:369-372` | Should also state `NX_DISABLE_ERROR_CHECKING` in Release |
| [DC-03](#dc-03--ethernet_netxduoc-still-describes-the-s4-stub-driver-and-contradicts-itself-55-lines-later) | ✅ P3 | Doc | File header describes a stub driver and a `nx_driver_stub()` that does not exist | `ethernet_netxduo.c:8-9` | Contradicted by line 63 of the same file |
| [DC-04](#dc-04--mainc-says-the-ethernet-driver-is-still-a-stub) | ✅ P3 | Doc | "The Ethernet driver … is still a stub until S5a" | `main.c:187-190` | In the file most likely to be read first |
| [DC-05](#dc-05--the-drivers-default-case-describes-features-it-now-handles-explicitly) | ✅ P3 | Doc | `default:` comment says multicast/deferred "arrive in S5c"; both have `case` labels above | `eth_drv_netxduo.c:667-671` | |
| [DC-06](#dc-06--three-mac_drvc-comments-are-written-in-terms-of-a-deleted-stack) | ✅ P2 | Doc | Three comments argue in lwIP terms: `pbuf->time_s`, `lwipopts.h`'s `tx_cb`, and the whole bounded-wait rationale | `mac_drv.c:459,586,723-728` | The 723-728 hazard is still real under NetX Duo (`nx_ip_protection` held across the driver call) — rewrite, do not delete |
| [DC-07](#dc-07--three-references-to-a-cli-command-that-does-not-exist) | ✅ P3 | Doc | `ptp diag ts` does not exist; the counters are printed by `ethinfo` | `ptp_tx_timestamp.{c:54,h:17}`, `threadx_porting_plan.md:176` | The porting-plan instance describes a HIL dependency |
| [DC-08](#dc-08--flexptp_optionsh-describes-the-transmit-timestamp-deferral-as-future-work) | ✅ P3 | Doc | "S6 must defer the callback…" — S6 shipped and the deferral exists | `flexptp_options.h:42-44` | Keep the warning, point it at `ptp_tx_timestamp.c`. Also: stale include-guard name |
| [DC-09](#dc-09--the-igmp-comment-keeps-a-superseded-first-paragraph) | ✅ P3 | Doc | Superseded first paragraph precedes its own correction | `nsd_netxduo.c:328-336` | |
| [DC-10](#dc-10--readmemd-overstates-the-eth_stack-switch) | ✅ P3 | Doc | README/design_doc disagree on the EtherLib switch | `README.md:263-271` | Same as AR-05 |
| [DS-01](#ds-01--port_reviewmds-table-of-contents-omits-its-largest-section) | ✅ P2 | Self-consistency | `port_review.md` TOC omits "Second-Review Findings" (F67–F80, 620 lines) | `port_review.md:22-34` | Includes F80, the finding that required patch 0004 |
| [DS-02](#ds-02--the-disposition-summary-claims-completeness-it-does-not-have) | ✅ P2 | Self-consistency | Disposition Summary stops at F66 yet says "Every finding is tracked"; both phase tables likewise | `port_review.md:78-95,119-195` | 14 findings with no disposition row and no phase attribution |
| [DS-03](#ds-03--f80-is-attributed-to-the-second-review-pass-which-did-not-find-it) | ✅ P3 | Self-consistency | F80 filed under "raised by the second review pass"; it came from the S6 bench session | `port_review.md:1656-1660` | Visible symptom: it is the only entry with no `[Nnn]` bracket |
| [DS-04](#ds-04--two-options-is-followed-by-three) | ✅ P3 | Self-consistency | "## Two options" followed by A, B, C and "How the three compose" | `threadx_s5e_plan.md:80` | Status banner is correct; only the heading lagged |
| [DS-05](#ds-05--the-host-suites-runtime-is-quoted-as-three-different-numbers) | ✅ P3 | Self-consistency | Suite runtime quoted as 0.04 s, 0.06 s and 0.2 s; measured 0.16 s | four documents | The 0.04 s figure underpins the "250× headroom" arithmetic for `TIMEOUT 10` (really ~60×) |
| [DS-06](#ds-06--the-review-archives-readme-stops-at-f7x) | ✅ P3 | Self-consistency | "every finding (F1–F7x)"; the log reaches F80 | `docs/reviews/README.md:8` | |
| [GI-01](#gi-01--let-the-hil-runner-prove-which-firmware-it-is-testing) | ✅ P2 | Improvement | HIL cannot prove which image it tested; the control is a README warning | `tools/hil/run.py` | Print a build identity, compare against the ELF, fail on mismatch — the same transformation `build.sh` applied to the patch hazard |
| [GI-02](#gi-02--give-ethinfo-and-osinfo-a-machine-readable-mode) | ✅ P3 | Improvement | HIL regex-parses human prose; a `MSG()` edit can silently disable a check | `checks.py`, `eth_drv_report()` | `checks.py:256` already records one silent miscount |
| [GI-03](#gi-03--add-static-analysis-to-ci) | ✅ P3 | Improvement | No static analysis; `compile_commands.json` already available | `ci.yml` | Would likely have caught NX-05 and NX-08 |
| [GI-04](#gi-04--two-pyc-files-are-committed) | ✅ P3 | Hygiene | Two `__pycache__/*.pyc` tracked; `.gitignore` lacks the pattern | `tools/hil/__pycache__/` | Also missing a trailing newline |
| [GI-05](#gi-05--the-release-optimisation-level-analysis-deserves-a-re-run) | ✅ P3 | Stale data | The `-Og`/`-O2`/`-Os` figures predate the port (167 KB quoted; image is now 206 344 B) | `CMakeLists.txt:150-180` | Conclusion likely unchanged; the numbers describe a different firmware |
| [GI-06](#gi-06--consider-persisting-ptp-settings) | ✅ P3 | Decision | No settings store; profiles are lost on reset | — | Probably correct for a demo. Record it as a decision with a sketch of where it would go |

**Totals:** 56 findings — **6 × P1** (3 defects: NX-01/02/03; 3 test gaps: TS-01/02/03), 13 × P2,
28 × P3, 9 opportunities.

---

## Suggested sequencing

If only one thing is done: **NX-03**, because it is a one-line change that converts a silent
memory-corruption path into `ENOMEM` and makes an existing written claim true.

If a day is available, in this order:

1. **TS-03 then NX-01** — model first, so the fix is mutation-verified like the rest of the suite.
2. **NX-02** — make `ptp_nsd_init()` transactional.
3. **NX-03** — the `_sbrk` ceiling.
4. **TS-06** — loop CI's patch check over `build.sh`'s list; the silent patch is currently unguarded.
5. **DC-01 / NX-04** — decide the SysTick priority, then make code, comment and table agree.
6. **TS-01 and TS-02** — the two untested files with finding histories.
7. **DS-01 / DS-02** — a fourth phase row and 14 disposition rows in `port_review.md`.

The C++ extractions (CX-01…CX-07) are a separate piece of work and should be pulled by a need —
CX-02 by TS-01, CX-05 by TS-04, CX-04 by NX-02 — rather than pushed as a refactor. That is the
same argument `threadx_s5e_plan.md` makes about Option A, and it applies here for the same reason.
