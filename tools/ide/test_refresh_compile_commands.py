"""The path rewrite and the de-duplication in refresh_compile_commands.py, on synthetic aquery
actions. Getting either wrong does not fail anything -- clangd just quietly parses the file
with the wrong flags, or cannot find a header -- which is why they are pinned here."""
import pathlib
import unittest

import refresh_compile_commands as rcc

WS = pathlib.Path("/ws")
EXECROOT = "/ob/execroot/_main"
OUTPUT_BASE = "/ob"

ARM = "external/+http_archive+arm_gnu_toolchain/bin/arm-none-eabi-gcc"
HOST = "/usr/bin/gcc"


def action(compiler, source, *flags):
    return {
        "arguments": [
            compiler,
            *flags,
            "-MD",
            "-MF",
            "bazel-out/k8-dbg/bin/x.d",
            "-c",
            source,
            "-o",
            "bazel-out/k8-dbg/bin/x.o",
        ]
    }


def entries(*actions):
    return rcc.to_entries(list(actions), WS, EXECROOT, OUTPUT_BASE)


class Rewrite(unittest.TestCase):
    def test_external_and_bazel_out_become_absolute(self):
        (e,) = entries(
            action(
                ARM,
                "external/+vendored+threadx/common/src/tx_thread_create.c",
                "-iquote",
                "external/+vendored+threadx",
                "-Iexternal/+vendored+threadx/common/inc",
                "-Ibazel-out/k8-dbg-ST-1/bin/external/+vendored+embfmt/_virtual_includes/embfmt",
            )
        )
        self.assertEqual(e["arguments"][0], "/ob/external/+http_archive+arm_gnu_toolchain/bin/arm-none-eabi-gcc")
        self.assertEqual(e["file"], "/ob/external/+vendored+threadx/common/src/tx_thread_create.c")
        self.assertIn("/ob/external/+vendored+threadx", e["arguments"])
        self.assertIn("-I/ob/external/+vendored+threadx/common/inc", e["arguments"])
        self.assertIn(
            "-I/ob/execroot/_main/bazel-out/k8-dbg-ST-1/bin/external/+vendored+embfmt/_virtual_includes/embfmt",
            e["arguments"],
        )

    def test_workspace_paths_are_left_relative_to_directory(self):
        (e,) = entries(action(ARM, "projects/p/main.c", "-iquote", ".", "-Iprojects/p"))
        self.assertEqual(e["directory"], "/ws")
        self.assertEqual(e["file"], "projects/p/main.c")
        self.assertIn(".", e["arguments"])
        self.assertIn("-Iprojects/p", e["arguments"])

    def test_defines_are_not_mistaken_for_paths(self):
        (e,) = entries(action(ARM, "projects/p/main.c", r'-DETH_STACK="NETXDUO"', "-DSTM32H563xx"))
        self.assertIn(r'-DETH_STACK="NETXDUO"', e["arguments"])
        self.assertIn("-DSTM32H563xx", e["arguments"])

    def test_output_and_dependency_files_are_dropped(self):
        (e,) = entries(action(ARM, "projects/p/main.c"))
        for flag in ("-MD", "-MF", "-o"):
            self.assertNotIn(flag, e["arguments"])
        self.assertFalse(any(a.endswith((".d", ".o")) for a in e["arguments"]))
        self.assertEqual(e["arguments"][-2:], ["-c", "projects/p/main.c"])

    def test_the_analyzer_is_dropped_and_its_neighbours_kept(self):
        # clang rejects -fanalyzer as an unknown argument; the host-only files carry it.
        (e,) = entries(action(HOST, "projects/lib/eth/mac_drv_uut.c", "-fno-pie", "-fanalyzer", "-Wall"))
        self.assertNotIn("-fanalyzer", e["arguments"])
        self.assertIn("-fno-pie", e["arguments"])
        self.assertIn("-Wall", e["arguments"])


class OneEntryPerFile(unittest.TestCase):
    def test_board_command_wins_over_host_whatever_the_order(self):
        host = action(HOST, "projects/lib/eth/mac_drv.c", "-DHOST")
        board = action(ARM, "projects/lib/eth/mac_drv.c", "-DBOARD")
        for order in ((host, board), (board, host)):
            (e,) = entries(*order)
            self.assertIn("-DBOARD", e["arguments"])

    def test_host_only_files_are_kept(self):
        es = entries(action(ARM, "projects/p/main.c"), action(HOST, "projects/p/test/t.cc"))
        self.assertEqual([e["file"] for e in es], ["projects/p/main.c", "projects/p/test/t.cc"])

    def test_first_board_command_wins_among_board_commands(self):
        (e,) = entries(
            action(ARM, "projects/p/a.c", "-DFIRST"),
            action(ARM, "projects/p/a.c", "-DSECOND"),
        )
        self.assertIn("-DFIRST", e["arguments"])

    def test_actions_without_a_source_are_ignored(self):
        self.assertEqual(entries({"arguments": [ARM, "--version"]}), [])


if __name__ == "__main__":
    unittest.main()
