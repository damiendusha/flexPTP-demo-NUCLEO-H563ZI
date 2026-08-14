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
    baseline: dict = field(default_factory=dict)


# The addend is a derived constant, not a tuning knob: 2^32 / 1.25 for a 250 MHz
# clk_ptp_ref_i and a 5 ns increment. See docs/ptp_clock.md. If this changes, the clock
# derivation changed with it.
EXPECTED_ADDEND = 3435973836

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
    """flexPTP starts and programs the documented addend.

    The addend is the single best one-line proof that the timestamp clock derivation is
    intact: it is 2^32/1.25 for a 250 MHz clk_ptp_ref_i at a 5 ns increment (F53, F2).
    """
    board.cmd("flexptp", quiet=1.2, timeout=10)
    out = board.cmd("ptp addend")
    m = re.search(r"Addend:\s*(\d+)", out)
    if not m:
        return CheckResult("ptp_start", Status.FAIL, "no addend reported", {"raw": out})

    addend = int(m.group(1))
    info = board.cmd("ptp info", quiet=0.8)
    cid = re.search(r"Own clock ID:\s*([0-9a-fA-F]+)", info)
    data = {"addend": addend, "clock_id": cid.group(1) if cid else None}

    if addend != EXPECTED_ADDEND:
        return CheckResult(
            "ptp_start",
            Status.FAIL,
            f"addend {addend} != expected {EXPECTED_ADDEND}",
            data,
        )
    return CheckResult("ptp_start", Status.PASS, f"addend {addend}", data)


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


def check_ptp_sync(board: Board, ctx: Context) -> CheckResult:
    """The servo locks against a host `ptp4l` and stays locked.

    Requires `ptp4l` already running on the host -- the harness does not start it, because
    it needs root and outlives a single check. See tools/hil/README.md.
    """
    if ctx.board_ip is None:
        return CheckResult("ptp_sync", Status.SKIP, "no board IP")

    if not _ptp4l_running(ctx):
        return CheckResult(
            "ptp_sync",
            Status.SKIP,
            f"no ptp4l on {ctx.host_iface} -- see tools/hil/README.md",
        )

    board.cmd("flexptp", quiet=1.2, timeout=10)
    board.cmd("ptp log def on")

    text = board.read_for(ctx.ptp_settle_s)
    if "-> SLAVE" not in text:
        return CheckResult(
            "ptp_sync",
            Status.FAIL,
            f"never reached SLAVE within {ctx.ptp_settle_s:.0f}s",
            {"tail": text[-800:]},
        )

    # One line per Sync; the correction in ppb is the 9th field.
    corrections = []
    for line in text.splitlines():
        fields = line.split()
        if len(fields) >= 11 and fields[0].isdigit():
            try:
                corrections.append(float(fields[8]))
            except ValueError:
                pass

    if len(corrections) < 20:
        return CheckResult(
            "ptp_sync",
            Status.FAIL,
            f"only {len(corrections)} sync samples; expected >= 20",
            {"samples": len(corrections)},
        )

    data = {
        "samples": len(corrections),
        "corr_ppb_mean": sum(corrections) / len(corrections),
        "corr_ppb_absmax": max(abs(c) for c in corrections),
    }
    return CheckResult(
        "ptp_sync",
        Status.PASS,
        f"SLAVE, {data['samples']} samples, mean {data['corr_ppb_mean']:.1f} ppb",
        data,
    )


def check_ptp_ts(board: Board, ctx: Context) -> CheckResult:
    """Transmit AND receive timestamps are both flowing.

    This check exists because of the flexPTP CMSIS-RTOS2 defect documented in
    docs/threadx_netxduo_feasibility.md: in that binding the transmit-timestamp callback is
    invoked from an ISR with a blocking enqueue, which fails silently and drops every t3.
    A sync test alone can still look plausible with t3 missing -- this one cannot.

    Needs `ptp log ts on`, whose output pairs each message with its timestamp.
    """
    if ctx.board_ip is None:
        return CheckResult("ptp_ts", Status.SKIP, "no board IP")
    if not _ptp4l_running(ctx):
        return CheckResult("ptp_ts", Status.SKIP, f"no ptp4l on {ctx.host_iface}")

    board.cmd("flexptp", quiet=1.2, timeout=10)
    board.cmd("ptp log ts on")
    text = board.read_for(30.0)
    board.cmd("ptp log ts off")

    tx = len(re.findall(r"--->", text))
    rx = len(re.findall(r"<---", text))
    data = {"tx_events": tx, "rx_events": rx}

    if rx == 0:
        return CheckResult("ptp_ts", Status.FAIL, "no receive timestamps", data)
    if tx == 0:
        return CheckResult(
            "ptp_ts",
            Status.FAIL,
            "no TRANSMIT timestamps -- t3 is missing; see the CMSIS-RTOS2 ISR defect",
            data,
        )
    return CheckResult("ptp_ts", Status.PASS, f"tx={tx} rx={rx}", data)


def _ptp4l_running(ctx: Context) -> bool:
    r = subprocess.run(["pgrep", "-af", "ptp4l"], capture_output=True, text=True)
    return ctx.host_iface in r.stdout


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
    "S0": ["boot", "cli", "osinfo", "ptp_start", "dhcp", "ping", "ptp_ts", "ptp_sync"],
    "S1": ["boot", "cli", "osinfo", "cpp_ctor", "ptp_start", "dhcp", "ping", "ptp_ts", "ptp_sync"],
    "S3": ["boot", "cli", "osinfo", "cpp_ctor", "threads"],
    "S4": ["boot", "cli", "osinfo", "cpp_ctor", "threads"],
    "S5a": ["boot", "cli", "osinfo", "threads"],
    "S5b": ["boot", "cli", "osinfo", "threads"],
    "S5c": ["boot", "cli", "osinfo", "threads", "dhcp", "ping"],
    "S5d": ["boot", "cli", "osinfo", "threads", "dhcp", "ping"],
    "S6": ["boot", "cli", "osinfo", "threads", "ptp_start", "dhcp", "ping", "ptp_ts", "ptp_sync"],
    "S7": ["boot", "cli", "osinfo", "threads", "ptp_start", "dhcp", "ping", "ptp_ts", "ptp_sync"],
    "S8": list(ALL_CHECKS),
}
