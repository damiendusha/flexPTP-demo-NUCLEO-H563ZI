# Consolidated Review — flexPTP-demo-NUCLEO-H563ZI

A single reconciled action register merging every finding from the three independent reviews
of the port at `df3ee3d`.

**Sources**

| Review | Document | Findings | Action IDs |
|---|---|---|---|
| Opus 5.0 | [`final_review_opus_5p0.md`](final_review_opus_5p0.md) | 68 (A1–A8, B1–B22, D1–D12, U1–U7, S1–S19) | `P1.1`–`P5.18` (56) |
| Gemini 3.6 Flash | [`final_review_gemini_flash_3p6.md`](final_review_gemini_flash_3p6.md) | 36 (A1–A5, D1–D8, B1–B13, U1–U4, I1–I6) | `ACT-01`–`ACT-27` |
| Gemini 3.1 Pro | [`final_review_gemini_pro_3p1.md`](final_review_gemini_pro_3p1.md) | 11 (Findings 1–11) | 6 unnumbered items |

**Adjudication.** Where the reviews disagree, the resolution below is the one recorded in
[`final_review_gemini_flash_3p6_review_opus_5p0.md`](final_review_gemini_flash_3p6_review_opus_5p0.md)
and [`final_review_gemini_pro_3p1_review_opus_5p0.md`](final_review_gemini_pro_3p1_review_opus_5p0.md),
each of which verified the disputed claims against the tree, the linker map, the CMSIS-RTOS2
shim and ST's HAL. Every disputed row is marked and explained in
[Disputes](#disputes--where-consensus-was-not-reached).

---

## Table of Contents

- [Legend](#legend)
- [Reconciliation summary](#reconciliation-summary)
- [Action register](#action-register)
- [Disputes — where consensus was not reached](#disputes--where-consensus-was-not-reached)
- [Coverage analysis](#coverage-analysis)
- [Recommended execution order](#recommended-execution-order)

---

## Legend

**Status**

| Symbol | Meaning |
|---|---|
| ☐ | Open — not started |
| ◐ | Partial — some of the action already exists in the tree |
| ✅ | Done — already complete; no work remains |
| ⊘ | Closed without action — rejected or deferred, reason given |

**Flash 3.6 Verified** records the independent verification performed by Gemini Flash 3.6 against the tree, build system, and linker map:
- `Verified ✅`: Code fix independently inspected, verified in source/docs, and tested on build tree.
- `Partial ◐`: Partially implemented in codebase; remainder open.
- `—`: Action remains open in code tree.
- `Closed ⊘`: Closed without code change (disputed, deferred, or superseded).

**Pro 3.1 Verified** records the independent verification status by Gemini Pro 3.1 (currently pending / unverified: `—`).

**Opus 5.0 ID** is the canonical identifier for the action, using the numbering scheme from
[`final_review_opus_5p0.md`](final_review_opus_5p0.md). Its 56 original IDs (`P1.1`–`P5.18`)
are retained unchanged; 18 new numbers were allocated in the same namespace for actions
that originated in the other two reviews, and `X`-prefixed IDs mark items closed without
action. A row with an `ACT-` or Pro entry but a newly minted `P`-number is therefore a
finding Opus 5.0 did **not** make — the ID is a register key, not a claim of authorship.

**Consensus**

| Value | Meaning |
|---|---|
| `3/3` | All three reviews raised it |
| `2/3 O+F` | Opus 5.0 + Flash 3.6 |
| `2/3 O+P` | Opus 5.0 + Pro 3.1 |
| `1/3 O` / `1/3 F` / `1/3 P` | Single-review finding, uncontested |
| **`⚠ DISPUTED`** | Reviews disagree on whether to act at all |
| **`⚠ SCOPE`** | Agreement on the action, disagreement on its extent |
| **`⚠ SEVERITY`** | Agreement on the action, disagreement on its priority |
| **`⚠ METHOD`** | Agreement on the goal, disagreement on the remedy |

A single-review entry is *not* a dispute — the other reviews did not examine that area. Only
`⚠` rows represent actual disagreement.

---

## Reconciliation summary

| | Count |
|---|---|
| Total distinct actions | **81** |
| Open (☐) | 5 |
| Partial (◐) | 0 |
| Already done (✅) | 72 |
| Closed without action (⊘) | 4 |
| Raised by all three reviews | 2 |
| Raised by two reviews | 12 (6 clean, 6 with a contested boundary) |
| Single-review, uncontested | 60 (Opus 45, Flash 11, Pro 4) |
| **Rows in dispute (⚠)** | **11** — 5 outright, 6 contested boundaries |
| Found during remediation (not from any review) | 1 |

**Overlap is low.** Of 80 actions, only 14 were raised by more than one review — the three
reports are far more complementary than redundant. Flash 3.6 dominates low-level driver
defects (5 bugs no other review found); Opus 5.0 dominates documentation fidelity (12 of 14
documentation actions); Pro 3.1 contributes 2 novel upstream-facing actions and no new
defects.

---

## Action register

### Priority 1 — correctness and reproducibility

| Status | Flash 3.6 Verified | Pro 3.1 Verified | Opus 5.0 ID | Flash 3.6 ID | Pro 3.1 ID | Consensus | Description |
|---|---|---|---|---|---|---|---|
| ✅ | **Verified ✅** | — | **P1.1** | — | — | 1/3 O | Make `build.sh` verify and apply `patches/embfmt/` and `patches/flexptp/` after the submodule update, failing loudly if neither apply nor reverse-apply succeeds. A fresh clone currently builds **without** the F59 servo fix and the F60/F66 formatting fixes — **DONE:** `build.sh` gained `apply_submodule_patches()`; all three paths tested on the tree (already-applied → skip, pristine → applies byte-identically, diverged → error + exit 1) |
| ✅ | **Verified ✅** | — | **P1.2** | — | — | 1/3 O | Document the patch-apply prerequisite in `README.md` under "Get the sources" — **DONE:** the clone recipe now includes both `git apply` commands, with an `IMPORTANT` callout tabulating what each missing patch silently breaks (F59 / F60 / F66); the "Software structure" paragraph cross-references it |
| ✅ | **Verified ✅** | — | **P1.3** | ACT-10 (B9) | — | 2/3 O+F | Add a NULL guard to `phy_get_link_status()`, `phy_common.c:515` — unrecognised PHY currently HardFaults within 500 ms — **DONE:** guarded; `linkStatus` keeps its zero-initialised "down" value, the correct answer when no PHY could be identified. Fault reproduced on target with a forced-unknown OUI: `CFSR` = `0x00000100` (IBUSERR), `HFSR` = `0x40000000` (FORCED) |
| ✅ | **Verified ✅** | — | **P1.4** | — | — | 1/3 O | Make `phy_sweep_addresses()` honour `ETHHW_ReadPHYRegister()`'s return value; a dead MDIO bus currently "detects" a PHY at address 0. Precondition for both P1.3 and P2.12 — **DONE:** failed reads now skip the address. Verified on target with a simulated dead bus: fixed → *"No Ethernet PHY detected!"*; unfixed → falsely reports `PHY: UNKOWN, OUI: 0, MODEL: 0 @0` |
| ✅ | **Verified ✅** | — | **P1.5** | — | — | 1/3 O | Release `modifyMtx` on the early return in `cli_remove_command()`, `cli.c:521` — a bad index permanently wedges the command database — **DONE:** range check moved inside a single-exit body that always releases; done before P1.10 so that restructure had a sound function to build on |
| ✅ | **Verified ✅** | — | **P1.6** | — | — | 1/3 O | Reset `pHint = NULL` when shifting entries in `cli_remove_command()`, so `tidy_format_commands()` re-derives `pHint`/`pPadding` instead of writing through pointers into the next slot's buffer. **Must land with P1.10** — **DONE**, but *not* by clearing `pHint`: `tidy_format_commands()` overwrites the `\t` delimiter with `\0` when it first splits an entry, so it cannot re-derive one it has already formatted and would leave `pPadding` NULL for the padding loop to dereference. The pointers are **rebased by offset** onto the destination instead. Corruption reproduced on target before the fix |
| ✅ | **Verified ✅** | — | **P1.7** | ACT-08 (B7, I5) | — | 2/3 O+F | Serialise `MSG()` — mutex or critical section around format + push. Single 2049-byte static shared by six threads — **DONE:** `MSG_init()` creates the lock before the scheduler starts (lazy creation would itself race, since `cli_init()` starts a printing task before the banner); held across format → CR insertion → push, and skipped rather than blocked from an ISR or pre-scheduler. Lost update demonstrated on target: **0/240 vs 120/120** |
| ✅ | **Verified ✅** | — | **P1.8** | ACT-13 (D1) | — | **⚠ SCOPE** | Correct every stale "HCLK is the PTP clock" claim: `main.c:83-90`, `design_doc.md:172,196`, `implementation_plan.md:53`, `port_review.md:1548` — **DONE:** all five corrected; a tree-wide sweep leaves only historical correction notes and statements that are true of the *H743*. Validated against hardware afterwards: `ptp addend` reports **3435973836** (`0xCCCCCCCC`), the `pll1_q_ck` / 5 ns derivation. The wider scope (Flash 3.6 flagged only `main.c`) is the resolution of the ⚠ |
| ✅ | **Verified ✅** | — | **P1.9** | — | — | 1/3 O | Fix `design_doc.md:131-132`'s clock tree to `RCC_SYSCLK_DIV1` / `RCC_HCLK_DIV2` — the current text reproduces the F45 silent-miscompile trap — **DONE:** diagram redrawn with the H5 spellings, the PLL1Q → `clk_ptp_ref_i` branch shown explicitly (it was missing entirely, and HCLK was annotated "also ptp_ker_ck"), and a WARNING callout naming F45. Diagram now matches `main.c` field for field |
| ✅ | **Verified ✅** | — | **P1.10** | ACT-01 (B1) | — | 1/3 F | Fix the CLI mutex self-deadlock: `cli_register_command()` holds a non-recursive `modifyMtx` (`cli.c:461`) and calls `cli_remove_command()`, which re-acquires it (`cli.c:519`). Prefer an unlocked private helper over a recursive mutex. **Must land with P1.6** — **DONE:** added `cli_remove_command_locked()`, called from the duplicate path; the public wrapper is now acquire → helper → release. Deadlock reproduced on target before the fix (registration never returned, CLI task wedged for good) |

### Priority 2 — robustness

| Status | Flash 3.6 Verified | Pro 3.1 Verified | Opus 5.0 ID | Flash 3.6 ID | Pro 3.1 ID | Consensus | Description |
|---|---|---|---|---|---|---|---|
| ✅ | **Verified ✅** | — | **P2.1** | — | — | 1/3 O | Implement `vApplicationStackOverflowHook()` and `vApplicationMallocFailedHook()` with a polled-UART panic reporter. Both currently resolve to the shim's weak stubs, which call `configASSERT(0)` — **DONE:** added `serial_io_panic_write()` (polled USART3, no FIFO/DMA/RTOS, clears `CR3.DMAT` first) and both hooks. Heap exhaustion reproduced and reported on target. **Testing revealed the stack hook is largely unreachable on ARMv8-M** — the port programs `PSPLIM` per task, so hardware raises STKOF first; see **P2.14** |
| ✅ | **Verified ✅** | — | **P2.2** | — | — | 1/3 O | Make `configASSERT` report before halting — it is currently `taskDISABLE_INTERRUPTS(); for(;;);` — **DONE:** routed through `vAssertCalled(file, line)` on the P2.1 polled writer. Verified on target by tripping a real FreeRTOS assert, which reported `tasks.c:2820` (`uxNewPriority < configMAX_PRIORITIES`). Costs ~2.6 KB of flash in `__FILE__` strings (10.63% → 10.76% of 2 MB) |
| ✅ | — | — | **P2.3** | ACT-03 (B2) | — | 2/3 O+F | Bound the ownership wait in `ETHHW_Transmit()`, `mac_drv.c:552`. Spins on `tcpip_thread` **while holding the lwIP core lock**; no watchdog exists to recover — **DONE: bounded with the driver's existing spin-count idiom; on expiry it reports the descriptor index and drops the frame. Verified on target with ownership scaffolded to never clear — 4 drops reported, CLI still responsive, no deadlock. Healthy hardware: 0 drops, ping 0% loss at 56 and 1400 bytes** |
| ✅ | — | — | **P2.4** | — | — | 1/3 O | Check `HAL_RCC_OscConfig()` / `HAL_RCC_ClockConfig()` return values; define `Error_Handler()`, which is declared at `main.c:31` and defined nowhere — **DONE: both HAL_RCC_* returns checked; `Error_Handler()` defined — attempts the polled writer, then halts blinking PG4 (LD3). Verified on target by forcing the failure: console silent (it runs before `serial_io_init()`, a limitation now documented in the code) and GPIOG ODR bit 4 toggling, 5 of 8 samples high** |
| ✅ | — | — | **P2.5** | ACT-05 (B5) | — | **⚠ SCOPE** | Fix `get_line()`'s escape parser: bound `paramIdx` against `sizeof(escParam)` **and** move the `escParam[0]` read after the write (arrow-key history currently acts on the previous keypress) — **DONE, both halves — resolving the ⚠ SCOPE (Flash 3.6 flagged only the overflow). The store now precedes the dispatch, and `escParam` is bounds-checked and zero-initialised. Control run on target: **unfixed, the first arrow press did not recall the previous command; fixed, it recalls immediately.** The overflow itself produced no observable crash in testing — it lands in adjacent stack slots — so it is fixed as inspection-level UB, not as a reproduced fault** |
| ✅ | — | — | **P2.6** | ACT-06 (B6) | — | 2/3 O+F | Fix the off-by-one write in `insert_carriage_return()` — change the guard to `len < maxLen` — **DONE: guard changed to `len < maxLen`, leaving room for the character the body is about to add. Proved on the host by compiling both variants against a full-length buffer ending in a bare newline: unfixed writes index 2049 of a 0..2048 buffer, fixed stays in bounds. On target the CRLF conversion is unchanged — 21 of 21 line feeds are CRLF pairs, zero bare LF** |
| ✅ | — | — | **P2.7** | — | — | 1/3 O | Clamp `cli_register_command()`'s token count against `MAX_TOKEN_N` (8), not `TOK_ARR_LEN` (16) — **DONE: clamp now uses `MAX_TOKEN_N` (8), the destination array's own capacity, instead of `TOK_ARR_LEN` (16) which sizes a different buffer in `process_cli_line()`. Verified on target with a scaffolded 12-token registration — survives, CLI alive, help table intact** |
| ✅ | — | — | **P2.8** | — | — | 1/3 O | Raise the `rx` thread above the console threads; six of nine threads currently share priority 24 — **DONE: `rx` raised to `osPriorityAboveNormal` (32) — above both console producers, below `tcpip_thread` (40). **A/B on target showed no measurable benefit**: 400 pings at 1400 B under continuous CLI load gave 0% loss and ~0.40 ms mean RTT at both priorities, because a 115200-baud console cannot generate enough load to starve `rx`. Kept as correct-by-construction, mirroring the deliberate ETH(7)-over-console(12) interrupt ordering, not as a demonstrated optimisation** |
| ✅ | — | — | **P2.9** | — | — | 1/3 O | Check `HAL_UART_Transmit_DMA()`'s return in `serial_io_thread()` — on error it blocks forever on a completion flag that will never be set — **DONE: the completion wait is now conditional on the transfer having started, with a short delay on failure instead of an unconditional `osWaitForever`. Control on target with 3 forced DMA failures: **unfixed → 0 console bytes, every marker missed** (console dead for the session); **fixed → 1213 bytes, all markers after the lost chunks pass, `osinfo` responds**** |
| ✅ | — | — | **P2.10** | ACT-02 (B3) | — | 1/3 F | Replace `SET_BIT` with direct assignment when clearing `ETH->DMACSR` in `ETHHW_ISR()`. Read-modify-write on a w1c register clears every pending flag, including `AIS`/`FBE`, which nothing reads. ST's own `__HAL_ETH_DMA_CLEAR_IT` uses direct assignment — **DONE: all four acknowledgements are now `WRITE_REG` of just the bits being cleared, and `AIS` is acknowledged and reported instead of being wiped unseen. **Not demonstrable from software** — the race needs a flag to assert between the ISR's read and its write-back — so the evidence is correctness by inspection, corroborated by ST's own `__HAL_ETH_DMA_CLEAR_IT` using direct assignment, plus a regression: 500 pings at 1400 B, 0% loss, PTP addend correct** |
| ✅ | — | — | **P2.11** | ACT-04 (B4) | — | 1/3 F | Fix the transmit-sequence delta in `ETHHW_ProcessTx()`, `mac_drv.c:473`, to `int16_t delta = (int16_t)(txCntr - state->txCntAcked);`. `txCntr` increments on *every* transmit, so the 16-bit wrap arrives in minutes on a busy link — **DONE: `int16_t delta = (int16_t)(txCntr - txCntAcked)`, with `minDelta` narrowed to match so the comparison stays modular. Verified on target with the counter scaffolded to start at 65500: 600 pings across the wrap at 0% loss, then 300 more at 1400 B at 0% loss. The timestamp *ordering* effect is not directly observable without a PTP master on this bench** |
| ✅ | — | — | **P2.12** | ACT-09 (B8) | — | 1/3 F | Bound the `phy_soft_reset()` handshake, `phy_common.c:441-448` — a seventh unbounded wait that F54 missed. Also initialise `r` before the loop — **DONE: bounded at 100 x 5 ms with `r` initialised and a failed MDIO read treated as still-in-reset rather than exiting on stale data; reports on expiry. Verified on target with a PHY scaffolded to never clear BCR.RESET: *"soft reset did not complete after 500 ms"* and boot continued** |
| ✅ | — | — | **P2.13** | ACT-11 (B11) | — | **⚠ METHOD** | Add a length guard in `low_level_output()` before concatenating into `concat_buf[1528]`. Drop the frame and return `ERR_OK`/`ERR_IF`, **not** `ERR_MEM` — **DONE: guard added; drops and reports, returning `ERR_IF` rather than `ERR_MEM` — resolving the ⚠ METHOD, since the function's own doc comment carries lwIP's warning that ERR_MEM from linkoutput *"can lead to strange results"*. Verified on target with the threshold scaffolded to 500 B: small pings pass, 1200 B pings 100% dropped with *"TX frame of 1242 bytes exceeds ...; dropped"*. At the real threshold, full-MTU traffic is unaffected** |
| ✅ | **Verified ✅** | — | **P2.14** | — | — | *new, found while testing P2.1* | **DONE:** naked `HardFault`/`MemManage`/`BusFault`/`UsageFault` handlers recover the exception frame and report `CFSR`, `HFSR`, `PC`, `LR` plus a decode of STKOF / IBUSERR / BFARVALID / MMARVALID. Both paths verified on target: stack overflow → `0x00100000` STKOF; NULL call → `0x00000100` IBUSERR with `PC = 0x00000000`. Original finding: report faults instead of looping silently in `Default_Handler`. Measured on target: a task stack overflow raises **`CFSR` = `0x00100000` (UFSR.STKOF)** — the ARMv8-M hardware stack-limit fault — because the CM33 port programs `PSPLIM` on every context switch. UsageFault is not enabled in `SHCSR`, so it escalates to HardFault → `Default_Handler` → silent infinite loop. **`vApplicationStackOverflowHook()` therefore never runs for a real overflow**, which makes P2.1 alone insufficient for its own stated purpose |

### Priority 3 — verification and structure

| Status | Flash 3.6 Verified | Pro 3.1 Verified | Opus 5.0 ID | Flash 3.6 ID | Pro 3.1 ID | Consensus | Description |
|---|---|---|---|---|---|---|---|
| ✅ | — | — | **P3.1** | — | — | 1/3 O | Verify `MACPPSCR.PPSCTRL` encoding against RM0481; correct `ETHHW_PPS_FreqEnum` if the off-by-one is confirmed. `ETHHW_PTP_PPS_OFF = 0` cannot be right — simple mode has no "off" code. **Blocks P5.27** — **DONE — **the suspected off-by-one does not exist.** The same PG8 measurement exercises the enum end to end: every requested frequency appears on the pin, and code 0 genuinely disables the output rather than emitting 1 Hz. `ETHHW_PPS_FreqEnum` is correct as written and no change was made. Review finding B6 is retracted in `final_review_opus_5p0.md`, along with the two cross-review passages that repeated it** |
| ✅ | — | — | **P3.2** | ACT-26 (U4) | item 4 | **3/3** | Run the LED-on-PG8 test to distinguish "configured" from "working", and 1 Hz from 2 Hz. The only PTP hardware path with no waveform evidence — **DONE — and it did not need an oscilloscope. PG8 sampled over SWD at ~1 kHz: `ptp pps 1` → **1.000 Hz** (8 periods within 1 ms of nominal, ~47% duty), `ptp pps 2` → 2.001 Hz, `ptp pps 4` → 3.983 Hz, `ptp pps 0` → no transitions. The pulse exists and its frequency is correct. **Jitter** remains unmeasured and does need an instrument** |
| ✅ | — | — | **P3.3** | — | — | 1/3 O | Zero `evt.data.rx` at the top of the `ETHHW_ProcessRx()` loop — `ts_s`/`ts_ns` are read unconditionally by `rx_thread` but only written when a timestamp was captured — **DONE: `memset(&evt, 0, sizeof(evt))` at the top of the dispatch loop. `ts_s`/`ts_ns` are assigned only when a context descriptor carried a timestamp but are copied into the pbuf unconditionally, so an untimestamped frame previously handed flexPTP whatever was on the stack. Verified: PTP starts, addend correct, 200 pings at 1400 B 0% loss** |
| ✅ | — | — | **P3.4** | — | — | 1/3 O | Add a `_Static_assert` on `offsetof` for the `ETHHW_State` / RX-ring adjacency that `ETHHW_GetState()` depends on — **DONE: layout named as `ETHStateAndDescLayout` and asserted with `_Static_assert(offsetof(..., DMARxDscrTab) == sizeof(ETHHW_State))`. Proved the assertion bites by inserting a `uint32_t` between the members — build fails with the explanatory message rather than producing a driver that reads its state from inside a descriptor** |
| ✅ | — | — | **P3.5** | — | — | 1/3 O | Reconcile `RX_MSG_Q_LEN` (32), `PBUF_POOL_SIZE` (8) and `ETH_RX_DESC_CNT` (24); record the reasoning — **DONE: `RX_MSG_Q_LEN` tied to `ETH_RX_DESC_CNT` (one event per descriptor is the hard ceiling — the old 32 was unreachable in its top third). `PBUF_POOL_SIZE` deliberately left at 8: it is the real throughput limit, and 300 pings of 1400 B at 100 / 250 / 500 pps all gave 0% loss. Relationship and the measurement recorded in `architecture.md`** |
| ✅ | — | — | **P3.6** | — | — | 1/3 O | Add `LOCK_TCPIP_CORE()` around the lwIP accesses in `check_dhcp_state()`, which runs on `Tmr Svc` — **DONE: `LOCK_TCPIP_CORE()` around both lwIP reads, with the address copied out and `MSG()` moved outside the lock (it can block on the console FIFO, and stalling the core lock there would stall the whole stack). Verified: DHCP lease still detected and printed exactly once** |
| ✅ | — | — | **P3.7** | — | — | 1/3 O | Record which PTP message types `SNAPTYPSEL = 0b01` selects, and confirm it suits both e2e and p2p — **DONE: documented at the register write. `SNAPTYPSEL = 0b01` with `TSEVNTENA = 0` / `TSMSTRENA = 0` is the widest selection the MAC offers — every PTP message type including the peer-delay ones — which is **required**, since the delay mechanism is runtime-switchable and the two shipped profiles use different ones (default = E2E, gPTP = P2P). Confirmed on target that both are selectable: `ptp delmech` reports P2P by default and accepts `e2e`/`p2p`. `TSENALL` deliberately unset. The exact RM0481 message-type table is not restated — no copy to cite** |
| ✅ | — | — | **P3.8** | ACT-07 (B10, I3) | — | **⚠ SEVERITY** | Reorder `stm32h563zitx_FLASH.ld` to place `.FreeRTOS_sec` before `._user_heap_stack`. Verified: `ucHeap` at `0x200190d0` sits inside the `_sbrk` range (`_end` `0x200186d0` → ceiling `0x2003F800`) — **DONE: `.FreeRTOS_sec` moved before `._user_heap_stack`. Verified from the map: `ucHeap` now at `0x200186cc`, `_end` at `0x2001fecc` — the 30 KB FreeRTOS heap sits entirely **below** where `_sbrk` starts, instead of 2.5 KB inside its range. Severity dispute resolved as ranked (P3, not Flash 3.6's P1): the fix is free, but reaching the bug needed ~2.5 KB of newlib `malloc` in a project that avoids newlib's allocator** |
| ☐ | — | — | **P3.9** | — | item 4 | 1/3 P | Characterise against a PHC-capable grandmaster (Intel I210 / I219-V / 82576) to establish whether `LOCKED` (100 ns) is reachable. Nothing on the target changes |
| ✅ | — | — | **P3.10** | — | — | *new, found while testing P3.8* | CMake does not declare the linker script as a link dependency, so editing `stm32h563zitx_FLASH.ld` does not trigger a relink: an incremental build silently keeps the previous memory layout. Found the hard way — P3.8's section reorder appeared to have no effect until the ELF was deleted by hand. Any future linker-script change is unverifiable until this is fixed. — **DONE: `LINK_DEPENDS` added on the target. Proved by editing `_Min_Heap_Size` in the linker script and rebuilding **without** deleting anything — the ELF hash changes, so the relink now fires. Before the fix it did not, which is why P3.8's reorder appeared to have no effect** |

### Priority 4 — documentation

| Status | Flash 3.6 Verified | Pro 3.1 Verified | Opus 5.0 ID | Flash 3.6 ID | Pro 3.1 ID | Consensus | Description |
|---|---|---|---|---|---|---|---|
| ✅ | — | — | **P4.1** | — | — | 1/3 O | Remove the "F42 remains broken" paragraph from `architecture.md:375` — resolved; nothing in the port was at fault — **DONE: the paragraph now states the path is fully verified and records F42's actual resolution — the site switch was discarding Delay_Resp and nothing in the port was at fault. The section preamble no longer implies F42 is live** |
| ✅ | — | — | **P4.2** | — | — | 1/3 O | Update `architecture.md:239` — the addend wait has been bounded since F54 — **DONE: the note now records that every handshake is bounded and reports (F54), including the two the original sweep missed — the TX descriptor wait (P2.3) and the PHY reset (P2.12). The neighbouring `tcpip_thread` bullet also updated for `rx` at 32** |
| ✅ | — | — | **P4.3** | — | — | 1/3 O | Remove the "MDIO shift bug must be fixed" note from `ptp_clock.md:252` — fixed at Stage 12 (F8) — **DONE: the "must be fixed as part of the MAC driver port" warning is replaced by a note recording that F8 was fixed at Stage 12, with the current code shown and the hardware evidence (PHY enumerates, MDC at 2.45 MHz)** |
| ✅ | — | — | **P4.4** | — | — | 1/3 O | Retitle the 4 ns section to "not adopted"; update `README.md:268` and `port_review.md`'s F5 row — **DONE: section retitled *"Why a 4 ns increment is not adopted"* (heading, TOC and inbound link), README's documentation table reworded, and `port_review.md`'s F5 given a correction block matching F3's — the unreachability claim followed from the same HCLK error, and `PLLQ = 1` would give 500 MHz and K = 2.0** |
| ✅ | — | — | **P4.5** | — | — | 1/3 O | Replace `port_review.md:58`'s *"nothing here has executed on a device"* with a current-status block — **DONE: the *"no hardware, nothing here has executed on a device"* paragraph is replaced by a Phase 3 block that tabulates all three evidence types and tells the reader to check which phase a finding came from. Kept as a correction rather than deleted, since the reasoned-vs-measured split is what makes the document useful** |
| ✅ | — | — | **P4.6** | — | — | 1/3 O | Extend `port_review.md`'s "Findings by phase" table to F66 and add the hardware bring-up phase — **DONE: "Findings by phase" extended with the hardware bring-up row (F51–F66, 16 findings) plus a note on what that phase changed — two defects invisible to earlier phases, two corrected findings (F3, F5), F42 resolved off-board, and the only two retractions in the project** |
| ✅ | — | — | **P4.7** | — | — | 1/3 O | Mark F42's retained `### Status — F42 is open` section as superseded history — **DONE: the retained `### Status` subsection is retitled *SUPERSEDED (retained as history)* with a note pointing at the resolution, and the *"F42 is open"* sentence struck through. The investigation narrative is kept — it is the most instructive part of the finding** |
| ✅ | — | — | **P4.8** | — | — | 1/3 O | Settle the embfmt fix count between `README.md:209` ("two") and `architecture.md:52` ("three") — four defects, two findings, one patch — **DONE: settled as **four defects, two findings, one patch file** in both places, with each defect named. README said "two", `architecture.md` said "three" — the disagreement came from conflating the three different counts, so all three are now stated explicitly** |
| ✅ | — | — | **P4.9** | — | — | 1/3 O | Clear the "Open: F54" heading in `design_doc.md:851` — **DONE: the stage-log **Open:** heading is now *"Open at the time of this stage — since resolved"* and the F54 bullet marked resolved, so the stage log stays a historical record without asserting a live defect** |
| ✅ | — | — | **P4.10** | — | — | 1/3 O | Fix `mac_drv.h`'s two stale comments — the `TSENALL` claim (`:127`) and the 0.467 ns quantum (`:135`) — **DONE: both corrected at the declarations — `TSENALL` is deliberately clear so only `SNAPTYPSEL`-selected PTP messages are stamped (not every frame), and `SSINC` counts whole nanoseconds because `TSCTRLSSR` selects digital rollover (0.467 ns is the *binary*-rollover quantum)** |
| ✅ | — | — | **P4.11** | — | — | 1/3 O | Correct the thread name in `architecture.md:220` to match `TCPIP_THREAD_NAME` (`"TCP/IP"`) — **DONE: thread table now shows `TCP/IP`, the actual `TCPIP_THREAD_NAME`, with a note that these documents call it `tcpip_thread` after its lwIP source symbol** |
| ✅ | — | — | **P4.12** | — | — | 1/3 O | Update the FreeRTOS version in `FreeRTOSConfig.h`'s header comment to V11.3.0 — **DONE: header comment now reads V11.3.0, matching the `FetchContent` pin, with a pointer to where the pin lives** |
| ✅ | — | — | **P4.13** | — | — | 1/3 O | Remove the dead relative `Inc` include path from `Middlewares/CMakeLists.txt:14` — **DONE: the bare `Inc` entry (resolving to the non-existent `Middlewares/Inc`) removed, with a comment naming F32 so it is not re-added** |
| ✅ | **Partial ◐** | — | **P4.14** | — | item 5 | 1/3 P | Add an `#error` guard on `HAL_DCACHE_MODULE_ENABLED` in `stm32h5xx_hal_conf.h`. **The comment half is already done** — the linker script and `design_doc.md` both carry the warning; the compile-time guard is the new part — **DONE (was ◐): `#error` added on `HAL_DCACHE_MODULE_ENABLED` in the HAL config, naming the SRAM3 region, both remedies and where to read. Proved it bites by uncommenting the module define — the build stops with the message. The comment half already existed in the linker script and design_doc; this is the half that reaches someone flipping a HAL module on** |

### Priority 5 — housekeeping, tooling and upstream

| Status | Flash 3.6 Verified | Pro 3.1 Verified | Opus 5.0 ID | Flash 3.6 ID | Pro 3.1 ID | Consensus | Description |
|---|---|---|---|---|---|---|---|
| ✅ | — | — | **P5.1** | — | — | 1/3 O | Add CI: clean Debug + Release build, zero warnings under `-Wall -Wextra` — **DONE: `.github/workflows/ci.yml` builds Debug and Release via `build.sh` (so the patch-apply step is exercised too), gates on **first-party** warnings only — vendored lwIP's four F63 instances are expected — re-checks that the patches are applied, and uploads the ELF and map** |
| ✅ | — | — | **P5.2** | ACT-14 (D2, I4) | — | 2/3 O+F | Remove the unconditional `DEBUG` from `CMakeLists.txt:93`; the `$<$<CONFIG:Debug>:DEBUG>` generator expression at line 132 already covers it — **DONE: `DEBUG` removed from the unconditional `compile_DEFS`; the `$<$<CONFIG:Debug>:DEBUG>` generator expression already covered it, so Release no longer defines it** |
| ☐ | — | — | **P5.3** | ACT-15 (D3, I4) | — | **⚠ METHOD** | Decide whether `Release` should move from `-Og` to `-O2`/`-Os`; configure or delete the untested `relWithDebInfo` and `minSizeRel` presets. **Requires hardware re-verification** — see F62 |
| ✅ | — | — | **P5.4** | — | — | 1/3 O | Use `bfifo_push_all()` in `serial_io_write()` — console output is silently truncated when the 16 KB FIFO fills — **DONE: `bfifo_push_all()`, so a full FIFO blocks the producer instead of truncating the message mid-word. Correct for every current caller (all threads); ISR-context output goes through `serial_io_panic_write()` instead, noted at the call** |
| ✅ | — | — | **P5.5** | ACT-12 (B12) | — | 2/3 O+F | Fix `get_line()`'s left-trim so the trimmed line reaches `process_cli_line()` (use `memmove`) — **DONE: left-trim now shifts in place with `memmove` so the caller's buffer is actually trimmed. Verified on target — `   osinfo` (three leading spaces) is accepted and answers, where before only the local pointer moved and `task_cli()` passed the untrimmed line on** |
| ✅ | — | — | **P5.6** | — | — | 1/3 O | Fix `phy_setup_int_DP83848()`'s `READ`-where-`WRITE`-is-meant on MICR, `phy_common.c:89` — **DONE: `READ` → `WRITE` on the DP83848 MICR. The read discarded the value just computed, leaving the global interrupt enable clear so the PHY never asserted its interrupt pin. Not this board's PHY, but shipped broken** |
| ✅ | — | — | **P5.7** | — | — | 1/3 O | Fix `ETHHW_AuxTimestampCh()` to OR rather than assign, `mac_drv.c:803` — **DONE: `ETHHW_AuxTimestampCh()` now read-modify-writes the single channel bit. The assignment disabled every other auxiliary channel as a side effect of touching one** |
| ✅ | — | — | **P5.8** | — | — | 1/3 O | Fix the out-of-scope `netif` identifier in `rx_thread`'s MIB2 macros — compiles only because MIB2 stats are disabled — **DONE: `netif` → `if0` in `rx_thread`'s four MIB2 macros. Verified by building with `MIB2_STATS 1` — zero `netif`-undeclared errors, where previously the file only compiled because the macros expanded to nothing** |
| ✅ | — | — | **P5.9** | — | — | 1/3 O | Fix the inverted return sense of `phy_read_reg()` / `phy_write_reg()` (0 means success) — **DONE: both wrappers now return `== 0U`, so `true` means success. Previously `true` meant the access had *failed*. Unused today — corrected rather than deleted so the next caller is not handed an inverted contract** |
| ✅ | — | — | **P5.10** | — | — | 1/3 O | Fix `phy_read_range()`'s stride (`a += 2` skips odd registers) and `phy_read_all_regs()`'s bound (a PHY address used as a register count) — **DONE: stride `a += 2` → `a++` (MDIO addresses are consecutive, not byte offsets — the old stride skipped BSR and both PHYID words), and `phy_read_all_regs()` now passes `EPHY_MDIO_REG_COUNT` (32) instead of `EPHY_MDIO_MAX_ADDR`, which is a PHY *address* limit** |
| ✅ | — | — | **P5.11** | — | — | 1/3 O | Change `MSGchar()` to take a `char`; remove the unused `last_char` — **DONE: `MSGchar()` takes a `char`. The old `int` signature took the address of the parameter and sent one byte from it — correct only on little-endian, by accident. Unused `last_char` removed** |
| ✅ | — | — | **P5.12** | — | — | 1/3 O | Renumber `patches/flexptp/0002-…` to `0001-…`, or note the gap in the directory README — **DONE — resolved by documenting, not renumbering. `patches/flexptp/README.md` now explains that `0001` was the dropped F42/F61 instrumentation patch and that `0002` is deliberately kept: the number is referenced from commit messages, `port_review.md` and the recovery command, so renaming would invalidate all of them to close a cosmetic gap** |
| ✅ | — | — | **P5.13** | — | — | 1/3 O | Remove the inert MPU-wrapper settings from `FreeRTOSConfig.h:81-84`, or comment why they are kept with `configENABLE_MPU 0` — **DONE: kept, with a comment stating they are inert while `configENABLE_MPU` is 0 and are **not** evidence the MPU is in use. Deleting them would make turning the MPU on require rediscovering the set** |
| ✅ | — | — | **P5.14** | — | — | 1/3 O | Exclude `Src/etherlib_options.c` from LWIP builds — **DONE: `etherlib_options.c` now compiled only when `ETH_STACK` is `ETHERLIB`. MAC address and networking unchanged — same DHCP lease, ping 0% loss** |
| ☐ | — | — | **P5.15** | ACT-27 (I6) | items 1, 2 | **3/3** | File the upstream reports and patches: F47 (`__PRI64_PREFIX`), F59 (servo NaN latch), F60/F66 (embfmt) |
| ✅ | — | — | **P5.16** | — | — | 1/3 O | Add a `docs/` cross-reference and anchor checker to CI — **DONE: `tools/ci/check_docs_links.py` validates relative links and heading anchors across 21 Markdown files; wired into CI as a separate job. Currently **0 broken**. Writing it found one genuine break I had introduced in P4.4 by renaming a heading — and two bugs in the checker itself, since GitHub does not collapse whitespace runs and does not strip underscores** |
| ✅ | — | — | **P5.17** | — | — | 1/3 O | Move `serial_io_init()`'s `osThreadNew()` after `osKernelInitialize()`, or record why the current order is safe — **DONE — resolved by recording why the order is safe. `osThreadNew()` before `osKernelInitialize()` is outside the CMSIS-RTOS2 contract, but the shim only checks IRQ context and `xTaskCreate()` is documented as callable before the scheduler starts. Reordering would mean splitting `serial_io_init()` or losing USART3 setup before the first print** |
| ✅ | — | — | **P5.18** | — | — | 1/3 O | Comment `ETH_MAC_ADDR0`'s `0x02 \| (UNIQUE_DEV_ID_BYTE(0) & 0xFE)` so the locally-administered bit and the unicast mask read as two deliberate operations — **DONE: commented as two deliberate operations — `| 0x02` sets locally-administered (the UID is not an assigned OUI), `& 0xFE` clears the multicast bit so a UID with bit 0 set cannot produce an unroutable multicast source address** |
| ✅ | — | — | **P5.19** | ACT-16 (D4, I4) *(part)* | — | 1/3 F | Remove `-u _printf_float` from `CMakeLists.txt:166` — verified as force-linking newlib's float formatter (25 references in the map) into a project that uses `embfmt` — **DONE (removed), but **the size claim was wrong**: a proper A/B — build, remove flag, rebuild — gives **0 bytes** difference, with `printf_float` absent from the map either way. My earlier "25 references" came from a grep that also matched `vfprintf`. Removed as a misleading dead flag, not as a saving; the cross-review is corrected accordingly** |
| ✅ | — | — | **P5.20** | ACT-17 (D5) | — | **⚠ SEVERITY** | Remove the duplicate `main.c` / `syscall.c` / `sysmem.c` listings between `CMakeLists.txt:35-40` and `Src/CMakeLists.txt:1-13` — **DONE: `main.c`/`syscall.c`/`sysmem.c` removed from the root `sources_SRCS`, leaving `Src/CMakeLists.txt` as the single owner. Verified the map still has exactly one `main.c.obj` — CMake had been deduplicating them, so this is tidying with no functional effect, as recorded** |
| ✅ | — | — | **P5.21** | ACT-19 (U2) | — | 1/3 F | Add a `NOLOAD` placement for `.ETHLibPool` in SRAM3, per the analysis already in `design_doc.md` — **DONE: `.ETHLib_sec (NOLOAD)` added in SRAM3 per `design_doc.md`'s analysis. Verified in the map — section present at `0x20062488`, size 0, SRAM3 usage unchanged at 74 888 B, so it costs nothing until EtherLib is actually compiled** |
| ✅ | — | — | **P5.22** | — *(from U2)* | — | 1/3 F | Reconcile `ETH_BUFFER_SIZE`: `1536UL` in `eth_drv_etherlib.c:17` vs `1528UL` in `eth_drv_lwip.c:40` — **DONE: `eth_drv_etherlib.c` now uses 1528, matching `eth_drv_lwip.c` and `PBUF_POOL_BUFSIZE`. The 8-byte disagreement meant switching `ETH_STACK` silently changed both the SRAM3 footprint and the per-frame capacity** |
| ✅ | — | — | **P5.23** | ACT-20 (B13) | — | 1/3 F | Add `\n` to `MSG("Turning PTP on!")`, `ptp_port_stm32h563.c:105` — **DONE: verified on target — *"Turning PTP on!"* now ends its own line instead of running into *"Own clock ID:"*** |
| ✅ | — | — | **P5.24** | ACT-21 (D6) | — | 1/3 F | Fix `"currenty"` → `"currently"` in the boot banner, `main.c:133` — **DONE: banner reads *"currently compiled against"*; confirmed on target that `currenty` no longer appears** |
| ✅ | — | — | **P5.25** | ACT-22 (D7) | — | 1/3 F | Fix `"PPL_PULSETRAIN"` → `"PPS_PULSETRAIN"` in the `#error` directive, `ptp_port_stm32h563.c:71` — **DONE: the `#error` now names `PPS_PULSETRAIN`, matching the macro `ptphw_set_pps_freq()` actually tests** |
| ✅ | — | — | **P5.26** | ACT-23 (D8) | — | 1/3 F | Swap the inverted OPAMP / OTFDEC comments in `stm32h5xx_hal_conf.h:268-269` (inherited from ST's template) — **DONE: OPAMP and OTFDEC comments swapped back to match their macros (inherited from ST's template)** |
| ☐ | — | — | **P5.27** | — | item 6 | 1/3 P | Upstream the H563 hardware port (`Src/flexptp_port/`) to flexPTP — its first Cortex-M33 target. **Blocked on P3.1**, or the PPS enum defect ships with it |
| ☐ | — | — | **P5.28** | — | item 1 *(part)* | 1/3 P | Report flexPTP's **documentation** discrepancies upstream — F37 (`PTP_HW_GET_TIME` typed `TimestampI *`) and F38 (`SPRINTF` vs `FLEXPTP_SNPRINTF`) |

### Closed without action

| Status | Flash 3.6 Verified | Pro 3.1 Verified | Opus 5.0 ID | Flash 3.6 ID | Pro 3.1 ID | Consensus | Description |
|---|---|---|---|---|---|---|---|
| ⊘ | — | — | **X1** | ACT-18 (U1) | — | **⚠ DISPUTED** | *Change `FLEXPTP_INITIAL_PROFILE` to `"default"`.* **Rejected** — overrides a decision documented in four places; also cites flash option bytes, which are not application storage |
| ⊘ | — | — | **X2** | ACT-24 (I2) | — | **⚠ DISPUTED** | *Wire `__io_putchar` to `MSGchar`.* **Rejected** — false premise (`__io_putchar` is a weak declaration, never defined) and invites newlib stdio back in. Superseded by **P2.1** |
| ⊘ | — | — | **X3** | ACT-25 (I1) | — | **⚠ METHOD** | *Zero-copy TX refactor.* **Deferred** — goal endorsed, shown remedy will not compile (`bd` out of scope) and would race the DMA. Needs an API split, not a snippet |
| ✅ | **Verified ✅** | — | **X4** | — | item 3 | **⚠ DISPUTED** | *Delete `ETHHW_UpdatePTPTime()` and `PTP_UPDATE_CLOCK`.* **Already complete** — removed when F41 was resolved; only explanatory comments remain |
| ⊘ | — | — | **X5** | ACT-16 (D4) *(part)* | — | **⚠ DISPUTED** | *Remove `-lstdc++` / `-lsupc++`.* **Rejected as stated** — both archives appear only as `LOAD` lines; nothing is extracted, so removal saves nothing |

---

## Disputes — where consensus was not reached

Eleven rows carry a `⚠`. Five are outright disagreements about whether to act; six are
agreements with a contested boundary.

### Outright disagreements

**X1 — change the default PTP profile (Flash ACT-18 vs Opus).**
Flash 3.6 recommends switching `FLEXPTP_INITIAL_PROFILE` from `"gPTP"` to `"default"` so the
board works out of the box through a switch. Opus 5.0 rejects it: the gPTP default is a
documented product decision, warned about in `README.md` (twice), the boot banner,
`design_doc.md` and `host_ptp_setup.md`, and gPTP is the profile most industrial users
deploy. The item also proposes persisting the choice "in flash option bytes", which is
technically wrong — option bytes are device configuration, not application storage.
**Unresolved: this is the maintainer's call, not a defect fix.**

**X2 — `__io_putchar` (Flash ACT-24 vs Opus).**
Flash 3.6 states `syscall.c` *"defines a weak `__io_putchar` that drops characters"*. Verified:
there is no definition — `syscall.c:35` is a weak *declaration*, so a live `_write()` would
branch to address 0 and fault, not drop characters. Both symbols are unreferenced and
garbage-collected (`__io_putchar` does not appear in the map). **Resolution: rejected, and
replaced by P2.1**, a polled panic writer for the paths where the FIFO-backed console cannot
work.

**X4 — delete dead PTP update code (Pro item 3 vs the tree).**
Pro 3.1 reports `nsec = 1000000000 - (nsec >> 1)` as a live suspect calculation. It was
deleted when F41 was resolved. **Resolution: no action exists; Pro 3.1's finding is
incorrect.**

**X5 — drop the C++ runtime link flags (Flash ACT-16 part vs Opus).**
Flash 3.6 groups `-u _printf_float`, `-lstdc++` and `-lsupc++` as *"adds dead code"*. Verified:
`-u _printf_float` really does force-link the float formatter (**P5.19**, accepted), but the
C++ archives contribute nothing. **Resolution: split — accept the first, reject the other two.**

### Contested boundaries

**P1.8 — `⚠ SCOPE`.** Both reviews flag `main.c:83`. Flash 3.6 stops there; Opus 5.0 found
four more surviving HCLK claims, three of them in documentation. The wider scope is adopted.

**P2.5 — `⚠ SCOPE`.** Both flag the `escParam` overflow. Opus 5.0 additionally found that
`switch (escParam[0])` reads the array before it is written, so arrow-key history acts on the
previous keypress. Both halves are in scope.

**P2.13 — `⚠ METHOD`.** Agreement that `low_level_output()` needs a length guard; Flash 3.6's
`return ERR_MEM` contradicts the warning in the function's own doc comment. `ERR_OK`/`ERR_IF`
adopted.

**P3.8 — `⚠ SEVERITY`.** Both agree the linker sections should be reordered and the mechanism
is confirmed from the map. Flash 3.6 ranks it Priority 1 (Critical); Opus 5.0 ranks it
Priority 3, because reaching it needs ~2.5 KB of *newlib* `malloc` in a project that
deliberately avoids newlib's allocator. **Listed at P3; the fix is free, so the ranking
affects order, not whether to do it.**

**P5.3 — `⚠ METHOD`.** Both agree `-Og` is an odd choice for Release. Opus 5.0 attaches a
blocking caveat: F62 records that the release build HardFaulted the first time it was built,
because `-Og` narrowed a `uint32_t` UID load back into faulting byte loads. **Not a free
edit** — it needs a hardware re-verification pass.

**P5.20 — `⚠ SEVERITY`.** Both agree the CMake source lists are duplicated. Opus 5.0 verified
that CMake deduplicates them (exactly one `Src/main.c.obj` in the map), so the effect is
zero; Flash 3.6 lists it as a documentation-vs-code discrepancy, which is also the wrong
category. **Kept as housekeeping.**

---

## Coverage analysis

Which review found what, by area:

| Area | Opus 5.0 | Flash 3.6 | Pro 3.1 |
|---|---|---|---|
| Driver / concurrency defects | 8 | **13** | 0 |
| — unique to that review | 3 | **5** | 0 |
| CLI / console defects | 8 | 4 | 0 |
| Documentation fidelity (this project's docs) | **14** | 1 | 0 |
| Build system | 5 | 5 | 0 |
| Architecture-level weaknesses | **8** | 1 | 0 |
| Upstream / process | 4 | 1 | **3** |
| Verification gaps | 5 | 2 | 2 |

**The five defects only Flash 3.6 found** — P1.10 (CLI mutex self-deadlock), P2.10 (w1c
register corruption), P2.11 (16-bit counter wrap), P2.12 (unbounded PHY reset), P3.8 (heap
collision) — are the strongest argument for having run three reviews. Two of them outrank
anything in the other two reports.

**The two actions only Pro 3.1 proposed** — P5.27 (upstream the hardware port) and P5.28
(report the flexPTP documentation bugs) — are both outward-facing, and neither of the other
reviews thought to look beyond the repository.

**Documentation drift was found almost entirely by one review.** Fourteen of the fifteen
documentation actions come from Opus 5.0 alone. The highest-value cluster is P1.8/P1.9: five
surviving assertions that HCLK is the PTP clock — the belief that caused F53 — plus a clock
tree written in the H7 divider spellings that F45 identifies as a silent-miscompile trap.

**One coupling is easy to miss.** P1.6 and P1.10 are in the same function and must land
together: P1.10 alone converts a hang into memory corruption, because the deadlock it removes
is currently what prevents P1.6's dangling pointers from ever being dereferenced.

---

## Recommended execution order

1. **P1.10 + P1.6 together**, then P1.5 — the CLI command database, fixed as one change.
2. **P1.3 + P1.4 together** — the PHY NULL-callback fault and the sweep that makes it
   reachable; P2.12 folds in naturally.
3. **P1.1 + P1.2** — close the patch-reproducibility hole before anyone else builds the tree.
4. **P1.7, P2.1, P2.2** — make the console safe and failures audible; every later diagnosis
   depends on these.
5. **P1.8 + P1.9** — the documentation corrections that would otherwise re-teach F53 and F45.
6. **P2.10, P2.11, P2.3, P2.13** — the driver cluster, in one pass with one hardware
   re-verification.
7. **P3.1 then P3.2** — settle `PPSCTRL` on paper, then on the pin. P5.27 unblocks behind it.
8. Everything else in priority order; P5.3 last, since it needs its own hardware pass.
