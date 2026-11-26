"""Unit tests for the scenario helpers and the malformed-PTP packet builders.

The parsers here decide PASS or FAIL on the board, so their failure modes are pinned: a `ptp
info` reply cut off mid-line once read "00a0c9" as the master's clock identity, and a test of a
receive-path fix is only as good as the packets it sends.
"""

import struct
import unittest

import ptp_fuzz
from scenario import expand
from scenarios import PRESETS, SCENARIOS, eui64, offsets, parse_master_own, sync_lines


class Parsing(unittest.TestCase):
    def test_master_and_own_ids(self):
        info = "Own clock ID: 56001EFFFE000451\nMaster clock ID: 00a0c9fffe2621e4\n"
        self.assertEqual(parse_master_own(info), ("00a0c9fffe2621e4", "56001efffe000451"))

    def test_truncated_id_is_not_an_id(self):
        self.assertEqual(parse_master_own("Master clock ID: 00a0c9")[0], None)
        self.assertEqual(parse_master_own("Master clock ID: 00a0c9fffe2621e4ff")[0], None)

    def test_sync_lines_and_offsets(self):
        text = ("[LOG-DEF] 1764 123456789 0 1 2 -37 x y 12.5 z w\n"
                "garbage line\n"
                "1765 1 0 1 2 451 x y 1.0 z w\n"
                "short 1 2\n")
        lines = sync_lines(text)
        self.assertEqual(len(lines), 2)
        self.assertEqual(offsets(lines), [37, 451])

    def test_eui64(self):
        self.assertEqual(eui64("00:a0:c9:26:21:e4\n"), "00a0c9fffe2621e4")
        with self.assertRaises(ValueError):
            eui64("00:a0:c9")


class Packets(unittest.TestCase):
    def test_header_is_34_bytes_with_the_right_fields(self):
        h = ptp_fuzz.hdr(0x0, 44, 7, domain=3, version=2)
        self.assertEqual(len(h), 34)
        mtype, ver, length, domain = struct.unpack_from("!BBHB", h)
        self.assertEqual((mtype, ver, length, domain), (0x0, 2, 44, 3))
        self.assertEqual(struct.unpack_from("!H", h, 6)[0], 0x0200)    # twoStepFlag
        self.assertEqual(h[20:28], ptp_fuzz.FOREIGN)
        self.assertEqual(struct.unpack_from("!H", h, 30)[0], 7)        # sequenceId

    def test_message_sizes(self):
        self.assertEqual(len(ptp_fuzz.sync(1)), 44)
        self.assertEqual(len(ptp_fuzz.announce_worse(1)), 64)
        self.assertEqual(ptp_fuzz.announce_worse(1)[47], 255)          # grandmasterPriority1
        self.assertEqual(ptp_fuzz.announce_worse(1)[48], 248)          # clockClass

    def test_every_class_is_what_it_says(self):
        pk = {n: make(3) for n, (_, make, _) in ptp_fuzz.CLASSES.items()}
        self.assertEqual(len(pk["trunc1"]), 1)
        self.assertEqual(len(pk["trunc33"]), 33)
        self.assertEqual(pk["badver"][1], 1)
        self.assertEqual(pk["badtype"][0] & 0x0F, 0x5)
        self.assertEqual(len(pk["oversize"]), 1400)
        self.assertEqual(pk["domain1"][4], 1)
        self.assertEqual(pk["foreign"][4], 0)
        self.assertEqual(pk["announce"][0] & 0x0F, 0xB)


class Registry(unittest.TestCase):
    def test_presets_name_real_scenarios(self):
        for p, names in PRESETS.items():
            for n in names:
                self.assertIn(n, SCENARIOS, f"preset {p}")

    def test_no_preset_shadows_a_scenario(self):
        self.assertFalse(set(PRESETS) & set(SCENARIOS))
        self.assertEqual(expand(["health"]), ["health"])

    def test_expand(self):
        self.assertEqual(expand(["robust", "u1"]), ["fuzz", "u1"])
        with self.assertRaises(SystemExit):
            expand(["nope"])


if __name__ == "__main__":
    unittest.main()
