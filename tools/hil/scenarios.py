"""Long-running hardware scenarios: fix regressions, floods, robustness, board health.

`run.py`'s checks are small and stage-gated; these are the opposite. Each takes minutes, drives
the host's ptp4l units, floods the link or speaks PTP at the board, and asserts what the firmware
must do under that load. They came out of the final test campaigns of November 2026
(docs/flexptp_upstream_review.md, sections 6 and 7) and are run with `scenario.py`.

Results are of two kinds, and the distinction is the point:

  PASS / FAIL   something the firmware must do. A FAIL is a regression.
  OBSERVED      a property worth recording that is NOT a defect of this firmware -- how long the
                CLI is starved during a saturation flood, whether ptp4l kept the board during one.
                Asserting those would make the suite permanently red on healthy firmware, and a
                red suite that is always red says nothing.

Every scenario restores what it changed -- host ptp4l unit, board profile, flexPTP running --
in a `finally`, so one failure does not poison the next.

Host requirements: the units from tools/hil/privileged/ (flexptp-ptp4l-default, -gptp, -slave),
the board-facing interface configured, and the board on DHCP. See tools/hil/README.md.
"""

from __future__ import annotations

import datetime
import math
import re
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import ptp_fuzz
from hil import Board

HERE = Path(__file__).resolve().parent
FLOOD = HERE / "flood.py"

UNIT_DEFAULT = "flexptp-ptp4l-default"   # ptp4l master, default profile (UDP/IPv4 E2E)
UNIT_GPTP = "flexptp-ptp4l-gptp"         # ptp4l gPTP grandmaster (802.1AS, 802.3 P2P)
UNIT_SLAVE = "flexptp-ptp4l-slave"       # ptp4l slave, for testing the board as master

FLAGS = ["def", "corr", "ts", "info", "locked", "bmca", "logid"]
PRIMARY, PDELAY = "01:1b:19:00:00:00", "01:80:c2:00:00:0e"


# --------------------------------------------------------------------------------------------
# Environment and reporting
# --------------------------------------------------------------------------------------------


def eui64(mac: str) -> str:
    """The clock identity ptp4l derives from a MAC: ff:fe inserted in the middle."""
    b = mac.strip().lower().split(":")
    if len(b) != 6:
        raise ValueError(f"not a MAC: {mac!r}")
    return "".join(b[:3] + ["ff", "fe"] + b[3:])


def clock_identity_of(iface: str) -> str:
    return eui64(Path(f"/sys/class/net/{iface}/address").read_text())


@dataclass
class Env:
    firmware: str                       # "freertos" | "threadx"
    board_ip: str = "192.168.2.191"
    iface: str = "enp5s0"
    host_ip: str = "192.168.2.1"
    elf: "Path | None" = None           # enables the SWD parts of `health` and `stacks`
    host_gm_id: str = ""

    def __post_init__(self):
        if not self.host_gm_id:
            self.host_gm_id = clock_identity_of(self.iface)

    @property
    def lwip(self) -> bool:
        return self.firmware == "freertos"


@dataclass
class Report:
    fails: int = 0
    passes: int = 0
    lines: "list[str]" = field(default_factory=list)

    def _out(self, s: str) -> None:
        self.lines.append(s)
        print(s, flush=True)

    def check(self, label: str, ok: bool, detail: str = "") -> bool:
        if ok:
            self.passes += 1
        else:
            self.fails += 1
        self._out(f"  {'PASS' if ok else 'FAIL'}  {label}{('  ' + detail) if detail else ''}")
        return ok

    def observed(self, text: str) -> None:
        self._out(f"    OBSERVED: {text}")

    def note(self, text: str) -> None:
        self._out(f"    {text}")


# --------------------------------------------------------------------------------------------
# Board and host helpers
# --------------------------------------------------------------------------------------------


def unit(action: str, name: str) -> None:
    """Start/stop/restart one of the polkit-permitted ptp4l units. Masters Conflict= each other."""
    subprocess.run(["systemctl", action, name], check=True)


def journal(name: str, since: str, until: "str | None" = None) -> str:
    args = ["journalctl", "-u", name, "--since", since, "--no-pager"] + (["--until", until] if until else [])
    return subprocess.run(args, capture_output=True, text=True).stdout


def now() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def quiet(b: Board) -> None:
    """flexPTP running (the command is idempotent) and every log flag off."""
    b.cmd("flexptp", quiet=1.5, timeout=15)
    for f in FLAGS:
        b.cmd(f"ptp log {f} off")


def strip_prefixes(ln: str) -> str:
    return re.sub(r"^(?:\[LOG-[^\]]*\]\s*)+", "", ln)


def sync_lines(text: str) -> "list[str]":
    """The per-Sync lines `ptp log def` prints: numeric first field, eleven or more fields."""
    return [ln for ln in map(strip_prefixes, text.splitlines())
            if len(ln.split()) >= 11 and ln.split()[0].isdigit()]


def offsets(lines: "list[str]") -> "list[int]":
    """|offset| in ns from Sync lines (field 5)."""
    out = []
    for ln in lines:
        try:
            out.append(abs(int(ln.split()[5])))
        except (ValueError, IndexError):
            pass
    return out


MASTER_RE = re.compile(r"Master clock ID:\s*([0-9a-fA-F]{16})\b")
OWN_RE = re.compile(r"Own clock ID:\s*([0-9a-fA-F]{16})\b")


def parse_master_own(info: str) -> "tuple[str | None, str | None]":
    """Exactly 16 hex digits each: a reply cut off mid-line once read '00a0c9' as the master."""
    m, o = MASTER_RE.search(info), OWN_RE.search(info)
    return (m.group(1).lower() if m else None, o.group(1).lower() if o else None)


def master_id(b: Board, attempts: int = 3) -> "tuple[str | None, str | None]":
    for _ in range(attempts):
        got, own = parse_master_own(b.cmd("ptp info", quiet=1.5, timeout=15))
        if got and own:
            return got, own
        b.read_for(2.0)
    return None, None


def wait_master(b: Board, want_other: bool = True, seconds: float = 120.0) -> "tuple[str | None, str | None]":
    """Poll until the board has selected a master other than itself (or itself)."""
    deadline, got, own = time.time() + seconds, None, None
    while time.time() < deadline:
        b.read_for(5.0)
        got, own = master_id(b)
        if got and own and ((got != own) == want_other):
            break
    return got, own


def cmd_wait(b: Board, line: str, pattern: str, timeout: float, report: "Report | None" = None) -> "tuple[bool, float]":
    """Send a command, wait up to `timeout` for `pattern`. `cmd()` gives up after a short quiet
    window, which a CPU-starved board is; this waits for the reply itself."""
    b.drain()
    t0 = time.time()
    b.ser.write(line.encode() + b"\r")
    b.ser.flush()
    try:
        b.expect(pattern, timeout=timeout)
        return True, time.time() - t0
    except Exception as e:
        if report:
            got = str(e).split("--- got ---", 1)[-1].strip()
            lines = [ln for ln in got.splitlines() if ln.strip()]
            seen = "seen" if any(ln.strip() == line for ln in lines) else "NOT seen"
            report.note(f"no reply to {line!r} in {timeout:.0f} s: echo {seen}, {len(got)} chars, last {lines[-3:]}")
        return False, time.time() - t0


def fresh_samples(b: Board, seconds: float = 15.0) -> "list[str]":
    """Sync lines logged now. `ptp info` alone is not proof the board syncs: it reports the last
    master it knew, and passed against a dead network stack."""
    b.cmd("ptp log def on")
    lines = sync_lines(b.read_for(seconds))
    b.cmd("ptp log def off")
    return lines


def ping_loss(ip: str, n: int = 10) -> float:
    r = subprocess.run(["ping", "-c", str(n), "-i", "0.2", "-W", "1", "-q", ip],
                       capture_output=True, text=True, timeout=60)
    m = re.search(r"([\d.]+)% packet loss", r.stdout)
    return float(m.group(1)) if m else 100.0


def ethstat(b: Board) -> "dict[str, str]":
    for _ in range(4):
        out = b.cmd("ethstat", quiet=2.5, timeout=25)
        if "BEGIN ethstat" in out and "END ethstat" in out:
            body = out.split("BEGIN ethstat", 1)[1].split("END ethstat", 1)[0]
            return dict(ln.strip().split("=", 1) for ln in body.splitlines() if "=" in ln)
        b.read_for(2.0)
    raise RuntimeError("ethstat produced no parseable reply")


def l2_hist(b: Board) -> "dict[str, int]":
    hist = {}
    for k, v in ethstat(b).items():
        if k.startswith("nsd.l2_dst"):
            mac, _, n = v.rpartition(":")
            hist[mac.lower()] = int(n)
    return hist


def flood_bg(env: Env, seconds: float, pps: "int | None" = None) -> subprocess.Popen:
    """tools/hil/flood.py in its own process, so the flood does not share the GIL with the reader."""
    args = [sys.executable, str(FLOOD), "--iface", env.iface, "--seconds", str(seconds), "--board", env.board_ip]
    if pps:
        args += ["--pps", str(pps)]
    return subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def flood_result(p: subprocess.Popen, timeout: float = 900) -> str:
    out = p.communicate(timeout=timeout)[0].strip().splitlines()
    return out[0] if out else "flood produced no output"


def osinfo_intact(b: Board) -> "tuple[bool, int]":
    """A complete `osinfo` reply, start to end: what a garbled console cannot produce."""
    out = b.cmd("osinfo", quiet=2.0, timeout=20)
    ok = "Kernel version:" in out and "Interrupt priorities:" in out and "Build ID:" in out
    return ok, len(out)


# --------------------------------------------------------------------------------------------
# Fix regressions (each finding's own before/after test, on the firmware as built)
# --------------------------------------------------------------------------------------------


def u1(b: Board, env: Env, r: Report) -> None:
    """U1 (#20): a remote master's clock identity is not byte-reversed."""
    quiet(b)
    got, _ = wait_master(b)
    r.check("U1 master clock ID is the host's", got == env.host_gm_id, f"got {got}, want {env.host_gm_id}")


def u2(b: Board, env: Env, r: Report) -> None:
    """U2 (#21): the logid flag survives a config store/load, and the record is accepted."""
    quiet(b)
    try:
        for f in FLAGS:
            b.cmd(f"ptp log {f} on")
        out = b.cmd("ptpcfg store", quiet=1.0, timeout=10)
        m = re.search(r"logging=0x([0-9a-fA-F]{2})", out)
        byte = int(m.group(1), 16) if m else 0
        for f in FLAGS:
            b.cmd(f"ptp log {f} off")
        loaded = b.cmd("ptpcfg load", quiet=1.2, timeout=10)
        text = b.read_for(12.0)
    finally:
        quiet(b)
    r.check("U2 logid bit stored", bool(byte & 0x80), f"logging=0x{byte:02x}")
    r.check("U2 record accepted on load", "corrupted" not in loaded)
    r.check("U2 flags restored (def)", len(sync_lines(text)) > 0, f"{len(sync_lines(text))} sync lines in 12 s")


def u3(b: Board, env: Env, r: Report) -> None:
    """U3 (#25): no [LOG-LCKD] prefix without a message behind it."""
    quiet(b)
    try:
        b.cmd("ptp log logid on")
        b.cmd("ptp log locked on")
        text = b.read_for(25.0)
    finally:
        quiet(b)
    real = [ln for ln in text.splitlines() if "LOCKED" in ln or "DIVERGED" in ln]
    orphans = text.count("[LOG-LCKD]") - len(real)
    r.check("U3 no orphan [LOG-LCKD] prefixes in 25 s", orphans == 0, f"{orphans} orphans")


def u4(b: Board, env: Env, r: Report) -> None:
    """U4 (#24) and U7 (#23), lwIP only: 802.3 destinations follow the profile."""
    quiet(b)
    steps = [("gPTP", ["ptp profile preset gPTP"], {PDELAY: True, PRIMARY: False}),
             ("defp2p+802.3", ["ptp profile preset defp2p", "ptp transport 802.3"], {PDELAY: True, PRIMARY: True})]
    try:
        for label, cmds, want in steps:
            for c in cmds:
                b.cmd(c, quiet=1.5, timeout=20)
            b.cmd("ethstat clear", quiet=1.2, timeout=15)
            b.read_for(100.0)                  # past LISTENING and PRE_MASTER
            hist = l2_hist(b)
            r.note(f"{label}: " + ", ".join(f"{m} x{n}" for m, n in sorted(hist.items())))
            for mac, expected in want.items():
                r.check(f"U4 {label}: {mac} {'present' if expected else 'absent'}", (hist.get(mac, 0) > 0) == expected)
    finally:
        b.cmd("ptp profile preset default", quiet=1.5, timeout=20)


def pup(b: Board, env: Env, r: Report) -> None:
    """#28/#29, lwIP only: the IEC/IEEE 61850-9-3 preset sends to the right 802.3 groups."""
    quiet(b)
    try:
        out = b.cmd("ptp profile preset PUP", quiet=2.5, timeout=20)
        r.check("PUP preset loads as 802.3 P2P domain 93", all(k in out for k in ("802.3", "P2P", "93")))
        b.cmd("ethstat clear", quiet=1.2, timeout=15)
        b.read_for(100.0)
        hist = l2_hist(b)
        r.note("PUP 802.3 destinations: " + ", ".join(f"{m} x{n}" for m, n in sorted(hist.items())))
        r.check("PUP primary 01:1b:19:00:00:00 used", hist.get(PRIMARY, 0) > 0)
        r.check("PUP peer-delay 01:80:c2:00:00:0e used", hist.get(PDELAY, 0) > 0)
        r.check("PUP no frames to the old typo 01:18:19:00:00:00", hist.get("01:18:19:00:00:00", 0) == 0)
    finally:
        b.cmd("ptp profile preset default", quiet=2.0, timeout=20)


def gptp(b: Board, env: Env, r: Report) -> None:
    """gPTP preset against a ptp4l gPTP grandmaster: selects it and CONVERGES.

    Measured 60-120 s after slaving. The first 30 s are inside the transient a profile switch
    causes and moved by hundreds of nanoseconds from run to run; the converged figure still
    varies by a factor of a few, so the bound is loose and the median is reported.
    """
    unit("start", UNIT_GPTP)
    time.sleep(8)
    try:
        quiet(b)
        b.cmd("ptp profile preset gPTP", quiet=2.0, timeout=20)
        got, _ = wait_master(b)
        r.check("gPTP board selected the host as grandmaster", got == env.host_gm_id, f"master {got}")
        b.read_for(60.0)
        lines = fresh_samples(b, 60.0)
        offs = sorted(offsets(lines))
        r.note(f"60-120 s after slaving: {len(lines)} Sync samples, median |offset| "
               f"{offs[len(offs) // 2] if offs else None} ns, max {offs[-1] if offs else None} ns")
        r.check("gPTP syncing at 8 Sync/s (>= 400 samples in 60 s)", len(lines) >= 400, f"{len(lines)}")
        r.check("gPTP converged: max |offset| < 1 us over 60 s", bool(offs) and offs[-1] < 1000)
    finally:
        quiet(b)
        b.cmd("ptp profile preset default", quiet=1.5, timeout=20)
        unit("start", UNIT_DEFAULT)


def smss(b: Board, env: Env, r: Report) -> None:
    """SLAVE -> MASTER -> SLAVE (#33's scenario): the servo recovers on the way back."""
    quiet(b)
    try:
        b.cmd("ptp log bmca on")
        b.read_for(65.0)
        unit("stop", UNIT_DEFAULT)
        t = b.read_for(45.0)
        went_master = "-> MASTER" in t
        b.read_for(20.0)
        unit("start", UNIT_DEFAULT)
        b.cmd("ptp log def on")
        t = b.read_for(75.0)
    finally:
        quiet(b)
        unit("start", UNIT_DEFAULT)
    rows = []
    for ln in sync_lines(t):
        f = ln.split()
        try:
            rows.append((int(f[5]), float(f[8])))
        except (ValueError, IndexError):
            rows.append((None, float("nan")))
    bad = [c for _, c in rows if not math.isfinite(c) or abs(c) > 1e6]
    tail = [abs(d) for d, _ in rows[-15:] if d is not None]
    r.note(f"{len(rows)} Sync lines after return; first 4 (Dt ns, corr ppb): {rows[:4]}; "
           f"last 15 |Dt| max {max(tail) if tail else None} ns")
    r.check("smss board went MASTER when the host master stopped", went_master)
    r.check("smss board returned to SLAVE", "-> SLAVE" in t)
    r.check("smss no non-finite or absurd servo output after return", not bad, f"{len(bad)} bad values")
    r.check("smss converged after return (last 15 within 2 us)", bool(tail) and max(tail) < 2000)


def stopstart(b: Board, env: Env, r: Report) -> None:
    """U9 (#34) and U11: stopping PTP does not take the network stack, or PTP's restart, with it.
    Two cycles, because a teardown that leaks something usually shows on the second."""
    quiet(b)
    r.check("stopstart ping before any stop", ping_loss(env.board_ip) == 0.0)
    for cycle in (1, 2):
        out = b.cmd("flexptp stop", quiet=2.0, timeout=20)
        r.check(f"stopstart cycle {cycle}: flexptp stop returned", "flexPTP stopped" in out, out.strip()[-60:])
        loss = ping_loss(env.board_ip)
        r.check(f"stopstart cycle {cycle}: stack alive after stop (ping)", loss == 0.0, f"{loss}% loss")
        try:
            ethstat(b)
            r.check(f"stopstart cycle {cycle}: CLI responsive after stop", True)
        except RuntimeError as e:
            r.check(f"stopstart cycle {cycle}: CLI responsive after stop", False, str(e))
        b.cmd("flexptp", quiet=2.0, timeout=20)
        got, _ = wait_master(b)
        fresh = fresh_samples(b)
        r.check(f"stopstart cycle {cycle}: PTP syncing again after restart",
                got == env.host_gm_id and len(fresh) >= 5, f"master {got}, {len(fresh)} fresh samples in 15 s")
    r.check("stopstart final ping", ping_loss(env.board_ip) == 0.0)


def _board_master_vs_ptp4l(b: Board, env: Env, r: Report, label: str, matched: bool) -> "tuple[int, bool]":
    unit("start", UNIT_SLAVE)                  # Conflicts= stops the host masters
    quiet(b)
    b.cmd("ptp profile preset default", quiet=2.0, timeout=20)
    if matched:
        # "match", NOT the "matched" the command's own help advertises: the parser compares
        # against "match", and "matched" falls through to atoi() == 0, silently.
        out = b.cmd("ptp period delreq match", quiet=2.0, timeout=20)
        r.check(f"U8 {label}: board reports Delay_Req period MATCHED", "MATCHED" in out)
    wait_master(b, want_other=False)
    since = now()
    unit("restart", UNIT_SLAVE)                # a fresh ptp4l: it rate-limits the warning to 1/300 s
    b.read_for(80.0)
    log = journal(UNIT_SLAVE, since)
    bogus, slave = log.count("bogus delay request interval"), "to SLAVE" in log
    offs = [ln.split("master offset")[1].split()[0] for ln in log.splitlines() if "master offset" in ln][-5:]
    r.note(f"{label}: ptp4l reached SLAVE={slave}, bogus warnings={bogus}, last offsets {offs}")
    return bogus, slave


def master_delresp(b: Board, env: Env, r: Report) -> None:
    """U8 (#36, issue #30): as master, Delay_Resp carries logMinDelayReqInterval, not 0x7F --
    for an ordinary Delay_Req period and for SYNCMATCHED, which is 127 itself."""
    try:
        for label, matched in (("delreq period 0", False), ("delreq SYNCMATCHED", True)):
            bogus, slave = _board_master_vs_ptp4l(b, env, r, label, matched)
            r.check(f"U8 {label}: ptp4l slaved to the board", slave)
            r.check(f"U8 {label}: no 'bogus delay request interval'", bogus == 0, f"{bogus} warnings")
    finally:
        unit("start", UNIT_DEFAULT)
        b.cmd("ptp profile preset default", quiet=2.0, timeout=20)


# --------------------------------------------------------------------------------------------
# Load: floods while doing things
# --------------------------------------------------------------------------------------------

# What each firmware is expected to manage at 20 kpps (20 Mbit/s), measured: ThreadX answers the
# CLI in 0.2 s; FreeRTOS + lwIP gets no CPU to the CLI at all, on every image tested including the
# pre-session one and with FreeRTOS and lwIP put back to their old pins.
CLI_SERVED_AT_20KPPS = {"threadx": True, "freertos": False}


def gptp_flood(b: Board, env: Env, r: Report) -> None:
    """gPTP slave through a 120 s saturation flood: still the host's slave after, back at full rate."""
    unit("start", UNIT_GPTP)
    time.sleep(8)
    try:
        quiet(b)
        b.cmd("ptp profile preset gPTP", quiet=2.0, timeout=20)
        got, _ = wait_master(b)
        r.check("gptp_flood slaved before the flood", got == env.host_gm_id, f"master {got}")
        b.read_for(30.0)
        fl = flood_bg(env, 120)
        b.read_for(60.0)
        during = fresh_samples(b, 20.0)
        r.note("flood: " + flood_result(fl))
        r.observed(f"{len(during)} Sync samples in 20 s during the flood (8/s nominal)")
        b.read_for(15.0)
        got, _ = master_id(b)
        after = fresh_samples(b, 30.0)
        tail = offsets(after[len(after) // 2:])
        r.check("gptp_flood still slaved to the host after the flood", got == env.host_gm_id, f"master {got}")
        r.check("gptp_flood back at full rate (>= 200 samples in 30 s)", len(after) >= 200, f"{len(after)}")
        r.check("gptp_flood offset under 1 us after the flood", bool(tail) and max(tail) < 1000,
                f"max {max(tail) if tail else None} ns")
    finally:
        quiet(b)
        b.cmd("ptp profile preset default", quiet=2.0, timeout=20)
        unit("start", UNIT_DEFAULT)


def master_flood(b: Board, env: Env, r: Report) -> None:
    """The board as master through a 120 s flood, judged by the ptp4l slave watching it."""
    unit("start", UNIT_SLAVE)
    try:
        quiet(b)
        b.cmd("ptp profile preset default", quiet=2.0, timeout=20)
        wait_master(b, want_other=False)
        unit("restart", UNIT_SLAVE)
        b.read_for(60.0)
        t0 = now()
        fl = flood_bg(env, 120)
        res = flood_result(fl)
        t1 = now()
        b.read_for(120.0)
        dur, aft = journal(UNIT_SLAVE, t0, t1), journal(UNIT_SLAVE, t1)
        offs = lambda log: [abs(int(m)) for m in re.findall(r"master offset\s+(-?\d+)", log)]
        od, oa = offs(dur), offs(aft)
        trans = [ln.split("]: ")[-1] for ln in (dur + aft).splitlines() if " to " in ln and "port 1" in ln]
        r.note("flood: " + res)
        r.observed(f"ptp4l {'kept' if len(od) >= 100 else 'lost'} the board during the flood "
                   f"({len(od)} offset reports)")
        back = (not trans) or "to SLAVE" in trans[-1]
        tail = oa[-10:]
        r.check("master_flood ptp4l back in SLAVE after the flood", back, trans[-1] if trans else "never left")
        r.check("master_flood last 10 offsets after the flood within 1 us", len(tail) == 10 and max(tail) < 1000, f"{tail}")
        r.check("master_flood no 'bogus delay request interval'", "bogus delay request interval" not in dur + aft)
    finally:
        unit("start", UNIT_DEFAULT)
        b.cmd("ptp profile preset default", quiet=2.0, timeout=20)


def _stopstart_under(b: Board, env: Env, r: Report, tag: str, pps: "int | None", served: bool) -> None:
    """Two stop/start cycles during a 150 s flood. Where the CLI is starved (`served` False),
    each command must still complete -- by the time the flood ends -- and nothing may be lost."""
    quiet(b)
    secs = 150.0
    fl = flood_bg(env, secs, pps)
    t0 = time.time()
    b.read_for(20.0)
    for cycle in (1, 2):
        for line, pat, what in (("flexptp stop", r"flexPTP stopped|is not running", "stop"),
                                ("flexptp", r"Starting flexPTP|already up", "restart")):
            # Served: 5 s. Starved: until 60 s after the flood ends, and never less than 30 s.
            limit = 5.0 if served else max(30.0, secs + 60.0 - (time.time() - t0))
            ok, dt = cmd_wait(b, line, pat, limit, r)
            if served:
                r.check(f"{tag} cycle {cycle}: {what} answered under load", ok, f"{dt:.1f} s")
            else:
                r.observed(f"cycle {cycle} {what} answered after {dt:.1f} s" + ("" if ok else " (NOT answered)"))
                r.check(f"{tag} cycle {cycle}: {what} completed by the end of the load", ok)
            b.read_for(5.0)
    r.note("flood: " + flood_result(fl))
    b.read_for(10.0)
    r.check(f"{tag} stack alive after the load (ping)", ping_loss(env.board_ip) == 0.0)
    got, _ = wait_master(b)
    fresh = fresh_samples(b)
    r.check(f"{tag} PTP syncing again afterwards", got == env.host_gm_id and len(fresh) >= 5,
            f"master {got}, {len(fresh)} fresh samples")


def stopstart_flood(b: Board, env: Env, r: Report) -> None:
    """U9/U11 under a saturation flood. No firmware serves its CLI at full rate, so the commands
    are only required to complete by the time the flood ends."""
    _stopstart_under(b, env, r, "stopstart_flood", None, served=False)


def stopstart_load(b: Board, env: Env, r: Report) -> None:
    """U9/U11 at 20 kpps: answered promptly where the firmware serves its CLI at that rate."""
    _stopstart_under(b, env, r, "stopstart_load", 20000, served=CLI_SERVED_AT_20KPPS[env.firmware])


def _cli_under(b: Board, env: Env, r: Report, tag: str, pps: "int | None", served: bool) -> None:
    quiet(b)
    secs = 120.0
    fl = flood_bg(env, secs, pps)
    t0 = time.time()
    b.read_for(10.0)
    lat = []
    while time.time() - t0 < secs - 15:
        ok, dt = cmd_wait(b, "osinfo", r"Kernel version:\s*\S+", 60)
        lat.append(round(dt, 1) if ok else None)
        b.read_for(max(0.0, 15.0 - dt))
    r.note("flood: " + flood_result(fl))
    r.observed(f"osinfo reply latency during the load (s): {lat}")
    if served:
        r.check(f"{tag} every command answered within 5 s", all(x is not None and x <= 5 for x in lat), f"{lat}")
    ok, dt = cmd_wait(b, "osinfo", r"Kernel version:\s*\S+", 30)
    r.check(f"{tag} CLI answers within 30 s of the load ending", ok, f"{dt:.1f} s")
    intact, n = osinfo_intact(b)
    r.check(f"{tag} console intact afterwards (complete osinfo)", intact, f"{n} chars")


def cli_flood(b: Board, env: Env, r: Report) -> None:
    """`osinfo` every 15 s through a saturation flood: the CLI comes back, the console intact.

    This is the load that found all three FreeRTOS bugs of November 2026 (CLI stack overflow,
    stale pbuf tx_cb, console flag desync); every one of them failed the 'afterwards' checks."""
    _cli_under(b, env, r, "cli_flood", None, served=False)


def cli_load(b: Board, env: Env, r: Report) -> None:
    """`osinfo` every 15 s at 20 kpps: answered within 5 s where the firmware serves its CLI."""
    _cli_under(b, env, r, "cli_load", 20000, served=CLI_SERVED_AT_20KPPS[env.firmware])


def console_flood(b: Board, env: Env, r: Report) -> None:
    """A silent 120 s flood -- nothing sent to the console during it -- then three complete
    `osinfo` replies and a ping. The pre-session FreeRTOS image failed this 1 run in 2."""
    quiet(b)
    b.read_for(10.0)
    intact0, n0 = osinfo_intact(b)
    fl = flood_bg(env, 120)
    b.read_for(122.0)
    r.note("flood: " + flood_result(fl))
    b.read_for(15.0)
    res = [osinfo_intact(b) for _ in range(3)]
    loss = ping_loss(env.board_ip)
    r.check("console_flood osinfo intact before", intact0, f"{n0} chars")
    r.check("console_flood osinfo intact after, 3 of 3", all(ok for ok, _ in res), f"{[n for _, n in res]} chars")
    r.check("console_flood stack alive after (ping)", loss == 0.0, f"{loss}% loss")
    if env.elf:
        try:
            from swd import Swd
            c = Swd(env.elf).console()
            r.check("console_flood console thread in step (SWD: no stale TX-complete flag while idle)",
                    not c["desynced"], f"flag {c['tx_cplt_flag']}, FIFO used {c['fifo_used']}, DMA idle {c['dma_idle']}")
        except Exception as e:      # a missing toolchain is not a firmware failure
            r.note(f"SWD console check not run: {e}")


# --------------------------------------------------------------------------------------------
# Robustness and health
# --------------------------------------------------------------------------------------------


def fuzz(b: Board, env: Env, r: Report, classes: "list[str] | None" = None) -> None:
    """Malformed and foreign PTP traffic; see ptp_fuzz.py for the classes and why each must be
    ignored. Every class: the Sync rate holds (>= 80% of a quiet reference) with no offset over
    10 us, and the board keeps the host as master."""
    names = classes or list(ptp_fuzz.CLASSES)
    sock = ptp_fuzz.sender(env.host_ip)
    quiet(b)
    got, _ = wait_master(b)
    r.check("fuzz board slaved to the host before", got == env.host_gm_id, f"master {got}")
    b.read_for(20.0)
    ref = ptp_fuzz.window(b, sock, sync_lines)
    r.note(f"reference: {len(ref)} Sync cycles in {ptp_fuzz.WINDOW:.0f} s, max |offset| "
           f"{max(offsets(ref)) if ref else None} ns")
    for n in names:
        lines = ptp_fuzz.window(b, sock, sync_lines, n)
        offs = offsets(lines)
        got, _ = master_id(b)
        frac = len(lines) / max(1, len(ref))
        big = sum(1 for o in offs if o > 10_000)
        r.note(f"{n:9s} {ptp_fuzz.CLASSES[n][2]}: {len(lines)} cycles ({frac:.0%}), "
               f"max |offset| {max(offs) if offs else None} ns, {big} over 10 us")
        r.check(f"fuzz {n}: ignored (Sync rate >= 80% of reference, no offset over 10 us)",
                frac >= 0.8 and big == 0, f"{len(lines)}/{len(ref)}, {big} big")
        r.check(f"fuzz {n}: still slaved to the host", got == env.host_gm_id, f"master {got}")
        b.read_for(10.0)                       # let a disturbed servo settle before the next class
    b.read_for(30.0)
    r.check("fuzz stack alive afterwards (ping)", ping_loss(env.board_ip) == 0.0)
    fo = offsets(fresh_samples(b, 30.0))
    got, _ = master_id(b)
    r.check("fuzz recovered: host master, syncing, last 5 offsets < 1 us",
            got == env.host_gm_id and len(fo) >= 5 and max(fo[-5:]) < 1000, f"master {got}, last {fo[-5:]}")


def health(b: Board, env: Env, r: Report) -> None:
    """Is the board alive and its memory consistent? Meant for the END of a long run.

    Liveness by ping, a complete osinfo and fresh Sync samples; with --elf, over SWD as well: a
    FreeRTOS heap_4 walk, and if the board is not alive, a snapshot and a fault decode, which is
    what told the November 2026 wedges apart."""
    b.read_for(3.0)
    loss = ping_loss(env.board_ip)
    intact, n = osinfo_intact(b)
    fresh = fresh_samples(b, 12.0)
    alive = loss == 0.0 and intact
    r.check("health stack alive (ping)", loss == 0.0, f"{loss}% loss")
    r.check("health console intact (complete osinfo)", intact, f"{n} chars")
    r.check("health PTP syncing (fresh Sync samples)", len(fresh) >= 5, f"{len(fresh)} in 12 s")
    if not env.elf:
        r.note("no --elf: SWD checks skipped")
        return
    try:
        from swd import Swd
        swd = Swd(env.elf)
        if swd.is_freertos:
            rep, tasks = swd.freertos_heap_and_tasks()
            r.check("health FreeRTOS heap_4 consistent (SWD walk)", rep.ok, "; ".join(rep.problems) or
                    f"{len(rep.blocks)} blocks, {rep.free_bytes_walked} B free")
        if not alive:
            snap = swd.snapshot(20)
            r.note(f"SWD snapshot: tick advanced {snap.get('tick_advanced')}; vars {snap.get('vars')}; "
                   f"halted in {swd.syms.func(snap.get('regs', {}).get('pc', 0))}")
            f = swd.fault()
            if f["cfsr"] or f["hfsr"]:
                r.note(f"FAULT: CFSR {f['cfsr']:#x} HFSR {f['hfsr']:#x} {f['cfsr_bits']}; frames "
                       + "; ".join(f"{x['exception_name']} pc {x['pc']:#x} lr {x['lr_func']}" for x in f["frames"]))
    except Exception as e:
        r.note(f"SWD checks not run: {e}")


# Minimum untouched bytes per stack, after every deep CLI command has run. The FreeRTOS CLI held
# 56 bytes under load before its osinfo table moved off the stack, and overflowed; it holds 640+
# now. 256 leaves room for the context a preemption saves, with margin.
MIN_STACK_HEADROOM = 256


def stacks(b: Board, env: Env, r: Report) -> None:
    """Every task's stack low-water mark over SWD, after the deepest CLI commands have run."""
    if not env.elf:
        r.check("stacks needs --elf (the image on the board)", False)
        return
    from swd import Swd
    quiet(b)
    for c in ["osinfo"] * 3 + ["ethstat", "ptp info", "flexptp stop", "flexptp",
                               "ptp profile preset gPTP", "ptp profile preset default", "osinfo"]:
        b.cmd(c, quiet=2.0, timeout=20)
    quiet(b)
    swd = Swd(env.elf)
    rows = ([(t.name, t.untouched) for t in swd.freertos_heap_and_tasks()[1]] if swd.is_freertos
            else [(t.name, t.untouched) for t in swd.threadx_threads()])
    for name, u in sorted(rows, key=lambda x: x[1]):
        r.note(f"{name:22s} {u:5d} B never touched")
    low = [(n, u) for n, u in rows if 0 <= u < MIN_STACK_HEADROOM]
    r.check(f"stacks every task keeps >= {MIN_STACK_HEADROOM} B untouched", not low, f"{low}" if low else "")


# --------------------------------------------------------------------------------------------
# Registry
# --------------------------------------------------------------------------------------------

@dataclass(frozen=True)
class Scenario:
    fn: object
    minutes: float
    firmwares: "tuple[str, ...]" = ("freertos", "threadx")


SCENARIOS = {
    "u1": Scenario(u1, 1), "u2": Scenario(u2, 1), "u3": Scenario(u3, 1),
    "u4": Scenario(u4, 4, ("freertos",)), "pup": Scenario(pup, 2, ("freertos",)),
    "gptp": Scenario(gptp, 3), "smss": Scenario(smss, 4), "stopstart": Scenario(stopstart, 3),
    "master_delresp": Scenario(master_delresp, 5),
    "gptp_flood": Scenario(gptp_flood, 5), "master_flood": Scenario(master_flood, 6),
    "stopstart_flood": Scenario(stopstart_flood, 5), "stopstart_load": Scenario(stopstart_load, 5),
    "cli_flood": Scenario(cli_flood, 3), "cli_load": Scenario(cli_load, 3),
    "console_flood": Scenario(console_flood, 3),
    "fuzz": Scenario(fuzz, 8), "stacks": Scenario(stacks, 1), "health": Scenario(health, 1),
}

PRESETS = {
    "fixes": ["u1", "u2", "u3", "u4", "pup", "gptp", "smss", "stopstart", "master_delresp"],
    "load": ["gptp_flood", "master_flood", "stopstart_flood", "cli_flood", "cli_load", "stopstart_load",
             "console_flood"],
    "robust": ["fuzz"],
    "finish": ["stacks", "health"],
}
PRESETS["all"] = PRESETS["fixes"] + PRESETS["load"] + PRESETS["robust"] + PRESETS["finish"]
# A preset may not share a scenario's name: `health` once expanded to the preset, not the scenario.
assert not set(PRESETS) & set(SCENARIOS), set(PRESETS) & set(SCENARIOS)
