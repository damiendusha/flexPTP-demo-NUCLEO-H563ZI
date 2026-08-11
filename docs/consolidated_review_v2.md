# Consolidated Review v2 — flexPTP-demo-NUCLEO-H563ZI

**Date:** 2026-08-11 · **Tree:** `review/priority-1-fixes` @ `d359782`
**Sources:** [`final_review_v2_opus_5p0.md`](final_review_v2_opus_5p0.md) (Opus 5) ·
[`final_review_v2_gemini_flash_3p6.md`](final_review_v2_gemini_flash_3p6.md) (Gemini 3.6 Flash)

This document merges the two second-pass reviews into one actionable list, and states what
survived scrutiny, what was corrected, and what was rejected.

## 0. Provenance — read this before treating anything below as corroborated

**The two source reviews are not independent, and agreement between them is not evidence.**
The Gemini document lists `final_review_v2_opus_5p0.md` among its predecessors (its line 6) and
reproduces that review's finding IDs (N1–N16), titles, ordering, severity assignments and
remediation approach. Every finding it reports is one the Opus review already reported; it
contributes no finding of its own.

That does not make it worthless — it independently re-ran the build, the disassembly and the
link-symbol audit, and its `-O2` instruction addresses match a fresh `objdump` byte for byte, so
the mechanical verification is real. But it means:

- **No finding below is confirmed by two independent readings.** Where a finding rests on a
  single piece of evidence, it still does.
- **Where the two documents differ, the difference is Gemini's embellishment**, not a second
  opinion. Five such additions are factually wrong and are rejected in [§4](#4-claims-rejected-or-downgraded).

Everything below has been re-verified against the code for this consolidation. Statements are
marked:

| Mark | Meaning |
|---|---|
| **Measured** | Reproduced here by building, disassembling, simulating or reading the map/ELF |
| **Read** | Established by reading the code and its contracts; no execution required |
| **Unverified** | Depends on documentation or hardware not available in this repo; stated as a hypothesis |

## 1. Consolidated findings

Severity is impact-when-triggered. Reachability is separate and is stated per finding, because
three of the four High items are unlikely-but-terminal rather than routine.

| ID | Severity | Subsystem | Finding | Status |
|---|---|---|---|---|
| [N1](#n1--bfifo-reports-empty-when-exactly-full) | High | `blocking_io` | Ring buffer cannot distinguish full from empty | ✅ **fixed** (F67) |
| [N2](#n2--the-tx-ownership-wait-is-compiled-away-above--og) | High | `EthDrv` | TX ownership retry loop optimised away at `-O2`/`-O3` | ✅ **fixed** (F70) |
| [N3](#n3--rx-dma-suspension-on-rbu-is-never-cleared) | High | `EthDrv` | RBU suspend is never cleared; AIS flags discarded silently | ✅ **fixed** (F71), mechanism now **confirmed on hardware** |
| [N4](#n4--known-vendor--unknown-model-dereferences-null-at-boot) | High | `phy_drv` | Known OUI + unknown model dereferences NULL `phyName` | ✅ **fixed** (F69) |
| [N5](#n5--every-lwip-assertion-is-a-silent-no-op) | Medium | `lwip_port` | lwIP assertions print nothing and do not halt; ~9 KB wasted | ✅ **fixed** (F72) |
| [N6](#n6--the-isr-logging-contract-is-self-contradictory) | Medium | `standard_output` | ISR-logging contract contradicts itself; ISR path can hang | ✅ **fixed** (F68) |
| [N7](#n7--bfifo_pop-hands-the-write-token-to-a-second-writer) | Medium | `blocking_io` | `bfifo_pop()` releases the writer token mid-push | ✅ **fixed** (F67) |
| [N8](#n8--cli-tokenizer-one-byte-overflow-and-cross-thread-reuse) | Medium | `cliutils` | 1-byte `.bss` overflow + static buffer/`strtok` shared across threads | ✅ **fixed** (F74) |
| [N9](#n9--no-memory-barriers-at-descriptor-handoff) | Medium | `EthDrv` | No `__DMB()`/`__DSB()` around descriptor ownership handoff | ✅ **fixed** (F70) |
| [N10](#n10--n16--low-and-latent) | Low | `EthDrv` | EtherLib RX path strands a descriptor per allocation failure | **Read** |
| [N11](#n10--n16--low-and-latent) | Low | `ethernet` | Wire-sized VLA in the EtherLib UDP callback | **Read** |
| [N12](#n10--n16--low-and-latent) | Low | `cliutils` | `strncpy` without termination in two places | ✅ **fixed** (F74) |
| [N13](#n10--n16--low-and-latent) | Low | `EthDrv` | FCS not stripped; every RX frame is 4 bytes long | ✅ **fixed** (F76) |
| [N14](#n10--n16--low-and-latent) | Low | `EthDrv` | Dropped TX frame reported to lwIP as sent | ✅ **fixed** (F75) |
| [N15](#n10--n16--low-and-latent) | Low | `lwipopts` | `#undef LWIP_PROVIDE_ERRNO` undone by `cc.h` | **Read** |
| [N16](#n10--n16--low-and-latent) | Low | CI | No `pipefail`: a failing build can pass green | **Read** |
| [N17](#n17--the-flash-figure-depends-on-the-build-directory-path-new-in-this-consolidation) | Low | Build | Flash size varies with the build-directory path | **Measured**, new here |
| [D-a](#documentation) | Doc | `port_review.md` | F63 gives the wrong reason (`LWIP_DEBUG` vs `LWIP_NOASSERT`) | ✅ **corrected**, and F63 itself resolved |
| [D-b](#documentation) | Doc | `architecture.md` | RX table calls a permanent stall a stall | **Read** |
| [D-c](#documentation) | Doc | `README.md` | embfmt URL disagrees with `.gitmodules` | **Read** |
| [H-1…H-5](#housekeeping) | Hygiene | repo | Untracked scripts, docs-CI submodule constraint, dead config | **Read** |

Only the Opus review carried D-a…D-c and H-1…H-5; the Gemini review omits documentation and
hygiene entirely. They are retained because D-a is the reason N5 went unnoticed for the whole
port.

## 2. High severity

### N1 — `BFifo` reports empty when exactly full

**Where:** [`blocking_fifo.c:26-40`](../Modules/blocking_io/blocking_fifo.c#L26),
[`blocking_fifo.c:42-95`](../Modules/blocking_io/blocking_fifo.c#L42)
**Affects:** the 16 KB console FIFO and the 512-byte CLI input FIFO.

Two indices, no count and no reserved slot, so `writeIdx == readIdx` encodes both *empty* and
*full*. `bfifo_push()` clamps to `bfifo_get_free()` and then wraps, so it will happily produce
that state. Simulated with the real parameters (`size` = 16384, one 16384-byte push):

```
after push(16384): pushed=16384 writeIdx=0 readIdx=0 used=0 free=16384
reader sees 0 bytes available (buffer physically holds 16384)
```

`serial_io_thread()` then wakes, reads 0, pops 0, calls `HAL_UART_Transmit_DMA(…, 0)` which
returns `HAL_ERROR`, and re-enters its `osDelay(1)` branch at 1 kHz — while producers, told the
buffer is entirely free, overwrite the un-emitted backlog. A second variant needs no full
buffer: a producer that exactly fills the free space between the consumer's `bfifo_read()` and
its `bfifo_pop()` makes `popSize = 0`, so the same block is transmitted twice.

**Reachability.** The console drains at 11.5 KB/s, so 16 KB must be queued faster than that —
not routine PTP logging, but a `?` dump plus several log streams, or any future print loop. The
CLI input FIFO is 512 bytes and reachable by pasting into the terminal.

**Both reviews note, correctly, that P5.4 made this more likely:** `bfifo_push_all()` fills the
remaining space *exactly* whenever a message is larger than it, which is precisely the state
that reads back as empty.

> ✅ **Fixed** in P6.1, together with N7. Occupancy now comes from two free-running byte
> counters whose modular difference is unambiguous at both ends. Measured on hardware: ten
> back-to-back help dumps (~25 KB against a 16 KB FIFO) went from **3 of 10** arriving to
> **10 of 10**, 8 605 → 24 962 bytes. Recorded as [F67](port_review.md).

**Fix.** Explicit `count`, or a reserved slot (`free = size - used - 1`), or monotonic indices
masked on access — see [§7.1](#71--n1--n7-the-monotonic-index-option-carries-a-precondition)
for which to pick. *Correction to the Gemini remediation:* its snippet changes only
`bfifo_get_used()`/`bfifo_get_free()` to monotonic form. Dropped in as shown it breaks the file
— `bfifo_push()`, `bfifo_read()` and `bfifo_pop()` all use `writeIdx`/`readIdx` directly as
memory offsets and one of them compares `readIdx > writeIdx`. Monotonic indices require masking
at every access site; all five functions change together.

### N2 — The TX ownership wait is compiled away above `-Og`

**Where:** [`mac_drv.c:596-607`](../Drivers/EthDrv/mac_drv.c#L596),
[`mac_drv.h:29-31`](../Drivers/EthDrv/mac_drv.h#L29)

`ETHHW_Desc` declares the four DMA-written words as plain `uint32_t`, and the P2.3 retry loop
has no barrier, call or side effect — so the load is loop-invariant and gets hoisted. Measured
with the project's own flags, changing only `-O`:

| Level | Result |
|---|---|
| `-O0`, `-Og` | `ldr r2, [r4, #12]` **inside** the loop — 100 000 real retries |
| `-O2` | `614: ldr r2,[r3,#12]` · `618: cmp r2,#0` · `61e: blt.n 696` — loop gone |
| `-O3` | same, with the failure path tail-called into `MSG` |

Above `-Og` the bounded wait degenerates to a single test: a descriptor still owned by the DMA
means an immediate drop. This matters because [`CMakeLists.txt:160-185`](../CMakeLists.txt#L160)
advertises `-O2` and `-Os` as hardware-tested known-good, and wires `-O2` into
`RelWithDebInfo`. Those runs were real; what they did not cover is that the retry loop is absent
from those images.

Same class as [F62](port_review.md) — a hardware-ordering workaround expressed without
`volatile` is not a constraint — applied to descriptor memory instead of the UID region.

> ✅ **Fixed** in P6.4 with N9. The `-O2` object now shows the spin counter, a `DES3` re-read
> inside the loop and a backward branch. Hardware-tested at all three levels — `Debug`,
> `Release` (`-Og`) and `RelWithDebInfo` (`-O2`), the last of which had never been run on the
> board: boot, DHCP, flexPTP → MASTER, addend 3435973836, ping 1400 B 0% loss, no TX-drop
> reports. Recorded as [F70](port_review.md).

**Fix.** `volatile uint32_t DES0, DES1, DES2, DES3;`. The `ext` half is CPU-only and stays.
Re-check the three disassemblies afterwards and extend the F62 note in `CMakeLists.txt`.

### N3 — RX DMA suspension on RBU is never cleared

**Where:** [`mac_drv.c:176`](../Drivers/EthDrv/mac_drv.c#L176),
[`mac_drv.c:253-265`](../Drivers/EthDrv/mac_drv.c#L253),
[`mac_drv.c:547-579`](../Drivers/EthDrv/mac_drv.c#L547)

The RX tail pointer is written once at init (`DMACRDTPR = 0`, "tail pointer WON'T STOP"), which
prevents suspension on *tail* but not on *ownership*. When every RX descriptor is outstanding
the MAC sets `DMACSR.RBU` and the receive engine suspends. `ETHHW_RestoreRXDesc()` returns
ownership but issues no receive poll demand, so — on the documented behaviour of this DMA — the
engine never resumes: link up, TX fine, no frames ever again.

Compounding it, the abnormal-interrupt branch promises in its comment to *report* DMA faults and
then only acknowledges them, so `RBU`, `FBE`, `RPS` and `TPS` are still consumed in silence.
That comment/code divergence is the more certain half of this finding and is worth fixing on its
own merit.

> ✅ **Fixed in P6.5, and the mechanism is no longer a hypothesis.** Measured with `rx_thread`
> stalled 1.5 s under a 500 pps flood, everything else identical: **without** the poll demand
> 85.1% loss during and **100% loss permanently** afterwards; **with** it, 6.25% during and
> **0% after**. A third defect turned up on the way — `ETHHW_Start()` never enabled `AIE` or
> any per-condition enable, so `DMACSR.AIS` could not assert and the error branch had been
> unreachable since it was written. That is why the board had never once reported the fault.
> The flags are now latched in the ISR and printed by `phy_thread`. Recorded as
> [F71](port_review.md).
>
> **Provenance note, retained deliberately.** The Gemini review supported this with a verbatim
> quotation attributed to "RM0481, Section 59.6.2". The *rule* has now been confirmed
> experimentally — but the quotation still could not be verified, and rejecting it was still
> correct: a citation nobody can check is not evidence, even when the claim behind it turns
> out to be true. The board settled this, not the quote. This repository holds
> no copy of RM0481 — `port_review.md` says so explicitly when declining to cite it for
> `SNAPTYPSEL` — and the wording appears in no document here. Treat the resume-requires-poll-demand
> rule as **Unverified** until someone reads the register description, and do not propagate the
> quote. The remediation is cheap, harmless and correct either way: a register write per
> restored descriptor costs nothing, and if the DMA was not suspended the poll demand is a no-op.

**Reachability.** Needs all 24 descriptors outstanding at once. `PBUF_POOL_SIZE` is 8, so
`rx_thread` drops frames long before the ring fills — which is why the documented 500 pps tests
pass. Reachable via the queue-full path in `ETHHW_ReadCallback()` under a burst, or anything
that delays `rx_thread` by ~2 ms at line rate. Low probability, terminal consequence, and it
presents as "the board stopped receiving" with no message.

**Fix.** Poke `DMACRDTPR` after returning a descriptor (per descriptor in
`ETHHW_RestoreRXDesc()`, or once per pass), and make the AIS branch print the flag word it is
about to clear — subject to N6, since that branch runs in the ISR. Fold the barriers from
[N9](#n9--no-memory-barriers-at-descriptor-handoff) into the same edit; see
[§7.2](#72--n3-the-poll-demand-is-context-safe-the-open-question-is-elsewhere) for the two
context questions this raises:

```c
void ETHHW_RestoreRXDesc(ETHHW_DescFull *bd) {
    if (bd == NULL) return;
    bd->desc.DES0 = bd->ext.bufAddr;
    __DMB();                                    // descriptor body visible before ownership
    bd->desc.DES3 = ETH_DMARXNDESCRF_OWN | ETH_DMARXNDESCRF_IOC | ETH_DMARXNDESCRF_BUF1V;
    bd->ext.inFlight = 0;                       // only after ownership has gone back (F61)
    __DSB();
    WRITE_REG(ETH->DMACRDTPR, 0);               // receive poll demand
}
```

### N4 — Known vendor + unknown model dereferences NULL at boot

**Where:** [`phy_common.c:21`](../Drivers/EthDrv/phy_drv/phy_common.c#L21),
[`phy_common.c:382-451`](../Drivers/EthDrv/phy_drv/phy_common.c#L382)

`static const char *phyName = NULL;` is assigned only on a full match or on an unknown **OUI**.
A PHY whose vendor is in the table but whose model is not falls through both inner
`default: break;` arms with `phyName` still NULL, and `return phyName[0] != '\0';` dereferences
address 0 — a HardFault inside `low_level_init()`, before the network comes up.
`phy_print_full_name()` repeats the dereference one line later.

This is the sibling of the first review's **B1**, which guarded the *unknown-PHY* NULL callback
in `phy_get_link_status()` and did not revisit this site. It is the more likely of the two:
vendors change models far more often than they change OUIs.

The affected vendor IDs, from [`phy_common.h:72-76`](../Drivers/EthDrv/phy_drv/phy_common.h#L72),
are TI `0x080017` / `0x080028` / `0x080000`, SMSC `0x0001F0` and Realtek `0x000732`. (The Gemini
review lists SMSC as `0x00800F`; that value does not appear in this project — see
[§4](#4-claims-rejected-or-downgraded).)

**Fix.** `static const char *phyName = "";` and delete the `phyName = "\0"` assignment in the
outer default. Both reads then behave.

## 3. Medium and low severity

### N5 — Every lwIP assertion is a silent no-op

**Where:** [`cc.h:83`](../Modules/lwip_port/arch/cc.h#L83),
[`syscall.c:35`](../Src/syscall.c#L35), [`syscall.c:80-90`](../Src/syscall.c#L80),
[`lwipopts.h:48-62`](../Inc/lwipopts.h#L48)

The stock macro `printf("Assertion \"%s\" failed …")` fails twice over, both verified in the
linked image:

1. **It cannot print.** `printf` → `_write` → `__io_putchar`, which is declared weak in
   `syscall.c` and defined nowhere. The linker resolves the undefined weak symbol to 0 and
   relaxes the call away:

   ```
   080041a0 <_write>:
     80041aa:  ldrb.w  r0, [r4], #1
     80041ae:  nop.w              <-- "bl __io_putchar" was here
     80041b2:  adds    r5, #1
   ```

   `_write` walks the buffer, emits nothing, and returns `len` — every byte reported written.

2. **It does not stop.** No halt, no `abort()`. Execution continues into the condition the
   assertion guards.

The cost is concrete: `lwipopts.h` deliberately enables `LWIP_FREERTOS_CHECK_CORE_LOCKING` and
`LWIP_ASSERT_CORE_LOCKED()`, which route every unlocked lwIP call into
`sys_check_core_locking()` → `LWIP_ASSERT`. That is the automated form of the bug P3.6 fixed by
hand in `check_dhcp_state()`, **and it has never been able to fire**. The map shows `printf`
pulled in by `eth_drv_lwip.c.obj (printf)` — the two `LWIP_ASSERT`s in that file — dragging the
float-capable `_vfprintf_r` (8 836 B) and its support in with it.

> ✅ **Fixed** in P6.6 — and it immediately earned its keep: the very first boot with working
> assertions halted on a **real** core-lock violation that had been present since the port
> began. `init_ethernet()` calls `netif_add()`, `netif_set_default()` and
> `netif_set_link_callback()` from the `init` thread without the lock, and `netif_add()`
> asserts it. P3.6 had fixed the same class of bug in `check_dhcp_state()` and concluded it was
> "the only place in the project that got this wrong"; it was only the only place anyone had
> looked. Fixed as [F73](port_review.md).
>
> Verified by scaffolding a real violation — `netif_set_link_up()` from
> `phy_thread`, which holds no core lock — which now prints `*** FATAL: lwIP assertion failed /
> Function called without core lock` and halts. Flash returned: **13 736 bytes** (Release
> 181 100 → 167 364 B), more than the ~9 KB estimated. And F63 resolved itself: a halting
> assertion is `noreturn`, so GCC can use the `options_out_len` bound and the Release build
> went from **4 warnings to 0** with lwIP still pinned and unpatched. Recorded as
> [F72](port_review.md).

**Fix.** Point `LWIP_PLATFORM_ASSERT` at `serial_io_panic_write()` (written for exactly this
class of caller: polled, no FIFO, no RTOS object, safe at any priority) and halt. Both reviews
propose the same replacement. Expect ~9 KB of flash back as `printf` leaves the link, and D-a
below to become moot.

### N6 — The ISR logging contract is self-contradictory

**Where:** [`standard_output.c:33-46`](../Src/standard_output/standard_output.c#L33),
[`serial_io.c:120-131`](../Src/standard_output/serial_io.c#L120),
[`mac_drv.c:9-17`](../Drivers/EthDrv/mac_drv.c#L9)

Three comments written in the same review cycle disagree: `mac_drv.c` says its timeout reports
"must be callable from an ISR"; `standard_output.c` skips the mutex in interrupt context on that
basis, reasoning that "a garbled message beats a silently dropped one"; `serial_io.c` says
`serial_io_write()` "must NOT be called from an ISR".

The last is right, and the consequence is worse than garbling. From interrupt context the
CMSIS-RTOS2 shim rejects a blocking acquire outright
([`cmsis_os2.c:1969-1972`](../Drivers/CMSIS/CMSIS_RTOS_V2/cmsis_os2.c#L1969)):
`osSemaphoreAcquire(sem, osWaitForever)` returns `osErrorParameter` without taking anything.
`bfifo_push()` ignores the return and proceeds unserialised; with no free space it returns 0,
and `bfifo_push_all()`'s `while (pushLeft > 0)` **never terminates** — a CPU hang inside the ISR.

**Reachability today: none found.** `ETHHW_Transmit()` runs on `tcpip_thread`, the MDIO waits
run at init, and flexPTP's TX timestamp callback correctly branches on
`xPortIsInsideInterrupt()` (`Modules/flexptp/src/flexptp/task_ptp.c:543-551`). But the trap is
armed and advertised: `ETHHW_PrintRingBufStatus()` is a `MSG()`-based dumper whose call sites sit
inside `ETHHW_ProcessRx()` — which runs in the ETH ISR — commented out with a note recommending
its use, and N3's fix proposes another ISR-context `MSG()`.

> ✅ **Fixed** in P6.2, and the reachability judgement above was wrong in the board's favour
> only by luck. Reproduced on hardware with a scaffold printing from the ETH ISR: the board
> **hung hard** — 100% ping loss, dead console. Two corrections to the suggested fix, both
> forced by measurement:
>
> - **Routing to the polled writer does not work.** It steals the `TC` event HAL's DMA
>   completion path waits on, stranding the drain thread; neither `DMAT` nor `gState` closes
>   the window. Both attempts left the board pinging at 0% loss and never printing again.
>   Interrupt context is now **refused and counted**, and diagnostics latch for a thread to
>   report.
> - **`bfifo_push_all()`'s bail-out is not sufficient**, because the spin is inside
>   `bfifo_push()`'s wait loop. And making the failed acquire fatal there broke the CLI
>   outright: `serial_io_rx_callback()` pushes keystrokes into the input FIFO *from the USART3
>   ISR*, so that FIFO has always had an unsynchronised interrupt producer. Writer exclusion
>   is now a critical section — the only primitive both contexts can use.
>
> Recorded as [F68](port_review.md). `osinfo` now reports the discarded count.

**Fix.** Choose one contract and make the code enforce it: in `serial_io_write()`, route to
`serial_io_panic_write()` when `__get_IPSR() != 0`; give `bfifo_push_all()` a bail-out when
`bfifo_push()` returns 0 with no progress; then correct all three comments.

### N7 — `bfifo_pop()` hands the write token to a second writer

**Where:** [`blocking_fifo.c:49`](../Modules/blocking_io/blocking_fifo.c#L49),
[`blocking_fifo.c:142`](../Modules/blocking_io/blocking_fifo.c#L142)

`writeSem` does two jobs — "space exists" and "no other writer inside `bfifo_push()`" — and
`bfifo_pop()` releases it unconditionally from the consumer thread, without regard for a
producer currently mid-push. Writer A preempted between computing and storing `writeIdx`, a pop,
and writer B entering `bfifo_push()` is enough: both compute block sizes from the same
`writeIdx` and copy into the same region.

There is more than one producer, and the `MSG()` mutex does not cover them: `MSGraw()` and
`MSGchar()` go straight to `serial_io_write()`
([`standard_output.c:97-103`](../Src/standard_output/standard_output.c#L97)), and the CLI echoes
every keystroke that way from the `cli` thread while other threads log.

*Scope note:* the damage is confined to FIFO contents and indices — interleaved and partly
overwritten console output. The Gemini review's "memory corruption" overstates it; nothing
outside the FIFO's own buffer is written.

> ✅ **Fixed** in P6.1, with N1. `BFifo` gained a `writeMtx` held only inside `bfifo_push()`;
> `writeSem` is now purely the "space became available" signal, and every waiter re-checks the
> real occupancy after waking, so releasing it unconditionally from `bfifo_pop()` is safe.

**Fix.** Separate the two roles: a dedicated mutex for writer exclusion, the semaphore purely as
the space signal. Same object as N1 — do both in one pass over a 161-line file.

### N8 — CLI tokenizer: one-byte overflow and cross-thread reuse

**Where:** [`cli.c:78-106`](../Src/cliutils/cli.c#L78), [`cli.c:453`](../Src/cliutils/cli.c#L453)

`sLineBuf` is `CLI_LINEBUF_LENGTH + 1` (192 B) and `get_line()` can fill all of it; `pLineCpy`
inside `tokenize_cli_line()` is `CLI_LINEBUF_LENGTH` (191 B). A maximum-length line is therefore
`strcpy`'d one byte past the end of a `static` object — a 1-byte `.bss` overflow into whatever
the linker placed next.

Same function: `pLineCpy` is `static` and `strtok` keeps global state, yet `tokenize_cli_line()`
is entered from the `cli` thread (parsing a line) *and* from `cli_register_command()` on the
`ptp` thread (flexPTP registers ~19 commands during `reg_task_ptp()` and re-registers on
`ptp reset`) and the `init` thread (`cmd_init()`). `modifyMtx` protects the command *table*, not
this buffer.

> ✅ **Fixed** in P6.7 with N12. Verified on hardware: a 191-character line, a 250-character
> line and a 40-character token are all rejected normally with no fault, and the CLI is
> otherwise unchanged. Recorded as [F74](port_review.md).

**Fix.** Size it `CLI_LINEBUF_LENGTH + 1` and make it a local (192 B on a 2048 B stack);
`strtok_r` removes the third hazard.

### N9 — No memory barriers at descriptor handoff

**Where:** [`mac_drv.c:258-264`](../Drivers/EthDrv/mac_drv.c#L258),
[`mac_drv.c:632-640`](../Drivers/EthDrv/mac_drv.c#L632)

Both handoffs write the descriptor body and then the `OWN` bit with nothing in between, then
poke a tail register. The Cortex-M33 has a store buffer and the fabric gives no ordering
guarantee between these stores and the DMA's view of them; ST's own HAL places a `__DSB()`
before the tail-pointer write in `HAL_ETH_Transmit`.

Not an observed failure — the H5 fabric is simple, DCACHE is off (a build error until SRAM3 is
handled, per P4.14) and traffic is light. It belongs with N2: same root cause, descriptor memory
treated as ordinary variables. **Fix:** `__DMB()` before the `DES3` store, `__DSB()` before the
tail write, in both paths (see the N3 snippet).

> ✅ **Fixed** in P6.4 with N2. `dmb sy` and `dsb sy` are present in the generated code at
> `-Og`, `-O2` and `-O3`. See [F70](port_review.md).

### N10 – N16 — low and latent

| ID | Where | Summary and fix |
|---|---|---|
| **N10** | [`eth_drv_etherlib.c:139-142`](../Drivers/EthDrv/eth_drv_etherlib.c#L139) | On `dynmem_alloc()` failure the callback returns 0, which means "the receive thread owns it now" — but nobody took it, and `inFlight` stays set. Each failure retires one descriptor permanently; 24 of them is N3 by another route. The lwIP path gets this right ([`eth_drv_lwip.c:393-397`](../Drivers/EthDrv/eth_drv_lwip.c#L393)). **Fix:** return `ETHHW_RET_RX_PROCESSED`, and drop the `MSG()` — this runs in the ETH ISR (N6). Not compiled today; P5.21/P5.22 prepared this path precisely so its first build is not a debugging session. |
| **N11** | [`ethernet_etherlib.c:7-13`](../Src/ethernet/ethernet_etherlib.c#L7) | `char str[packet->payloadSize + 1]` — a VLA sized off the wire, memcpy'd into, no bound and no zero-length guard. 1.5 KB of stack at MTU, attacker-chosen in any case. **Fix:** fixed buffer + `MIN()`. |
| **N12** | [`cli.c:94`](../Src/cliutils/cli.c#L94), [`:102`](../Src/cliutils/cli.c#L102), [`:506`](../Src/cliutils/cli.c#L506) | `strncpy` leaves tokens ≥ `MAX_TOK_LEN` (32) and help lines ≥ `MAX_HELP_LEN` (192) unterminated; later `strcmp`s run into the next array element. Longest help today is ~75 chars, so latent — but nothing enforces that. **Fix:** copy `n-1`, terminate explicitly. |
| **N13** | [`mac_drv.c:143`](../Drivers/EthDrv/mac_drv.c#L143) | `WRITE_REG(eth->MACCR, mode \| ETH_MACCR_IPC)` clears `ACS` and `CST`, so the reported packet length — and every RX pbuf — includes the 4-byte FCS. ST's HAL enables both by default (`stm32h5xx_hal_eth.c:3042,3048`). Harmless in the paths exercised (lwIP trims IPv4 by header length, flexPTP parses by field), but a max frame occupies 1518 of 1528 bytes and any future code trusting `size` is wrong by 4. **Fix (optional):** add `ETH_MACCR_CST \| ETH_MACCR_ACS`, or say at the register write that it is deliberate. |
| **N14** | [`eth_drv_lwip.c:290-308`](../Drivers/EthDrv/eth_drv_lwip.c#L290) | `ETHHW_Transmit()` can drop the frame (and with N2, does so on the first try at `-O2`) but returns `void`; `low_level_output()` unconditionally does `LINK_STATS_INC(link.xmit); return ERR_OK;`. **Fix:** return `bool`, map onto `ERR_IF` + `link.drop`, following the reasoning already written for the over-length case (not `ERR_MEM`). |
| **N15** | [`lwipopts.h:238-239`](../Inc/lwipopts.h#L238), [`cc.h:42`](../Modules/lwip_port/arch/cc.h#L42) | `lwip/opt.h` includes `lwipopts.h` first and `arch/cc.h` later, so the bare `#define LWIP_PROVIDE_ERRNO` in `cc.h` undoes the `#undef`. Benign here, but the comment claims a configuration the build does not have. **Fix:** delete the define from `cc.h` — that is the port file this project owns. |
| **N16** | [`.github/workflows/ci.yml:35`](../.github/workflows/ci.yml#L35) | GitHub runs `run:` with `bash -e`, not `-eo pipefail`, so the pipeline exits with `tee`'s status and a compile or link failure does not fail the step. Nothing downstream catches it: the warning gate passes on a truncated log, the patch check is build-independent, and `upload-artifact` only *warns* on missing paths. **Fix:** `set -o pipefail`. One line, and it protects every other fix in this document. |

### N17 — The flash figure depends on the build-directory path *(new in this consolidation)*

Not in either source review; it emerged from reconciling their numbers. The two reviews and
`CMakeLists.txt` report three different Release sizes for the same tree:

| Source | Release flash | Build directory |
|---|---|---|
| [`CMakeLists.txt:166`](../CMakeLists.txt#L166) | 180 496 B | unrecorded |
| Opus v2 review | 180 504 B | long scratchpad path |
| Gemini v2 review | 180 120 B | unrecorded |
| **This consolidation** | **180 168 B** | `build/Release` |

Cause, confirmed with `strings` on the ELF: `configASSERT()` embeds `__FILE__`, and the FreeRTOS
kernel is fetched by `FetchContent` *into the build tree*, so six absolute paths of the form
`<builddir>/_deps/freertos_kernel-src/*.c` end up in `.rodata`. Rebuilding the identical tree at
`build/Release` instead of the long scratchpad path moved the figure by 336 bytes — consistent
with six strings times the path-length difference.

**Consequence.** The `-Og` / `-O2` / `-Os` comparison in `CMakeLists.txt` is still sound — those
three were built at one path, so the *deltas* (+456 B, −2 128 B) hold — but the absolute byte
counts are not reproducible by anyone else, and a future reader who rebuilds and gets a
different number has no way to know why. **Fix:** state the build directory alongside the
numbers, or quote deltas only. (A stricter fix — `-ffile-prefix-map` — would also shrink the
image slightly and make builds path-reproducible, but that is a change of scope, not a defect.)

### Documentation

- **D-a — `port_review.md` F63 gives the wrong reason.** It says the `options_out_len` bound
  "exists only inside `LWIP_ASSERT`, which compiles to nothing here — `LWIP_DEBUG` is not
  defined". `LWIP_ASSERT` is gated on `LWIP_NOASSERT`, not `LWIP_DEBUG`
  (`Modules/lwip/src/include/lwip/debug.h:115-120`), and `LWIP_NOASSERT` is defined nowhere in
  this project — so the assertion *is* compiled. The conclusion (the warning is unreachable as
  configured) stands, for a different reason: the assert prints and falls through, so GCC
  correctly sees the write happen regardless. Worth fixing because it is the same
  misunderstanding that let N5 survive the whole port — and because fixing N5 makes the warning
  disappear on its own.
- **D-b — `architecture.md:278`** describes `ETH_RX_DESC_CNT` as "descriptors the MAC's DMA can
  fill before it stalls". True as far as it goes; N3 is that the stall is a latch. One clause
  about the poll-demand requirement belongs here (subject to N3's verification caveat).
- **D-c — `README.md` and `.gitmodules` disagree on embfmt's origin** (`gitea.epagris.com`
  vs `github.com`). The `patches/embfmt/` workflow depends on the second.

### Housekeeping

- **H-1** Three untracked scripts in the repo root — `add_column.py`, `fix_table.py`,
  `fix_table2.py` — one-shot Markdown table editors, neither committed nor ignored. Delete or
  move under `tools/`.
- **H-2** The `docs` CI job checks out **without** submodules while `check_docs_links.py`
  requires every relative target to exist. No document links into `Modules/` or
  `Drivers/stm32h5xx-hal-driver/` today, so it passes — the first such link will fail CI while
  being valid locally. Add `submodules: recursive` to that job, or teach the checker to skip
  submodule paths. (Both v2 reviews and this one deliberately reference submodule files in prose
  rather than as links for this reason.)
- **H-3** `-Wl,--gc-sections` sits in `CMAKE_C_FLAGS`
  ([`cmake/gcc-arm-none-eabi.cmake:6`](../cmake/gcc-arm-none-eabi.cmake#L6)) and is passed to
  every compile as well as the link. Ignored by the compiler; inherited from the ST template.
- **H-4** `lwipopts.h` carries a full `LWIP_HTTPD_*` block
  ([`lwipopts.h:165-175`](../Inc/lwipopts.h#L165)) for an httpd that is not compiled.
- **H-5** `_Min_Heap_Size` in the linker script constrains nothing — `_sbrk()` bounds the newlib
  heap against `_estack - _Min_Stack_Size` only. A clause saying the symbol is unused would
  close it.

## 4. Claims rejected or downgraded

Everything in this section comes from the Gemini review and does **not** appear in the merged
list above. Each was checked against the code or the toolchain output.

| Claim | Verdict |
|---|---|
| **RM0481 §59.6.2 verbatim quotation** supporting N3 ("returning ownership … is not sufficient … Software must issue a Receive Poll Demand") | **Rejected as a citation.** The manual is not in this repository, the wording appears in no document here, and `port_review.md` elsewhere explicitly declines to quote RM0481 for exactly this reason. The underlying *rule* is plausible and the fix is cheap, so N3 is retained and marked **Unverified**; the quotation is not carried forward. Fabricated provenance is worse than no provenance — it removes the reason to go and check. |
| **"SMSC/Microchip `0x00800F`"** in N4 | **Wrong.** `phy_common.h:75` defines `EPHY_OUI_SMSC` as `0x0001F0`. The TI (`0x080028`) and Realtek (`0x000732`) values quoted alongside it are real, which makes the wrong one harder to spot. Corrected in N4 above. |
| **Thread stack table** with "Peak Frame" and "Headroom" columns for eight tasks, sourced to "`-fstack-usage` and high-water mark tracking" | **Rejected.** No hardware ran in either review, so no high-water marks exist; and `-fstack-usage` yields per-function frames, not per-task peaks — deriving those needs whole-call-graph analysis nobody performed. The single defensible number in that table (`Tmr Svc`, 104 B deepest frame in the `MSG()` path) is the one the Opus review actually measured, for the one question it was asked: whether the 1040-byte timer-daemon stack can afford `check_dhcp_state()` → `MSG()`. It can. |
| **`ptp` task stack = 4096 B** in that table | **Wrong.** `FLEXPTP_TASK_STACK_SIZE` is 2048 (`Modules/flexptp/src/flexptp/ptp_defs.h:124`), which is also what `architecture.md` records. |
| **N1 remediation snippet** (monotonic `used`/`free` only) | **Downgraded to incomplete.** Correct in direction, wrong as a drop-in — see the correction under N1. |
| **"N10–N15: … and duplicate C++ link flags"** in the action plan | **Rejected.** No such finding exists in N10–N15 or anywhere else in either review. |
| **Build sizes** (Debug 226 916 B, Release 180 120 B) | **Not reproducible, and neither are the Opus numbers.** See [N17](#n17--the-flash-figure-depends-on-the-build-directory-path-new-in-this-consolidation). Nobody is wrong; the metric is path-dependent. |

Two Gemini contributions are worth keeping, and are folded in above: the observation that N8's
overflow lands in `.bss` specifically, and the merge of N3's poll demand with N9's barriers into
a single `ETHHW_RestoreRXDesc()` edit.

## 5. What neither review can verify

Unchanged from the first review cycle and honestly recorded in `port_review.md`: 1PPS edge
quality, the X3 crystal path, EtherLib on this target, and any claim about behaviour at
sustained line rate. Added by this pass: the RBU resume rule in N3, which needs a reading of
RM0481.

N1 and N3 both have deterministic bench reproductions available — fill the console FIFO from a
tight loop; scaffold `rx_thread` to sleep 5 ms under load — and both are worth running once the
fixes are in. The first review's practice of verifying every fix on target is what makes its
record trustworthy, and these two findings are the ones most likely to be argued with.

## 6. Action plan

Ordered by (consequence × confidence) ÷ effort. Both source reviews propose substantially this
sequence; the differences are noted.

**Phase 1 — one file, two findings, highest certainty**
1. **N1 + N7** — rewrite the index arithmetic in
   [`blocking_fifo.c`](../Modules/blocking_io/blocking_fifo.c) so full and empty are
   distinguishable, and split writer exclusion from the space signal. All five functions change
   together.
2. **N4** — one-line initialiser; it is a boot-time HardFault on a plausible board revision.

**Phase 2 — the driver's ordering assumptions**
3. **N2 + N9** — `volatile` on the descriptor words plus the two barriers; re-run the three
   disassemblies and extend the F62 note in `CMakeLists.txt`.
4. **N3** — poll demand in `ETHHW_RestoreRXDesc()` (same edit as N9) and a real report in the
   AIS branch. Read RM0481 first if a copy is available; ship the poll demand regardless.

**Phase 3 — restore the diagnostics**
5. **N5 + D-a** — redefine `LWIP_PLATFORM_ASSERT` onto `serial_io_panic_write()` and halt;
   correct F63's explanation; take back ~9 KB.
6. **N6** — decide the ISR logging contract, enforce it in `serial_io_write()`, fix the three
   comments. Needed before N3's AIS report is written.
7. **N8** — CLI buffer size and `strtok_r`.

**Phase 4 — build, docs, latent**
8. **N16** first in this phase — it is one line and it protects everything above.
9. **N17, D-b, D-c, H-1…H-5** — documentation and hygiene.
10. **N10 – N15** — with the EtherLib bring-up, or as cleanup.

## 7. Implementation notes on the fixes

Raised against the consolidated list after the fact. Each was checked against the code; the
first two change how a fix should be written, the third does not.

### 7.1 — N1 / N7: the monotonic-index option carries a precondition

**Accepted, and it argues against that option being the default.**

Free-running indices masked on access (`mem[idx & (size - 1)]`, `used = writeIdx - readIdx`)
require a **power-of-two** buffer size, twice over: the mask is only equivalent to a modulo for
powers of two, and the unsigned subtraction only survives the 2³² counter wrap if the size
divides 2³². Both current buffers qualify — `SERIAL_IO_FIFO_LEN` = 16384 = 2¹⁴
([`serial_io.c:13`](../Src/standard_output/serial_io.c#L13)) and `CLI_INPUT_FIFO_BUFSIZE` = 512
= 2⁹ ([`cli.c:39`](../Src/cliutils/cli.c#L39)) — and these are the only two `BFifo` instances in
the project.

Two things follow that the observation itself does not cover:

1. **`BFifo` is a generic runtime API, not a pair of fixed buffers.** `bfifo_create(BFifo *,
   uint8_t *, uint32_t size)` accepts any size and stores it in a `uint32_t` field, so
   power-of-two would become an **unenforced precondition** — a future caller passing 1000
   would get silent corruption rather than a diagnostic, which is the same shape of trap as N1
   itself.
2. **The existing guard is already being thrown away.** `bfifo_create()` returns `bool`, and
   **both call sites discard it** ([`serial_io.c:27`](../Src/standard_output/serial_io.c#L27),
   [`cli.c:58`](../Src/cliutils/cli.c#L58)) — today that means a failed `osSemaphoreNew()` goes
   unnoticed too. Any masking rewrite must add `size && ((size & (size - 1)) == 0)` to that
   return value *and* make both callers check it.

**Recommendation: prefer the explicit-`count` variant.** It works for any size, needs no
precondition, no caller changes and no new failure mode, and it fixes N1 by construction —
`count` distinguishes full from empty because it is not derived from the indices. The masking
variant is faster in a way that cannot matter here: this FIFO is drained by a 115200-baud UART.
If masking is chosen anyway, land the size check and the caller checks in the same commit.

> ✅ **Implemented in P6.1 as recommended**, using two free-running totals whose modular
> difference is the count — so no power-of-two precondition was introduced. One correction to
> this section's advice on N7: the writer exclusion is a **critical section, not a mutex**. A
> mutex cannot be taken by `serial_io_rx_callback()`, which pushes keystrokes into the CLI
> input FIFO from the USART3 ISR, and making that failure fatal killed the CLI outright on
> hardware. See [F68](port_review.md).

### 7.2 — N3: the poll demand is context-safe; the open question is elsewhere

**Accepted as far as it goes, but it answers the easier of the two questions.**

The call-context claim is correct and verified: `ETHHW_RestoreRXDesc()` runs in the ETH ISR via
`ETHHW_ProcessRx()` ([`mac_drv.c:434-435`](../Drivers/EthDrv/mac_drv.c#L434)) *and* in thread
context via `rx_thread` ([`eth_drv_lwip.c:382-383`](../Drivers/EthDrv/eth_drv_lwip.c#L382)). A
single aligned 32-bit store to a peripheral register is atomic on this core, needs no critical
section, and both contexts write the identical value, so no interleaving is possible — the
`__DSB()` from N9 is the only ordering requirement. Adding the write to this function is safe.

What that does **not** settle is whether the write achieves anything, which is the part N3 marks
**Unverified**. Two questions need RM0481, not reasoning:

- Does returning ownership plus a tail-pointer write actually resume a suspended RX engine on
  this MAC — the rule the rejected Gemini quotation asserted without a checkable source?
- Does writing the tail pointer *the same value it already holds* (0) constitute a poll demand,
  or does this DMA trigger on a change? The driver deliberately parks the tail outside the ring
  ([`mac_drv.c:176`](../Drivers/EthDrv/mac_drv.c#L176), "tail pointer WON'T STOP"), so the fix
  necessarily rewrites 0 over 0. If the trigger is edge-on-write this is fine; if it is
  value-sensitive, the fix is inert and the finding needs a different remedy.

Both were answered instead by experiment, in P6.5: reception recovers with the poll demand and
never recovers without it, so returning ownership is indeed insufficient and writing the tail
pointer the value it already holds does constitute a poll demand on this MAC. N3 is closed on
measurement rather than on documentation.

### 7.3 — N9: barrier placement, and why `volatile` does not replace it

**No change: this restates what N9 and the N3 snippet already specify** — `__DMB()` before the
`DES3` ownership store, `__DSB()` before the tail-pointer write, in both the TX and RX paths.

One clarification is worth adding, because it is the natural misreading once N2 and N9 are
applied together: **`volatile` and the barriers solve different problems and neither substitutes
for the other.** `volatile` constrains the *compiler* — it stops the load being hoisted out of
the retry loop (N2) and stops the descriptor stores being reordered or elided relative to each
other. The barriers constrain the *hardware* — the store buffer and the fabric, which are free
to make the ownership store visible to the DMA before the descriptor body regardless of what the
compiler emitted. A reader who applies N2 and concludes N9 is now redundant has fixed half the
problem, and the remaining half is the one that fails intermittently and under load.
