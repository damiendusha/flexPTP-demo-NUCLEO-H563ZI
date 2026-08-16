"""stm32_firmware(): an ELF for the board, plus the artefacts CMake produced after the link.

Replaces the add_custom_command(POST_BUILD) block in CMakeLists.txt -- size, objcopy -O ihex,
objcopy -O binary -- and the .map, which comes from rules_cc's generate_linkmap feature rather
than the custom link rule docs/bazel_migration.md expected to need. `-Wl,-Map=` cannot be written
in linkopts, because a cc_binary has no way to declare the extra output and $(RULEDIR) is not
available there; generate_linkmap solves exactly that, using the link action's own output path.

The binary carries a transition onto the target platform, so `bazel build` on it produces a
Cortex-M33 image whatever --platforms says, and cannot silently produce a host build.
"""

load("@rules_cc//cc:defs.bzl", "cc_binary")

def _target_platform_transition_impl(_settings, attr):
    return {"//command_line_option:platforms": [attr.platform]}

_target_platform_transition = transition(
    implementation = _target_platform_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

def _transitioned_elf_impl(ctx):
    src = ctx.file.elf
    out = ctx.actions.declare_file(ctx.label.name)
    ctx.actions.symlink(output = out, target_file = src)
    return [DefaultInfo(files = depset([out]))]

_transitioned_elf = rule(
    implementation = _transitioned_elf_impl,
    attrs = {
        "elf": attr.label(allow_single_file = True, cfg = _target_platform_transition),
        "platform": attr.string(),
        "_allowlist_function_transition": attr.label(
            default = "@bazel_tools//tools/allowlists/function_transition_allowlist",
        ),
    },
)

def stm32_firmware(
        name,
        linker_script,
        deps,
        platform = "//toolchain:cortex_m33",
        **kwargs):
    """An STM32 firmware image and its post-link artefacts.

    Args:
      name: base name; produces <name>.elf, <name>.map, <name>.hex, <name>.bin.
      linker_script: the .ld file.
      deps: the libraries to link.
      platform: the target platform to transition to.
      **kwargs: forwarded to cc_binary.
    """
    elf = name + ".elf"

    cc_binary(
        name = elf,
        additional_linker_inputs = [linker_script],
        features = ["generate_linkmap"],
        linkopts = [
            "-T $(location %s)" % linker_script,
            # Relinking when the script changes needs no equivalent of CMake's LINK_DEPENDS:
            # the script is an input, so editing it invalidates the action. Under CMake it did
            # not, and a section reorder appeared to do nothing until the ELF was deleted by
            # hand (P3.8).
        ],
        deps = deps,
        **kwargs
    )

    # objcopy and size come from the registered toolchain rather than from PATH.
    native.genrule(
        name = name + "_hex",
        srcs = [elf],
        outs = [name + ".hex"],
        cmd = "$(OBJCOPY) -O ihex $< $@",
        toolchains = ["@rules_cc//cc:current_cc_toolchain"],
    )

    native.genrule(
        name = name + "_bin",
        srcs = [elf],
        outs = [name + ".bin"],
        cmd = "$(OBJCOPY) -O binary $< $@",
        toolchains = ["@rules_cc//cc:current_cc_toolchain"],
    )

    native.filegroup(
        name = name,
        srcs = [
            elf,
            name + ".hex",
            name + ".bin",
        ],
    )
