# ST's HAL for the STM32H5, as Drivers/CMakeLists.txt builds it.
#
# Every NON-TEMPLATE source the repository ships, which is the same rule
# Drivers/CMakeLists.txt states. Sources whose module is not enabled in the project's
# stm32h5xx_hal_conf.h compile to empty translation units, so globbing the rest is safe and
# keeps the list from drifting.
#
# The *_template.c files are not translation units to compile: they are starting points meant
# to be copied into an application, and they reference handles and types the application is
# expected to declare. Compiling them fails with "unknown type name RTC_HandleTypeDef".
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

label_flag(
    name = "hal_conf",
    build_setting_default = "@flexptp_demo//projects/flexptp_threadx_netx_demo:hal_conf",
)

# Headers only, for the host test suite: mac_drv.c is compiled for x86 there and needs ST's real
# HAL and CMSIS definitions, so that register layouts and bit positions come from the same place
# the firmware gets them rather than from a hand-copied fake that can drift.
cc_library(
    name = "hal_headers",
    hdrs = glob(["Inc/**/*.h"]),
    copts = ["-w"],
    includes = [
        "Inc",
        "Inc/Legacy",
    ],
    deps = [":hal_conf"],
)

cc_library(
    name = "hal",
    srcs = glob(
        ["Src/*.c"],
        exclude = ["Src/*_template.c"],
    ),
    hdrs = glob(["Inc/**/*.h"]),
    copts = ["-w"],  # vendored: not this project's warnings to fix
    includes = [
        "Inc",
        "Inc/Legacy",
    ],
    deps = [
        ":hal_conf",
        "@flexptp_demo//third_party/CMSIS:cmsis",
    ],
)
