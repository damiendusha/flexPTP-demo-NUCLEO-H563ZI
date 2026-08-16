"""stm32_firmware(): an ELF for the board, plus the artefacts CMake produced after the link.

Replaces the add_custom_command(POST_BUILD) block in CMakeLists.txt -- size, objcopy -O ihex,
objcopy -O binary -- and the .map, which comes from rules_cc's generate_linkmap feature. That
feature is why no custom link rule is needed for the map: `-Wl,-Map=` cannot be written in
linkopts, because a cc_binary has no way to declare the extra output and $(RULEDIR) is not
available there.

A custom rule IS needed, but for a different reason than docs/bazel_migration.md first expected:
CONFIGURATION. The .hex and .bin actions resolve objcopy from the cc toolchain, so if they run
in the host configuration they get the host objcopy and an ELF built for a different platform
than the tool processing it. `_stm32_artifacts` therefore carries a rule-level incoming
transition onto the target platform, which applies to every attribute at once -- the ELF and the
toolchain together -- rather than an attribute-level transition on the ELF alone, which would
leave objcopy behind in the host configuration.

The effect a caller sees: `bazel build` on a firmware target produces a Cortex-M33 image
whatever --platforms says, and cannot silently produce a host build.
"""

load("@rules_cc//cc:defs.bzl", "cc_binary")

def _target_platform_transition_impl(_settings, attr):
    return {"//command_line_option:platforms": [attr.platform]}

_target_platform_transition = transition(
    implementation = _target_platform_transition_impl,
    inputs = [],
    outputs = ["//command_line_option:platforms"],
)

def _stm32_artifacts_impl(ctx):
    # objcopy is taken by LABEL rather than from CcToolchainInfo.objcopy_executable, which the
    # rule-based cc/toolchains API leaves empty -- the action then runs '' and fails with
    # execvp(""): No such file or directory. An attribute keeps it overridable and explicit.
    objcopy = ctx.file.objcopy

    elf = ctx.file.elf
    hex_out = ctx.actions.declare_file(ctx.attr.basename + ".hex")
    bin_out = ctx.actions.declare_file(ctx.attr.basename + ".bin")

    for out, fmt in ((hex_out, "ihex"), (bin_out, "binary")):
        ctx.actions.run(
            arguments = ["-O", fmt, elf.path, out.path],
            executable = objcopy,
            inputs = depset([elf, objcopy]),
            mnemonic = "Objcopy",
            outputs = [out],
            progress_message = "Extracting %s from %s" % (fmt, elf.basename),
        )

    return [DefaultInfo(files = depset([elf, hex_out, bin_out]))]

_stm32_artifacts = rule(
    implementation = _stm32_artifacts_impl,
    # An INCOMING transition, on the rule rather than on the elf attribute: it has to cover the
    # cc toolchain as well, or objcopy is resolved for the host while the ELF is not.
    cfg = _target_platform_transition,
    attrs = {
        "basename": attr.string(mandatory = True),
        "elf": attr.label(allow_single_file = True, mandatory = True),
        "objcopy": attr.label(
            allow_single_file = True,
            cfg = "exec",
            default = "@arm_gnu_toolchain//:bin/arm-none-eabi-objcopy",
        ),
        "platform": attr.string(mandatory = True),
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
      name: base name; produces <name>.elf, <name>.elf.map, <name>.hex, <name>.bin.
      linker_script: the .ld file.
      deps: the libraries to link.
      platform: the target platform the transition selects.
      **kwargs: forwarded to cc_binary.
    """
    elf = name + ".elf"

    cc_binary(
        name = elf,
        additional_linker_inputs = [linker_script],
        features = ["generate_linkmap"],
        linkopts = ["-T $(location %s)" % linker_script],
        # Tagged manual so a wildcard build reaches the firmware through :name, which carries
        # the transition, rather than building this directly in whatever configuration happens
        # to be current.
        tags = ["manual"],
        deps = deps,
        **kwargs
    )

    _stm32_artifacts(
        name = name,
        basename = name,
        elf = ":" + elf,
        platform = platform,
    )
