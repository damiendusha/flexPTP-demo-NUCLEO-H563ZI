#!/usr/bin/env python3
"""Write compile_commands.json at the workspace root, from Bazel's own action graph.

    tools/ide/refresh_compile_commands.py                  # --config=dbg, both firmwares + every cc_test
    tools/ide/refresh_compile_commands.py --config=opt     # any bazel flags, in place of the default
    tools/ide/refresh_compile_commands.py --config=dbg -- //projects/flexptp_demo_stm32h563/lib:mac_drv_tx_ring_test

clangd reads the result; so does cpptools, through .vscode/c_cpp_properties.json.

Why not hedronvision/bazel-compile-commands-extractor, the usual answer: it is not on BCR, so it
would be the second exception to D2 (docs/bazel_decisions.md), and as published it is written
against native rules Bazel 9 removed -- its BUILD file uses a bare cc_binary (the failure
MODULE.bazel records for googletest 1.15.2), and its macro calls native.py_binary, i.e. it wants
rules_python, which this module has so far avoided (tools/hil/BUILD.bazel). Everything it would
do here is one aquery and a path rewrite.

What this does:

  1. `bazel build` the targets with the given flags. That creates the generated headers the
     commands refer to (_virtual_includes/), and it is what makes the paths real: the board's
     objects live in bazel-out/k8-dbg-ST-<hash>/, a directory that exists only for flags
     somebody has built with.
  2. `bazel aquery` the same targets, with the same flags, for their CppCompile actions.
  3. Rewrite the paths. Actions run in the execution root, where external/ and bazel-out/ are
     relative. The database's "directory" is the WORKSPACE instead, so that go-to-definition
     lands on the file you edit and not on its symlink under the execroot -- which means
     external/... and bazel-out/... are made absolute, and everything else is already right.
  4. One entry per source file. A file compiled both for the board and for the host tests keeps
     the board's command, because that is the code that ships.

The dependency-file and output flags (-MD, -MF, -o) are dropped. They name files inside Bazel's
output tree, and a tool that replays a command literally would overwrite them -- which Bazel
notices, and answers with a rebuild.

So is -fanalyzer, which the host-only first-party files carry (HOST_ANALYZER in
projects/flexptp_demo_stm32h563/lib/BUILD.bazel). It only adds diagnostics and changes nothing
about what the code means, and clang does not know it: left in, it is an "unknown argument"
driver error on every one of those files.
"""
import json
import pathlib
import subprocess
import sys

WORKSPACE = pathlib.Path(__file__).resolve().parents[2]

FIRMWARES = [
    "//projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flexptp-demo",
    "//projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo:flexptp-demo-freertos",
]

# Every C/C++ test, and nothing else. Not tests(//...): that includes
# //tools/flash:openocd_capability_test, and building it builds ST's OpenOCD from source --
# minutes, for an editor feature.
HOST_TESTS = 'kind("cc_test", //...)'

DEFAULT_FLAGS = ["--config=dbg"]

# Flags whose value is a path, written joined ("-Iexternal/...") or as the next argument.
PATH_FLAGS = ("-I", "-iquote", "-isystem", "-idirafter", "-include", "-imacros")

# Dropped, with the argument after them where they take one. See the module docstring.
DROP = {"-MD": 0, "-MMD": 0, "-MF": 1, "-MT": 1, "-o": 1, "-fanalyzer": 0}


def absolute(path, execroot, output_base):
    """An execroot-relative path, as the workspace-rooted database needs it."""
    if path.startswith("external/"):
        return f"{output_base}/{path}"
    if path.startswith("bazel-out/"):
        return f"{execroot}/{path}"
    return path


def rewrite(arg, execroot, output_base):
    for flag in PATH_FLAGS:
        if arg.startswith(flag) and len(arg) > len(flag):
            return flag + absolute(arg[len(flag):], execroot, output_base)
    return absolute(arg, execroot, output_base)


def is_board(arguments):
    return "arm-none-eabi-" in pathlib.PurePath(arguments[0]).name


def to_entries(actions, workspace, execroot, output_base):
    """compile_commands.json entries from aquery's jsonproto "actions" list."""
    # Board compiles first; sorted() is stable, so aquery's order decides among the rest.
    actions = sorted(actions, key=lambda a: not is_board(a["arguments"]))

    entries = {}
    for action in actions:
        args, source = [], None
        it = iter(action["arguments"])
        for arg in it:
            if arg in DROP:
                for _ in range(DROP[arg]):
                    next(it)
                continue
            arg = rewrite(arg, execroot, output_base)
            if args and args[-1] == "-c":
                source = arg
            args.append(arg)
        if source is not None and source not in entries:
            entries[source] = {"directory": str(workspace), "file": source, "arguments": args}

    return [entries[f] for f in sorted(entries)]


def bazel(*args, capture=False):
    result = subprocess.run(
        ["bazel", *args],
        check=True,
        cwd=WORKSPACE,
        stdout=subprocess.PIPE if capture else None,
        text=True,
    )
    return result.stdout


def main(argv):
    if "-h" in argv or "--help" in argv:
        print(__doc__)
        return 0

    if "--" in argv:
        split = argv.index("--")
        flags, targets = argv[:split], argv[split + 1:]
    else:
        flags, targets = argv, []
    flags = flags or DEFAULT_FLAGS

    if not targets:
        tests = bazel("query", HOST_TESTS, "--output=label", capture=True).split()
        targets = FIRMWARES + tests

    info = dict(
        line.split(": ", 1)
        for line in bazel("info", "execution_root", "output_base", capture=True).splitlines()
    )

    bazel("build", *flags, *targets)

    expr = 'mnemonic("CppCompile", deps({}))'.format(" + ".join(f'"{t}"' for t in targets))
    graph = json.loads(
        bazel(
            "aquery", *flags, expr,
            "--output=jsonproto",
            "--include_artifacts=false",
            capture=True,
        )
    )

    entries = to_entries(
        graph.get("actions", []), WORKSPACE, info["execution_root"], info["output_base"]
    )
    out = WORKSPACE / "compile_commands.json"
    out.write_text(json.dumps(entries, indent=2) + "\n")

    board = sum(is_board(e["arguments"]) for e in entries)
    print(f"{out}: {len(entries)} files, {board} for the board, {len(entries) - board} host-only")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
