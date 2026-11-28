"""Unit tests for hil.find_openocd().

The harness used to find OpenOCD in toolchains/openocd, a copy of Bazel's build made by hand --
which went stale, silently, whenever the pin moved, so the harness flashed with one OpenOCD and
`bazel run :flash` with another. It now asks Bazel for //tools/flash:openocd_exec, the binary
the flash targets run. What these pin is the part that keeps that true: $OPENOCD is the only
thing that overrides Bazel, and nothing is returned that was not built first.

Bazel itself is faked -- a script on PATH -- because these run under `bazel test`, where a
nested Bazel cannot.
"""

import contextlib
import io
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import hil
from hil import BoardError, OpenOcd, find_openocd

FAKE_BAZEL = """#!/bin/sh
echo "$PWD|$1" >> "$LOG"
case "$1" in
    build)  if [ -n "$FAIL_BUILD" ]; then echo "ERROR: no such target" >&2; exit 1; fi ;;
    cquery) echo "bazel-out/k8-fastbuild/bin/tools/flash/openocd_exec/openocd" ;;
    info)   echo "/ob/execroot/_main" ;;
esac
"""


class FindOpenocd(unittest.TestCase):
    def setUp(self):
        self._tmp = TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self.log = self.root / "bazel.log"
        bindir = self.root / "bin"
        bindir.mkdir()
        bazel = bindir / "bazel"
        bazel.write_text(FAKE_BAZEL)
        bazel.chmod(0o755)
        self.env = {"PATH": str(bindir), "LOG": str(self.log)}

    def _make(self, rel: str) -> Path:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("#!/bin/sh\n")
        p.chmod(0o755)
        return p

    def _find(self, **env):
        with contextlib.redirect_stderr(io.StringIO()):
            return find_openocd(env={**self.env, **env}, repo_root=self.root)

    def _calls(self):
        return self.log.read_text().splitlines() if self.log.exists() else []

    def test_bazel_decides_when_there_is_no_override(self):
        self.assertEqual(self._find(),
                         Path("/ob/execroot/_main/bazel-out/k8-fastbuild/bin/tools/flash/openocd_exec/openocd"))

    def test_it_builds_before_it_asks_where_the_binary_is(self):
        """The build is what makes the answer current -- the property a copy did not have."""
        self._find()
        self.assertEqual([c.split("|")[1] for c in self._calls()], ["build", "cquery", "info"])

    def test_bazel_runs_in_the_workspace(self):
        self._find()
        self.assertTrue(all(c.split("|")[0] == str(self.root) for c in self._calls()))

    def test_env_override_wins_and_bazel_is_not_run(self):
        override = self._make("elsewhere/openocd")
        self.assertEqual(self._find(OPENOCD=str(override)), override)
        self.assertEqual(self._calls(), [])

    def test_a_nonexistent_override_does_not_win(self):
        """An OPENOCD pointing at nothing falls through to Bazel, not to a missing file."""
        found = self._find(OPENOCD=str(self.root / "nope"))
        self.assertEqual(found.name, "openocd")
        self.assertIn("build", [c.split("|")[1] for c in self._calls()])

    def test_toolchains_and_path_are_no_longer_searched(self):
        """A stale copy in toolchains/ or a distro openocd on PATH must not shadow Bazel's.
        (Local mode still finds them -- through Bazel, which verifies it is ST's fork.)"""
        self._make("toolchains/openocd/bin/openocd")
        self._make("bin/openocd")
        self.assertIn("openocd_exec", str(self._find()))

    def test_a_failed_build_is_an_error_not_a_stale_path(self):
        with self.assertRaises(BoardError) as e:
            self._find(FAIL_BUILD="1")
        self.assertIn("no such target", str(e.exception))
        self.assertIn("--//tools/flash:openocd=local", str(e.exception))
        self.assertEqual([c.split("|")[1] for c in self._calls()], ["build"])

    def test_no_bazel_and_no_override_says_to_set_openocd(self):
        with self.assertRaises(BoardError) as e:
            find_openocd(env={"PATH": ""}, repo_root=self.root)
        self.assertIn("OPENOCD", str(e.exception))


class NothingRunsBazelUntilOpenocdIsNeeded(unittest.TestCase):
    """Importing hil, or building an OpenOcd, must not resolve anything: the unit tests import
    this module under `bazel test`, and the resolution builds a Bazel target."""

    def test_import_resolves_nothing(self):
        self.assertIsNone(hil._default_openocd)

    def test_an_openocd_resolves_on_first_use(self):
        self.assertIsNone(OpenOcd().binary)


if __name__ == "__main__":
    unittest.main()
