#!/usr/bin/env python3
"""Run the long hardware scenarios (scenarios.py) against the board.

    tools/hil/scenario.py --list
    tools/hil/scenario.py --firmware freertos --elf IMAGE.elf fixes            a preset
    tools/hil/scenario.py --firmware threadx --elf IMAGE.elf gptp stopstart     named scenarios
    tools/hil/scenario.py --firmware freertos --elf IMAGE.elf --flash all       flash first

Presets: fixes (each upstream finding's regression test), load (floods while doing things),
robust (malformed and foreign PTP), finish (stack headroom and end-of-run health over SWD),
all. A scenario that does not apply to the firmware -- u4 and pup need lwIP -- is skipped and
said so.

With --elf, the board's build ID is compared with the image before anything runs, and the run
refuses to start on a mismatch: a long campaign against the wrong firmware is the most expensive
way there is to learn nothing. --elf also enables the SWD parts of console_flood, stacks and
health.

Exit status 0 only when no assertion FAILed. OBSERVED lines never fail a run.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

from checks import read_elf_build_id
from hil import Board, OpenOcd
from scenarios import PRESETS, SCENARIOS, Env, Report, quiet, unit, UNIT_DEFAULT


def expand(names: "list[str]") -> "list[str]":
    out: "list[str]" = []
    for n in names:
        for x in PRESETS.get(n, [n]):
            if x not in SCENARIOS:
                raise SystemExit(f"unknown scenario or preset: {x} (see --list)")
            out.append(x)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="scenarios and/or presets")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--firmware", choices=["freertos", "threadx"])
    ap.add_argument("--elf", type=Path, help="the image on the board (checked by build ID)")
    ap.add_argument("--flash", action="store_true", help="flash --elf first")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--iface", default="enp5s0")
    ap.add_argument("--board-ip", default="192.168.2.191")
    ap.add_argument("--host-ip", default="192.168.2.1")
    ap.add_argument("--save", type=Path, help="write the report lines and counts as JSON")
    a = ap.parse_args()

    if a.list:
        for name, sc in SCENARIOS.items():
            doc = (sc.fn.__doc__ or "").strip().splitlines()[0]
            only = "" if len(sc.firmwares) == 2 else f" [{sc.firmwares[0]} only]"
            print(f"  {name:16s} ~{sc.minutes:>2.0f} min{only}  {doc}")
        print()
        for p, items in PRESETS.items():
            print(f"  preset {p:9s} {' '.join(items)}")
        return 0
    if not a.firmware or not a.names:
        ap.error("--firmware and at least one scenario or preset are required (or --list)")

    todo = expand(a.names)
    env = Env(firmware=a.firmware, board_ip=a.board_ip, iface=a.iface, host_ip=a.host_ip, elf=a.elf)
    report = Report()
    if a.flash:
        if not a.elf:
            ap.error("--flash needs --elf")
        print(f"flashing {a.elf}")
        OpenOcd().flash(a.elf)
    unit("start", UNIT_DEFAULT)

    t_start = time.time()
    per: "list[tuple[str, int, int, float]]" = []
    with Board(port=a.port) as board:
        board.read_for(25.0 if a.flash else 2.0)        # boot and DHCP after a flash
        if a.elf:
            out = board.cmd("osinfo", quiet=1.5, timeout=15)
            m = re.search(r"Build ID:\s*([0-9a-f]{8,})", out)
            want = read_elf_build_id(a.elf)
            if not m or m.group(1) != want:
                print(f"REFUSING: board reports {m.group(1)[:16] if m else 'no build ID'}, "
                      f"{a.elf.name} is {want[:16] if want else '?'}")
                return 2
            print(f"build ID {want[:16]}… matches {a.elf.name}")
        for name in todo:
            sc = SCENARIOS[name]
            print(f"== {name} ==", flush=True)
            if a.firmware not in sc.firmwares:
                print(f"  SKIP  {name}: {sc.firmwares[0]} only")
                continue
            f0, p0, t0 = report.fails, report.passes, time.time()
            try:
                sc.fn(board, env, report)
            except Exception as e:                       # a crash in one scenario is its FAIL
                report.check(f"{name} ran to completion", False, f"{type(e).__name__}: {e}")
                try:
                    quiet(board)
                except Exception:
                    pass
            per.append((name, report.passes - p0, report.fails - f0, time.time() - t0))

    print("\n== summary ==")
    for name, p, f, dt in per:
        print(f"  {'FAIL' if f else 'ok  '}  {name:16s} {p} passed, {f} failed, {dt / 60:.1f} min")
    print(f"{report.passes} passed, {report.fails} failed in {(time.time() - t_start) / 60:.0f} min")
    if a.save:
        a.save.write_text(json.dumps({"firmware": a.firmware, "elf": str(a.elf) if a.elf else None,
                                      "passes": report.passes, "fails": report.fails,
                                      "scenarios": per, "lines": report.lines}, indent=1))
    return 0 if report.fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
