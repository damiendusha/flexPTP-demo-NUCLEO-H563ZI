#!/usr/bin/env python3
"""Look inside a running (or dead) board over SWD, without a debugger session or debug info.

    tools/hil/swd.py --elf IMAGE.elf snapshot      where is the CPU, and is the scheduler alive?
    tools/hil/swd.py --elf IMAGE.elf heap          FreeRTOS heap_4 integrity walk
    tools/hil/swd.py --elf IMAGE.elf stacks        untouched bytes on every task/thread stack
    tools/hil/swd.py --elf IMAGE.elf fault         decode a HardFault the board is parked in
    tools/hil/swd.py --elf IMAGE.elf console       state of the console's FIFO, flag, UART, DMA
    tools/hil/swd.py --elf IMAGE.elf all

WHY THIS EXISTS
The firmware's only diagnostic channel is its console, and the failures that matter most are the
ones that take the console down with them. Every one of the three FreeRTOS wedges found in
November 2026 (docs/flexptp_upstream_review.md, section 7) presented identically -- garbled
console, then silence, then no ping -- and was told apart only by reading the target over SWD:
a stack overflow (hook entered from PendSV, heap header overwritten), a HardFault in the ETH
ISR (stacked PC 0xfffffe00), and a console thread one completion flag out of step. This module
is those investigations made repeatable.

HOW
The images carry no debug info, so every address comes from `nm`, and every structure is read
by its documented layout (FreeRTOS TCB and List_t, heap_4's BlockLink_t, ThreadX TX_THREAD and
TX_BYTE_POOL). Memory is read in ONE short halt per operation -- a few kilobytes, milliseconds --
and the core is resumed whatever happens: an error between a halt and its resume once left the
core halted, which looks exactly like a wedged board (frozen tick, no ping, no console). Parsing
is done here in Python on the dumped bytes, by pure functions the unit tests drive directly
(test_swd.py).

`snapshot` additionally samples the PC N times, halting for well under a millisecond each, which
is the poor man's profiler that showed where a starved FreeRTOS board spends its time.
"""

from __future__ import annotations

import argparse
import bisect
import collections
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

from hil import DEFAULT_BOARD_CFG, DEFAULT_OPENOCD

# --------------------------------------------------------------------------------------------
# Toolchain and symbols
# --------------------------------------------------------------------------------------------


def find_arm_tool(name: str, env: "dict[str, str] | None" = None) -> "Path | None":
    """Locate an arm-none-eabi binutil: $ARM_TOOLCHAIN_BIN, then PATH, then Bazel's toolchain.

    The last is the toolchain the firmware is built with (MODULE.bazel's arm_gnu_toolchain), so
    a machine that can build can always inspect, without a separate install.
    """
    env = os.environ if env is None else env
    tool = f"arm-none-eabi-{name}"
    override = env.get("ARM_TOOLCHAIN_BIN")
    if override and (Path(override) / tool).exists():
        return Path(override) / tool
    on_path = shutil.which(tool, path=env.get("PATH"))
    if on_path:
        return Path(on_path)
    home = env.get("HOME", str(Path.home()))
    hits = sorted(glob.glob(f"{home}/.cache/bazel/_bazel_*/*/external/*arm_gnu_toolchain/bin/{tool}"),
                  key=os.path.getmtime, reverse=True)
    return Path(hits[0]) if hits else None


@dataclass(frozen=True)
class Sym:
    addr: int
    size: int
    kind: str  # nm's type letter
    name: str


class Symbols:
    """`nm -S -n` of an image. Static symbols can share a name across files -- `flags`, `lineBuf`
    -- so lookup by name returns all of them and `near()` picks by proximity."""

    def __init__(self, syms: "list[Sym]"):
        self.all = sorted(syms, key=lambda s: s.addr)
        self.by_name: "dict[str, list[Sym]]" = collections.defaultdict(list)
        for s in self.all:
            self.by_name[s.name].append(s)
        self._code = [s for s in self.all if s.kind in "tTwW"]
        self._code_addrs = [s.addr for s in self._code]

    @classmethod
    def parse(cls, nm_output: str) -> "Symbols":
        syms = []
        for ln in nm_output.splitlines():
            p = ln.split()
            if len(p) == 4:
                syms.append(Sym(int(p[0], 16), int(p[1], 16), p[2], p[3]))
            elif len(p) == 3 and len(p[0]) == 8:
                syms.append(Sym(int(p[0], 16), 0, p[1], p[2]))
        return cls(syms)

    @classmethod
    def from_elf(cls, elf: Path) -> "Symbols":
        nm = find_arm_tool("nm")
        if nm is None:
            raise SystemExit("swd: no arm-none-eabi-nm (set ARM_TOOLCHAIN_BIN, or build once with Bazel)")
        out = subprocess.run([str(nm), "-S", "-n", str(elf)], capture_output=True, text=True, check=True).stdout
        return cls.parse(out)

    def addr(self, name: str) -> int:
        hits = self.by_name.get(name)
        if not hits:
            raise KeyError(name)
        if len(hits) > 1:
            raise KeyError(f"{name} is ambiguous ({len(hits)} definitions); use near()")
        return hits[0].addr

    def get(self, name: str) -> "Sym | None":
        hits = self.by_name.get(name)
        return hits[0] if hits and len(hits) == 1 else None

    def near(self, name: str, anchor: str) -> Sym:
        """The definition of `name` closest to `anchor`: statics of one file sit together."""
        a = self.addr(anchor)
        return min(self.by_name[name], key=lambda s: abs(s.addr - a))

    def func(self, pc: int) -> str:
        if pc >= 0xFFFFFF00:
            return f"EXC_RETURN {pc:#x}"
        if not (0x08000000 <= pc < 0x0C000000):
            return f"not code ({pc:#x})"
        i = bisect.bisect_right(self._code_addrs, pc & ~1) - 1
        if i < 0:
            return "?"
        s = self._code[i]
        return f"{s.name}+{(pc & ~1) - s.addr:#x}"


# --------------------------------------------------------------------------------------------
# Memory dumped in one halt
# --------------------------------------------------------------------------------------------


class Mem:
    """Dumped regions, read like target memory. Reading outside them raises."""

    def __init__(self, regions: "dict[int, bytes]"):
        self.regions = dict(regions)

    def covers(self, addr: int, n: int = 4) -> bool:
        return any(b <= addr and addr + n <= b + len(d) for b, d in self.regions.items())

    def read(self, addr: int, n: int) -> bytes:
        for b, d in self.regions.items():
            if b <= addr and addr + n <= b + len(d):
                return d[addr - b: addr - b + n]
        raise IndexError(f"{addr:#010x}+{n} not dumped")

    def u32(self, addr: int) -> int:
        return struct.unpack("<I", self.read(addr, 4))[0]

    def cstr(self, addr: int, limit: int = 24) -> str:
        s = b""
        while len(s) < limit and self.covers(addr + len(s), 1):
            c = self.read(addr + len(s), 1)
            if c == b"\0":
                break
            s += c
        return s.decode("ascii", "replace")


def untouched(mem: Mem, start: int, limit: int, fill: bytes) -> int:
    """Bytes from `start` upward still holding the stack-fill pattern: the low-water headroom."""
    n = 0
    while n + 4 <= limit and mem.covers(start + n) and mem.read(start + n, 4) == fill:
        n += 4
    return n


# --------------------------------------------------------------------------------------------
# FreeRTOS: heap_4, task lists
# --------------------------------------------------------------------------------------------

HEAP4_ALLOCATED = 0x80000000
FREERTOS_FILL = b"\xa5\xa5\xa5\xa5"   # tskSTACK_FILL_BYTE
# TCB_t, with no MPU wrappers and no list-integrity bytes (this project's configuration):
#   pxTopOfStack 0, xStateListItem 4..23, xEventListItem 24..43, uxPriority 44, pxStack 48,
#   pcTaskName 52.
TCB_TOP, TCB_PRIO, TCB_STACK, TCB_NAME = 0, 44, 48, 52
LIST_SIZE = 20            # uxNumberOfItems, pxIndex, xListEnd (MiniListItem: value, next, prev)
LIST_END = 8              # offset of xListEnd inside List_t
ITEM_NEXT, ITEM_OWNER = 4, 12


@dataclass
class HeapReport:
    blocks: "list[tuple[int, int, bool]]" = field(default_factory=list)  # (addr, size, allocated)
    free_list: "list[int]" = field(default_factory=list)
    free_bytes_walked: int = 0
    free_bytes_reported: int = 0
    problems: "list[str]" = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.problems

    def owner(self, addr: int) -> "tuple[int, int, bool] | None":
        return next((b for b in self.blocks if b[0] + 8 <= addr < b[0] + b[1]), None)


def walk_heap4(mem: Mem, ucheap: int, x_start: int, px_end: int, free_reported: int,
               alignment: int = 8) -> HeapReport:
    """Walk every heap_4 block from the aligned start of ucHeap to pxEnd, then the free list.

    A block is BlockLink_t {pxNextFreeBlock, xBlockSize}, the top bit of xBlockSize marking it
    allocated. A stack that overflows downward writes over the header of its own allocation,
    which is exactly where this walk stops -- that is how the CLI overflow was proven.
    """
    r = HeapReport(free_bytes_reported=free_reported)
    start = (ucheap + alignment - 1) & ~(alignment - 1)
    end = mem.u32(px_end)
    addr = start
    while addr < end:
        size_word = mem.u32(addr + 4)
        size = size_word & ~HEAP4_ALLOCATED
        if size < 8 or size % alignment or addr + size > end:
            r.problems.append(f"bad block at {addr:#010x}: size word {size_word:#x}, "
                              f"next {mem.u32(addr):#x}")
            break
        r.blocks.append((addr, size, bool(size_word & HEAP4_ALLOCATED)))
        addr += size
    if not r.problems and addr != end:
        r.problems.append(f"block walk ended at {addr:#010x}, pxEnd is {end:#010x}")
    seen: "set[int]" = set()
    f = mem.u32(x_start)
    while f and f != end:
        if f in seen or not (start <= f < end):
            r.problems.append(f"free list leaves the heap or loops at {f:#010x}")
            break
        seen.add(f)
        r.free_list.append(f)
        if mem.u32(f + 4) & HEAP4_ALLOCATED:
            r.problems.append(f"free-list block {f:#010x} is marked allocated")
        f = mem.u32(f)
    free_blocks = {a for a, _, al in r.blocks if not al}
    r.free_bytes_walked = sum(n for _, n, al in r.blocks if not al)
    if not r.problems and set(r.free_list) != free_blocks:
        r.problems.append("free list and free blocks disagree")
    if not r.problems and r.free_bytes_walked != free_reported:
        r.problems.append(f"free bytes by walk {r.free_bytes_walked} != xFreeBytesRemaining {free_reported}")
    return r


@dataclass
class Task:
    name: str
    tcb: int
    priority: int
    stack: int
    top: int
    untouched: int    # bytes of fill left at the low end: the high-water headroom
    current: bool = False


def list_owners(mem: Mem, list_addr: int, limit: int = 64) -> "list[int]":
    """The pvOwner of every item on a FreeRTOS List_t (each owner is a TCB)."""
    end = list_addr + LIST_END
    owners, item = [], mem.u32(end + ITEM_NEXT)
    while item != end and len(owners) < limit:
        owners.append(mem.u32(item + ITEM_OWNER))
        item = mem.u32(item + ITEM_NEXT)
    return owners


def freertos_tasks(mem: Mem, syms: Symbols) -> "list[Task]":
    """Every task, from the scheduler's own lists. Nothing is assumed about how many exist."""
    lists = []
    ready = syms.get("pxReadyTasksLists")
    if ready:
        lists += [ready.addr + i * LIST_SIZE for i in range(max(1, ready.size // LIST_SIZE))]
    for n in ("xDelayedTaskList1", "xDelayedTaskList2", "xPendingReadyList",
              "xSuspendedTaskList", "xTasksWaitingTermination"):
        s = syms.get(n)
        if s:
            lists.append(s.addr)
    current = mem.u32(syms.addr("pxCurrentTCB"))
    tcbs: "list[int]" = []
    for la in lists:
        for t in list_owners(mem, la):
            if t not in tcbs:
                tcbs.append(t)
    if current and current not in tcbs:
        tcbs.append(current)
    tasks = []
    for t in tcbs:
        if not mem.covers(t, TCB_NAME + 16):
            tasks.append(Task(name=f"<TCB {t:#x} outside the dump>", tcb=t, priority=-1, stack=0, top=0,
                              untouched=-1, current=(t == current)))
            continue
        stack = mem.u32(t + TCB_STACK)
        tasks.append(Task(name=mem.cstr(t + TCB_NAME, 16), tcb=t, priority=mem.u32(t + TCB_PRIO),
                          stack=stack, top=mem.u32(t + TCB_TOP),
                          untouched=untouched(mem, stack, 64 * 1024, FREERTOS_FILL), current=(t == current)))
    return tasks


# --------------------------------------------------------------------------------------------
# ThreadX: threads found by control-block ID
# --------------------------------------------------------------------------------------------

TX_THREAD_ID = 0x54485244          # 'THRD'
THREADX_FILL = b"\xef\xef\xef\xef"  # TX_STACK_FILL
# TX_THREAD: id 0, run_count 4, stack_ptr 8, stack_start 12, stack_end 16, stack_size 20, ...
#   name 40 (TX_THREAD_EXTENSION_0 is empty in this port). TX_BYTE_POOL: start 24, size 28.
TXT_STACK_PTR, TXT_START, TXT_END, TXT_SIZE, TXT_NAME = 8, 12, 16, 20, 40
TXP_START, TXP_SIZE = 24, 28


@dataclass
class Thread:
    name: str
    tcb: int
    stack: int
    size: int
    untouched: int


def threadx_threads(mem: Mem, flash_str) -> "list[Thread]":
    """Every TX_THREAD in the dumped regions, recognised by its ID and a self-consistent stack."""
    out = []
    for base, data in mem.regions.items():
        for off in range(0, len(data) - 48, 4):
            if struct.unpack_from("<I", data, off)[0] != TX_THREAD_ID:
                continue
            a = base + off
            start, end, size = mem.u32(a + TXT_START), mem.u32(a + TXT_END), mem.u32(a + TXT_SIZE)
            if not (start and size and end == start + size - 1):
                continue
            namep = mem.u32(a + TXT_NAME)
            name = mem.cstr(namep) if mem.covers(namep, 1) else flash_str(namep)
            out.append(Thread(name=name, tcb=a, stack=start, size=size,
                              untouched=untouched(mem, start, size, THREADX_FILL)))
    return out


# --------------------------------------------------------------------------------------------
# Faults
# --------------------------------------------------------------------------------------------

EXCEPTIONS = {0: "thread", 2: "NMI", 3: "HardFault", 4: "MemManage", 5: "BusFault",
              6: "UsageFault", 11: "SVCall", 14: "PendSV", 15: "SysTick",
              16 + 27: "GPDMA1_Ch0", 16 + 49: "TIM6", 16 + 50: "TIM7", 16 + 60: "USART3", 16 + 106: "ETH"}

CFSR_BITS = {0: "MMFSR.IACCVIOL (instruction fetch from a non-executable address)",
             1: "MMFSR.DACCVIOL", 7: "MMFSR.MMARVALID", 8: "BFSR.IBUSERR", 9: "BFSR.PRECISERR",
             10: "BFSR.IMPRECISERR", 15: "BFSR.BFARVALID", 16: "UFSR.UNDEFINSTR",
             17: "UFSR.INVSTATE", 18: "UFSR.INVPC", 19: "UFSR.NOCP", 20: "UFSR.STKOF (stack limit)",
             24: "UFSR.UNALIGNED", 25: "UFSR.DIVBYZERO"}


def exception_name(n: int) -> str:
    return EXCEPTIONS.get(n, f"exception {n}")


def find_exception_frames(words: "list[int]", base: int) -> "list[dict]":
    """Candidate hardware exception frames {r0-r3, r12, lr, pc, xpsr} in a stack dump.

    A fault handler that has called a C function no longer has the frame pointer in a register,
    so the frame is found by shape: an xPSR whose reserved bits 23:20 are clear and whose
    exception number is one this part has, and a return address that is Thumb code in flash.
    Neither the stacked PC nor the Thumb bit is required to be sane -- a corrupt PC is the
    interesting case. The HardFault of November 2026 stacked PC 0xfffffe00 with T CLEAR: a
    `blx` to an even address clears it, so requiring T would reject exactly that frame.
    """
    out = []
    for i in range(len(words) - 7):
        r0, r1, r2, r3, r12, lr, pc, xpsr = words[i:i + 8]
        if (xpsr & 0x00F00000) or (xpsr & 0x1FF) >= 16 + 160:
            continue
        if not (0x08000000 <= lr < 0x08200000 and (lr & 1)):
            continue
        out.append({"at": base + 4 * i, "lr": lr, "pc": pc, "xpsr": xpsr, "r0": r0, "r12": r12,
                    "exception": xpsr & 0x1FF})
    return out


# --------------------------------------------------------------------------------------------
# OpenOCD
# --------------------------------------------------------------------------------------------


class Swd:
    """One image's view of the target. Every operation halts at most briefly and always resumes."""

    def __init__(self, elf: Path, openocd: "Path | None" = None, board_cfg: str = DEFAULT_BOARD_CFG):
        self.elf = Path(elf)
        self.syms = Symbols.from_elf(self.elf)
        self.openocd = Path(openocd or DEFAULT_OPENOCD or "openocd")
        self.board_cfg = board_cfg
        self._flash = None

    # -- transport ---------------------------------------------------------------------------

    def tcl(self, body: "list[str]", timeout: float = 120.0) -> str:
        """Run `body` with the core halted. Whatever happens inside, the core is resumed."""
        script = ["init", "proc rd {a} { return [lindex [read_memory $a 32 1] 0] }",
                  "if {[catch {", "halt", *body, "resume", '} err]} { echo "TCLERR $err" }',
                  "catch {resume}", "echo RESUMED", "exit"]
        with tempfile.NamedTemporaryFile("w", suffix=".tcl", delete=False) as f:
            f.write("\n".join(script) + "\n")
            path = f.name
        try:
            p = subprocess.run([str(self.openocd), "-f", self.board_cfg, "-f", path],
                               capture_output=True, text=True, timeout=timeout)
        finally:
            os.unlink(path)
        out = p.stdout + p.stderr
        if "RESUMED" not in out:
            raise RuntimeError("swd: could not confirm the core was resumed:\n" + out[-1500:])
        m = re.search(r"TCLERR (.*)", out)
        if m:
            raise RuntimeError("swd: " + m.group(1))
        return out

    def dump(self, regions: "list[tuple[int, int]]", extra: "list[str] | None" = None) -> "tuple[Mem, str]":
        """Dump each (addr, size) in one halt; `extra` Tcl runs in the same halt."""
        tmp = tempfile.mkdtemp(prefix="swd")
        files = []
        body = []
        for i, (a, n) in enumerate(regions):
            f = os.path.join(tmp, f"r{i}.bin")
            files.append((a, f))
            body.append(f"dump_image {f} {a:#x} {n:#x}")
        out = self.tcl(body + (extra or []))
        mem = Mem({a: open(f, "rb").read() for a, f in files})
        shutil.rmtree(tmp, ignore_errors=True)
        return mem, out

    def flash_str(self, addr: int) -> str:
        """A string in flash, read from the ELF file rather than the target."""
        if self._flash is None:
            readelf = find_arm_tool("readelf")
            out = subprocess.run([str(readelf), "-S", "-W", str(self.elf)], capture_output=True, text=True).stdout
            secs = []
            for m in re.finditer(r"\]\s+(\S+)\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", out):
                vma, off, size = int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16)
                if 0x08000000 <= vma < 0x0C000000:
                    secs.append((vma, off, size))
            self._flash = (self.elf.read_bytes(), secs)
        data, secs = self._flash
        for vma, off, size in secs:
            if vma <= addr < vma + size:
                return data[off + addr - vma: off + addr - vma + 24].split(b"\0")[0].decode("ascii", "replace")
        return f"<{addr:#x}>"

    @property
    def is_freertos(self) -> bool:
        return "pxCurrentTCB" in self.syms.by_name

    # -- operations --------------------------------------------------------------------------

    def snapshot(self, samples: int = 40) -> dict:
        """PC, exception and current task sampled `samples` times, 20 ms apart; tick before/after."""
        s = self.syms
        cur = s.get("pxCurrentTCB") or s.get("_tx_thread_current_ptr")
        tick = s.get("xTickCount") or s.get("_tx_timer_system_clock")
        body = [f'echo "TICK0 [rd {tick.addr:#x}]"', "resume"] if tick else ["resume"]
        body += [f"for {{set i 0}} {{$i < {samples}}} {{incr i}} {{ halt; "
                 f"set pc [lindex [get_reg pc] 1]; set ps [lindex [get_reg xpsr] 1]; "
                 f"set t [rd {cur.addr:#x}]; resume; echo \"S $pc $ps $t\"; sleep 20 }}", "halt"]
        if tick:
            body.append(f'echo "TICK1 [rd {tick.addr:#x}]"')
        body.append('echo "REGS [get_reg {pc lr sp xpsr basepri primask control}]"')
        for v in ("uxSchedulerSuspended", "xPendedTicks", "ulCriticalNesting", "lwip_core_lock_count"):
            if s.get(v):
                body.append(f'echo "VAR {v} [rd {s.addr(v):#x}]"')
        out = self.tcl(body)
        res: dict = {"samples": [], "vars": {}}
        for pc, ps, t in re.findall(r"^S (0x[0-9a-f]+) (0x[0-9a-f]+) (\S+)$", out, re.M):
            pc_i, ps_i = int(pc, 16), int(ps, 16)
            res["samples"].append({"pc": pc_i, "func": s.func(pc_i), "exception": exception_name(ps_i & 0x1FF),
                                   "task_ptr": int(t, 0)})
        t0 = re.search(r"TICK0 (\S+)", out)
        t1 = re.search(r"TICK1 (\S+)", out)
        if t0 and t1:
            res["tick_advanced"] = int(t1.group(1), 0) - int(t0.group(1), 0)
        for k, v in re.findall(r"VAR (\S+) (\S+)", out):
            res["vars"][k] = int(v, 0)
        m = re.search(r"REGS (.*)", out)
        if m:
            p = m.group(1).split()
            res["regs"] = {p[i]: int(p[i + 1], 0) for i in range(0, len(p) - 1, 2)}
        res["task_names"] = self.task_names({x["task_ptr"] for x in res["samples"] if x["task_ptr"]})
        return res

    def task_names(self, tcbs: "set[int]") -> "dict[int, str]":
        """Names for task/thread control blocks, in one short read."""
        if not tcbs:
            return {}
        mem, _ = self.dump([(t, 64) for t in sorted(tcbs)])
        if self.is_freertos:
            return {t: mem.cstr(t + TCB_NAME, 16) for t in tcbs}
        out = {}
        for t in tcbs:
            p = mem.u32(t + TXT_NAME)
            out[t] = self.flash_str(p) if p >= 0x08000000 else f"<{p:#x}>"
        return out

    def freertos_heap_and_tasks(self) -> "tuple[HeapReport, list[Task]]":
        s = self.syms
        heap = s.get("ucHeap")
        kernel = [x for x in (s.get(n) for n in ("pxReadyTasksLists", "xDelayedTaskList1", "xDelayedTaskList2",
                                                 "xPendingReadyList", "xSuspendedTaskList", "xTasksWaitingTermination",
                                                 "pxCurrentTCB", "xStart", "pxEnd", "xFreeBytesRemaining")) if x]
        lo = min(x.addr for x in kernel)
        hi = max(x.addr + max(x.size, 4) for x in kernel)
        mem, _ = self.dump([(heap.addr, heap.size), (lo, hi - lo)])
        report = walk_heap4(mem, heap.addr, s.addr("xStart"), s.addr("pxEnd"), mem.u32(s.addr("xFreeBytesRemaining")))
        return report, freertos_tasks(mem, s)

    def threadx_threads(self) -> "list[Thread]":
        s = self.syms
        end = s.get("_end")
        pools = [s.get(n) for n in ("StackBytePool", "HeapBytePool") if s.get(n)]
        hdr, _ = self.dump([(p.addr, 64) for p in pools])
        regions = [(0x20000000, (end.addr if end else 0x20020000) - 0x20000000)]
        for p in pools:
            regions.append((hdr.u32(p.addr + TXP_START), hdr.u32(p.addr + TXP_SIZE)))
        mem, _ = self.dump(regions)
        threads = threadx_threads(mem, self.flash_str)
        # NetX Duo's IP thread keeps its stack in SRAM3, outside the regions above: read any
        # stack not yet covered and measure it again, rather than report it as fully used.
        missing = [(t.stack, t.size) for t in threads if not mem.covers(t.stack, t.size)]
        if missing:
            extra, _ = self.dump(missing)
            for t in threads:
                if extra.covers(t.stack, t.size):
                    t.untouched = untouched(extra, t.stack, t.size, THREADX_FILL)
        return threads

    def fault(self) -> dict:
        body = ['echo "SCB [rd 0xE000ED28] [rd 0xE000ED2C] [rd 0xE000ED34] [rd 0xE000ED38]"',
                'echo "REGS [get_reg {pc lr sp msp psp xpsr control}]"',
                'set msp [lindex [get_reg msp] 1]', 'set psp [lindex [get_reg psp] 1]',
                'echo "MSPW $msp [read_memory $msp 32 48]"', 'echo "PSPW $psp [read_memory $psp 32 16]"']
        out = self.tcl(body)
        cfsr, hfsr, mmfar, bfar = (int(x, 0) for x in re.search(r"SCB (\S+) (\S+) (\S+) (\S+)", out).groups())
        p = re.search(r"REGS (.*)", out).group(1).split()
        regs = {p[i]: int(p[i + 1], 0) for i in range(0, len(p) - 1, 2)}
        frames = []
        for tag in ("MSPW", "PSPW"):
            m = re.search(tag + r" (\S+) (.*)", out)
            base, words = int(m.group(1), 0), [int(w, 0) for w in m.group(2).split()]
            for f in find_exception_frames(words, base):
                f["stack"] = tag[:3].lower()
                f["lr_func"], f["pc_func"] = self.syms.func(f["lr"]), self.syms.func(f["pc"])
                f["exception_name"] = exception_name(f["exception"])
                frames.append(f)
        return {"cfsr": cfsr, "hfsr": hfsr, "mmfar": mmfar, "bfar": bfar, "regs": regs,
                "cfsr_bits": [d for b, d in CFSR_BITS.items() if cfsr & (1 << b)],
                "hfsr_forced": bool(hfsr & 0x40000000), "frames": frames,
                "halted_in": self.syms.func(regs.get("pc", 0))}

    def console(self) -> dict:
        """lib/io/serial_io.c's state: the FIFO counters, the TX-complete flag, USART3 and GPDMA1 ch0.

        `flags` is not unique (other files have one), so the one beside serial_io's `uart` is used.
        """
        s = self.syms
        flags = s.near("flags", "uart")
        fifo = s.near("fifo", "uart")
        body = [f'set eg [rd {flags.addr:#x}]', 'echo "EVENTBITS [expr {$eg ? [rd $eg] : -1}]"',
                f'echo "FIFO [read_memory {fifo.addr:#x} 32 4]"',
                'echo "USART3 [rd 0x40004800] [rd 0x40004808] [rd 0x4000481C]"',
                'echo "GPDMA0 [rd 0x40020060] [rd 0x40020064] [rd 0x40020098]"']
        out = self.tcl(body)
        w, r, pushed, popped = (int(x, 0) for x in re.search(r"FIFO (.*)", out).group(1).split())
        cr1, cr3, isr = (int(x, 0) for x in re.search(r"USART3 (\S+) (\S+) (\S+)", out).groups())
        sr, cr, br1 = (int(x, 0) for x in re.search(r"GPDMA0 (\S+) (\S+) (\S+)", out).groups())
        bits = int(re.search(r"EVENTBITS (\S+)", out).group(1), 0)
        idle = (pushed == popped) and bool(sr & 1) and not (cr & 1)
        return {"fifo_used": pushed - popped, "tx_cplt_flag": bits & 1 if bits >= 0 else None,
                "dma_idle": bool(sr & 1), "dma_enabled": bool(cr & 1), "dma_remaining": br1 & 0xFFFF,
                "usart_cr1": cr1, "usart_cr3": cr3, "usart_isr": isr,
                # The desync signature: nothing to send, DMA idle, completion flag still set.
                "desynced": idle and bits >= 0 and bool(bits & 1)}


# --------------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------------


def _print_snapshot(r: dict, syms: Symbols, swd: Swd) -> None:
    sm = r["samples"]
    exc = collections.Counter(x["exception"] for x in sm)
    fn = collections.Counter(x["func"].split("+")[0] for x in sm)
    print(f"  {len(sm)} samples; tick advanced {r.get('tick_advanced', '?')}")
    print("  exception: " + ", ".join(f"{k} {v}" for k, v in exc.most_common()))
    print("  function:  " + ", ".join(f"{k} {v}" for k, v in fn.most_common(8)))
    names = r.get("task_names", {})
    tasks = collections.Counter(names.get(x["task_ptr"], "?") for x in sm if x["exception"] == "thread")
    if tasks:
        print("  task (thread-mode samples): " + ", ".join(f"{k} {v}" for k, v in tasks.most_common(8)))
    if r.get("vars"):
        print("  vars: " + ", ".join(f"{k} {v}" for k, v in r["vars"].items()))
    if r.get("regs"):
        g = r["regs"]
        print("  halted at: " + ", ".join(f"{k} {v:#x}" for k, v in g.items())
              + f"  [{syms.func(g.get('pc', 0))} <- {syms.func(g.get('lr', 0))}]")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", type=Path, required=True, help="the image running on the board")
    ap.add_argument("--openocd", type=Path, default=None)
    ap.add_argument("--samples", type=int, default=40)
    ap.add_argument("what", choices=["snapshot", "heap", "stacks", "fault", "console", "all"])
    a = ap.parse_args()
    swd = Swd(a.elf, a.openocd)
    todo = ["snapshot", "heap", "stacks", "console"] if a.what == "all" else [a.what]
    bad = False
    for what in todo:
        print(f"== {what} ==")
        if what == "snapshot":
            _print_snapshot(swd.snapshot(a.samples), swd.syms, swd)
        elif what == "heap":
            if not swd.is_freertos:
                print("  (ThreadX image: no heap_4)")
                continue
            rep, _ = swd.freertos_heap_and_tasks()
            print(f"  {len(rep.blocks)} blocks, {sum(1 for b in rep.blocks if b[2])} allocated, "
                  f"{rep.free_bytes_walked} B free (xFreeBytesRemaining {rep.free_bytes_reported})")
            print("  HEAP OK" if rep.ok else "  HEAP CORRUPT: " + "; ".join(rep.problems))
            bad |= not rep.ok
        elif what == "stacks":
            rows = ([(t.name, t.untouched, "*" if t.current else "") for t in swd.freertos_heap_and_tasks()[1]]
                    if swd.is_freertos else [(t.name, t.untouched, f"of {t.size}") for t in swd.threadx_threads()])
            for name, u, note in sorted(rows, key=lambda r: r[1]):
                print(f"  {name:22s} {u:5d} B never touched {note}")
        elif what == "fault":
            f = swd.fault()
            print(f"  halted in {f['halted_in']}; CFSR {f['cfsr']:#x} HFSR {f['hfsr']:#x}"
                  f"{' (forced)' if f['hfsr_forced'] else ''}")
            for b in f["cfsr_bits"]:
                print(f"    {b}")
            if not (f["cfsr"] or f["hfsr"]):
                print("  no fault recorded (CFSR and HFSR clear); not looking for exception frames")
                continue
            for fr in f["frames"]:
                print(f"  frame on {fr['stack']} at {fr['at']:#x}: taken in {fr['exception_name']}, "
                      f"pc {fr['pc']:#010x} [{fr['pc_func']}], lr {fr['lr']:#010x} [{fr['lr_func']}]")
        elif what == "console":
            c = swd.console()
            print("  " + ", ".join(f"{k} {v:#x}" if isinstance(v, int) and k.startswith("usart") else f"{k} {v}"
                                   for k, v in c.items()))
            if c["desynced"]:
                print("  CONSOLE DESYNCED: idle with the TX-complete flag set")
                bad = True
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
