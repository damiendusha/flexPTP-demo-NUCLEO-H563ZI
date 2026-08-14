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
    baseline: dict = field(default_factory=dict)


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

# The CLI's registered/available slot count once flexPTP has registered its commands.
EXPECTED_CMD_COUNT = 23


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
    link = re.search(r"ETH LINK:\s*(UP|DOWN)", text)

    data = {
        "banner_present": True,
        "phy": phy.group(1).strip() if phy else None,
        "link": link.group(1) if link else None,
    }
    if phy is None:
        return CheckResult("boot", Status.FAIL, "PHY was never identified", data)
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
    """Expected threads exist.

    Valid from S3. Requires a CLI command that enumerates threads; `osinfo` is extended to
    provide one during the port. Skips cleanly until then.
    """
    out = board.cmd("osinfo")
    if "Threads:" not in out:
        return CheckResult(
            "threads",
            Status.SKIP,
            "osinfo does not enumerate threads yet (added in S3)",
        )
    names = re.findall(r"^\s+(\S+)\s+pri=(\d+)", out, re.M)
    return CheckResult(
        "threads", Status.PASS, f"{len(names)} threads", {"threads": dict(names)}
    )


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
    "boot": check_boot,
    "cli": check_cli,
    "osinfo": check_osinfo,
    "cpp_ctor": check_cpp_ctor,
    "threads": check_threads,
    "ptp_start": check_ptp_start,
    "dhcp": check_dhcp,
    "ping": check_ping,
    "ptp_ts": check_ptp_ts,
    "ptp_sync": check_ptp_sync,
}

# Which checks are meaningful at each stage of the ThreadX port. Ordering matters: `dhcp`
# populates the board IP that `ping`, `ptp_ts` and `ptp_sync` need.
STAGES = {
    "S0": ["boot", "cli", "osinfo", "dhcp", "ping", "ptp_start", "ptp_ts", "ptp_sync"],
    "S1": ["boot", "cli", "osinfo", "cpp_ctor", "dhcp", "ping", "ptp_start", "ptp_ts", "ptp_sync"],
    "S3": ["boot", "cli", "osinfo", "cpp_ctor", "threads"],
    "S4": ["boot", "cli", "osinfo", "cpp_ctor", "threads"],
    "S5a": ["boot", "cli", "osinfo", "threads"],
    "S5b": ["boot", "cli", "osinfo", "threads"],
    "S5c": ["boot", "cli", "osinfo", "threads", "dhcp", "ping"],
    "S5d": ["boot", "cli", "osinfo", "threads", "dhcp", "ping"],
    "S6": ["boot", "cli", "osinfo", "threads", "dhcp", "ping", "ptp_start", "ptp_ts", "ptp_sync"],
    "S7": ["boot", "cli", "osinfo", "threads", "dhcp", "ping", "ptp_start", "ptp_ts", "ptp_sync"],
    "S8": list(ALL_CHECKS),
}
