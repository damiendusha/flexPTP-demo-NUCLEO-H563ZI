#!/usr/bin/env python3
"""Hardware-in-the-loop runner for the flexPTP demo.

    ./run.py --stage S0                     run the checks valid at stage S0
    ./run.py --stage S0 --save baseline.json
    ./run.py --stage S6 --compare baseline.json
    ./run.py --checks boot,cli              run named checks only
    ./run.py --flash build/Debug/flexptp-demo.elf --stage S0

Exit status is 0 only when every selected check PASSed. A SKIP is not a pass: the suite is
reported as incomplete, because "we could not test it" and "it works" are different claims
and this project has been bitten by conflating them.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from checks import (ALL_CHECKS, EXPECTED_IRQ_PRIORITIES_FREERTOS,
                    EXPECTED_IRQ_PRIORITIES_THREADX, FREERTOS_MAX_SYSCALL_PRIO,
                    FREERTOS_SYSCALL_ISRS, STAGES, STAGES_WITHOUT_IP_THREAD,
                    STAGES_WITHOUT_PHY, CheckResult, Context, Status)
from hil import DEFAULT_OPENOCD, Board, BoardError, OpenOcd

GREEN, RED, YELLOW, DIM, RESET = (
    "\033[32m",
    "\033[31m",
    "\033[33m",
    "\033[2m",
    "\033[0m",
)
COLOUR = {Status.PASS: GREEN, Status.FAIL: RED, Status.SKIP: YELLOW}

# Everything that differs between the two firmwares, in one place.
#
# Kept as a table rather than as separate flags because the fields are not independent: a run
# against the FreeRTOS image with the ThreadX priority table is not a partial check, it is a
# FAILING one, and before this existed the only per-firmware knob was --cli-commands. Getting
# that wrong was survivable; getting a priority table wrong is a red suite that says nothing.
#
# The values themselves, and why the two differ where they do, are documented next to the
# tables in checks.py.
FIRMWARES = {
    "threadx": {
        "cli_commands": 25,
        "irq_priorities": EXPECTED_IRQ_PRIORITIES_THREADX,
    },
    "freertos": {
        "cli_commands": 25,
        "irq_priorities": EXPECTED_IRQ_PRIORITIES_FREERTOS,
        "max_syscall_prio": FREERTOS_MAX_SYSCALL_PRIO,
        "syscall_isrs": FREERTOS_SYSCALL_ISRS,
    },
}

# Tolerances for --compare. Absolute figures move with the network and the host's load; the
# point of comparison is to catch a regression, not to pin noise.
TOLERANCE = {
    "ping.rtt_avg_ms": 2.0,  # multiplier
    "osinfo.free_bytes": 0.5,  # multiplier, lower bound only
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sel = p.add_mutually_exclusive_group()
    sel.add_argument("--stage", choices=sorted(STAGES), help="run the checks valid at this stage")
    sel.add_argument("--checks", help="comma-separated check names")
    p.add_argument("--list", action="store_true", help="list checks and stages, then exit")
    p.add_argument("--port", default="/dev/ttyACM0")
    p.add_argument("--firmware", choices=sorted(FIRMWARES), default="threadx",
                   help="which firmware is on the board. Selects every per-firmware expectation "
                        "at once -- the CLI command count and the interrupt priority table, "
                        "which differ because the two kernels require different things. "
                        "Default: threadx.")
    p.add_argument("--cli-commands", type=int, default=None,
                   help="override --firmware's registered CLI command count. Rarely needed; "
                        "prefer --firmware, which sets this along with everything else.")
    p.add_argument("--openocd", type=Path,
                   help="OpenOCD to use. Defaults to the search in hil.find_openocd(): "
                        "$OPENOCD, then toolchains/openocd/bin/openocd, then PATH.")
    p.add_argument("--iface", default="enp5s0", help="host interface facing the board")
    p.add_argument("--flash", type=Path, help="flash this ELF before running")
    p.add_argument("--elf", type=Path,
                   help="the ELF the board is expected to be running, for the build_id check. "
                        "Defaults to --flash. Give it on its own to check a board you did not "
                        "just flash.")
    p.add_argument("--save", type=Path, help="write results as JSON (the baseline)")
    p.add_argument("--compare", type=Path, help="compare results against a saved baseline")
    p.add_argument("--settle", type=float, default=90.0, help="PTP settle window, seconds")
    return p.parse_args()


def select(args: argparse.Namespace) -> list[str]:
    if args.checks:
        names = [c.strip() for c in args.checks.split(",") if c.strip()]
        unknown = [n for n in names if n not in ALL_CHECKS]
        if unknown:
            sys.exit(f"unknown check(s): {', '.join(unknown)}")
        return names
    if args.stage:
        return STAGES[args.stage]
    return STAGES["S0"]


def compare(results: list[CheckResult], baseline: dict) -> list[str]:
    """Return a list of human-readable regressions against the baseline."""
    problems: list[str] = []
    base = {c["name"]: c for c in baseline.get("checks", [])}
    for r in results:
        b = base.get(r.name)
        if b is None:
            continue
        if b["status"] == Status.PASS.value and r.status is not Status.PASS:
            problems.append(f"{r.name}: was PASS in baseline, now {r.status.value}")
            continue
        for key, mult in TOLERANCE.items():
            check, _, field = key.partition(".")
            if check != r.name:
                continue
            now, then = r.data.get(field), b.get("data", {}).get(field)
            if not isinstance(now, (int, float)) or not isinstance(then, (int, float)):
                continue
            if field == "free_bytes":
                if now < then * mult:
                    problems.append(f"{key}: {now} < {then * mult:.0f} (baseline {then})")
            elif now > then * mult:
                problems.append(f"{key}: {now} > {then * mult:.2f} (baseline {then})")
    return problems


def main() -> int:
    args = parse_args()

    if args.list:
        print("checks:")
        for name in ALL_CHECKS:
            print(f"  {name}")
        print("\nstages:")
        for stage, names in STAGES.items():
            print(f"  {stage:4s} {', '.join(names)}")
        return 0

    names = select(args)
    ctx = Context(
        host_iface=args.iface,
        ptp_settle_s=args.settle,
        expect_phy=(args.stage not in STAGES_WITHOUT_PHY),
        expect_ip_thread=(args.stage not in STAGES_WITHOUT_IP_THREAD),
        elf=(args.elf or args.flash),
        **FIRMWARES[args.firmware],
        **({"cli_commands": args.cli_commands} if args.cli_commands else {}),
    )

    if args.flash:
        print(f"{DIM}flashing {args.flash}{RESET}")
        try:
            OpenOcd(binary=args.openocd or DEFAULT_OPENOCD).flash(args.flash)
        except BoardError as e:
            print(f"{RED}flash failed:{RESET} {e}")
            return 2

    results: list[CheckResult] = []
    try:
        # The SAME OpenOCD for flashing and for reset(). Board() constructs its own by
        # default, so not threading the flag here would let one run flash with one binary and
        # reset with another.
        with Board(port=args.port,
                   openocd=OpenOcd(binary=args.openocd or DEFAULT_OPENOCD)) as board:
            for name in names:
                print(f"{DIM}· {name}…{RESET}", flush=True)
                try:
                    r = ALL_CHECKS[name](board, ctx)
                except BoardError as e:
                    r = CheckResult(name, Status.FAIL, str(e))
                except Exception as e:  # a broken check must not look like a broken board
                    r = CheckResult(name, Status.FAIL, f"check raised {type(e).__name__}: {e}")
                results.append(r)
                print(f"  {COLOUR[r.status]}{r.status.value:4s}{RESET} {name:10s} {r.detail}")
    except BoardError as e:
        print(f"{RED}cannot talk to the board:{RESET} {e}")
        return 2

    passed = sum(r.status is Status.PASS for r in results)
    failed = sum(r.status is Status.FAIL for r in results)
    skipped = sum(r.status is Status.SKIP for r in results)
    print(f"\n{passed} passed, {failed} failed, {skipped} skipped")

    if args.save:
        payload = {
            "stage": args.stage,
            "checks": [
                {"name": r.name, "status": r.status.value, "detail": r.detail, "data": r.data}
                for r in results
            ],
        }
        args.save.write_text(json.dumps(payload, indent=2) + "\n")
        print(f"{DIM}wrote {args.save}{RESET}")

    if args.compare:
        problems = compare(results, json.loads(args.compare.read_text()))
        if problems:
            print(f"\n{RED}regressions vs {args.compare}:{RESET}")
            for p in problems:
                print(f"  - {p}")
            return 1
        print(f"{GREEN}no regressions vs {args.compare}{RESET}")

    if failed:
        return 1
    if skipped:
        print(f"{YELLOW}incomplete: {skipped} check(s) could not run{RESET}")
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
