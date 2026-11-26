"""Unit tests for swd.py's parsers, on synthetic target memory.

Each structure the SWD tools read -- heap_4's block chain, FreeRTOS's task lists, ThreadX's
thread control blocks, an exception frame -- is laid out here by hand, the way the target lays
it out, including the corruptions that were actually found on the board: a stack that overflowed
into its own allocation's heap header, a free list that loops, a stacked PC of 0xfffffe00.
"""

import struct
import unittest

from swd import (FREERTOS_FILL, HEAP4_ALLOCATED, PTP_ITEM_QUEUES, PTP_NOTIFY_QUEUE, THREADX_FILL,
                 TX_THREAD_ID, Mem, Symbols, find_exception_frames, freertos_tasks, list_owners,
                 queue_at, stranded_items, threadx_threads, untouched, walk_heap4)

HEAP = 0x20001004          # deliberately misaligned, as the linker is free to place it
X_START, PX_END, FREE = 0x20000100, 0x20000108, 0x2000010C


def put(buf: bytearray, base: int, addr: int, *words: int) -> None:
    struct.pack_into(f"<{len(words)}I", buf, addr - base, *words)


def heap_image(blocks):
    """A heap_4 image: `blocks` is [(size, allocated)], laid out from the aligned start."""
    base = 0x20000000
    buf = bytearray(0x3000)
    start = (HEAP + 7) & ~7
    addr, free = start, []
    for size, alloc in blocks:
        put(buf, base, addr, 0, size | (HEAP4_ALLOCATED if alloc else 0))
        if not alloc:
            free.append(addr)
        addr += size
    end = addr
    put(buf, base, end, 0, 0)                                  # pxEnd's own block
    for a, b in zip(free, free[1:] + [end]):                   # address-ordered free list
        put(buf, base, a, b)
    put(buf, base, X_START, free[0] if free else end, 0)
    put(buf, base, PX_END, end)
    put(buf, base, FREE, sum(s for s, al in blocks if not al))
    return buf, base, start, free


class Heap4(unittest.TestCase):
    def test_consistent_heap(self):
        buf, base, _, _ = heap_image([(64, True), (128, False), (32, True), (256, False)])
        mem = Mem({base: bytes(buf)})
        r = walk_heap4(mem, HEAP, X_START, PX_END, mem.u32(FREE))
        self.assertTrue(r.ok, r.problems)
        self.assertEqual(len(r.blocks), 4)
        self.assertEqual(r.free_bytes_walked, 384)

    def test_stack_overflow_into_its_own_block_header(self):
        # What the CLI overflow did: the allocation's header overwritten with stack contents.
        buf, base, start, _ = heap_image([(64, True), (2048, True), (128, False)])
        put(buf, base, start + 64, 0x08012D1D, 0x2C)            # return address, small size word
        mem = Mem({base: bytes(buf)})
        r = walk_heap4(mem, HEAP, X_START, PX_END, mem.u32(FREE))
        self.assertFalse(r.ok)
        self.assertIn(f"{start + 64:#010x}", r.problems[0])

    def test_free_list_loop(self):
        buf, base, _, free = heap_image([(64, False), (64, True), (64, False)])
        put(buf, base, free[1], free[0])                         # second free block points back
        mem = Mem({base: bytes(buf)})
        r = walk_heap4(mem, HEAP, X_START, PX_END, mem.u32(FREE))
        self.assertTrue(any("loops" in p for p in r.problems), r.problems)

    def test_free_count_disagrees(self):
        buf, base, _, _ = heap_image([(64, True), (128, False)])
        mem = Mem({base: bytes(buf)})
        r = walk_heap4(mem, HEAP, X_START, PX_END, 999)
        self.assertFalse(r.ok)

    def test_owner(self):
        buf, base, start, _ = heap_image([(64, True), (128, False)])
        r = walk_heap4(Mem({base: bytes(buf)}), HEAP, X_START, PX_END, 128)
        self.assertEqual(r.owner(start + 8)[0], start)
        self.assertEqual(r.owner(start + 64 + 20)[2], False)
        self.assertIsNone(r.owner(start + 4))                    # inside a header, not a payload


class FreeRtosTasks(unittest.TestCase):
    """Two tasks on one ready list and one on the delayed list, laid out as TCB_t and List_t."""

    def setUp(self):
        self.base = 0x20000000
        buf = bytearray(0x4000)
        self.ready, self.delayed, self.cur = 0x20000040, 0x20000080, 0x200000C0
        tcbs = {0x20000200: ("cli", 0x20001000, 24), 0x20000300: ("TCP/IP", 0x20002000, 40),
                0x20000400: ("IDLE", 0x20003000, 0)}
        for tcb, (name, stack, prio) in tcbs.items():
            put(buf, self.base, tcb, stack + 0x700)              # pxTopOfStack
            put(buf, self.base, tcb + 44, prio)
            put(buf, self.base, tcb + 48, stack)                 # pxStack
            nb = name.encode() + b"\0"
            buf[tcb + 52 - self.base: tcb + 52 - self.base + len(nb)] = nb
            buf[stack - self.base: stack - self.base + 0x800] = b"\xa5" * 0x800
        # cli used all but 136 bytes; TCP/IP all but 2000; IDLE all but 400
        for tcb, free in ((0x20000200, 136), (0x20000300, 2000), (0x20000400, 400)):
            stack = tcbs[tcb][1]
            buf[stack + free - self.base: stack + 0x800 - self.base] = b"\x11" * (0x800 - free)

        def link(lst, tcbs_on):
            end = lst + 8
            items = [t + 4 for t in tcbs_on]                     # xStateListItem lives at TCB+4
            seq = items + [end]
            put(buf, self.base, lst, len(items))
            put(buf, self.base, end + 4, seq[0])                 # xListEnd.pxNext
            for it, nxt, t in zip(items, seq[1:], tcbs_on):
                put(buf, self.base, it + 4, nxt)                 # pxNext
                put(buf, self.base, it + 12, t)                  # pvOwner

        link(self.ready, [0x20000200, 0x20000300])
        link(self.delayed, [0x20000400])
        put(buf, self.base, self.cur, 0x20000300)
        self.mem = Mem({self.base: bytes(buf)})
        self.syms = Symbols.parse("\n".join([
            f"{self.ready:08x} {20:08x} b pxReadyTasksLists",
            f"{self.delayed:08x} {20:08x} b xDelayedTaskList1",
            f"{self.cur:08x} {4:08x} b pxCurrentTCB",
        ]))

    def test_list_owners(self):
        self.assertEqual(list_owners(self.mem, self.ready), [0x20000200, 0x20000300])
        self.assertEqual(list_owners(self.mem, self.delayed), [0x20000400])

    def test_tasks_and_headroom(self):
        tasks = {t.name: t for t in freertos_tasks(self.mem, self.syms)}
        self.assertEqual(set(tasks), {"cli", "TCP/IP", "IDLE"})
        self.assertEqual(tasks["cli"].untouched, 136)
        self.assertEqual(tasks["TCP/IP"].untouched, 2000)
        self.assertTrue(tasks["TCP/IP"].current)
        self.assertEqual(tasks["IDLE"].priority, 0)


class ThreadX(unittest.TestCase):
    def test_threads_found_by_id_with_consistent_stack(self):
        base = 0x20000000
        buf = bytearray(0x3000)
        # A real thread, and a stray 'THRD' word with an inconsistent stack that must be ignored.
        put(buf, base, 0x20000100, TX_THREAD_ID, 0, 0, 0x20001000, 0x200013FF, 0x400)
        put(buf, base, 0x20000100 + 40, 0x20000900)
        buf[0x900:0x904] = b"cli\0"
        buf[0x1000:0x1400] = THREADX_FILL * 0x100
        buf[0x1000 + 648:0x1400] = b"\x00" * (0x400 - 648)
        put(buf, base, 0x20000200, TX_THREAD_ID, 0, 0, 0x20001000, 0x20001010, 0x400)
        threads = threadx_threads(Mem({base: bytes(buf)}), lambda a: "?")
        self.assertEqual([(t.name, t.size, t.untouched) for t in threads], [("cli", 0x400, 648)])


class Frames(unittest.TestCase):
    def test_finds_the_ethernet_isr_frame_with_a_garbage_pc_and_t_clear(self):
        # The HardFault of November 2026: taken in ETH (exception 122), PC 0xfffffe00, LR back
        # into ETHHW_ProcessTx.
        words = [0x8036018, 0x2003FED8, 0x40000000, 0x1, 0x2003FEF8, 0xFFFFFFB0,
                 0x0, 0x0, 0x20050318, 0x8010D35, 0xFFFFFE00, 0xA000007A, 0, 0]
        frames = find_exception_frames(words[2:], 0x2003FEC8)
        hit = [f for f in frames if f["pc"] == 0xFFFFFE00]
        self.assertEqual(len(hit), 1)
        self.assertEqual(hit[0]["exception"], 122)
        self.assertEqual(hit[0]["lr"], 0x8010D35)

    def test_rejects_a_return_address_that_is_not_thumb_code_in_flash(self):
        frame = [0, 0, 0, 0, 0, 0x20001000, 0x08002000, 0x01000000]   # LR in RAM
        self.assertEqual(find_exception_frames(frame, 0), [])
        frame[5] = 0x08001000                                          # even: not Thumb
        self.assertEqual(find_exception_frames(frame, 0), [])
        frame[5] = 0x08001001
        self.assertEqual(len(find_exception_frames(frame, 0)), 1)


def tx_queue(capacity: int, level: int, suspended: int) -> bytes:
    """A ThreadX TX_QUEUE: id 'QUEU', name, message size 1 word, capacity, enqueued, ..."""
    words = [0x51554555, 0x08001000, 1, capacity, level, capacity - level] + [0] * 4 + [0, suspended]
    return struct.pack("<12I", *words) + bytes(0x48 - 48)


def freertos_queue(length: int, level: int, receivers: int) -> bytes:
    """A FreeRTOS Queue_t: four pointers, the waiting-to-send and waiting-to-receive lists, the counts."""
    words = [0] * 4 + [0] * 5 + [receivers] + [0] * 4 + [level, length, 4]
    return struct.pack("<17I", *words) + bytes(0x48 - 68)


NAMES = PTP_ITEM_QUEUES + (PTP_NOTIFY_QUEUE,)


class PtpQueues(unittest.TestCase):
    def read(self, blocks, freertos):
        mem = Mem({0x20010000 + i * 0x100: b for i, b in enumerate(blocks)})
        return {n: queue_at(mem, 0x20010000 + i * 0x100, n, freertos) for i, n in enumerate(NAMES)}

    def test_the_stuck_threadx_board(self):
        # Read off the board after a gPTP flood: the PTP thread idle on an empty notification
        # FIFO, with twelve received frames and three heartbeats queued for nobody.
        q = self.read([tx_queue(16, 12, 0), tx_queue(16, 0, 0), tx_queue(10, 0, 0), tx_queue(16, 3, 0),
                       tx_queue(16, 0, 1)], freertos=False)
        self.assertEqual((q["sRxPacketFIFO"].level, q["sRxPacketFIFO"].capacity), (12, 16))
        self.assertEqual(q["sNotificationFIFO"].waiters, 1)
        self.assertEqual(stranded_items(q), 15)

    def test_in_step_freertos_queues(self):
        q = self.read([freertos_queue(16, 2, 0), freertos_queue(16, 1, 0), freertos_queue(10, 0, 0),
                       freertos_queue(16, 1, 0), freertos_queue(58, 4, 0)], freertos=True)
        self.assertEqual((q["sNotificationFIFO"].level, q["sNotificationFIFO"].capacity), (4, 58))
        self.assertEqual(stranded_items(q), 0)

    def test_idle_freertos_task_waiting_on_empty_queues(self):
        q = self.read([freertos_queue(16, 0, 0)] * 4 + [freertos_queue(58, 0, 1)], freertos=True)
        self.assertEqual(q["sNotificationFIFO"].waiters, 1)
        self.assertEqual(stranded_items(q), 0)


class SymbolsAndFill(unittest.TestCase):
    def test_near_disambiguates_statics(self):
        s = Symbols.parse("2000ba00 00000004 b flags\n20018730 00000004 b flags\n"
                          "2000b8bc 000000cc b uart\n08001000 00000020 T main\n")
        self.assertEqual(s.near("flags", "uart").addr, 0x2000BA00)
        with self.assertRaises(KeyError):
            s.addr("flags")
        self.assertEqual(s.func(0x08001011), "main+0x10")
        self.assertTrue(s.func(0xFFFFFFBC).startswith("EXC_RETURN"))

    def test_untouched_stops_at_first_used_word(self):
        mem = Mem({0x100: FREERTOS_FILL * 3 + b"\x00\x00\x00\x00" + FREERTOS_FILL})
        self.assertEqual(untouched(mem, 0x100, 64, FREERTOS_FILL), 12)


if __name__ == "__main__":
    unittest.main()
