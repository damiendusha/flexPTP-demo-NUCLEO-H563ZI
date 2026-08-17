# Second Review — flexPTP-demo-NUCLEO-H563ZI

**Reviewer:** Claude Opus 5 · **Date:** 2026-08-11 · **Tree:** `review/priority-1-fixes` @ `d359782`
· **Predecessor:** [`final_review_opus_5p0.md`](final_review_opus_5p0.md)

This is a second, independent pass. It was performed without consulting the first review until
the reading was done, so the findings below were reached from the code rather than by
re-checking a list. Where a finding touches ground the first review covered, that is stated
explicitly — everything marked **NEW** is not in any existing review document, and everything
marked **carried** is a previously-known item whose *current* state is different from what the
docs claim.

## Scope and method

Read in full: every first-party translation unit (`Src/`, `Inc/`, `Drivers/EthDrv/`,
`Modules/blocking_io/`), the build system (`CMakeLists.txt`, `cmake/`, `Middlewares/`,
`Modules/CMakeLists.txt`, `build.sh`), the linker script, the CI workflow, and the parts of
lwIP, flexPTP and the CMSIS-RTOS2 shim that first-party code depends on for its contracts.

Not read line by line: ST's HAL, the lwIP core, the flexPTP core — consulted where a
first-party contract pointed into them.

Beyond reading, the following was actually run:

| Check | Result |
|---|---|
| `./build.sh -t Debug` | clean, **0 warnings** |
| `./build.sh -t Release` | clean, 4 warnings, all `dhcp.c` (the documented [F63](../port_review.md)) |
| Flash/RAM | Release 180 504 B flash (8.61%), SRAM1 50.8%, SRAM3 22.9% |
| Disassembly of `ETHHW_Transmit()` at `-Og`, `-O2`, `-O3` | see [N2](#n2--the-tx-ownership-wait-is-compiled-away-at--o2-and-above-new) |
| `-fstack-usage` over the `MSG()` path | deepest frame 104 B; the `Tmr Svc` 1040 B stack is comfortable |
| Standalone simulation of the `BFifo` index arithmetic | see [N1](#n1--the-fifo-reports-empty-when-it-is-exactly-full-new) |
| Symbol/link analysis of the Release map + ELF | see [N5](#n5--every-lwip-assertion-is-a-silent-no-op-new) |
| `python3 tools/ci/check_docs_links.py` | 21 files, 0 broken links |

## Verdict

**The first review's 26 action items all landed, and they landed properly.** Spot-checking the
fixes rather than the summaries: the `MSG()` mutex, the bounded handshakes, the `inFlight`
marker, the `_Static_assert` on the state/ring adjacency, the CLI mutex single-exit, the
pointer rebasing in `cli_remove_command_locked()`, the escape-sequence bounds, the linker
section reorder and the fault reporters are all present and correct in the code, not merely
recorded as done. The commentary left behind at each site is unusually good: it explains the
failure mode, not just the change.

**What this pass found is a different layer.** The first review worked mostly on *unbounded
waits and silent hangs*. This one landed mostly on *invariants that hold only by accident*: a
ring-buffer that cannot distinguish full from empty, a retry loop that survives only because of
the optimisation level, a DMA suspend state nothing clears, an assertion mechanism that has
been disconnected since the port began. None of these were visible from the symptoms the first
review chased, and none of them are in it.

Four findings are worth fixing before the next hardware session. Nothing here contradicts the
project's central claim: the port is sound, the PTP path is correct, and the documentation is
honest about what has and has not been measured.

## Findings

Severity is "what happens when it fires", not "how likely it is to fire" — reachability is
stated separately in each item.

### High

#### N1 — The FIFO reports empty when it is exactly full *(NEW)*

[`blocking_fifo.c:26-40`](../../../../lib/blocking_io/blocking_fifo.c#L26) ·
[`blocking_fifo.c:42-95`](../../../../lib/blocking_io/blocking_fifo.c#L42)

`BFifo` tracks only `readIdx` and `writeIdx`, and derives occupancy from them:

```c
uint32_t bfifo_monotonic_write_index(const BFifo *fifo) {
    return (fifo->readIdx > fifo->writeIdx) ? (fifo->writeIdx + fifo->size) : fifo->writeIdx;
}
uint32_t bfifo_get_used(const BFifo *fifo) {
    return (bfifo_monotonic_write_index(fifo) - fifo->readIdx);
}
```

This is the classic two-index ring without a reserved slot or a count: `writeIdx == readIdx`
means *empty*, and there is no state that means *full*. `bfifo_push()` is happy to reach that
state, because it clamps the push to `bfifo_get_free()` and then wraps the index:

```c
uint32_t pushSize = MIN(freeSize, size);
...
uint32_t newWriteIndex = fifo->writeIdx + pushSize;
if (newWriteIndex >= fifo->size) newWriteIndex -= fifo->size;
```

Simulated against the real numbers (`size` = 16384, both indices at 0, one 16384-byte push):

```
after push(16384): pushed=16384 writeIdx=0 readIdx=0 used=0 free=16384
reader sees 0 bytes available (buffer physically holds 16384)
```

**Consequence.** The whole 16 KB console backlog is abandoned: `serial_io_thread()` wakes on
the read semaphore, `bfifo_read()` returns 0, `bfifo_pop(fifo, 0, …)` short-circuits without
advancing anything, and `HAL_UART_Transmit_DMA(…, 0)` returns `HAL_ERROR` — so the thread
spins at 1 kHz through the `osDelay(1)` path until a producer writes again. Meanwhile
`bfifo_get_free()` reports the buffer as entirely free, so producers immediately overwrite the
un-emitted data. The diagnostic channel loses 16 KB and says nothing about it.

A second, cheaper variant needs no full buffer at all: if a producer fills the free space
exactly in the window between the consumer's `bfifo_read()` and its `bfifo_pop()`, the pop
computes `popSize = MIN(0, size) = 0`, `readIdx` does not advance, and the same block is
transmitted twice.

**Reachability.** The console FIFO drains at 11.5 KB/s (115200 8N1), so filling 16 KB needs a
burst that outruns the UART — not normal PTP logging, but easily a `?` help dump chased by
several `ptp log … on` streams, or any future code that prints in a loop. The CLI *input* FIFO
([`cli.c:39-41`](../../../../lib/cli/cli.c#L39), 512 bytes) has the same defect and a far lower
bar: a 512-byte paste into the terminal reaches it directly.

Note that P5.4 made this **more** likely, not less. `bfifo_push_all()` loops until everything
is in, so a message larger than the remaining space now fills the buffer *exactly* by
construction — which is precisely the state that reads back as empty. The truncation P5.4
removed was the lesser of the two failures.

**Fix.** One of: keep an explicit `count`; or leave one byte unused (`free = size - used - 1`);
or make the indices monotonic and mask only on access. The last is cleanest and touches four
functions. Whichever is chosen, `bfifo_push()` must never be able to make `writeIdx` equal
`readIdx`.

#### N2 — The TX ownership wait is compiled away at `-O2` and above *(NEW)*

[`mac_drv.c:596-607`](../../../../lib/eth/mac_drv.c#L596) ·
[`mac_drv.h:29-52`](../../../../lib/eth/mac_drv.h#L29)

P2.3 bounded the descriptor-ownership wait in `ETHHW_Transmit()` with the driver's spin-count
idiom:

```c
for (uint32_t i = 0; i < ETHHW_HW_TIMEOUT_SPINS; i++) {
    if (ETHHW_DESC_OWNED_BY_APPLICATION(bd)) { descFree = true; break; }
}
```

`bd->desc.DES3` is written by the Ethernet DMA, but `ETHHW_Desc` declares its four words as
plain `uint32_t`. The loop body has no side effect, no function call and no barrier, so nothing
stops the compiler from hoisting the load. Measured, same flags as the project uses, only `-O`
changed:

| Level | Generated code |
|---|---|
| `-O0`, `-Og` | `ldr r2, [r4, #12]` **inside** the loop — 100 000 real retries |
| `-O2` | one `ldr r2, [r3, #12]`, then `blt` — the loop is gone |
| `-O3` | same, and the failure path is tail-called into `MSG` |

So above `-Og` the "bounded wait" is a single test: a descriptor still owned by the DMA means
the frame is dropped immediately, with no retry at all. That inverts the trade P2.3 was
verified against ("healthy hardware: 0 drops") — on a busy link the drop rate becomes whatever
fraction of transmits arrive while the ring is briefly behind.

This matters *now* because [`CMakeLists.txt:160-185`](../../../../CMakeLists.txt#L160) explicitly
advertises `-O2` and `-Os` as measured, hardware-tested, known-good alternatives, and wires
`-O2` into `RelWithDebInfo`. The measurement is real; what it did not cover is that the retry
loop is not present in those images.

Note this is the same class as [F62](../port_review.md) — "a workaround for a hardware access
constraint **must** be expressed with `volatile`" — applied to descriptor memory rather than to
the UID region. The lesson was drawn and then not extended to the rings.

**Fix.** Declare the DMA-visible words `volatile` in `ETHHW_Desc` (`volatile uint32_t DES0,
DES1, DES2, DES3;`). The extension struct is CPU-only and can stay as it is. Re-checking the
three disassemblies afterwards is a five-minute job and should be added next to the F62 note in
`CMakeLists.txt`.

#### N3 — A receive-buffer-unavailable stall is permanent, and the flag that reports it is discarded *(NEW)*

[`mac_drv.c:547-579`](../../../../lib/eth/mac_drv.c#L547) ·
[`mac_drv.c:253-265`](../../../../lib/eth/mac_drv.c#L253) ·
[`mac_drv.c:172-176`](../../../../lib/eth/mac_drv.c#L172)

The RX tail pointer is written exactly once, at init, with the comment `// tail pointer WON'T
STOP`:

```c
WRITE_REG(eth->DMACRDTPR, 0);
```

Pointing the tail outside the ring stops the DMA suspending on *tail*, which is the intent. It
does not stop it suspending on *ownership*. When the DMA reaches a descriptor whose `OWN` bit
is clear — every RX descriptor in flight, i.e. `rx_thread` behind by a full ring — the MAC sets
`DMACSR.RBU` and the receive process enters Suspended. Per RM0481 the software must return
ownership **and** issue a receive poll demand (a write to `DMACRDTPR`) to resume it.
`ETHHW_RestoreRXDesc()` does the first and nothing does the second, so RX stops for good: link
up, TX fine, PHY fine, no frames ever again.

The second half compounds it. The abnormal-interrupt branch reads:

```c
// Abnormal Interrupt Summary: previously wiped by the read-modify-writes above without
// anyone looking at it. Report it instead -- a DMA bus error or a stopped process is
// exactly the kind of fault this driver used to absorb in silence.
if (csr & ETH_DMACSR_AIS) {
    WRITE_REG(eth->DMACSR, (csr & (ETH_DMACSR_AIS | ETH_DMACSR_FBE | ETH_DMACSR_RBU | …)));
}
```

The comment promises a report; the code acknowledges the flags and returns. `RBU`, `FBE`,
`RPS` and `TPS` are still consumed in silence — the exact behaviour the comment says was
fixed. This is a comment/code divergence, and it hides the one event that explains a dead
receive path.

**Reachability.** Needs all 24 descriptors outstanding at once. `PBUF_POOL_SIZE` is 8, so
`rx_thread` drops frames long before the ring fills — that is why the documented 500 pps ping
tests pass. It is reachable through the queue-full path in `ETHHW_ReadCallback()` under a
burst, or by anything that delays `rx_thread` for ~2 ms at line rate. Low probability, terminal
consequence, and it presents as "the board stopped receiving" with no message — the signature
this project has spent the most effort eliminating.

**Fix.** Two lines. Poke the tail pointer after returning a descriptor:

```c
void ETHHW_RestoreRXDesc(ETHHW_DescFull *bd) { … bd->ext.inFlight = 0; WRITE_REG(ETH->DMACRDTPR, 0); }
```

(or once per pass in `ETHHW_ProcessRx()` / at the end of `rx_thread`'s loop), and make the AIS
branch print the flag word it is about to clear — it already runs in the ISR, so the same
constraint applies as for every other `MSG()` in this file; see [N6](#n6--msg-is-documented-as-isr-safe-in-one-file-and-isr-unsafe-in-another-new).

#### N4 — A recognised vendor with an unrecognised model dereferences NULL at boot *(NEW)*

[`phy_common.c:21`](../../../../lib/eth/phy_drv/phy_common.c#L21) ·
[`phy_common.c:382-451`](../../../../lib/eth/phy_drv/phy_common.c#L382)

```c
static const char *phyName = NULL;
…
    default:
        phyName = "\0";
        break;
    }
    return phyName[0] != '\0';
```

`phyName` is only assigned on two paths: a fully matched PHY, or an unmatched **OUI**. A PHY
whose OUI *is* in the table but whose model is not — any SMSC/Microchip part that is not
LAN8720A/8742A/8670, any TI part that is not DP83848/TC813/TD510E, any Realtek that is not
RTL8201F — falls through both inner `default: break;` arms with `phyName` still NULL, and the
return statement dereferences it. `phy_print_full_name()` does the same one line later.

That is a HardFault during `ETHHW_setupPHY()`, i.e. inside `low_level_init()` before the
network is up. The fault reporters added in P2 will at least name it now, which is exactly the
improvement they were for.

This is the sibling of the first review's **B1**, which fixed the *unknown-PHY* NULL callback
in `phy_get_link_status()`. The guard was added there; this site was not revisited, and it is
the more likely of the two on real hardware — vendor families change models far more often than
they change OUIs.

**Fix.** Initialise `static const char *phyName = "";` and delete the `phyName = "\0"` in the
outer default. Both remaining reads then behave.

### Medium

#### N5 — Every lwIP assertion is a silent no-op *(NEW)*

[`cc.h:83`](../../../../Modules/lwip_port/arch/cc.h#L83) · [`syscall.c:35`](../../../../lib/runtime/syscall.c#L35) ·
[`syscall.c:80-90`](../../../../lib/runtime/syscall.c#L80) · [`lwipopts.h:48-62`](../../../../Inc/lwipopts.h#L48)

The port's assertion macro is the lwIP stock one:

```c
#define LWIP_PLATFORM_ASSERT(x) do {printf("Assertion \"%s\" failed at line %d in %s\n", \
                                     x, __LINE__, __FILE__); } while(0)
```

Two things are wrong with it here, and they were verified in the linked image rather than
argued:

1. **It cannot print.** `printf` → `_write` → `__io_putchar`, which is declared
   `__attribute__((weak))` in `syscall.c` and **never defined anywhere in the project**. The
   linker resolves the undefined weak symbol to address 0 and relaxes the call away entirely:

   ```
   080041a0 <_write>:
     80041aa:  ldrb.w  r0, [r4], #1
     80041ae:  nop.w              <-- this is where "bl __io_putchar" was
     80041b2:  adds    r5, #1
   ```

   `_write` walks the buffer, emits nothing and returns `len`, so every byte is reported
   written and none leaves the chip.

2. **It does not stop.** There is no `while(1)`, no `abort()`, no halt. A failed assertion
   formats a string into the void and execution continues into the condition the assertion was
   guarding.

The cost is not theoretical. `Inc/lwipopts.h` deliberately enables
`LWIP_FREERTOS_CHECK_CORE_LOCKING`, `LWIP_TCPIP_CORE_LOCKING` and `LWIP_ASSERT_CORE_LOCKED()`,
which route every unlocked lwIP call into `sys_check_core_locking()` →
`LWIP_ASSERT("Function called without core lock", …)`. That check is the automated version of
the bug P3.6 fixed by hand in `check_dhcp_state()`. **It has never been able to fire.** The
next thread that touches lwIP without the core lock will be caught by nothing.

Side effect worth knowing: the map file shows `printf` pulled in by
`eth_drv_lwip.c.obj (printf)` — the two `LWIP_ASSERT`s in that file — which drags in the
float-capable `_vfprintf_r` (8 836 B) and its support. Roughly 9 KB of flash is spent on a
diagnostic path that is physically incapable of producing a character.

**Fix.** Redefine the macro in `cc.h` onto the machinery the project already has and trusts in
exactly this situation:

```c
#define LWIP_PLATFORM_ASSERT(x) do { serial_io_panic_write("\n\n*** FATAL: lwIP assertion: "); \
                                     serial_io_panic_write(x); \
                                     serial_io_panic_write("\nHalted.\n"); \
                                     for(;;){} } while(0)
```

`serial_io_panic_write()` was written for precisely this class of caller — polled, no FIFO, no
RTOS object, safe at any priority. Halting is the right behaviour for a violated lwIP
invariant, and it makes `LWIP_FREERTOS_CHECK_CORE_LOCKING` mean something. Expect ~9 KB of
flash back as `printf` drops out of the link.

#### N6 — `MSG()` is documented as ISR-safe in one file and ISR-unsafe in another *(NEW)*

[`standard_output.c:33-46`](../../../../lib/io/standard_output.c#L33) ·
[`serial_io.c:120-131`](../../../../lib/io/serial_io.c#L120) ·
[`mac_drv.c:9-17`](../../../../lib/eth/mac_drv.c#L9)

Three comments, written during the same review cycle, disagree:

- `mac_drv.c`: the bounded-handshake reports "must be callable from an ISR".
- `standard_output.c`: `msg_lock()` skips the mutex in interrupt context because "mac_drv.c
  documents its timeout reports as ISR-callable, and a garbled message beats a silently
  dropped one".
- `serial_io.c`: `serial_io_write()` "must NOT be called from an ISR, where the semaphore wait
  cannot complete".

The last one is right, and the consequence is worse than a garbled message. From interrupt
context the CMSIS-RTOS2 shim rejects a blocking acquire outright
([`cmsis_os2.c:1969-1972`](../../../../Drivers/CMSIS/CMSIS_RTOS_V2/cmsis_os2.c#L1969)):
`osSemaphoreAcquire(sem, osWaitForever)` returns `osErrorParameter` without taking anything.
`bfifo_push()` ignores that return and proceeds unserialised — and if the FIFO has no free
space it returns 0, at which point `bfifo_push_all()`'s `while (pushLeft > 0)` loop **never
terminates**. A diagnostic printed from an ISR against a full console FIFO hangs the CPU
inside the ISR.

**Reachability today: none that I can find.** No live path reaches `MSG()` from an ISR:
`ETHHW_Transmit()` runs on `tcpip_thread`, the MDIO waits run at init, and flexPTP's TX
timestamp callback correctly uses `xQueueSendFromISR` when
`xPortIsInsideInterrupt()` (`Modules/flexptp/src/flexptp/task_ptp.c:543-551`).
But the trap is armed and invited: `ETHHW_PrintRingBufStatus()` is a `MSG()`-based dumper whose
call sites sit inside `ETHHW_ProcessRx()` — which runs in the ETH ISR — commented out with a
note recommending its use, and [N3](#n3--a-receive-buffer-unavailable-stall-is-permanent-and-the-flag-that-reports-it-is-discarded-new)'s
suggested fix is another ISR-context `MSG()`.

**Fix.** Pick one contract and make the code enforce it. The cheap version: in
`serial_io_write()`, route to `serial_io_panic_write()` when `__get_IPSR() != 0`, and give
`bfifo_push_all()` a bail-out when `bfifo_push()` returns 0 twice in a row. Then correct the
three comments so they agree.

#### N7 — `bfifo_pop()` hands the write token to a second writer *(NEW)*

[`blocking_fifo.c:142`](../../../../lib/blocking_io/blocking_fifo.c#L142) ·
[`blocking_fifo.c:49`](../../../../lib/blocking_io/blocking_fifo.c#L49)

`writeSem` is created with count 1 and is used as a writer mutex: `bfifo_push()` acquires it on
entry and releases it on exit only if space remains. But `bfifo_pop()` releases it
unconditionally, from the consumer thread, with no regard for whether a producer is currently
inside `bfifo_push()`:

```c
osSemaphoreRelease(fifo->writeSem); // it's ceratin, that the FIFO can be written
```

Sequence: writer A acquires the token and is preempted between computing `writeIdx` and
storing it back; `serial_io_thread` pops and releases the token; writer B acquires it and
enters `bfifo_push()` concurrently. Both compute their block sizes from the same `writeIdx` and
memcpy into the same region; the later store of `fifo->writeIdx` wins. Output is interleaved
mid-byte-range and one message is partly overwritten.

There is more than one producer in practice, and the `MSG()` mutex does not cover them:
`MSGraw()` and `MSGchar()` go straight to `serial_io_write()`
([`standard_output.c:97-103`](../../../../lib/io/standard_output.c#L97)), and the CLI
echoes every keystroke that way from the `cli` thread while other threads log.

**Fix.** The write token is doing two jobs — "there is space" and "no other writer" — and they
need separating. Give `BFifo` a real mutex for writer exclusion and keep the semaphore purely
as the space signal. This is the same object as [N1](#n1--the-fifo-reports-empty-when-it-is-exactly-full-new);
both are worth fixing in one pass over a 161-line file.

#### N8 — `tokenize_cli_line()` overflows its copy by one byte, and is shared across threads *(NEW)*

[`cli.c:78-106`](../../../../lib/cli/cli.c#L78) · [`cli.c:453`](../../../../lib/cli/cli.c#L453)

```c
#define CLI_LINEBUF_LENGTH (191)
static char sLineBuf[CLI_LINEBUF_LENGTH + 1];      // 192 bytes -- correct
…
static char pLineCpy[CLI_LINEBUF_LENGTH];          // 191 bytes -- one short
strcpy(pLineCpy, pLine);
```

`get_line()` accepts up to `CLI_LINEBUF_LENGTH` characters and NUL-terminates at
`pStr[*pLen]`, so a full-length command line is 191 characters plus a terminator — 192 bytes
copied into a 191-byte buffer. One byte past the end of a `static` object, into whatever the
linker placed next.

Same function, second issue: `pLineCpy` is `static` and `strtok`'s state is global, yet
`tokenize_cli_line()` is called from two directions — the `cli` thread parsing a line, and
`cli_register_command()` from whichever thread registers commands. That is the `ptp` thread
(flexPTP registers ~19 commands during `reg_task_ptp()`, and re-registers on `ptp reset`) and
the `init` thread (`cmd_init()`). `modifyMtx` protects the command *table*; it does not protect
this buffer or `strtok`. A user pressing Enter while flexPTP is starting can have their line
tokenized from a buffer another thread is overwriting.

**Fix.** `pLineCpy[CLI_LINEBUF_LENGTH + 1]`, and make it a local (it is 192 bytes on a
2048-byte stack) or take `modifyMtx` in `process_cli_line()` too. `strtok_r` removes the third
hazard for free.

#### N9 — No memory barriers at descriptor handoff *(NEW)*

[`mac_drv.c:258-264`](../../../../lib/eth/mac_drv.c#L258) ·
[`mac_drv.c:632-640`](../../../../lib/eth/mac_drv.c#L632)

Both handoffs to the DMA write the payload/buffer fields and then the `OWN` bit, with nothing
in between:

```c
bd->desc.DES0 = bd->ext.bufAddr;
bd->desc.DES2 = opts | (len & 0x3FFF);
bd->desc.DES3 = ETH_DMATXNDESCRF_OWN | …;   // hands the descriptor over
…
WRITE_REG(eth->DMACTDTPR, 0);
```

The Cortex-M33 has a write buffer and the AHB/AXI fabric does not guarantee that these stores
become visible to the Ethernet DMA in program order. ARM's and Synopsys' guidance, and ST's own
HAL (`__DSB()` before the tail-pointer write in `HAL_ETH_Transmit`), is to place a barrier
between the descriptor body and the ownership store, and again before the tail-pointer poke.

This is a "has not bitten yet" finding rather than an observed failure — the H5's fabric is
simple, DCACHE is off (and is a build error until SRAM3 is handled, per P4.14), and the
project's traffic is light. It belongs with [N2](#n2--the-tx-ownership-wait-is-compiled-away-at--o2-and-above-new):
same root cause, that descriptor memory is treated as ordinary variables.

**Fix.** `__DMB()` before the `DES3` store in `ETHHW_Transmit()` and `ETHHW_RestoreRXDesc()`,
and `__DSB()` before writing `DMACTDTPR` / `DMACRDTPR`. Combined with the `volatile` from N2
this makes the ring accesses correct by construction rather than by fabric behaviour.

### Low

#### N10 — The EtherLib RX path retires a descriptor for every allocation failure *(NEW)*

[`eth_drv_etherlib.c:129-158`](../../../../Drivers/EthDrv/eth_drv_etherlib.c#L129)

```c
uint8_t *plBuf = dynmem_alloc(size);
if (plBuf == NULL) {
    MSG("malloc failed in ethdrv_input()!");
    return 0; // processing failed
}
```

`ETHHW_ProcessRx()` sets `bd->ext.inFlight = 1` *before* dispatching, and only restores the
descriptor when the callback returns `ETHHW_RET_RX_PROCESSED`. Returning 0 means "the receive
thread now owns it and will restore it" — but on this path nobody took it. Each allocation
failure permanently removes one descriptor from the ring; 24 failures and reception stops for
good, which is [N3](#n3--a-receive-buffer-unavailable-stall-is-permanent-and-the-flag-that-reports-it-is-discarded-new)
arrived at by a different route. The lwIP path gets this right — its queue-full branch returns
`ETHHW_RET_RX_PROCESSED` ([`eth_drv_lwip.c:393-397`](../../../../Drivers/EthDrv/eth_drv_lwip.c#L393)).

Not compiled today. P5.21/P5.22 prepared this path so that its first build is not a debugging
session; this is one more thing to fix while that is still cheap. **Fix:** return
`ETHHW_RET_RX_PROCESSED`, and drop the `MSG()` — this runs in the ETH ISR (see
[N6](#n6--msg-is-documented-as-isr-safe-in-one-file-and-isr-unsafe-in-another-new)).

#### N11 — A received packet sizes a stack array *(NEW)*

[`ethernet_etherlib.c:7-13`](../../../../Src/ethernet/ethernet_etherlib.c#L7)

```c
int udp_recv_cb(const Pckt *packet, PcktSieveLayerTag tag) {
    char str[packet->payloadSize + 1];
    memcpy(str, packet->payload, packet->payloadSize);
```

A variable-length array whose length comes straight off the wire, with no bound and no
`payloadSize == 0` guard, memcpy'd into. On a 1500-byte MTU this is a 1.5 KB stack allocation
inside whatever thread runs the sieve callback — survivable on a 2048-byte stack only by
luck, and an attacker-chosen number in any case. Same status as N10: EtherLib path, not
compiled, cheap to fix now. **Fix:** a fixed bounded buffer and `MIN(payloadSize, sizeof buf)`.

#### N12 — `strncpy` without termination in two CLI paths *(NEW)*

[`cli.c:94`](../../../../lib/cli/cli.c#L94) · [`cli.c:102`](../../../../lib/cli/cli.c#L102) ·
[`cli.c:506`](../../../../lib/cli/cli.c#L506)

`strncpy(&ppTok[i][0], pTok, tokMaxLen)` leaves the token unterminated when it is exactly
`MAX_TOK_LEN` (32) characters or longer; every later `strcmp` then runs off the end of the
token into the next array element. Likewise `strncpy(pCmd->pHelp, pCmdParsHelp, MAX_HELP_LEN)`
for a help line of 192 characters or more — the longest today is ~75, so this is latent, but
the longest is not enforced anywhere. **Fix:** copy `n-1` and terminate explicitly, in both
places.

#### N13 — The FCS is never stripped, so every frame is four bytes longer than it was on the wire *(NEW)*

[`mac_drv.c:143`](../../../../lib/eth/mac_drv.c#L143)

```c
WRITE_REG(eth->MACCR, mode | ETH_MACCR_IPC);
```

This clears `ACS` and `CST` along with everything else, so the MAC neither strips the CRC nor
the pad, and the packet length reported in `DES3` — which becomes `evt.data.rx.size` and then
`pbuf->tot_len` — includes the 4-byte FCS. ST's HAL sets both by default
(`stm32h5xx_hal_eth.c:3042,3048`).

Harmless in the paths this project exercises: lwIP trims IPv4 by header length, ARP ignores
trailing bytes, and flexPTP parses by field, so nothing misbehaves. It does mean every RX pbuf
carries four junk bytes, that a maximum frame occupies 1518 rather than 1514 bytes of the
1528-byte buffer, and that any future code trusting `size` as the frame length is wrong by 4.
**Fix (optional):** `mode | ETH_MACCR_IPC | ETH_MACCR_CST | ETH_MACCR_ACS`. If it is left as
is, say so at the register write — silence here reads as an oversight, which is what it
appears to be.

#### N14 — A dropped frame is reported to lwIP as sent *(NEW)*

[`eth_drv_lwip.c:290-308`](../../../../Drivers/EthDrv/eth_drv_lwip.c#L290)

`ETHHW_Transmit()` can drop the frame (descriptor busy — and with [N2](#n2--the-tx-ownership-wait-is-compiled-away-at--o2-and-above-new)
that happens on the first try at `-O2`), but it returns `void`, and `low_level_output()`
unconditionally does `LINK_STATS_INC(link.xmit); return ERR_OK;`. The stack cannot see the
drop, and the `link.drop` counter the over-length branch above is careful to bump is not
touched. **Fix:** return `bool` from `ETHHW_Transmit()` and map it onto `ERR_IF` plus the
matching `LINK_STATS_INC`, using the same reasoning already written out for the over-length
case (not `ERR_MEM`).

#### N15 — The `LWIP_PROVIDE_ERRNO` undef does not take effect *(NEW)*

[`lwipopts.h:238-239`](../../../../Inc/lwipopts.h#L238) · [`cc.h:42`](../../../../Modules/lwip_port/arch/cc.h#L42)

```c
/* LIBC ships errno */
#undef  LWIP_PROVIDE_ERRNO
```

`lwip/opt.h` includes `lwipopts.h` first and `arch/cc.h` only later (via `lwip/arch.h`), and
`cc.h` contains a bare `#define LWIP_PROVIDE_ERRNO`. The `#undef` therefore runs before the
definition it is meant to cancel, and lwIP provides its own `errno` after all — the opposite
of the stated intent. Benign in this build (nothing shares an `errno` across the boundary), but
the comment claims a configuration the code does not have. **Fix:** delete the `#define` from
`cc.h` — that is the port file this project owns.

#### N16 — CI cannot fail on a failed build *(NEW)*

[`.github/workflows/ci.yml:35`](../../../../.github/workflows/ci.yml#L35)

```yaml
run: ./build.sh -t ${{ matrix.config }} -b build/${{ matrix.config }} 2>&1 | tee build.log
```

GitHub runs `run:` steps with `bash -e`, **not** `bash -eo pipefail`. The exit status of the
pipeline is `tee`'s, which is always 0, so a compile or link error does not fail the step. The
following steps do not catch it either: the warning gate passes trivially on a truncated log,
the patch check is independent of the build, and `upload-artifact` only *warns* when its paths
are missing. A broken build can therefore produce a green tick.

**Fix:** add `shell: bash` + `set -o pipefail`, or drop the pipe and use
`tee` via `set -o pipefail` in the step body. One line, and it restores the whole point of the
job.

## Documentation accuracy

The docs are in good shape — the six cross-referencing documents agree with each other and with
the code on everything I spot-checked (the thread table, priorities and stack sizes in
`architecture.md`; the RX depth reasoning; the clock derivation in `ptp_clock.md`; the pin map;
the patch inventory). The link checker passes. Three corrections:

**D-a — `port_review.md` F63 states the wrong reason** ([`port_review.md:1367`](../port_review.md)).
"The bound on `options_out_len` exists only inside `LWIP_ASSERT`, which compiles to nothing
here — `LWIP_DEBUG` is not defined". `LWIP_ASSERT` is gated on `LWIP_NOASSERT`, not
`LWIP_DEBUG` (`Modules/lwip/src/include/lwip/debug.h:115-120`), and
`LWIP_NOASSERT` is not defined anywhere in this project — so the assertion *is* compiled. The
conclusion (the warning is unreachable as configured) still stands, but for a different reason:
the assert prints and *falls through*, so GCC correctly observes that the write happens
regardless. Worth fixing because it is the same misunderstanding that leaves
[N5](#n5--every-lwip-assertion-is-a-silent-no-op-new) unnoticed — and because if N5 is fixed
by making the assert halt, this warning disappears on its own.

**D-b — `architecture.md`'s RX table understates the stall** ([`architecture.md:278`](../architecture.md)).
"`ETH_RX_DESC_CNT` | 24 | descriptors the MAC's DMA can fill before it stalls" — correct as far
as it goes, but the stall is a latch, not a pause ([N3](#n3--a-receive-buffer-unavailable-stall-is-permanent-and-the-flag-that-reports-it-is-discarded-new)).
One clause about the poll-demand requirement belongs here.

**D-c — `README.md` and `.gitmodules` disagree on where embfmt lives.** The README's software
structure section points at `https://gitea.epagris.com/epagris/embfmt`; `.gitmodules` clones
`https://github.com/epagris/embfmt`. The `patches/embfmt/README.md` route depends on the
second. Pick one.

## Repository hygiene

- **Three untracked scripts sit in the repository root**: `add_column.py`, `fix_table.py`,
  `fix_table2.py` — one-shot Markdown table editors from the documentation work. They are
  neither committed nor ignored, so they show up in every `git status` and in any `git add -A`.
  Delete them or move them under `tools/`.
- **The docs CI job checks out without submodules** (`.github/workflows/ci.yml`, `docs` job),
  while `check_docs_links.py` verifies that every relative link target exists. No document
  currently links into `Modules/` or `Drivers/stm32h5xx-hal-driver/`, so it passes — but the
  first such link will fail CI while being perfectly valid locally. Either add
  `submodules: recursive` to that job or teach the checker to skip submodule paths. (This
  review deliberately links to `Modules/` targets only in prose, not as Markdown links, for
  this reason.)
- **`-Wl,--gc-sections` is set in `CMAKE_C_FLAGS`** ([`cmake/gcc-arm-none-eabi.cmake:6`](../../../../cmake/gcc-arm-none-eabi.cmake#L6))
  rather than in the link options, so it is passed to every compile invocation as well. Ignored
  by the compiler; cosmetic, inherited from the ST template.
- **`lwipopts.h` carries a full block of `LWIP_HTTPD_*` options** ([`lwipopts.h:165-175`](../../../../Inc/lwipopts.h#L165))
  for an httpd that is not compiled, and `MEMP_NUM_PARALLEL_HTTPD_CONNS` with it. Harmless,
  but it reads as configuration and is not.
- **`_Min_Heap_Size` in the linker script is decorative.** `_sbrk()` bounds the newlib heap
  against `_estack - _Min_Stack_Size` only, so the 0x200 reservation constrains nothing. The
  section-order comment explains the part that matters; a clause saying this symbol is unused
  would close it.

## Checked and found correct

Recorded so a third pass need not re-derive it:

- **The PTP timestamp path.** `PTP_MAIN_OSCILLATOR_FREQ_HZ` = 250 MHz, `PTP_INCREMENT_NSEC` =
  5, addend `0xCCCCCCCC`, PLL1Q enabled explicitly, `RCC_PLL1_VCIRANGE_1` for a 2 MHz VCI,
  `FLASH_LATENCY_5` at VOS0 — all internally consistent and consistent with `ptp_clock.md`.
  `ptphw_init()`'s increment-before-addend ordering matches its comment and RM0481.
- **`ptphw_gettime()`** re-reads `MACSTSR` after the nanosecond read and loops on
  disagreement — correct, and matches the upstream fix at `3601a6f`.
- **`SNAPTYPSEL = 0b01` with `TSEVNTENA`/`TSMSTRENA` clear** is the widest selection, which the
  runtime-switchable delay mechanism requires. The reasoning at the call site is sound.
- **`ETHHW_ProcessTx()`'s modular 16-bit delta** is right, including the wrap argument, and the
  clear-before-callback ordering closes F42 properly.
- **The `inFlight` marker** (F61) is set before dispatch and cleared after ownership returns, in
  that order, in both the ISR and thread paths.
- **The `_Static_assert` on `ETHHW_State` adjacency** does what its comment claims.
- **Interrupt priorities** are consistent with `configMAX_SYSCALL_INTERRUPT_PRIORITY` = 5: ETH
  at 7, USART3 and GPDMA1 at 12, all numerically above 5 and therefore permitted to call the
  `FromISR` API. `osMessageQueuePut(…, 0, 0)` from the ETH ISR takes the `FromISR` path.
- **MAC address derivation** — `0x02 | (UID[0] & 0xFE)` sets locally-administered and clears
  the group bit. Correct, and answers the first review's U7.
- **`insert_carriage_return()`** is now correct at the boundary (`len < maxLen`, buffer sized
  `+1`).
- **`check_dhcp_state()`** holds the core lock across both lwIP reads and copies the string out
  before releasing — correct, including keeping `MSG()` outside the lock.
- **`build.sh`'s patch handling** — reverse dry-run first, forward apply second, hard failure
  otherwise — is the right shape, and the CI step that re-checks with `--reverse` is a genuine
  guard rather than a restatement.
- **Stack depths.** Measured with `-fstack-usage`: the deepest frame anywhere in the `MSG()`
  path is 104 B (`pfn_double`), so the 1040-byte `Tmr Svc` stack that runs `check_dhcp_state()`
  → `MSG()` has ample headroom. No stack in the table looks tight.

## Not verifiable without hardware

Unchanged from the first review, and honestly recorded in `port_review.md`: the 1PPS output
edge quality, the crystal (X3) path, EtherLib on this target, and any claim about behaviour
under sustained line-rate load. [N1](#n1--the-fifo-reports-empty-when-it-is-exactly-full-new)
and [N3](#n3--a-receive-buffer-unavailable-stall-is-permanent-and-the-flag-that-reports-it-is-discarded-new)
both have deterministic bench reproductions available — fill the console FIFO from a tight loop;
scaffold `rx_thread` to sleep for 5 ms — and both are worth running once the fixes are in,
since the first review's practice of verifying each fix on target is what makes its record
trustworthy.

## Suggested order

1. **N1 + N7 together** — one pass over `blocking_fifo.c`, both fixes in the same 161 lines.
2. **N2 + N9** — `volatile` on the descriptor words plus the two barriers; re-run the three
   disassemblies and extend the F62 note in `CMakeLists.txt`.
3. **N3** — the tail-pointer poke and the AIS report (which needs N6's decision first).
4. **N4** — one-line initialiser; it is a boot-time HardFault on a plausible board.
5. **N5 + D-a** — redefine `LWIP_PLATFORM_ASSERT`, correct F63's explanation, and take the ~9 KB.
6. **N16** — one line of CI, and it protects everything above.
7. **N6, N8** — the contract decision and the CLI buffer, at leisure.
8. **N10 – N15** — with the EtherLib bring-up, or as cleanup.
