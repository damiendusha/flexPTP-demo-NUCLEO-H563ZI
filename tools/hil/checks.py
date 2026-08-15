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

# The CLI's registered command count once flexPTP has registered its own.
#
# 23 in the pre-port firmware: 4 application commands plus flexPTP's 19. S5b added `ethinfo`,
# a driver-state and counter dump, making 24; the review's GI-02 added `ethstat`, the same
# counters as machine-readable key=value, making 25. Asserted exactly rather than as a minimum,
# because a command silently failing to register is a real failure mode -- that is what the
# check is for -- so a deliberate addition is expected to move this number.
EXPECTED_CMD_COUNT = 25


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

    if registered != EXPECTED_CMD_COUNT:
        return CheckResult(
            "cli",
            Status.FAIL,
            f"expected {EXPECTED_CMD_COUNT} registered commands, got {registered}",
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
EXPECTED_IRQ_PRIORITIES = {
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

    found = {m.group(1): int(m.group(2))
             for m in re.finditer(r"^\s+(\S+)\s+pri=(\d+)\s*$", out, re.M)}
    data = {"priorities": found}

    missing = [n for n in EXPECTED_IRQ_PRIORITIES if n not in found]
    if missing:
        return CheckResult("irq_prio", Status.FAIL, "not reported: " + ", ".join(missing), data)

    wrong = [f"{n} is {found[n]}, expected {want}"
             for n, want in EXPECTED_IRQ_PRIORITIES.items() if found[n] != want]
    if wrong:
        return CheckResult("irq_prio", Status.FAIL, "; ".join(wrong), data)

    return CheckResult("irq_prio", Status.PASS,
                       f"{len(EXPECTED_IRQ_PRIORITIES)} priorities as documented", data)


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
            return None
        time.sleep(2.0)

    return f"never reached SLAVE within {ctx.ptp_settle_s:.0f}s"


def check_ptp_ts(board: Board, ctx: Context) -> CheckResult:
    """Both hardware timestamps -- receive AND transmit -- are being captured.

    THIS IS THE CHECK THAT MUST RUN BEFORE ptp_sync, and the reason is finding 1 of
    docs/threadx_netxduo_feasibility.md: flexPTP's CMSIS-RTOS2 binding invokes the
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
    "S6": ["build_id", "boot", "cli", "osinfo", "threads", "dhcp", "ping", "ptp_start", "ptp_ts", "ptp_sync", "counters"],
    "S7": ["build_id", "boot", "cli", "osinfo", "threads", "dhcp", "ping", "ptp_start", "ptp_ts", "ptp_sync", "counters"],
    "S8": list(ALL_CHECKS),
}
