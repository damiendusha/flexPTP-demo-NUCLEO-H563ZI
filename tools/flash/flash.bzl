"""stm32_flash_binary(): `bazel run` a firmware onto the board.

openocd_exec(): the same OpenOCD, at a stable path, for tools that run it themselves.

Pure Starlark. imu's equivalent wraps a Python flasher packaged as a PEX, which costs
rules_python *and* rules_pex to build an argv and call subprocess.run; the rule already writes a
shell script, so the Python layer is dropped and the BCR footprint stays where D2 put it.

Two things it does that imu's does not:

  - the OpenOCD binary and its scripts directory are taken as explicit LABELS. imu's recovers
    them by string-splitting a runfile's short_path and searching the runfiles tree with `find`,
    which breaks quietly when repository-name mangling changes.
  - the ELF is transitioned onto the target platform, so `bazel run :flash` cannot flash a host
    build. With two firmwares in the tree (phase 8) that stops being theoretical.
"""

def _target_transition_impl(_settings, attr):
    return {"//command_line_option:platforms": [attr.platform]}

_target_transition = transition(
    implementation = _target_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

_SCRIPT = """#!/usr/bin/env bash
set -euo pipefail

# Resolve paths through the runfiles tree, not through $0's directory. short_path is relative
# to the runfiles root: main-repo files are under _main/, and external ones come out as
# ../<repo>/..., which resolves correctly from _main/ too.
RUNFILES="${{RUNFILES_DIR:-${{BASH_SOURCE[0]}}.runfiles}}/_main"

OPENOCD="${{RUNFILES}}/{openocd}"
SCRIPTS="${{RUNFILES}}/{scripts}"
ELF="${{RUNFILES}}/{elf}"

for f in "${{OPENOCD}}" "${{ELF}}"; do
    if [ ! -e "${{f}}" ]; then
        echo "flash: missing runfile ${{f}}" >&2
        exit 1
    fi
done

echo "Flashing ${{ELF}}"
exec "${{OPENOCD}}" -s "${{SCRIPTS}}" -f {board_cfg} \\
    -c init -c halt -c "program ${{ELF}} verify reset" -c exit
"""

def _stm32_flash_impl(ctx):
    elf = ctx.file.elf_image
    openocd = ctx.file.openocd
    scripts = ctx.files.scripts

    out = ctx.actions.declare_file(ctx.label.name + ".sh")
    ctx.actions.write(
        output = out,
        content = _SCRIPT.format(
            board_cfg = ctx.attr.board_cfg,
            elf = elf.short_path,
            openocd = openocd.short_path,
            scripts = scripts[0].short_path.split("/scripts/")[0] + "/scripts" if scripts else ".",
        ),
        is_executable = True,
    )

    runfiles = ctx.runfiles(files = [elf, openocd] + scripts)
    return [DefaultInfo(executable = out, runfiles = runfiles)]

def _openocd_exec_impl(ctx):
    # A symlink, not a copy: OpenOCD finds its scripts relative to the binary it resolves to, so
    # this behaves exactly as the binary it points at -- and it follows that binary when the pin
    # or the mode changes, which a copy does not.
    out = ctx.actions.declare_file(ctx.label.name + "/openocd")
    ctx.actions.symlink(output = out, target_file = ctx.file.openocd, is_executable = True)
    return [DefaultInfo(files = depset([out]), executable = out)]

# The OpenOCD stm32_flash_binary() runs -- same label, same exec configuration, same mode -- at a
# path a tool outside Bazel can reach: tools/flash/<name>/openocd under the building configuration's
# bin directory -- `bazel cquery --output=files` names it. It exists so the HIL
# harness and the VS Code debug configurations do not need a copy of their own.
openocd_exec = rule(
    implementation = _openocd_exec_impl,
    executable = True,
    attrs = {
        "openocd": attr.label(
            allow_single_file = True,
            cfg = "exec",
            default = "//tools/flash:openocd_binary",
        ),
    },
)

stm32_flash_binary = rule(
    implementation = _stm32_flash_impl,
    executable = True,
    attrs = {
        "board_cfg": attr.string(default = "board/st_nucleo_h5.cfg"),
        "elf_image": attr.label(
            allow_single_file = True,
            cfg = _target_transition,
            mandatory = True,
        ),
        "openocd": attr.label(
            allow_single_file = True,
            cfg = "exec",
            # Follows --//tools/flash:openocd; see that package for the two modes.
            default = "//tools/flash:openocd_binary",
        ),
        "platform": attr.string(default = "//toolchain:cortex_m33"),
        "scripts": attr.label(cfg = "exec", default = "//tools/flash:openocd_scripts"),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)
