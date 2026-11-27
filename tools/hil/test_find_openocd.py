"""Unit tests for hil.find_openocd().

The resolver replaced a hardcoded path into toolchains/, and it is the thing standing between
phase 7 (which deletes the build.sh that populates that directory) and every HIL run dying on a
missing file. The search ORDER is also shared with //bazel/openocd.bzl so the two cannot
disagree about which binary is in play -- which is only true if it stays as written.
"""

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from hil import find_openocd


class FindOpenocd(unittest.TestCase):
    def setUp(self):
        self._tmp = TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def _make(self, rel: str) -> Path:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("#!/bin/sh\n")
        p.chmod(0o755)
        return p

    def test_none_when_nothing_is_available(self):
        self.assertIsNone(find_openocd(env={"PATH": ""}, repo_root=self.root))

    def test_env_override_wins(self):
        override = self._make("elsewhere/openocd")
        self._make("toolchains/openocd/bin/openocd")
        self.assertEqual(
            find_openocd(env={"OPENOCD": str(override), "PATH": ""}, repo_root=self.root),
            override,
        )

    def test_falls_back_to_the_vendored_build(self):
        vendored = self._make("toolchains/openocd/bin/openocd")
        self.assertEqual(
            find_openocd(env={"PATH": ""}, repo_root=self.root), vendored
        )

    def test_falls_back_to_path(self):
        on_path = self._make("bin/openocd")
        self.assertEqual(
            find_openocd(env={"PATH": str(on_path.parent)}, repo_root=self.root), on_path
        )

    def test_a_nonexistent_override_does_not_win(self):
        """An OPENOCD pointing at nothing must fall through, not resolve to a missing file."""
        vendored = self._make("toolchains/openocd/bin/openocd")
        self.assertEqual(
            find_openocd(env={"OPENOCD": str(self.root / "nope"), "PATH": ""},
                         repo_root=self.root),
            vendored,
        )

    def test_vendored_is_preferred_over_path(self):
        """The order matters: toolchains/openocd is a copy of the build the project pins."""
        vendored = self._make("toolchains/openocd/bin/openocd")
        self._make("bin/openocd")
        self.assertEqual(
            find_openocd(env={"PATH": str(self.root / "bin")}, repo_root=self.root),
            vendored,
        )


if __name__ == "__main__":
    unittest.main()
