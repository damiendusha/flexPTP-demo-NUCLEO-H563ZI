# Final Review — flexPTP-demo-NUCLEO-H563ZI

An independent evaluation of the port at `df3ee3d` (Stage 20 signed off), covering
architecture, documentation fidelity, defects, open questions and suggested improvements.

**Method.** Full read of every first-party source file (`Src/`, `Inc/`, `Drivers/EthDrv/`,
`Modules/lwip_port/`, `Modules/blocking_io/`), the whole CMake tree, the linker script,
`build.sh`, the VS Code configuration, and all six documents in `docs/`. Cross-checked
against the pinned CMSIS device header, the HAL submodule, the CMSIS-RTOS2 shim and the
fetched FreeRTOS kernel. Both build configurations were rebuilt from clean to confirm the
claims made about them. Nothing was run on hardware; every claim below is static or
build-time.

**Build verification performed for this review:**

| Configuration | Result |
|---|---|
| `./build.sh -c -t Debug` | ✅ links, **0 warnings**, FLASH 223 032 B / SRAM1 133 328 B / SRAM3 74 888 B |
| `./build.sh -c -t Release` | ✅ links, **0 warnings**, FLASH 176 288 B / SRAM1 133 120 B / SRAM3 74 888 B |
| `patches/*` vs submodule working trees | ✅ both reverse-apply cleanly — the patch files are an exact record |

The zero-warning claim in `design_doc.md` holds. The F63 lwIP `-Wstringop-overflow`
warnings did **not** reproduce with toolchain 15.3.rel1 in either configuration.

---

## Table of Contents

- [Verdict](#verdict)
- [1. Is the architecture sound?](#1-is-the-architecture-sound)
- [2. Documentation vs code](#2-documentation-vs-code)
- [3. Bugs](#3-bugs)
- [4. What is unclear](#4-what-is-unclear)
- [5. Suggested improvements](#5-suggested-improvements)
- [Action item checklist](#action-item-checklist)

---

## Verdict

**The architecture is sound and the port is in good shape.** The layering is deliberate,
the hardware-specific surface is small and isolated, the third-party boundary is respected
almost everywhere, and the documentation is unusually good — the audit trail in
`port_review.md` is better than most commercial projects produce.

Three things temper that:

1. **The `pll1_q_ck` correction was applied to some documents but not all.** Four places —
   including a load-bearing comment in `main.c` and the "Resolved Design Decisions" table in
   the implementation plan — still assert that HCLK is the PTP clock. That is the exact
   belief that caused F53, preserved verbatim in the places a future maintainer is most
   likely to read first.
2. **The reproducibility of the two submodule patches depends on an undocumented manual
   step.** They live as uncommitted working-tree edits; `build.sh` does not apply them, and
   `git submodule update` destroys them. A fresh clone silently produces a build with the
   F59 servo bug and the F60/F66 formatting bugs back in it, and nothing warns.
3. **There is a cluster of real defects in the CLI and PHY layers** that the port review
   never reached, because its attention was — reasonably — on the PTP path. Two of them can
   hang or corrupt the console, and one is a guaranteed HardFault if the PHY is not
   recognised.

None of these is an architectural problem. All are addressable without restructuring.

---

## 1. Is the architecture sound?

### What is right

**The layering holds.** The dependency graph in `architecture.md` matches the code:
application → (`standard_output` | flexPTP | lwIP) → `EthDrv` → HAL/CMSIS/FreeRTOS. There
are no reach-arounds. `flexptp_options.h` really is the single configuration surface for
flexPTP, and `Src/flexptp_port/` really is the only file that knows about H563 PTP
registers outside `mac_drv.c`.

**The custom MAC driver is the right call, and is argued rather than assumed.** The
`mac_drv.c` vs `stm32h5xx_hal_eth.c` comparison in `design_doc.md` is honest about the
trade — the descriptor extension makes the TX-timestamp-to-message match structural rather
than pointer-keyed, and the cost was three defects (F54, F57, F61) that the HAL would not
have had. That is the correct way to record an inherited decision.

**Third-party code is genuinely unmodified where it says it is.** Verified: the lwIP
extension goes through `LWIP_PBUF_CUSTOM_DATA`, a supported hook; the F47 workaround is a
compile definition, not a patch; the CMSIS-RTOS2 change really is one `#include` line. The
two submodules that *are* patched carry permanent bug fixes, not instrumentation, and the
patch files match the working trees byte for byte.

**Failure modes were designed for, not just debugged.** `ETHHW_WaitBitsClear()` bounding
all six hardware handshakes and naming the failing one on timeout (F54) is the kind of fix
that pays for itself. The `inFlight` descriptor marker (F61) is a correct solution to a
genuine ISR/thread handoff race, and the ordering comments explain *why* each half must
happen in that order.

**Verified accurate against the code:** the thread-priority claim (the CMSIS-RTOS2 shim
does pass `osPriority_t` straight to `xTaskCreate`, and does substitute Normal for
`osPriorityNone` — `cmsis_os2.c:577,586`), the interrupt priority table, the memory-region
figures, the boot sequence, and the descriptor/register audit.

### Architectural weaknesses

**A1 — The console has no mutual exclusion, and it is the system's only diagnostic.**
`MSG()` formats into a single 2049-byte `static char lineBuf`
([standard_output.c:12](../Src/standard_output/standard_output.c#L12)) with no lock. It is
called from at least six threads: `cli`, `ptp`, `init`, `rx`, `tcpip_thread`
(`link_chg_cb`) and `Tmr Svc` (`check_dhcp_state`). The FIFO underneath *is* serialised —
but only inside `bfifo_push()`, long after the shared buffer has been written. Two
concurrent `MSG()` calls interleave into the same buffer. This matters more than it
normally would, because every diagnostic this project relies on goes through it.

**A2 — The three most likely runtime failures all end in a silent hang.**
`configASSERT` is `taskDISABLE_INTERRUPTS(); for(;;);`
([FreeRTOSConfig.h:142](../Inc/FreeRTOSConfig.h#L142)) — no output, interrupts off, console
dead. The project defines `configCHECK_FOR_STACK_OVERFLOW 2` and
`configUSE_MALLOC_FAILED_HOOK 1` but supplies **neither hook**, so both resolve to the
CMSIS-RTOS2 shim's weak stubs, which call `configASSERT(0)`
([cmsis_os2.c:2891,2908](../Drivers/CMSIS/CMSIS_RTOS_V2/cmsis_os2.c#L2891)). A stack
overflow, a heap exhaustion or any HAL assert therefore presents exactly as F53 did: dead
board, no explanation. This directly contradicts the policy stated in `mac_drv.c`'s own
header comment — *"A silent timeout would have been worse than the hang, so it is
deliberately loud."*

**A3 — Six of nine threads sit at the same priority, including `rx` and the console.**
`rx` drains the Ethernet ring into lwIP; `cli` and `serialio` produce console output. They
round-robin. Heavy CLI output can therefore delay RX draining, which is precisely the
window the F61 `inFlight` marker exists to make safe — the marker closes the correctness
hole, but the descriptors still stay in flight longer. `rx` should sit above the console
threads (and below `tcpip_thread`).

**A4 — RX queue depths are mismatched three ways.** `ETH_RX_DESC_CNT` = 24,
`RX_MSG_Q_LEN` = 32 ([eth_drv_lwip.c:111](../Drivers/EthDrv/eth_drv_lwip.c#L111)),
`PBUF_POOL_SIZE` = 8 ([lwipopts.h:111](../Inc/lwipopts.h#L111)). The queue can hold more
events than there are descriptors to fill it, and four times more than there are pbufs to
service them. When `pbuf_alloc()` fails in `rx_thread` the frame is dropped *after* the
descriptor has already been committed. Nothing is broken, but the sizing is not a
considered set.

**A5 — `ETHHW_GetState()` is enforced by a comment.**
`((ETHHW_State *)eth->DMACRDLAR) - 1` ([mac_drv.c:198-200](../Drivers/EthDrv/mac_drv.c#L198))
requires `ETHHW_State` to immediately precede the RX ring in memory. That is guaranteed
today only by struct field order in `eth_drv_lwip.c:46-50` and a comment in `mac_drv.h:26`.
A `_Static_assert` on `offsetof` would make it structural.

**A6 — One unbounded wait survived the F54 sweep.** `ETHHW_Transmit()` spins on
`while (!ETHHW_DESC_OWNED_BY_APPLICATION(bd)) {}`
([mac_drv.c:552](../Drivers/EthDrv/mac_drv.c#L552)). F54 enumerated six waits and bounded
all six; this is a seventh, of the same shape, in the same file, directly under a header
comment asserting that none may spin forever.

**A7 — The EtherLib path is carried but never compiled.** Acknowledged in `design_doc.md`,
and the analysis of the missing `.ETHLibPool` placement is excellent. Worth noting the
weight: a submodule, `Src/etherlib_options.c` (compiled unconditionally, even in LWIP
builds), `Inc/etherlib_options.h`, `Drivers/EthDrv/eth_drv_etherlib.c` and
`Src/ethernet/ethernet_etherlib.c` — none of which has ever been built for this target.

**A8 — Clock configuration is fire-and-forget.** `HAL_RCC_OscConfig()` and
`HAL_RCC_ClockConfig()` return values are discarded
([main.c:70,97](../Src/main.c#L70)), and `Error_Handler()` is declared at
[main.c:31](../Src/main.c#L31) but **defined nowhere in the project** and never called. A
PLL that fails to lock produces a board running at an unexpected frequency with a detuned
PTP clock — the F45/F6 failure mode, unreported.

---

## 2. Documentation vs code

The documentation is a strong asset, which is why the drift matters: a reader trusts it.
Findings are ordered by how likely they are to mislead.

### D1 — The `pll1_q_ck` correction is incomplete (highest impact)

`ptp_clock.md` and `architecture.md` were correctly updated after F53. Four places were
not, and each still asserts the belief that caused F53:

| Location | Text |
|---|---|
| [`Src/main.c:83-90`](../Src/main.c#L83) | *"HCLK is also the Ethernet PTP timestamp clock on the STM32H563 … dividing here would silently detune the PTP clock"* — 20 lines below the correct comment at `main.c:61` |
| [`docs/design_doc.md:172`](design_doc.md) | *"The timestamp unit is clocked from **HCLK at 250 MHz**"* — contradicting lines 35, 122 and 186 of the same document |
| [`docs/design_doc.md:196`](design_doc.md) | *"which exceeds the device's 250 MHz HCLK ceiling"* |
| [`docs/implementation_plan.md:53`](implementation_plan.md) | Resolved Design Decisions: *"**PTP Clock** — **HCLK 250 MHz, 5 ns increment.** The H563 has no ETH PTP clock mux, so HCLK is the only source"* |
| [`docs/port_review.md:1548`](port_review.md) | F45 rationale: *"Given that HCLK is also the PTP clock on this part, that would have detuned the PTP clock…"* |

### D2 — `design_doc.md`'s clock tree documents the H7 spellings that F45 warns are traps

The clock-tree diagram at [`design_doc.md:131-132`](design_doc.md) reads:

```
AHBCLKDivider  = RCC_HCLK_DIV1     →  HCLK   = 250 MHz  ← also ptp_ker_ck
APB1/2/3 Div   = RCC_APBx_DIV2     →  PCLK   = 125 MHz
```

The code uses `RCC_SYSCLK_DIV1` and `RCC_HCLK_DIV2`
([main.c:91-95](../Src/main.c#L91)). `RCC_APBx_DIV2` does not exist on the H5 at all, and
`RCC_HCLK_DIV1` in the AHB slot is **exactly finding F45** — the spelling that compiles
silently and writes a PPRE-encoded value into the HPRE field. The Stage 10 log in the same
document (line 693) gets it right. Anyone copying from the diagram reintroduces F45.

### D3 — `architecture.md` states F42 is still broken

[`architecture.md:375-378`](architecture.md): *"What remains broken (F42) is downstream of
this diagram, inside flexPTP's message buffer: the timestamp is delivered but never matched
back to the Delay_Req that requested it."* F42 was resolved — the site switch was
discarding Delay_Resp and **nothing in the port was at fault**. The paragraph accuses
flexPTP of a defect it does not have.

### D4 — `architecture.md` states the addend wait is unbounded

[`architecture.md:239-241`](architecture.md): *"`ETHHW_SetPTPAddend()` waits on
`MACTSCR.TSADDREG` with no timeout; if the PTP timestamp unit is not running, that wait
never returns."* F54 bounded it; it now returns `bool` and reports.

### D5 — `ptp_clock.md` says the MDIO shift bug still needs fixing

[`ptp_clock.md:252-255`](ptp_clock.md): *"`mac_drv.c` currently contains a shift bug in
this register write … that must be fixed as part of the MAC driver port."* Fixed at Stage
12 (F8); the code at [mac_drv.c:113-117](../Drivers/EthDrv/mac_drv.c#L113) carries the
explanation.

### D6 — "A 4 ns increment is unreachable" survives in three places after being retracted

`ptp_clock.md` itself retracts this: with `PLLQ = 1`, `pll1_q_ck` is 500 MHz and K = 2.0 —
*"a valid 4 ns configuration on paper"*, not adopted for engineering reasons. Still
asserted as impossibility in:

- [`README.md:268`](../README.md) — *"why a 4 ns increment is unreachable on this part"*
- [`ptp_clock.md`](ptp_clock.md) — the section heading and TOC entry, *"Why a 4 ns increment is unreachable"*
- [`port_review.md:95`](port_review.md) — F5 row: *"A 4 ns increment is unreachable on this device | Critical"*, with no correction note (unlike F3, which has one)

### D7 — `port_review.md` still opens by saying nothing has run on hardware

[`port_review.md:58`](port_review.md): *"Still not done: **no hardware**. Nothing here has
executed on a device. Every claim in this document is a static or build-time one."* The
same document then contains F51–F66, all hardware findings, including oscilloscope-free but
silicon-verified register dumps.

### D8 — `port_review.md`'s phase table stops at F50

[`port_review.md:65-68`](port_review.md) lists *"Static review F1–F43 (43) / Implementation
F44–F50 (7)"*. There are 66 findings, and the hardware bring-up phase — which produced the
most valuable ones — has no row.

### D9 — F42's body still declares itself open

[`port_review.md:750-754`](port_review.md) has a `### Status` heading followed by *"F42 is
**open**"*,
retained beneath the resolution. The living-document style is defensible, but a bare
"**open**" status heading below a "RESOLVED" summary is the one thing a skimmer will
misread.

### D10 — embfmt patch count disagrees between README and architecture.md

[`README.md:209`](../README.md) says *"two formatting fixes in embfmt"*;
[`architecture.md:52`](architecture.md) says *"three formatting fixes"*. There are **four**
distinct defects (`%ll` parsing, NaN rendering, `%e` pre-normalisation rounding, `%f`
rounding-carry remainder) across **two** findings (F60, F66) in **one** patch file.

### D11 — `design_doc.md` lists F54 as open

[`design_doc.md:851-852`](design_doc.md), in the *"Hardware bring-up (partial Stage 20)"*
section, carries an **Open:** heading for F54 with *"Deliberately not yet fixed"*. Resolved
since. This is stage-log history, but "Open" is a status claim.

### D12 — Smaller items

| Where | Says | Actually |
|---|---|---|
| [`mac_drv.h:127`](../Drivers/EthDrv/mac_drv.h#L127) | *"currently every frame received gets timestamped"* | `TSENALL` is **not** set; only PTP messages selected by `SNAPTYPSEL = 0b01` are |
| [`mac_drv.h:135`](../Drivers/EthDrv/mac_drv.h#L135) | *"Time quantum is 0.467 ns"* | `TSCTRLSSR` is set (digital rollover), so `SSINC` is in whole nanoseconds. 0.467 ns is the binary-rollover figure |
| [`architecture.md:220`](architecture.md) | thread named `tcpip_thread` | `TCPIP_THREAD_NAME` is `"TCP/IP"` ([lwipopts.h:277](../Inc/lwipopts.h#L277)) |
| [`architecture.md:390`](architecture.md) | FLASH 221 552 B | Stage 15 figure; current build is 223 032 B (correctly labelled, but two numbers circulate) |
| [`FreeRTOSConfig.h:2`](../Inc/FreeRTOSConfig.h#L2) | *"FreeRTOS V202212.00"* | kernel pinned at `V11.3.0` |
| [`implementation_plan.md:224`](implementation_plan.md) | Stage 7 removes the dead `Middlewares/Inc` include (F32) | The root one went; [`Middlewares/CMakeLists.txt:14`](../Middlewares/CMakeLists.txt#L14) still adds a non-existent relative `Inc` |

---

## 3. Bugs

Ordered by severity. Line references are to the current tree.

### High

**B1 — Unrecognised or absent PHY is a guaranteed HardFault.**
[`phy_common.c:514-517`](../Drivers/EthDrv/phy_drv/phy_common.c#L514)

```c
const PHY_LinkStatus *phy_get_link_status() {
    phyFetchLinkStatusCb(&linkStatus);   /* no NULL check */
    return &linkStatus;
}
```

`phyFetchLinkStatusCb` is only assigned when `phy_populate_handlers()` matches the OUI and
model. `phy_get_event()` and `phy_refresh_link_status()` both guard against NULL; this one
does not. `fetch_link_properties()` calls it from the `phy` thread every 500 ms, so an
unknown PHY faults within half a second of boot.

It is reachable through a second defect: `phy_sweep_addresses()`
([phy_common.c:63-74](../Drivers/EthDrv/phy_drv/phy_common.c#L63)) treats *any* value other
than `0xFFFF` as a PHY. `ETHHW_ReadPHYRegister()` returns non-zero without writing
`*pRegValue` when MDIO is busy or times out, and `regVal` is pre-initialised to 0 — so a
completely dead MDIO bus "finds" a PHY at address 0, reads an ID of 0, fails to populate
handlers, and faults. Since F54 made MDIO waits time out rather than hang, this is now the
*expected* path for a wiring or power fault.

**B2 — `cli_remove_command()` returns while holding the mutex.**
[`cli.c:517-534`](../Src/cliutils/cli.c#L517)

```c
void cli_remove_command(int cmdIdx) {
    osMutexAcquire(modifyMtx, osWaitForever);
    if (cmdIdx + 1 > sCliCmdCnt || cmdIdx < 0) {
        return;                       /* mutex never released */
    }
    ...
```

Any out-of-range index permanently wedges the command database: every later
`cli_register_command()` and `cli_remove_command()` blocks forever, taking the calling
thread with it. `cli_remove_command_array()` walks a caller-supplied list terminated by
`-1`, so a stale handle is enough.

**B3 — Removing a command leaves dangling pointers into the next command's help buffer.**
[`cli.c:527`](../Src/cliutils/cli.c#L527) and [`cli.c:120,141,160-166`](../Src/cliutils/cli.c#L120)

`struct CliCommand` stores `pHelp[MAX_HELP_LEN]` **inline**, with `pHint` and `pPadding`
pointing into it. `cli_remove_command()` shifts entries with
`memcpy(&spCliCmds[i], &spCliCmds[i + 1], sizeof(struct CliCommand))`, which copies the
pointer values verbatim — so after the shift, `spCliCmds[i].pHint` and `.pPadding` point
into `spCliCmds[i + 1]`'s buffer. `tidy_format_commands()` then *skips* re-deriving them
(`if (pCmd->pHint == NULL)` is false) and writes the padding run through the stale
`pPadding`, corrupting a different command's help text.

Reachable on the normal path: `cli_register_command()` removes a duplicate whenever the
same command is registered twice, and `ptphw_register_cli_commands()` re-registers
`ptp pps` on every `ptphw_init()`.

**B4 — `MSG()` is not thread-safe.** See [A1](#architectural-weaknesses). Single shared
2049-byte static, six calling threads, no lock.

### Medium

**B5 — Arrow-key history acts on the previous keypress, and a long escape sequence
overflows the stack.** [`cli.c:315,357-398`](../Src/cliutils/cli.c#L357)

```c
char escParam[4];                      /* uninitialised */
...
if ((c == '~') || ((c >= 'A') && (c <= 'Z'))) {
    escString = false;
    switch (escParam[0]) {             /* READ before write */
    case 'A': ... case 'B': ...
}
if (c != '~') {
    escParam[paramIdx] = c;            /* written afterwards */
}
```

For a three-character sequence (`ESC [ A`), `escParam[0]` is read one iteration before it
is assigned. The first arrow press in each `get_line()` call switches on uninitialised
stack; every later press acts on the *previous* sequence's terminator. Up/Down history
therefore works with a one-press lag.

Separately, `paramIdx = escPos - 2` is unbounded — the loop only leaves escape state on
`~` or an uppercase letter. `ESC [ 0123456789` writes ten bytes into a four-byte stack
array. Reachable from anything sending escape sequences down the serial line.

**B6 — The PPS frequency codes look off by one, on the one path with no hardware
evidence.** [`mac_drv.h:108-125`](../Drivers/EthDrv/mac_drv.h#L108),
[`ptp_port_stm32h563.c:18-73`](../Src/flexptp_port/ptp_port_stm32h563.c#L18)

The driver writes `PPSCTRL` directly from this enum, with `PPSEN0` clear (simple mode):

```c
ETHHW_PTP_PPS_OFF   = 0,
ETHHW_PTP_PPS_1Hz   = 1,
ETHHW_PTP_PPS_2Hz,          /* … up to ETHHW_PTP_PPS_16384Hz = 15 */
```

On this MAC (Synopsys DWC_ether_qos, shared with the H743) `PPSCTRL = 0000` selects **1 Hz
output**, not "off" — there is no off code in simple mode; the output is disabled by
clearing the enable, not by a frequency code. If that holds, every entry is shifted by one:
`ptp pps 1` writes `PPSCTRL = 1` and emits **2 Hz** while reporting *"PPS frequency:
1 Hz"*, and `ptp pps 0` writes `PPSCTRL = 0` and emits **1 Hz** while reporting *"PPS
output is turned off."*

The `ETHHW_PTP_PPS_OFF = 0` naming is the strongest tell and is independent of the
1 Hz/2 Hz question. This is inherited from the H743 driver and has never been observed on a
scope in either project — it lands exactly on
[`implementation_plan.md`'s "1PPS output is unmeasured"](implementation_plan.md#blocked-on-equipment).
**Verify `PPSCTRL` against RM0481 before treating the CLI's reported frequency as truth**;
the LED-on-PG8 test the plan already suggests would settle 1 Hz vs 2 Hz in a minute.

**B7 — `ETHHW_Transmit()` spins unbounded.** See [A6](#architectural-weaknesses).
A stalled TX DMA hangs whichever thread is transmitting — under `LWIP_TCPIP_CORE_LOCKING`
that is `tcpip_thread`, holding the core lock, which takes the whole network stack with it.

**B8 — `insert_carriage_return()` can write one byte past its buffer.**
[`standard_output.c:14-29`](../Src/standard_output/standard_output.c#L14)

The guard `(i < len) && (len <= maxLen)` is evaluated at loop entry, but the body increments
`len` and then writes `str[len]`. With `maxLen` = 2048 and a message that reaches that
length through `\r` insertions, `str[2049]` is written into a `char[2049]`.

**B9 — Register-count clamp uses the wrong constant.** [`cli.c:468-469`](../Src/cliutils/cli.c#L468)

```c
tokenize_cli_line(pCmdParsHelp, pCmd->ppTok, MAX_TOK_LEN,
                  (cmdTokCnt > TOK_ARR_LEN ? TOK_ARR_LEN : cmdTokCnt), &tokCnt);
```

`pCmd->ppTok` is `CliToken_Type[MAX_TOKEN_N]` = 8 entries, but the clamp is against
`TOK_ARR_LEN` = 16. A command registered with 9–16 tokens writes past `ppTok` into
`tokCnt`, `minArgCnt` and `pHelp`. No current command exceeds three tokens, so this is
latent — but the clamp names the wrong limit.

### Low

**B10 — RX event timestamps are uninitialised when no timestamp was captured.**
[`mac_drv.c:397-424`](../Drivers/EthDrv/mac_drv.c#L397) declares `ETHHW_EventDesc evt;` on
the stack and only assigns `data.rx.ts_s` / `ts_ns` inside `if (tsFound)`.
[`eth_drv_lwip.c:300-301`](../Drivers/EthDrv/eth_drv_lwip.c#L300) copies them into
`p->time_s` / `p->time_ns` unconditionally. Harmless today — only PTP frames reach
`nsd_lwip.c`, and those carry timestamps — but it is an uninitialised read, and it would
turn "hardware failed to timestamp this frame" into a plausible-looking wrong value rather
than an obvious zero. Same class of trap as F59's NaN-printing-as-`0.0000`.

**B11 — `phy_setup_int_DP83848()` reads where it means to write.**
[`phy_common.c:88-89`](../Drivers/EthDrv/phy_drv/phy_common.c#L88)

```c
regVal = EPHY_DP83848_MICR_INT_OE | EPHY_DP83848_MICR_INTEN;
READ(EPHY_DP83848_MICR, regVal);   /* should be WRITE */
```

The computed value is immediately overwritten by the read, so the DP83848's global
interrupt enable is never set. Not this board's PHY, but shipped code.

**B12 — `check_dhcp_state()` touches lwIP without the core lock.**
[`ethernet_lwip.c:26-36`](../Src/ethernet/ethernet_lwip.c#L26) runs on the `Tmr Svc` thread
and calls `dhcp_supplied_address(&intf)` and reads `netif_default->ip_addr` with
`LWIP_TCPIP_CORE_LOCKING = 1` and no `LOCK_TCPIP_CORE()`. Low practical risk (word-sized
reads), but it violates lwIP's threading contract, and it is the one place in the project
that does — `phy_thread` correctly goes through `tcpip_callback()`, and flexPTP's
`nsd_lwip.c` correctly brackets its sends with `LOCK_TCPIP_CORE()`.

**B13 — `serial_io_thread()` can block forever on a DMA error.**
[`serial_io.c:134-135`](../Src/standard_output/serial_io.c#L134) ignores
`HAL_UART_Transmit_DMA()`'s return and then waits `osWaitForever` on a completion flag that
only the success path sets. This is precisely the console-death signature described in F51.

**B14 — Console output is silently truncated when the FIFO fills.**
`serial_io_write()` ([serial_io.c:120-122](../Src/standard_output/serial_io.c#L120)) drops
`bfifo_push()`'s return value, which is the *actually pushed* size. `bfifo_push_all()`
exists for exactly this.

**B15 — `ETHHW_AuxTimestampCh()` assigns instead of OR-ing.**
[`mac_drv.c:797-806`](../Drivers/EthDrv/mac_drv.c#L797) — `tmpreg = (en) << (ch + 4);`
clears every other auxiliary channel. Unused in this project.

**B16 — `rx_thread` references an out-of-scope `netif`.**
[`eth_drv_lwip.c:303-314`](../Drivers/EthDrv/eth_drv_lwip.c#L303) passes `netif` to
`MIB2_STATS_NETIF_*`, and no such identifier exists in that function. It compiles only
because MIB2 statistics are disabled and the macros expand to nothing; enabling them breaks
the build.

**B17 — `phy_read_reg()` / `phy_write_reg()` invert their result.**
[`phy_common.c:537-539`](../Drivers/EthDrv/phy_drv/phy_common.c#L537) return `bool` from
`READ`/`WRITE`, which return `uint32_t` with **0 meaning success**. Currently unused.

**B18 — `phy_read_range()` steps by 2 through MDIO register addresses**
([phy_common.c:525-533](../Drivers/EthDrv/phy_drv/phy_common.c#L525)), skipping every odd
register, and `phy_read_all_regs()` passes `EPHY_MDIO_MAX_ADDR` (a PHY *address* limit) as
a register count. Diagnostic only.

**B19 — `DEBUG` is defined in Release builds.**
[`CMakeLists.txt:93`](../CMakeLists.txt#L93) puts `DEBUG` in the unconditional
`compile_DEFS`, alongside the correct `$<$<CONFIG:Debug>:DEBUG>` at line 132.

**B20 — Only two of the five presets are configured.** `Release` maps to `-Og -g0`
([CMakeLists.txt:157](../CMakeLists.txt#L157)) — a debug-oriented level for a preset named
Release. `relWithDebInfo` and `minSizeRel` receive no project optimisation flags at all,
falling back to CMake defaults; neither has been built or tested.

**B21 — `get_line()`'s left-trim never reaches the caller.**
[`cli.c:411-413`](../Src/cliutils/cli.c#L411) advances a local `pStr` pointer. `task_cli()`
then passes the *original* `sLineBuf` to `process_cli_line()`, so a command typed with
leading whitespace fails to match. Right-trim works (it writes in place).

**B22 — `MSGchar(int c)` takes the address of an `int` and sends one byte.**
[`standard_output.c:45-47`](../Src/standard_output/standard_output.c#L45) — correct only on
little-endian. Also `last_char` (line 31) is written and never read.

---

## 4. What is unclear

**U1 — How a fresh clone is supposed to get the patches.** This is the most consequential
open question in the project. `patches/embfmt/0001-formatting-fixes.patch` and
`patches/flexptp/0002-servo-nan-latch-fix.patch` are exact records of the working trees
(verified). But:

- `build.sh` initialises submodules (`git submodule update --init --recursive`) and never
  applies or checks the patches — and that command *destroys* them if they are applied.
- No document states the apply step as a build prerequisite. `README.md:208-211` and
  `architecture.md:48-61` explain *why* the patches exist and where they live, but not that
  you must run `git apply` before your first build.
- The failure is silent and severe: without them the Kalman servo latches to NaN on the
  first duplicated Sync (F59) and prints `0.0000` instead of `nan` (F60) — the exact
  combination that made F59 take days to find.

**U2 — Patch numbering.** `patches/flexptp/` contains `0002-…` and no `0001-…`. The README
explains that 0001 was the dropped instrumentation patch, but a numbered series with a hole
invites a search for a missing file.

**U3 — Whether the PPS output has ever produced a pulse.** Documented honestly as blocked
on a scope. B6 raises the stakes: there is now a specific reason to expect the frequency to
be wrong, not merely unverified.

**U4 — Whether `SNAPTYPSEL = 0b01` is the intended selection.** It is the value the code
sets and F42's investigation confirmed it does not affect *reception* — but no document
states which message types it selects for timestamping, and `mac_drv.h:127`'s comment
claims something different (D12).

**U5 — MPU-related `FreeRTOSConfig.h` settings with `configENABLE_MPU 0`.**
`configSYSTEM_CALL_STACK_SIZE`, `configENFORCE_SYSTEM_CALLS_FROM_KERNEL_ONLY`,
`configPROTECTED_KERNEL_OBJECT_POOL_SIZE` and `configALLOW_UNPRIVILEGED_CRITICAL_SECTIONS`
([FreeRTOSConfig.h:81-84](../Inc/FreeRTOSConfig.h#L81)) are MPU-wrapper-v2 settings that do
nothing with the MPU disabled. Harmless, but they read as intent.

**U6 — Why `serial_io_init()` creates a thread before `osKernelInitialize()`.**
[main.c:206,211](../Src/main.c#L206). It works — the shim's `osThreadNew()` does not check
kernel state — but it is outside the CMSIS-RTOS2 contract, and `architecture.md` records
the order without remarking on it.

**U7 — Whether `ETH_MAC_ADDR*`'s locally-administered bit forcing is intended.**
`ETH_MAC_ADDR0` is `0x02 | (UNIQUE_DEV_ID_BYTE(0) & 0xFE)`, which sets the
locally-administered bit but leaves the multicast bit to the UID's bit 0 — masked off by
`& 0xFE`, so it is always a valid unicast address. Correct, but the expression reads as if
one of the two operations were an accident.

---

## 5. Suggested improvements

### Reproducibility

**S1 — Make `build.sh` own the patches.** After the submodule update, check whether each
patch is applied (`git apply --check --reverse`) and apply it if not, failing loudly if it
neither applies nor reverse-applies. This closes U1 and makes the patched state an
invariant of the build rather than a manual step.

**S2 — Add CI.** A clean Debug + Release build with `-Wall -Wextra` and zero warnings is
already the project's standard; a GitHub Actions job would keep it that way and would have
caught the F62 release-only HardFault before hardware.

**S3 — Add a `docs/` link checker.** Six documents cross-reference each other heavily by
anchor. Most of the drift in [section 2](#2-documentation-vs-code) is prose, but broken
anchors are cheap to prevent.

### Robustness

**S4 — Give `MSG()` a mutex** (recursive, or a critical section around the format+push),
and make the ISR case explicit — either a separate lock-free path or a documented ban.
Resolves A1/B4.

**S5 — Implement the two FreeRTOS hooks and make `configASSERT` speak.** A stack overflow
should name the task on the serial port before it stops. Given the console itself may be
the casualty, a direct polled `USART3->TDR` writer that bypasses the FIFO and DMA is the
right shape for the panic path. Resolves A2.

**S6 — Raise `rx` above the console threads.** `osPriorityAboveNormal` for `rx`, leaving
`tcpip_thread` at `osPriorityHigh`, restores the intended ordering (network > console)
end to end rather than only at interrupt level. Resolves A3.

**S7 — Bound `ETHHW_Transmit()`'s ownership wait** with the existing
`ETHHW_WaitBitsClear()`-style spin count, and have `low_level_output()` return `ERR_IF` on
expiry. Resolves A6/B7.

**S8 — Check the clock configuration.** Have `init_pll()` test both HAL return values and
define `Error_Handler()` to report and halt visibly. Resolves A8.

**S9 — Add `_Static_assert(offsetof(...))` for the `ETHHW_State` adjacency** in
`eth_drv_lwip.c`, so A5's layout requirement fails at compile time rather than at runtime.

**S10 — Reconcile the RX queue depths.** Either raise `PBUF_POOL_SIZE` toward
`ETH_RX_DESC_CNT` or drop `RX_MSG_Q_LEN` to match the ring, and record the reasoning.
Counting `pbuf_alloc()` failures behind a CLI command would make the pressure visible.

### Correctness

**S11 — Fix the CLI cluster (B2, B3, B5, B9, B21) together.** They are all in one 556-line
file and all reachable from the serial port. B3 is best fixed by re-deriving `pHint` and
`pPadding` from `pHelp` after any shift — i.e. clearing `pHint = NULL` in
`cli_remove_command()`'s loop so `tidy_format_commands()` rebuilds them.

**S12 — Guard `phy_get_link_status()` and make `phy_sweep_addresses()` honour the read
status** (B1). The `ETHHW_ReadPHYRegister()` return value is already there; use it.

**S13 — Verify `PPSCTRL` against RM0481** (B6). If the encoding is as suspected, correcting
the enum is a one-line shift plus removing `ETHHW_PTP_PPS_OFF`, and the fix belongs
upstream in the H743 driver too.

**S14 — Zero `evt.data.rx` in `ETHHW_ProcessRx()`** (B10) — one line, and it converts a
silent wrong value into an obvious zero.

### Documentation

**S15 — Do one pass for "HCLK"** across `Src/`, `Inc/` and `docs/` and correct every
instance that refers to the PTP clock source (D1). While there, fix `design_doc.md`'s clock
tree to the H5 spellings (D2) — that one is actively dangerous.

**S16 — Reconcile the status claims** (D3, D4, D5, D7, D8, D9, D11). A short "Current
status" block at the top of `port_review.md`, distinguishing the living summary from the
retained investigation history, would prevent this recurring — the document's value comes
from keeping the dead ends, so the fix is to mark them as history rather than delete them.

**S17 — Settle the 4 ns question** (D6). `ptp_clock.md`'s corrected position is the right
one; retitle the section to "Why a 4 ns increment is not adopted" and update the README and
F5 rows to match.

### Deferred but worth scheduling

**S18 — Report F47, F59 and F60/F66 upstream.** All four defects are target-independent,
already reduced to minimal patches, and the project owes them. F59 in particular is a
latent hang for every flexPTP user with a Kalman servo.

**S19 — Close the two blocked verification items** when equipment allows. The LED-on-PG8
suggestion already in the plan is worth doing now regardless, since B6 gives it a specific
hypothesis to test.

---

## Action item checklist

### Priority 1 — correctness and reproducibility

- [ ] **P1.1** Make `build.sh` verify and apply `patches/embfmt/` and `patches/flexptp/` after the submodule update, failing loudly if neither apply nor reverse-apply succeeds *(U1, S1)*
- [ ] **P1.2** Document the patch prerequisite in `README.md` under "Get the sources" *(U1)*
- [ ] **P1.3** Add a NULL guard to `phy_get_link_status()` in `phy_common.c:514` *(B1, S12)*
- [ ] **P1.4** Make `phy_sweep_addresses()` honour `ETHHW_ReadPHYRegister()`'s return value *(B1, S12)*
- [ ] **P1.5** Release `modifyMtx` on the early return in `cli_remove_command()`, `cli.c:521` *(B2, S11)*
- [ ] **P1.6** Reset `pHint = NULL` when shifting entries in `cli_remove_command()`, so `tidy_format_commands()` re-derives `pHint`/`pPadding` *(B3, S11)*
- [ ] **P1.7** Serialise `MSG()` — mutex or critical section around format + push *(A1, B4, S4)*
- [ ] **P1.8** Correct every stale "HCLK is the PTP clock" claim: `main.c:83-90`, `design_doc.md:172,196`, `implementation_plan.md:53`, `port_review.md:1548` *(D1, S15)*
- [ ] **P1.9** Fix `design_doc.md`'s clock tree to `RCC_SYSCLK_DIV1` / `RCC_HCLK_DIV2` — the current text reproduces finding F45 *(D2)*

### Priority 2 — robustness

- [ ] **P2.1** Implement `vApplicationStackOverflowHook()` and `vApplicationMallocFailedHook()` with a polled-UART panic reporter *(A2, S5)*
- [ ] **P2.2** Make `configASSERT` report before halting *(A2, S5)*
- [ ] **P2.3** Bound the ownership wait in `ETHHW_Transmit()`, `mac_drv.c:552` *(A6, B7, S7)*
- [ ] **P2.4** Check `HAL_RCC_OscConfig()` / `HAL_RCC_ClockConfig()` return values; define `Error_Handler()` *(A8, S8)*
- [ ] **P2.5** Fix the escape-sequence handling in `get_line()`: switch on `c`, and bound `paramIdx` against `sizeof(escParam)` *(B5, S11)*
- [ ] **P2.6** Fix the off-by-one write in `insert_carriage_return()` *(B8)*
- [ ] **P2.7** Clamp `cli_register_command()`'s token count against `MAX_TOKEN_N`, not `TOK_ARR_LEN` *(B9, S11)*
- [ ] **P2.8** Raise the `rx` thread above the console threads *(A3, S6)*
- [ ] **P2.9** Check `HAL_UART_Transmit_DMA()`'s return in `serial_io_thread()` *(B13)*

### Priority 3 — verification

- [ ] **P3.1** Verify `MACPPSCR.PPSCTRL` encoding against RM0481; correct `ETHHW_PPS_FreqEnum` if the off-by-one is confirmed *(B6, S13)*
- [ ] **P3.2** Run the LED-on-PG8 test to distinguish "configured" from "working" and 1 Hz from 2 Hz *(U3, S19)*
- [ ] **P3.3** Zero `evt.data.rx` at the top of the `ETHHW_ProcessRx()` loop *(B10, S14)*
- [ ] **P3.4** Add `_Static_assert` for the `ETHHW_State` / RX-ring adjacency *(A5, S9)*
- [ ] **P3.5** Reconcile `RX_MSG_Q_LEN`, `PBUF_POOL_SIZE` and `ETH_RX_DESC_CNT`; record the reasoning *(A4, S10)*
- [ ] **P3.6** Add `LOCK_TCPIP_CORE()` around the lwIP accesses in `check_dhcp_state()` *(B12)*
- [ ] **P3.7** Record which PTP message types `SNAPTYPSEL = 0b01` selects for timestamping, and confirm it is the intended choice for both e2e and p2p *(U4)*

### Priority 4 — documentation

- [ ] **P4.1** Remove the "F42 remains broken" paragraph from `architecture.md:373` *(D3)*
- [ ] **P4.2** Update `architecture.md:239` — the addend wait is bounded since F54 *(D4)*
- [ ] **P4.3** Remove the "MDIO shift bug must be fixed" note from `ptp_clock.md:252` *(D5)*
- [ ] **P4.4** Retitle the 4 ns section to "not adopted", and update `README.md:268` and `port_review.md`'s F5 row *(D6, S17)*
- [ ] **P4.5** Replace `port_review.md:58`'s "no hardware" claim with a current-status block *(D7, S16)*
- [ ] **P4.6** Extend the "Findings by phase" table to F66 and add the hardware bring-up phase *(D8)*
- [ ] **P4.7** Mark F42's retained "Status: open" section as superseded history *(D9)*
- [ ] **P4.8** Settle the embfmt fix count between `README.md:209` and `architecture.md:52` — four defects, two findings, one patch *(D10)*
- [ ] **P4.9** Clear the "Open: F54" heading in `design_doc.md`'s bring-up section *(D11)*
- [ ] **P4.10** Fix `mac_drv.h`'s two stale comments — `TSENALL` and the 0.467 ns quantum *(D12)*
- [ ] **P4.11** Correct the thread name in `architecture.md:220` to match `TCPIP_THREAD_NAME` *(D12)*
- [ ] **P4.12** Update the FreeRTOS version in `FreeRTOSConfig.h`'s header comment to V11.3.0 *(D12)*
- [ ] **P4.13** Remove the dead `Inc` include path from `Middlewares/CMakeLists.txt:14` *(D12)*

### Priority 5 — housekeeping

- [ ] **P5.1** Add CI: clean Debug + Release build, zero warnings *(S2)*
- [ ] **P5.2** Remove the unconditional `DEBUG` from `CMakeLists.txt:93` *(B19)*
- [ ] **P5.3** Decide whether `Release` should be `-O2`; configure or delete the untested `relWithDebInfo` and `minSizeRel` presets *(B20)*
- [ ] **P5.4** Use `bfifo_push_all()` in `serial_io_write()` *(B14)*
- [ ] **P5.5** Fix `get_line()`'s left-trim so the trimmed line reaches `process_cli_line()` *(B21, S11)*
- [ ] **P5.6** Fix `phy_setup_int_DP83848()`'s `READ`-for-`WRITE` on MICR *(B11)*
- [ ] **P5.7** Fix `ETHHW_AuxTimestampCh()` to OR rather than assign *(B15)*
- [ ] **P5.8** Fix the out-of-scope `netif` in `rx_thread`'s MIB2 macros *(B16)*
- [ ] **P5.9** Fix the inverted return sense of `phy_read_reg()` / `phy_write_reg()` *(B17)*
- [ ] **P5.10** Fix `phy_read_range()`'s stride and `phy_read_all_regs()`'s bound *(B18)*
- [ ] **P5.11** Change `MSGchar()` to take a `char`; remove the unused `last_char` *(B22)*
- [ ] **P5.12** Renumber `patches/flexptp/0002-…` to `0001-…`, or note the gap in the directory README *(U2)*
- [ ] **P5.13** Remove the inert MPU-wrapper settings from `FreeRTOSConfig.h:81-84`, or comment why they are kept *(U5)*
- [ ] **P5.14** Exclude `Src/etherlib_options.c` from LWIP builds *(A7)*
- [ ] **P5.15** File the three upstream reports: F47, F59, F60/F66 *(S18)*
- [ ] **P5.16** Add a `docs/` cross-reference and anchor checker to CI — six documents link to each other heavily by anchor *(S3)*
- [ ] **P5.17** Move `serial_io_init()`'s `osThreadNew()` after `osKernelInitialize()`, or record why the current order is safe *(U6)*
- [ ] **P5.18** Comment `ETH_MAC_ADDR0`'s `0x02 | (UNIQUE_DEV_ID_BYTE(0) & 0xFE)` so the locally-administered bit and the unicast mask read as two deliberate operations *(U7)*
