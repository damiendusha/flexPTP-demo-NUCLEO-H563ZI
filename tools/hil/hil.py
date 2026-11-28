"""Serial driver for the NUCLEO-H563ZI flexPTP demo's CLI.

The board speaks 115200-8-N-1 on the ST-Link VCP and has no prompt string: it echoes what
you type, prints the reply, and goes quiet. There is therefore nothing to synchronise on
except silence, which is what `cmd()` uses -- read until the line has been idle for
`quiet` seconds, with a hard ceiling at `timeout`.

Output is colourised with ANSI SGR sequences. Everything returned by this module is
stripped of them; use `raw=True` where the escapes themselves are the thing under test.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import serial

REPO_ROOT = Path(__file__).resolve().parents[2]

DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 115200
DEFAULT_BOARD_CFG = "board/st_nucleo_h5.cfg"


# The OpenOCD every `bazel run ...:flash` uses, at a path outside Bazel. See tools/flash/BUILD.bazel.
BAZEL_OPENOCD = "//tools/flash:openocd_exec"


class BoardError(RuntimeError):
    pass


def _bazel_openocd(repo_root: Path, env: "dict[str, str]") -> Path:
    """Build BAZEL_OPENOCD and return where it is.

    Built, not just queried: the build is what makes the answer current. A no-op once it is up
    to date; the first time, it is ST's fork from source, which is minutes.
    """
    exe = shutil.which("bazel", path=env.get("PATH")) or shutil.which("bazelisk", path=env.get("PATH"))
    if exe is None:
        raise BoardError(
            "No OpenOCD: OPENOCD is not set, and there is no bazel on PATH to build "
            f"{BAZEL_OPENOCD} with. Set OPENOCD to ST's fork (upstream OpenOCD has no "
            "stm32h5x flash driver and cannot program this board)."
        )

    def bazel(*args: str) -> str:
        p = subprocess.run([exe, *args], cwd=repo_root, env=dict(env), capture_output=True, text=True)
        if p.returncode != 0:
            raise BoardError(
                f"No OpenOCD: `bazel {' '.join(args)}` failed.\n{p.stderr[-2000:]}\n"
                "If ST's fork will not build on this host, --//tools/flash:openocd=local uses one "
                "you already have -- see tools/flash/BUILD.bazel -- or set OPENOCD."
            )
        return p.stdout.strip()

    print(f"hil: OpenOCD from {BAZEL_OPENOCD} (building ST's fork the first time takes minutes)",
          file=sys.stderr)
    bazel("build", BAZEL_OPENOCD)
    rel = bazel("cquery", "--output=files", BAZEL_OPENOCD).splitlines()[-1]
    return Path(bazel("info", "execution_root")) / rel


def find_openocd(env: "dict[str, str] | None" = None, repo_root: Path = REPO_ROOT,
                 bazel=_bazel_openocd) -> Path:
    """Locate the OpenOCD to use:

        $OPENOCD                       an explicit override, if it exists
        //tools/flash:openocd_exec     otherwise -- the OpenOCD `bazel run ...:flash` uses

    Bazel decides the second, so the harness follows --//tools/flash:openocd like the flash
    targets do: ST's fork built from source by default, or in local mode the one
    //bazel/openocd.bzl finds and verifies -- in $OPENOCD, toolchains/openocd or on PATH.

    This used to search toolchains/openocd and PATH itself, which needed Bazel's build copied
    into toolchains/ by hand. The copy went stale, silently, the first time the pin moved: from
    then on the harness flashed and read the target with one OpenOCD and `bazel run :flash` with
    another.

    Raises BoardError, saying why, if neither yields one.
    """
    env = os.environ if env is None else env

    override = env.get("OPENOCD")
    if override and Path(override).exists():
        return Path(override)

    return bazel(repo_root, env)


_default_openocd: "Path | None" = None


def default_openocd() -> Path:
    """find_openocd(), resolved once per process -- it runs Bazel -- and only when first needed,
    so that importing this module (the unit tests do, under `bazel test`) never does."""
    global _default_openocd
    if _default_openocd is None:
        _default_openocd = find_openocd()
    return _default_openocd


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def strip_ansi(text: str) -> str:
    """Remove ANSI escape sequences, including the cursor moves in the banner's \\033[2J\\033[H."""
    return _ANSI_RE.sub("", text)


@dataclass
class OpenOcd:
    """Minimal OpenOCD wrapper.

    ST's fork is required: `target/stm32h5x.cfg` names a compiled-in `stm32h5x` flash
    driver that upstream 0.12.0 does not have. `binary` defaults to default_openocd(): the
    one Bazel builds for the flash targets.
    """

    binary: "Path | None" = None
    board_cfg: str = DEFAULT_BOARD_CFG

    def _run(self, *commands: str, timeout: float = 120.0) -> subprocess.CompletedProcess:
        binary = Path(self.binary) if self.binary is not None else default_openocd()
        if not binary.exists():
            raise BoardError(f"No OpenOCD at {binary}.")
        argv = [str(binary), "-f", self.board_cfg]
        for c in commands:
            argv += ["-c", c]
        return subprocess.run(argv, capture_output=True, text=True, timeout=timeout)

    def reset(self) -> None:
        """Reset and run. Does not halt -- the board boots normally."""
        proc = self._run("init", "reset run", "exit")
        if proc.returncode != 0:
            raise BoardError(f"OpenOCD reset failed:\n{proc.stderr}")

    def flash(self, elf: Path) -> None:
        elf = Path(elf)
        if not elf.exists():
            raise BoardError(f"No such ELF: {elf}")
        proc = self._run(
            "init", "halt", f"program {elf} verify reset", "exit", timeout=300.0
        )
        if proc.returncode != 0 or "Verified OK" not in proc.stderr + proc.stdout:
            raise BoardError(f"OpenOCD flash/verify failed:\n{proc.stderr}")


class Board:
    """An open serial session with the board.

    Use as a context manager. `reset()` needs the port already open, because the banner
    starts arriving before OpenOCD's own process has exited.
    """

    def __init__(
        self,
        port: str = DEFAULT_PORT,
        baud: int = DEFAULT_BAUD,
        openocd: OpenOcd | None = None,
    ) -> None:
        self.port = port
        self.baud = baud
        self.ocd = openocd or OpenOcd()
        self._ser: serial.Serial | None = None

    # -- lifecycle ---------------------------------------------------------

    def __enter__(self) -> "Board":
        self.open()
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def open(self) -> None:
        try:
            self._ser = serial.Serial(self.port, self.baud, timeout=0.2)
        except serial.SerialException as e:
            raise BoardError(f"Cannot open {self.port}: {e}") from e
        time.sleep(0.2)
        self._ser.reset_input_buffer()

    def close(self) -> None:
        if self._ser is not None:
            self._ser.close()
            self._ser = None

    @property
    def ser(self) -> serial.Serial:
        if self._ser is None:
            raise BoardError("Board is not open")
        return self._ser

    # -- reading -----------------------------------------------------------

    def drain(self) -> None:
        """Discard anything already buffered, plus a beat of whatever is still arriving."""
        self.ser.reset_input_buffer()
        deadline = time.time() + 0.3
        while time.time() < deadline:
            self.ser.read(4096)

    def read_for(self, seconds: float, raw: bool = False) -> str:
        """Collect everything that arrives over a fixed window."""
        buf = bytearray()
        deadline = time.time() + seconds
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                buf.extend(chunk)
        text = bytes(buf).decode("utf-8", "replace")
        return text if raw else strip_ansi(text)

    def read_until_quiet(
        self, quiet: float = 0.4, timeout: float = 5.0, raw: bool = False
    ) -> str:
        """Read until the line has been idle for `quiet` seconds, or `timeout` elapses.

        This is the synchronisation primitive: the firmware emits no prompt, so silence is
        the only end-of-reply marker available.
        """
        buf = bytearray()
        deadline = time.time() + timeout
        last = time.time()
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                buf.extend(chunk)
                last = time.time()
            elif buf and (time.time() - last) >= quiet:
                break
        text = bytes(buf).decode("utf-8", "replace")
        return text if raw else strip_ansi(text)

    def expect(self, pattern: str, timeout: float = 10.0, raw: bool = False) -> re.Match:
        """Read until `pattern` matches the accumulated text, or raise."""
        rx = re.compile(pattern)
        buf = ""
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk.decode("utf-8", "replace")
                hay = buf if raw else strip_ansi(buf)
                m = rx.search(hay)
                if m:
                    return m
        raise BoardError(
            f"Timed out after {timeout}s waiting for /{pattern}/.\n"
            f"--- got ---\n{strip_ansi(buf)}"
        )

    # -- writing -----------------------------------------------------------

    def cmd(self, line: str, quiet: float = 0.4, timeout: float = 5.0) -> str:
        """Send a CLI command and return its reply, with the echoed command removed."""
        self.drain()
        self.ser.write(line.encode() + b"\r")
        self.ser.flush()
        out = self.read_until_quiet(quiet=quiet, timeout=timeout)
        # The firmware echoes the command; drop that first line so callers parse only the reply.
        lines = out.splitlines()
        if lines and lines[0].strip() == line.strip():
            lines = lines[1:]
        return "\n".join(lines).strip("\n")

    # -- reset -------------------------------------------------------------

    def reset(self, capture: float = 15.0) -> str:
        """Reset the board over SWD and capture the boot transcript.

        Reading runs on a background thread because OpenOCD blocks for several seconds and
        the banner is emitted while it is still running -- capturing afterwards loses it.
        """
        buf = bytearray()
        stop = threading.Event()

        def reader() -> None:
            while not stop.is_set():
                chunk = self.ser.read(4096)
                if chunk:
                    buf.extend(chunk)

        self.drain()
        thread = threading.Thread(target=reader, daemon=True)
        thread.start()
        try:
            self.ocd.reset()
            time.sleep(capture)
        finally:
            stop.set()
            thread.join(timeout=2.0)
        return strip_ansi(bytes(buf).decode("utf-8", "replace"))
