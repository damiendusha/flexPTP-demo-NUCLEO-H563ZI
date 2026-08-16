"""stm32_flash_binary(): `bazel run` a firmware onto the board.

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
            default = "@openocd//:openocd",
        ),
        "platform": attr.string(default = "//toolchain:cortex_m33"),
        "scripts": attr.label(cfg = "exec", default = "@openocd//:scripts"),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)
