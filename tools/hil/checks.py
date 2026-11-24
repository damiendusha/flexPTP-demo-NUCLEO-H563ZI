"""Named hardware checks.

Each check is a function `(board, ctx) -> CheckResult`. Checks are deliberately small and
independently runnable, so a stage can assert only what is supposed to work yet: during the
ThreadX port, `ping` is meaningless until S5c and `ptp_sync` until S6.

`SKIP` is a first-class result and is never a pass. A check that cannot run says so, with
the reason, and `run.py` reports the suite as incomplete rather than green.
"""

from __future__ import annotations

import re
import subprocess
import time
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path

import flood as flood_mod
from hil import Board, BoardError


class Status(str, Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"


@dataclass
class CheckResult:
    name: str
    status: Status
    detail: str = ""
    data: dict = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return self.status is Status.PASS


@dataclass
class Context:
    """Runtime configuration and cross-check state."""

    board_ip: str | None = None
    host_iface: str = "enp5s0"
    ptp_settle_s: float = 90.0
    ptp_locked: bool = False
    # When the board was first seen slaved in this run, so ptp_sync can wait out convergence.
    slaved_at: "float | None" = None

    # Two separate things, because between S3 and S5a they differ and conflating them makes
    # one of the checks lie. Set from run.py.
    #
    #   expect_phy       an Ethernet DRIVER exists, so the PHY is identified and a link is
    #                    reported. False for S3 (no stack at all) and S4 (stack, stub driver).
    #   expect_ip_thread a network STACK exists, so NetX Duo's IP thread is running. False
    #                    only for S3.
    expect_phy: bool = True
    expect_ip_thread: bool = True
    baseline: dict = field(default_factory=dict)

    # The ELF this run claims to be testing, for check_build_id. None means no claim was made.
    elf: "Path | None" = None

    # Registered CLI commands to expect. Per-firmware; the literal rather than
    # EXPECTED_CMD_COUNT because that constant is defined below this class, with the reasoning
    # for both firmwares' numbers.
    cli_commands: int = 26

    # The interrupt priority table this firmware must hold, and the FreeRTOS syscall ceiling to
    # check ISRs against. Both per-firmware and both defaulted to the ThreadX firmware's values;
    # run.py overrides them for the FreeRTOS one. See EXPECTED_IRQ_PRIORITIES_THREADX and
    # EXPECTED_IRQ_PRIORITIES_FREERTOS below for why the two tables differ where they do.
    #
    # max_syscall_prio is None on ThreadX, which has no equivalent rule -- it is the FreeRTOS
    # port that forbids calling a FromISR API from an interrupt more urgent than the ceiling.
    irq_priorities: dict = field(default_factory=lambda: dict(EXPECTED_IRQ_PRIORITIES_THREADX))
    max_syscall_prio: "int | None" = None
    syscall_isrs: tuple = ()

    # check_ptp_flood. 120 s because the queues have to fill before the defect it targets
    # bites -- a 30 s flood survives even the firmware that has the bug. Unlimited rate by
    # default; --flood-pps caps it when the point is a long soak rather than saturation.
    flood_s: float = 120.0
    flood_pps: "int | None" = None
    flood_size: int = 128


# The addend's NOMINAL value: 2^32 / 1.25 for a 250 MHz clk_ptp_ref_i at a 5 ns increment.
# See docs/ptp_clock.md.
NOMINAL_ADDEND = 3435973836

# ...but the servo tunes it, so the live value is never exactly nominal once the clock is
# disciplining -- that is the entire point of the addend interface. An early version of
# check_ptp_start asserted equality and failed against a perfectly healthy board that had
# simply locked before the check ran.
#
# The band still catches what matters: an addend of zero (F2's failure mode, where both
# planned configurations produced one) and any gross error in the clock derivation. The
# observed range on this bench during a normal lock is 3435585115 - 3436216126, roughly
# +/-0.02%; 1% is generous around that while remaining nowhere near zero.
ADDEND_TOLERANCE = 0.01

# How long after the board is first slaved before ptp_sync samples the offset. See
# check_ptp_sync: ~40 s to come inside 1 us from a cold reset on this bench, plus margin.
PTP_CONVERGE_S = 60.0

# The CLI's registered command count once flexPTP has registered its own.
#
# 23 in the pre-port firmware: 4 application commands plus flexPTP's 19. S5b added `ethinfo`,
# a driver-state and counter dump, making 24; the review's GI-02 added `ethstat`, the same
# counters as machine-readable key=value, making 25. Asserted exactly rather than as a minimum,
# because a command silently failing to register is a real failure mode -- that is what the
# check is for -- so a deliberate addition is expected to move this number.
# check. It is also per-FIRMWARE, which the monorepo makes concrete -- and both now register 26,
# for the same reason rather than by coincidence. The FreeRTOS + lwIP project is the pre-port
# lineage: it started at 23, phase 8 brought it `ethstat` (24), and phase 9's backport brought
# it `ethinfo` (25), which is the same set the ThreadX firmware arrived at by the other route.
# Both then gained `ptpcfg`, the hook that exercises flexPTP's retained-configuration round
# trip -- a path neither firmware reaches on its own, because neither defines PTP_CONFIG_PTR,
# and therefore the only way to test upstream finding U2 on hardware at all.
# Selected by run.py --firmware, which sets this along with the interrupt priority table.
EXPECTED_CMD_COUNT = 26

# The only counter check_ptp_flood asserts rather than reports. One sTxCbFIFO depth (10),
# rounded up: a saturated link may cost a real transmit timestamp or two, but a PTP slave
# transmits a handful of frames per second regardless of how hard it is being flooded, so this
# counter cannot legitimately track the flood. The reasoning, and the 284 392-event defect that
# made it worth asserting, are at the assertion itself.
FLOOD_MAX_DROP_TXTS = 16


# --------------------------------------------------------------------------
# Checks valid from S0 (no network required)
# --------------------------------------------------------------------------


def check_boot(board: Board, ctx: Context) -> CheckResult:
    """Reset produces the banner, the PHY is identified, and no fault handler fires."""
    text = board.reset(capture=12.0)

    if not text.strip():
        return CheckResult("boot", Status.FAIL, "no output at all after reset")

    faults = [
        line
        for line in text.splitlines()
        if "FATAL" in line or "HardFault" in line or "CFSR" in line
    ]
    if faults:
        return CheckResult("boot", Status.FAIL, "fault output during boot: " + faults[0])

    if "flexPTP demo" not in text:
        return CheckResult("boot", Status.FAIL, "banner missing")

    phy = re.search(r"PHY:\s*(.+?)\s*$", text, re.M)
    # The LAST reported state, not the first: the link legitimately starts down and comes up
    # when auto-negotiation completes, so the first match says nothing useful.
    links = re.findall(r"ETH LINK:\s*(UP|DOWN)", text)
    link = links[-1] if links else None

    data = {
        "banner_present": True,
        "phy": phy.group(1).strip() if phy else None,
        "link": link,
        "expect_phy": ctx.expect_phy,
    }

    if not ctx.expect_phy:
        # Assert the absence, rather than merely tolerating it: if a PHY turns up at a stage
        # with no Ethernet driver linked, something is being built that should not be.
        if phy is not None:
            return CheckResult(
                "boot", Status.FAIL,
                f"PHY '{data['phy']}' reported at a stage with no Ethernet driver", data)
        return CheckResult("boot", Status.PASS, "banner, no faults (no network expected)", data)

    if phy is None:
        return CheckResult("boot", Status.FAIL, "PHY was never identified", data)
    if link != "UP":
        return CheckResult("boot", Status.FAIL, f"link is {link}; is the cable in?", data)
    return CheckResult(
        "boot", Status.PASS, f"PHY {data['phy']}, link {data['link']}", data
    )


def check_cli(board: Board, ctx: Context) -> CheckResult:
    """`?` lists the expected command set.

    Before flexPTP starts only four commands are registered, so this check starts it.
    """
    if "flexPTP" not in board.cmd("flexptp", quiet=1.2, timeout=10):
        # Already running is fine; the daemon refuses a second start.
        pass
    out = board.cmd("?", quiet=1.0, timeout=10)

    m = re.search(r"Print this help \((\d+)/(\d+)\)", out)
    if not m:
        return CheckResult("cli", Status.FAIL, "help header not found", {"raw": out})

    registered, slots = int(m.group(1)), int(m.group(2))
    names = re.findall(r"^(\S.*?)\s{2,}\S", out, re.M)
    data = {"registered": registered, "slots": slots, "commands": names}

    if registered != ctx.cli_commands:
        return CheckResult(
            "cli",
            Status.FAIL,
            f"expected {ctx.cli_commands} registered commands, got {registered}",
            data,
        )
    return CheckResult("cli", Status.PASS, f"{registered}/{slots} commands", data)


def check_osinfo(board: Board, ctx: Context) -> CheckResult:
    """Kernel identity and free-memory figures parse and are non-degenerate.

    Deliberately does not assert *which* kernel: this check must keep working across the
    ThreadX port, where the name and the memory accounting both change.
    """
    out = board.cmd("osinfo")
    os_m = re.search(r"OS:\s*(\S+)", out)
    ver_m = re.search(r"Kernel version:\s*(\S+)", out)
    free_m = re.search(r"([\d]+)\s*bytes", out)

    if not (os_m and ver_m):
        return CheckResult("osinfo", Status.FAIL, "could not parse", {"raw": out})

    free = int(free_m.group(1)) if free_m else None
    data = {"os": os_m.group(1), "version": ver_m.group(1), "free_bytes": free}

    if free is not None and free <= 0:
        return CheckResult("osinfo", Status.FAIL, "free memory is zero", data)
    return CheckResult(
        "osinfo", Status.PASS, f"{data['os']} {data['version']}, {free} B free", data
    )


def check_cpp_ctor(board: Board, ctx: Context) -> CheckResult:
    """The C++ proof TU's static constructor ran before main().

    Valid from S1. Proves __libc_init_array reaches .init_array, which is the one part of
    the C++ toolchain configuration that cannot be verified by the build alone.
    """
    out = board.cmd("osinfo")
    m = re.search(r"C\+\+ static init:\s*(\S+)", out)
    if not m:
        return CheckResult(
            "cpp_ctor",
            Status.FAIL,
            "no 'C++ static init:' marker in osinfo -- constructor did not run",
            {"raw": out},
        )
    return CheckResult("cpp_ctor", Status.PASS, m.group(1), {"marker": m.group(1)})


# The exception and interrupt priorities docs/architecture.md documents, as the firmware must
# actually hold them.
#
# Asserted exactly, because every one of them is a decision:
#
#   fault handlers at 0    a fault must be diagnosed, not queued behind whatever was running
#   PendSV at 15           REQUIRED lowest -- the context switch must not preempt a live ISR
#   SysTick at 4           above ETH, so the kernel tick and the timer thread stay prompt under
#                          Ethernet load. NOT TICK_INT_PRIORITY (15): HAL_Init() sets that and
#                          tx_initialize_low_level.S then overrides it, which is why the table
#                          said 15 for two years while the board ran at 4 (NX-04)
#   ETH 7 above console 12 heavy CLI output must not delay Ethernet servicing
EXPECTED_IRQ_PRIORITIES_THREADX = {
    "MemManage": 0,
    "BusFault": 0,
    "UsageFault": 0,
    "SVCall": 15,
    "PendSV": 15,
    "SysTick": 4,
    "ETH": 7,
    "USART3": 12,
    "GPDMA1_Ch0": 12,
}

# The same table for the FreeRTOS firmware. The peripherals and the fault handlers agree with
# ThreadX -- same silicon, same drivers, same reasoning -- and the two rows that differ differ
# because the KERNELS REQUIRE DIFFERENT THINGS, which is exactly why this table is per-firmware
# rather than shared:
#
#   SysTick 15, not 4   FreeRTOS's Cortex-M port requires the tick at the LOWEST priority
#                       (configKERNEL_INTERRUPT_PRIORITY, from configLIBRARY_LOWEST_INTERRUPT_
#                       PRIORITY=0xf). ThreadX's 4 -- above ETH, so the tick stays prompt under
#                       Ethernet load -- would be a port violation here, not an improvement.
#   SVCall 0, not 15    FreeRTOS's port uses SVC exactly once, in prvPortStartFirstTask, to
#                       launch the first task before any other interrupt is live. It is left at
#                       the reset default because nothing preempts anything at that moment.
EXPECTED_IRQ_PRIORITIES_FREERTOS = {
    "MemManage": 0,
    "BusFault": 0,
    "UsageFault": 0,
    "SVCall": 0,
    "PendSV": 15,
    "SysTick": 15,
    "ETH": 7,
    "USART3": 12,
    "GPDMA1_Ch0": 12,
}

# configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY from FreeRTOSConfig.h, and the interrupts whose
# handlers reach a FromISR API. An ISR MORE URGENT than the ceiling (a NUMERICALLY SMALLER
# priority) that calls one corrupts the kernel's critical sections silently -- the failure is a
# scheduler that occasionally loses a wakeup, which looks like a hang somewhere else entirely.
#
# FreeRTOS can catch this itself via configASSERT in vPortValidateInterruptPriority, but only
# for an interrupt that actually fires while asserts are compiled in. Reading the registers back
# checks every one of them, on the shipped -O2 image, in one command.
FREERTOS_MAX_SYSCALL_PRIO = 5
FREERTOS_SYSCALL_ISRS = ("ETH", "USART3", "GPDMA1_Ch0")


def check_irq_prio(board: Board, ctx: Context) -> CheckResult:
    """Interrupt priorities match the documented table.

    Exists because two of that table's six exception rows were wrong and nothing checked them
    (finding NX-04). A priority table that is only ever read is a table that drifts: the values
    are set in three different places -- ST's HAL, this project's assembly, and the drivers --
    and the last writer wins without saying so.
    """
    out = board.cmd("osinfo", quiet=1.2, timeout=10)
    if "Interrupt priorities:" not in out:
        return CheckResult(
            "irq_prio", Status.SKIP, "osinfo does not report priorities (added after S8)"
        )

    # Anchored to the "Interrupt priorities:" heading. osinfo also prints "pri=" for every
    # thread, and on the FreeRTOS firmware those lines have the same leading-whitespace shape --
    # so an unanchored scan collects task priorities as though they were interrupts and then
    # reports whichever one happened to be named last. The thread block comes first in both
    # firmwares' output, so slicing from the heading is enough.
    section = out[out.index("Interrupt priorities:"):]
    found = {m.group(1): int(m.group(2))
             for m in re.finditer(r"^\s+(\S+)\s+pri=(\d+)\s*$", section, re.M)}
    data = {"priorities": found}

    expected = ctx.irq_priorities
    missing = [n for n in expected if n not in found]
    if missing:
        return CheckResult("irq_prio", Status.FAIL, "not reported: " + ", ".join(missing), data)

    wrong = [f"{n} is {found[n]}, expected {want}"
             for n, want in expected.items() if found[n] != want]

    # The FreeRTOS-only invariant: an ISR that calls a FromISR API must not be more urgent than
    # configMAX_SYSCALL_INTERRUPT_PRIORITY. Checked separately from the table above because it
    # is a DIFFERENT KIND of claim -- the table says "these are the values we chose", this says
    # "these values are safe" -- and because a future retune of ETH or USART3 could keep the
    # table honest by updating it while silently breaking the kernel.
    if ctx.max_syscall_prio is not None:
        wrong += [f"{n} at {found[n]} is more urgent than the syscall ceiling "
                  f"{ctx.max_syscall_prio}, so its FromISR calls are unsafe"
                  for n in ctx.syscall_isrs
                  if n in found and found[n] < ctx.max_syscall_prio]
        data["max_syscall_prio"] = ctx.max_syscall_prio

    if wrong:
        return CheckResult("irq_prio", Status.FAIL, "; ".join(wrong), data)

    detail = f"{len(expected)} priorities as documented"
    if ctx.max_syscall_prio is not None:
        detail += f", {len(ctx.syscall_isrs)} below the syscall ceiling"
    return CheckResult("irq_prio", Status.PASS, detail, data)


def read_stats(board: Board) -> dict:
    """The board's counters, from `ethstat`, as {key: int}.

    `ethinfo` is prose laid out for a human and this harness used to regex it -- which cost a
    silent miscount once already, recorded in check_threads() below. Every edit to a MSG()
    format string was a potential silent breakage of a check, and a check that quietly stops
    asserting is worse than no check (finding GI-02).

    `ethstat` emits one `key=value` per line between BEGIN/END markers. The key names are the
    contract; the prose is free to change.

    Returns {} if the firmware predates the command, so a caller can SKIP rather than fail.
    """
    out = board.cmd("ethstat", quiet=1.2, timeout=10)
    if "BEGIN ethstat" not in out or "END ethstat" not in out:
        return {}

    body = out.split("BEGIN ethstat", 1)[1].split("END ethstat", 1)[0]
    stats = {}
    for m in re.finditer(r"^\s*([a-z_]+\.[a-z_]+)=(\d+)\s*$", body, re.M):
        stats[m.group(1)] = int(m.group(2))
    return stats


def check_counters(board: Board, ctx: Context) -> CheckResult:
    """No counter that should be zero is non-zero.

    Every one of these is a drop or a refusal the firmware instruments precisely because its
    absence is invisible. They are all expected to read zero on a healthy bench, and asserting
    that is nearly free once the values are machine-readable.

    Deliberately NOT asserting that the positive counters are positive: the other checks do
    that where it means something (ptp_ts asserts T2/T3 advance, ping asserts loss), and a
    stage that has not started flexPTP would fail a blanket "> 0" for no reason.

    This is a HEALTHY-BENCH invariant, so it must run before `ptp_flood` and not after: a
    saturation flood legitimately drives `rx.ts_stray` into the hundreds of thousands, and
    that is the flood's finding, not this check's. The registry order puts `ptp_flood` last
    for exactly this reason.
    """
    stats = read_stats(board)
    if not stats:
        return CheckResult("counters", Status.SKIP,
                           "firmware has no `ethstat` command (added after S8)")

    must_be_zero = [
        "rx.ts_deferred", "rx.ts_stray",
        "nsd.runt", "nsd.chained",
        "ptpts.dropped",
        "drv.rx_drop_nopacket", "drv.rx_drop_size", "drv.rx_drop_queue",
        "drv.tx_drop_linkdown", "drv.tx_drop_mac",
        # flexPTP's own refusal counters (RC1.0-17). Zero on a healthy bench for the same
        # reason as the rest; the flood is where they are expected to move.
        "ptp.drop_rx", "ptp.drop_tx", "ptp.drop_txts",
        # A non-zero `stale` means one of the three rules that make the transmit-timestamp
        # registry's pointer key safe has a hole in it -- see PTP_TX_REGISTRY_MAX_AGE (AR-02).
        "txreg.overflows", "txreg.stale",
    ]
    bad = [f"{k}={stats[k]}" for k in must_be_zero if stats.get(k, 0) != 0]
    if bad:
        return CheckResult("counters", Status.FAIL, ", ".join(bad), {"stats": stats})

    checked = [k for k in must_be_zero if k in stats]
    return CheckResult("counters", Status.PASS,
                       f"{len(checked)} drop/refusal counters all zero", {"stats": stats})


def check_build_id(board: Board, ctx: Context) -> CheckResult:
    """The firmware running on the board is the image this run was given.

    Finding GI-01. Without --flash the runner drives whatever is already on the board, and
    every other check then reports a confident pass for code that was never loaded. The only
    control was a warning in two READMEs, which is the weakest available answer to a hazard
    this cheap to eliminate.

    The linker emits a SHA-1 over the linked output into .note.gnu.build-id; `osinfo` prints
    it and this reads the same value out of the ELF. A mismatch fails whether or not --flash
    was passed, so "remember to pass a flag" becomes "cannot report a pass for the wrong
    image".

    SKIPs -- not fails -- when no ELF was given, because there is then no claim to check.
    """
    out = board.cmd("osinfo", quiet=1.2, timeout=10)
    m = re.search(r"Build ID:\s*([0-9a-f]{8,})", out)
    if not m:
        return CheckResult(
            "build_id", Status.SKIP, "osinfo does not report a build ID (added after S8)"
        )
    on_board = m.group(1)
    data = {"board": on_board}

    if ctx.elf is None:
        return CheckResult("build_id", Status.SKIP,
                           f"board reports {on_board[:16]}…, no ELF to compare against", data)

    from_elf = read_elf_build_id(ctx.elf)
    data["elf"] = from_elf
    if from_elf is None:
        return CheckResult("build_id", Status.SKIP,
                           f"no build ID in {ctx.elf.name}; is it an old image?", data)

    if from_elf != on_board:
        return CheckResult(
            "build_id", Status.FAIL,
            f"the board is running {on_board[:16]}… but {ctx.elf.name} is {from_elf[:16]}… "
            f"-- every other result in this run is about the wrong firmware", data)

    return CheckResult("build_id", Status.PASS, f"{on_board[:16]}… matches {ctx.elf.name}", data)


def read_elf_build_id(elf: "Path") -> "str | None":
    """The build ID from an ELF, without needing a toolchain on PATH.

    Parsed here rather than shelled out to readelf: the ARM toolchain lives under toolchains/
    and may not be on PATH, and a check that silently skips because a binary was missing is
    the failure mode this whole check exists to remove.
    """
    import struct

    data = elf.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        return None

    # 32-bit little-endian ELF: e_shoff at 0x20, e_shentsize 0x2E, e_shnum 0x30.
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum = struct.unpack_from("<HH", data, 0x2E)

    for i in range(e_shnum):
        off = e_shoff + (i * e_shentsize)
        sh_type, = struct.unpack_from("<I", data, off + 4)
        if sh_type != 7:  # SHT_NOTE
            continue
        sh_offset, sh_size = struct.unpack_from("<II", data, off + 0x10)
        note = data[sh_offset:sh_offset + sh_size]

        pos = 0
        while pos + 12 <= len(note):
            namesz, descsz, ntype = struct.unpack_from("<III", note, pos)
            name_padded = (namesz + 3) & ~3
            desc_at = pos + 12 + name_padded
            if ntype == 3 and note[pos + 12:pos + 12 + 3] == b"GNU":  # NT_GNU_BUILD_ID
                return note[desc_at:desc_at + descsz].hex()
            pos = desc_at + ((descsz + 3) & ~3)
    return None


def check_ptp_start(board: Board, ctx: Context) -> CheckResult:
    """flexPTP starts and the timestamp clock is programmed plausibly.

    The addend is the best one-line proof that the clock derivation is intact -- but it is a
    tuning register, so assert a band around nominal rather than equality. See
    ADDEND_TOLERANCE.
    """
    board.cmd("flexptp", quiet=1.2, timeout=10)
    out = board.cmd("ptp addend")
    m = re.search(r"Addend:\s*(\d+)", out)
    if not m:
        return CheckResult("ptp_start", Status.FAIL, "no addend reported", {"raw": out})

    addend = int(m.group(1))
    info = board.cmd("ptp info", quiet=0.8)
    cid = re.search(r"Own clock ID:\s*([0-9a-fA-F]+)", info)
    deviation = abs(addend - NOMINAL_ADDEND) / NOMINAL_ADDEND
    data = {
        "addend": addend,
        "nominal_addend": NOMINAL_ADDEND,
        "deviation": deviation,
        "clock_id": cid.group(1) if cid else None,
    }

    if addend == 0:
        return CheckResult("ptp_start", Status.FAIL, "addend is zero -- see F2", data)
    if deviation > ADDEND_TOLERANCE:
        return CheckResult(
            "ptp_start",
            Status.FAIL,
            f"addend {addend} is {deviation * 100:.2f}% from nominal {NOMINAL_ADDEND}",
            data,
        )
    return CheckResult(
        "ptp_start", Status.PASS, f"addend {addend} ({deviation * 100:+.3f}% of nominal)", data
    )


def check_threads(board: Board, ctx: Context) -> CheckResult:
    """The expected threads exist, at the expected priorities.

    Valid from S3, where `osinfo` gained a thread enumeration. Worth asserting rather than
    just counting: the port changes both the thread set and the priority scale, and
    docs/architecture.md carries a table of both that was previously checkable only with a
    debugger.

    The line format is `  <name> pri=<tx> (cmsis <n>) state=<s> runs=<r>`, and names contain
    spaces ("System Timer Thread", "eth ip") -- so the parse anchors on " pri=" rather than
    treating the name as one token. An earlier version used \S+ and silently miscounted.
    """
    out = board.cmd("osinfo", quiet=1.2, timeout=10)
    if "Threads:" not in out:
        return CheckResult(
            "threads", Status.SKIP, "osinfo does not enumerate threads (added in S3)"
        )

    found = {
        m.group(1).strip(): int(m.group(2))
        for m in re.finditer(r"^\s+(.+?)\s+pri=(\d+)\s+\(cmsis", out, re.M)
    }
    data = {"threads": found}

    if not found:
        return CheckResult("threads", Status.FAIL, "no threads parsed", {"raw": out})

    # Always present: the kernel's timer thread, the console writer, the bring-up thread and
    # the CLI. flexPTP's "ptp" thread is created on demand and is not required here.
    missing = [n for n in ("System Timer Thread", "serialio", "init", "cli") if n not in found]

    # The threads the Ethernet driver creates, once there is a driver to create them.
    if ctx.expect_phy:
        missing += [n for n in ("rx", "phy") if n not in found]

    # The NetX Duo IP thread, once there is a stack to run.
    if ctx.expect_ip_thread and "eth ip" not in found:
        missing.append("eth ip")

    if missing:
        return CheckResult(
            "threads", Status.FAIL, "missing: " + ", ".join(missing), data
        )

    # Priority inversion is the single easiest thing to get wrong in this port, so pin it --
    # ALL of it, not just the flat part.
    #
    # docs/architecture.md presents this table as the specification and calls two of the
    # relationships load-bearing: `eth ip` highest so the stack stays ahead of everything that
    # feeds it, and `rx` above the console so a burst of CLI output cannot lengthen the window
    # F61's inFlight marker exists to make safe. Until this check asserted them, the only thing
    # verified was that three threads sat at 32 -- the part with no load-bearing relationship
    # in it at all (TS-05).
    #
    # `ptpts` is created by the NSD when flexPTP starts, so it is checked only if present;
    # `threads` runs before `ptp_start` in every stage.
    expected = {"System Timer Thread": 0, "serialio": 32, "init": 32, "cli": 32}
    if ctx.expect_phy:
        expected.update({"rx": 24, "phy": 32})
    if ctx.expect_ip_thread:
        expected["eth ip"] = 16
    if "ptpts" in found:
        expected["ptpts"] = 24
    if "ptp" in found:
        expected["ptp"] = 32

    wrong = [f"{n} at ThreadX {found[n]}, expected {want}"
             for n, want in expected.items() if found[n] != want]
    if wrong:
        return CheckResult("threads", Status.FAIL, "; ".join(wrong), data)

    return CheckResult("threads", Status.PASS,
                       f"{len(found)} threads, {len(expected)} priorities as documented", data)


# --------------------------------------------------------------------------
# Checks that need the host-side network (see tools/hil/setup_host_net.sh)
# --------------------------------------------------------------------------


def _host_iface_ready(ctx: Context) -> str | None:
    """Return a reason string if the host interface is not set up, else None."""
    r = subprocess.run(
        ["ip", "-4", "-brief", "addr", "show", ctx.host_iface],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        return f"interface {ctx.host_iface} not found"
    if not re.search(r"\d+\.\d+\.\d+\.\d+/\d+", r.stdout):
        return (
            f"{ctx.host_iface} has no IPv4 address -- "
            "run: sudo tools/hil/setup_host_net.sh"
        )
    return None


def check_dhcp(board: Board, ctx: Context) -> CheckResult:
    """The board acquires an address, and we learn it for the later checks."""
    reason = _host_iface_ready(ctx)
    if reason:
        return CheckResult("dhcp", Status.SKIP, reason)

    text = board.reset(capture=25.0)
    m = re.search(r"IP-address:\s*(\d+\.\d+\.\d+\.\d+)", text)
    if not m:
        return CheckResult(
            "dhcp",
            Status.FAIL,
            "no 'IP-address:' line within 25 s of reset (is dnsmasq running?)",
        )
    ctx.board_ip = m.group(1)
    return CheckResult("dhcp", Status.PASS, ctx.board_ip, {"ip": ctx.board_ip})


def check_ping(board: Board, ctx: Context) -> CheckResult:
    """300 x 1400-byte pings at 0% loss.

    1400 bytes is deliberate: it exercises a near-MTU frame against the 1528-byte buffer,
    which is where the over-length guard in the transmit path lives.
    """
    if ctx.board_ip is None:
        return CheckResult("ping", Status.SKIP, "no board IP (dhcp check did not run)")

    r = subprocess.run(
        ["ping", "-c", "300", "-i", "0.01", "-s", "1400", "-q", ctx.board_ip],
        capture_output=True,
        text=True,
        timeout=180,
    )
    loss = re.search(r"([\d.]+)% packet loss", r.stdout)
    rtt = re.search(r"= ([\d.]+)/([\d.]+)/([\d.]+)", r.stdout)
    data = {
        "loss_pct": float(loss.group(1)) if loss else None,
        "rtt_min_ms": float(rtt.group(1)) if rtt else None,
        "rtt_avg_ms": float(rtt.group(2)) if rtt else None,
        "rtt_max_ms": float(rtt.group(3)) if rtt else None,
    }
    if data["loss_pct"] is None:
        return CheckResult("ping", Status.FAIL, "could not parse ping output", data)
    if data["loss_pct"] > 0.0:
        return CheckResult("ping", Status.FAIL, f"{data['loss_pct']}% loss", data)
    return CheckResult(
        "ping", Status.PASS, f"0% loss, avg {data['rtt_avg_ms']} ms", data
    )


def _ptp4l_running(ctx: Context) -> bool:
    r = subprocess.run(["pgrep", "-af", "ptp4l"], capture_output=True, text=True)
    return ctx.host_iface in r.stdout


def _ptp_is_slaved(board: Board) -> bool:
    """True when the BMCA has selected a master other than ourselves.

    A state probe rather than a transition watch. Watching the console for '-> SLAVE' only
    works if the transition happens inside the observation window; once the board is already
    locked the string never appears again, so the second PTP check to run would always fail.
    """
    info = board.cmd("ptp info", quiet=0.8)
    own = re.search(r"Own clock ID:\s*([0-9a-fA-F]+)", info)
    master = re.search(r"Master clock ID:\s*([0-9a-fA-F]+)", info)
    if not (own and master):
        return False
    return own.group(1).lower() != master.group(1).lower()


def _ensure_ptp_slaved(board: Board, ctx: Context) -> str | None:
    """Get the board into the SLAVE state. Returns a failure reason, or None on success.

    Shared by the two PTP checks so that each is independently runnable but they do not pay
    for the settle twice.
    """
    if ctx.ptp_locked and _ptp_is_slaved(board):
        return None

    # Quiet the console first: polling `ptp info` against a stream of per-Sync log lines
    # makes the reply hard to isolate.
    board.cmd("flexptp", quiet=1.2, timeout=10)
    board.cmd("ptp log def off")
    board.cmd("ptp log ts off")

    deadline = time.time() + ctx.ptp_settle_s
    while time.time() < deadline:
        if _ptp_is_slaved(board):
            ctx.ptp_locked = True
            # Reaching this loop means the board was NOT slaved on entry, so this is a fresh
            # selection and the servo has only just started converging. Record when.
            ctx.slaved_at = time.time()
            return None
        time.sleep(2.0)

    return f"never reached SLAVE within {ctx.ptp_settle_s:.0f}s"


def check_ptp_ts(board: Board, ctx: Context) -> CheckResult:
    """Both hardware timestamps -- receive AND transmit -- are being captured.

    THIS IS THE CHECK THAT MUST RUN BEFORE ptp_sync, and the reason is finding 1 of
    projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_netxduo_feasibility.md: flexPTP's CMSIS-RTOS2 binding invokes the
    transmit-timestamp callback from an ISR with a blocking enqueue, which ST's wrapper
    rejects, so every t3 is silently discarded. A servo missing t3 can still *look* like it
    is converging, so a sync test alone does not exclude it.

    `ptp log ts on` prints one block per sync cycle:

        seqID: 403
        T1: 1786703371.012952002 <- Sync TX (master)
        T2: 1786703371.012963609 <- Sync RX (slave)
        T3: 1786703370.062188559 <- Del_Req TX (slave)
        T4: 1786703370.062219813 <- Del_Req RX (master)

    T2 is our receive hardware timestamp and **T3 is our transmit hardware timestamp**. T1
    and T4 come out of the master's messages and prove nothing about this board. So the
    assertion is on T2 and T3: present, non-zero, and advancing.
    """
    if ctx.board_ip is None:
        return CheckResult("ptp_ts", Status.SKIP, "no board IP")
    if not _ptp4l_running(ctx):
        return CheckResult("ptp_ts", Status.SKIP, f"no ptp4l on {ctx.host_iface}")

    reason = _ensure_ptp_slaved(board, ctx)
    if reason:
        return CheckResult("ptp_ts", Status.FAIL, reason)

    board.cmd("ptp log ts on")
    text = board.read_for(20.0)
    board.cmd("ptp log ts off")

    def stamps(label: str) -> list[float]:
        return [
            int(s) + int(ns) / 1e9
            for s, ns in re.findall(rf"^{label}:\s*(\d+)\.(\d+)", text, re.M)
        ]

    t2, t3 = stamps("T2"), stamps("T3")
    data = {"t2_count": len(t2), "t3_count": len(t3)}

    if not t2:
        return CheckResult("ptp_ts", Status.FAIL, "no T2 -- no receive timestamps", data)
    if not t3:
        return CheckResult(
            "ptp_ts",
            Status.FAIL,
            "no T3 -- transmit timestamps are missing (the CMSIS-RTOS2 ISR defect)",
            data,
        )
    if any(v == 0 for v in t3):
        return CheckResult("ptp_ts", Status.FAIL, "T3 contains zero timestamps", data)

    # Advancing, not merely present: a latched or stale value would otherwise pass.
    data["t2_span_s"] = t2[-1] - t2[0]
    data["t3_span_s"] = t3[-1] - t3[0]
    if len(t3) > 1 and data["t3_span_s"] <= 0:
        return CheckResult("ptp_ts", Status.FAIL, "T3 is not advancing", data)

    return CheckResult(
        "ptp_ts", Status.PASS, f"T2 x{len(t2)}, T3 x{len(t3)}, both advancing", data
    )


def check_ptp_sync(board: Board, ctx: Context) -> CheckResult:
    """The servo locks against a host `ptp4l` and the offset settles.

    Requires `ptp4l` already running on the host -- the harness does not start it, because
    it needs root and outlives a single check. See tools/hil/README.md.
    """
    if ctx.board_ip is None:
        return CheckResult("ptp_sync", Status.SKIP, "no board IP")
    if not _ptp4l_running(ctx):
        return CheckResult(
            "ptp_sync", Status.SKIP, f"no ptp4l on {ctx.host_iface} -- see tools/hil/README.md"
        )

    reason = _ensure_ptp_slaved(board, ctx)
    if reason:
        return CheckResult("ptp_sync", Status.FAIL, reason)

    # Measure the servo SETTLED, however soon after slaving this check happens to run.
    #
    # "Slaved" only means a master was selected; the servo starts converging there. Traced from a
    # cold reset on this bench: the offset overshoots to ~+7.5 us about 20 s after slaving and is
    # inside 1 us after ~40 s. Without this wait the figure depended on which checks ran first --
    # in S8 order ping and ptp_ts give the servo ~25 s and it read ~800 ns, while `--checks
    # dhcp,ptp_sync` sampled the overshoot and read 7-21 us. And since nothing asserts on the
    # offset, both PASSED, which is how a measurement artefact got diagnosed as a bench fault.
    settled_after = 0.0
    if ctx.slaved_at is not None:
        settled_after = PTP_CONVERGE_S - (time.time() - ctx.slaved_at)
        if settled_after > 0:
            board.read_for(settled_after)

    board.cmd("ptp log def on")
    text = board.read_for(30.0)
    board.cmd("ptp log def off")

    # One line per Sync:
    #   T1[s] T1[ns] T4[s] T4[ns] Dt[s] Dt[ns] Dt[tick] addend corr_ppb mpd_ns sync_period_ns
    offsets_ns: list[float] = []
    corrections: list[float] = []
    for line in text.splitlines():
        f = line.split()
        if len(f) >= 11 and f[0].isdigit():
            try:
                offsets_ns.append(float(f[5]))
                corrections.append(float(f[8]))
            except ValueError:
                pass

    if len(offsets_ns) < 10:
        return CheckResult(
            "ptp_sync",
            Status.FAIL,
            f"only {len(offsets_ns)} sync samples in 30 s; expected >= 10",
            {"samples": len(offsets_ns), "tail": text[-600:]},
        )

    # Steady state is judged on the back half: the front half may still be converging, and
    # including it would make the figure depend on when the check happened to start.
    tail = offsets_ns[len(offsets_ns) // 2 :]
    data = {
        "waited_for_convergence_s": round(max(settled_after, 0.0), 1),
        "samples": len(offsets_ns),
        "offset_ns_mean": sum(tail) / len(tail),
        "offset_ns_absmax": max(abs(v) for v in tail),
        "corr_ppb_mean": sum(corrections) / len(corrections),
    }
    return CheckResult(
        "ptp_sync",
        Status.PASS,
        f"SLAVE, {data['samples']} samples, |offset| <= {data['offset_ns_absmax']:.0f} ns",
        data,
    )


def check_ptp_flood(board: Board, ctx: Context) -> CheckResult:
    """The board survives a saturated link, and PTP recovers afterwards.

    This is the regression test for flexPTP `e873105`. Before it, every enqueue in the
    CMSIS-RTOS2 and FreeRTOS bindings used an unbounded wait, and on the lwIP firmware that is
    a deadlock rather than a stall: lwIP calls `ptp_receive_cb()` from `tcpip_thread` with
    `LOCK_TCPIP_CORE` held, so blocking there sleeps the thread that owns the core lock while
    it still holds it. tcpip_thread waits for task_ptp() to drain the FIFO; task_ptp() waits
    for the core lock to transmit. Nothing times out, ARP and ICMP stop with everything else,
    and only a reset recovers it.

    So the assertion is LIVENESS, not zero drops. Under a flood, dropping is the correct
    behaviour -- that is the whole content of the fix -- and a run that dropped nothing would
    more likely mean the flood never arrived. What must hold afterwards:

      * the CLI answers                 (the board is not wedged)
      * ping is 100% clean              (the stack still owns its lock -- the ICMP path is
                                         what died first when this deadlocked)
      * PTP is still SLAVE and syncing  (task_ptp() came back, rather than falling out of
                                         SLAVE and staying there)
      * `ptp.drop_txts` stayed tiny     (the one counter that CANNOT legitimately track the
                                         flood -- finding U5, at the assertion below)

    The 120 s default is not arbitrary. The queues have to fill before any of it bites; a 30 s
    flood survives even the broken firmware, so a short run proves nothing.
    """
    if ctx.board_ip is None:
        return CheckResult("ptp_flood", Status.SKIP, "no board IP (dhcp check did not run)")
    if not _ptp4l_running(ctx):
        return CheckResult("ptp_flood", Status.SKIP, f"no ptp4l on {ctx.host_iface}")

    reason = _ensure_ptp_slaved(board, ctx)
    if reason:
        return CheckResult("ptp_flood", Status.FAIL, f"not slaved before the flood: {reason}")

    before = read_stats(board)

    try:
        sent = flood_mod.flood(ctx.host_iface, ctx.board_ip, ctx.flood_s,
                               ctx.flood_pps, ctx.flood_size, quiet=True)
    except SystemExit as e:                       # the flooder's own refusals
        return CheckResult("ptp_flood", Status.SKIP, str(e))

    data = {"flood": sent}
    if sent["packets"] == 0:
        return CheckResult("ptp_flood", Status.SKIP, "flooder sent nothing", data)

    # 1. The CLI answers at all. Everything below depends on it, and a wedged board fails
    #    here rather than in a confusing way later.
    try:
        board.cmd("flexptp", quiet=1.5, timeout=15)
    except BoardError as e:
        return CheckResult("ptp_flood", Status.FAIL,
                           f"CLI unresponsive after the flood ({e}) -- the board is wedged",
                           data)

    # 2. The network stack still works. This is the one that caught the deadlock: ICMP goes
    #    through the same core lock that flexPTP's blocking enqueue used to strand.
    r = subprocess.run(["ping", "-c", "20", "-i", "0.05", "-q", ctx.board_ip],
                       capture_output=True, text=True, timeout=60)
    loss = re.search(r"([\d.]+)% packet loss", r.stdout)
    data["post_ping_loss_pct"] = float(loss.group(1)) if loss else None
    if data["post_ping_loss_pct"] is None or data["post_ping_loss_pct"] > 0.0:
        return CheckResult("ptp_flood", Status.FAIL,
                           f"{data['post_ping_loss_pct']}% ping loss after the flood -- "
                           "the stack did not recover", data)

    # 3. PTP itself resumed. Not merely "still says SLAVE": Sync lines have to be arriving
    #    again, which is what proves task_ptp() is running rather than parked.
    ctx.ptp_locked = False                        # force a real re-probe
    reason = _ensure_ptp_slaved(board, ctx)
    if reason:
        return CheckResult("ptp_flood", Status.FAIL,
                           f"PTP did not return to SLAVE after the flood: {reason}", data)

    board.cmd("ptp log def on")
    text = board.read_for(20.0)
    board.cmd("ptp log def off")
    samples = [ln for ln in text.splitlines()
               if len(ln.split()) >= 11 and ln.split()[0].isdigit()]
    data["post_sync_samples"] = len(samples)
    if len(samples) < 5:
        return CheckResult("ptp_flood", Status.FAIL,
                           f"only {len(samples)} sync samples in 20 s after the flood; "
                           "PTP is not running again", data)

    # The counters are REPORTED, not asserted -- with one exception, below. A drop under
    # saturation is the designed behaviour. They are the difference between "we survived" and
    # "we survived and here is what it cost", and a run that shows zero everywhere is worth a
    # second look at the flood.
    after = read_stats(board)
    if before and after:
        data["counter_delta"] = {
            k: after[k] - before.get(k, 0)
            for k in after
            if isinstance(after[k], int) and after[k] - before.get(k, 0) != 0
        }

        # 4. The exception: `ptp.drop_txts` must stay tiny, on either firmware.
        #
        # Unlike every other counter here, this one does NOT scale with the flood. It counts
        # transmit timestamps that arrived faster than task_ptp() could take them, and a PTP
        # slave transmits a handful of frames per second no matter how hard the link is being
        # hammered -- so a flood cannot legitimately produce many of them, and a large value
        # means timestamps are being requested for frames that never asked for one.
        #
        # That is exactly what finding U5 was: nsd_lwip.c tags a pbuf with `tx_cb`/`tag` and
        # frees it without clearing them, lwIP does not initialise LWIP_PBUF_CUSTOM_DATA on
        # allocation, and the taint then rides recycled heap blocks into ICMP and ARP frames.
        # It measured 284 392 in a 120 s flood on the lwIP firmware against 0 on the ThreadX
        # one, and it was invisible here because this check only printed the delta. Upstream
        # fixed the source in RC1.0-26 (flexPTP #22: nsd_lwip.c untags the pbuf before freeing
        # it), and the lwIP driver also clears both fields once it has read them
        # (eth_drv_lwip.c, low_level_output) as a second line of defence. This assertion is
        # what notices if EITHER goes away -- a pin bump, an upstream NSD rework, a new
        # producer of tx_cb -- because the flood still passes on every other criterion.
        #
        # The bound is small but not zero: sTxCbFIFO is TX_CALLBACK_FIFO_LENGTH (10) deep, and
        # a genuinely saturated link can starve task_ptp() long enough to lose a real
        # timestamp or two. More than one FIFO's worth is not starvation, it is the defect
        # coming back. Anything in between is worth looking at, which is what a failure here
        # asks for.
        # Asserted only on a counter that was actually READ. A missing key defaults to "moved
        # by 0" in counter_delta, which would let a firmware that does not expose the counter,
        # or an ethstat reply that failed to parse, pass as "no spurious timestamps" -- the
        # same silent pass that hid U5 in the first place. Not asserted is reported as such.
        if "ptp.drop_txts" not in before or "ptp.drop_txts" not in after:
            data["drop_txts_asserted"] = False
        else:
            data["drop_txts_asserted"] = True
        drop_txts = data["counter_delta"].get("ptp.drop_txts", 0)
        if data["drop_txts_asserted"] and drop_txts > FLOOD_MAX_DROP_TXTS:
            return CheckResult(
                "ptp_flood", Status.FAIL,
                f"ptp.drop_txts moved by {drop_txts} during the flood (bound is "
                f"{FLOOD_MAX_DROP_TXTS}) -- transmit timestamps are being requested for "
                "frames that did not ask for one; see finding U5", data)

    if data.get("drop_txts_asserted"):
        txts_note = f", drop_txts {data['counter_delta'].get('ptp.drop_txts', 0)}"
    else:
        txts_note = ", drop_txts NOT asserted (counter not read)"
    return CheckResult(
        "ptp_flood", Status.PASS,
        f"survived {sent['seconds']:.0f} s at {sent['pps']} pps "
        f"({sent['mbit_s']} Mbit/s), 0% loss, {len(samples)} sync samples after{txts_note}",
        data,
    )


# --------------------------------------------------------------------------
# Registry
# --------------------------------------------------------------------------

ALL_CHECKS = {
    "build_id": check_build_id,
    "boot": check_boot,
    "cli": check_cli,
    "osinfo": check_osinfo,
    "cpp_ctor": check_cpp_ctor,
    "threads": check_threads,
    "irq_prio": check_irq_prio,
    "ptp_start": check_ptp_start,
    "dhcp": check_dhcp,
    "ping": check_ping,
    "ptp_ts": check_ptp_ts,
    "ptp_sync": check_ptp_sync,
    "counters": check_counters,
    # Last, always: the flood is expected to move the very counters check_counters
    # asserts are zero, so running it earlier would fail the next check by design.
    "ptp_flood": check_ptp_flood,
}

# Which checks are meaningful at each stage of the ThreadX port. Ordering matters: `dhcp`
# populates the board IP that `ping`, `ptp_ts` and `ptp_sync` need.
# Stages with no Ethernet DRIVER linked: `boot` must expect no PHY and no link.
STAGES_WITHOUT_PHY = {"S3", "S4"}

# Stages with no network STACK at all: `threads` must not expect NetX Duo's IP thread.
STAGES_WITHOUT_IP_THREAD = {"S3"}

STAGES = {
    "S0": ["build_id", "boot", "cli", "osinfo", "dhcp", "ping", "ptp_start", "ptp_ts", "ptp_sync"],
    "S1": ["build_id", "boot", "cli", "osinfo", "cpp_ctor", "dhcp", "ping", "ptp_start", "ptp_ts", "ptp_sync"],
    "S3": ["build_id", "boot", "cli", "osinfo", "cpp_ctor", "threads"],
    "S4": ["build_id", "boot", "cli", "osinfo", "cpp_ctor", "threads"],
    "S5a": ["build_id", "boot", "cli", "osinfo", "threads"],
    "S5b": ["build_id", "boot", "cli", "osinfo", "threads"],
    "S5c": ["build_id", "boot", "cli", "osinfo", "threads", "dhcp", "ping"],
    "S5d": ["build_id", "boot", "cli", "osinfo", "threads", "dhcp", "ping"],
    "S6": ["build_id", "boot", "cli", "osinfo", "threads", "dhcp", "ping", "ptp_start", "ptp_ts", "ptp_sync", "counters", "ptp_flood"],
    "S7": ["build_id", "boot", "cli", "osinfo", "threads", "dhcp", "ping", "ptp_start", "ptp_ts", "ptp_sync", "counters", "ptp_flood"],
    "S8": list(ALL_CHECKS),
}
