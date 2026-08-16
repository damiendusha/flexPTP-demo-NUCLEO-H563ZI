# BUILD file injected into @arm_gnu_toolchain -- the Arm GNU toolchain archive that build.sh
# also downloads, fetched here by http_archive with its sha256 pinned.
#
# This file only exposes files. The toolchain itself is defined in //toolchain, which is
# possible because the rule-based cc/toolchains API takes tool *labels*; the older
# create_cc_toolchain_config_info API needs repo-relative tool_paths and would have forced the
# toolchain definition in here instead.

package(default_visibility = ["//visibility:public"])

exports_files(glob(["bin/*"], allow_empty = True))

# Everything GCC needs to run a compile, beyond the driver itself.
#
# arm-none-eabi/bin/** is NOT optional and is easy to leave out: it is GCC's exec prefix, where
# the driver finds `as` and `ld`. Without it the sandbox has no assembler and gcc silently falls
# through to the host's, which fails with
#
#     Assembler messages: Fatal error: invalid -march= option: `armv8-m.main+dsp+fp'
#
# That one is loud. The same class of mistake is not always.
filegroup(
    name = "compiler_support",
    srcs = glob(
        [
            "libexec/**",
            "lib/gcc/**",
            "arm-none-eabi/bin/**",
            "arm-none-eabi/include/**",
            "arm-none-eabi/lib/**",
            "lib/**/include/**",
        ],
        allow_empty = True,
    ),
)

filegroup(
    name = "all_files",
    srcs = glob(["**"], allow_empty = True),
)
