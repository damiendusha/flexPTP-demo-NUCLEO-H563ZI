# Eclipse ThreadX and its Cortex-M33 port, as Middlewares/threadx.cmake builds it.
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

label_flag(
    name = "tx_user",
    build_setting_default = "@flexptp_demo//projects/flexptp_threadx_netx_demo:tx_user",
)

# The board-specific low-level init is the integrator's to supply -- Eclipse keeps it in
# example_build/ for exactly that reason -- so it is NOT here. The project provides it and links
# it alongside; see the project's startup/tx_initialize_low_level.S.
#
# secure_stack is excluded: this part runs with TZEN=0 and the FreeRTOS port it replaced was the
# non-TrustZone variant, so there is no secure world to call into. TX_SINGLE_MODE_NON_SECURE
# below is the matching switch on the header side -- excluding the files without defining it
# fails to link, defining it without excluding them merely warns. Both.
cc_library(
    name = "threadx",
    srcs = glob(
        [
            "common/src/*.c",
            "ports/cortex_m33/gnu/src/*.c",
            "ports/cortex_m33/gnu/src/*.S",
        ],
        exclude = ["ports/cortex_m33/gnu/src/*secure_stack*"],
    ),
    hdrs = glob([
        "common/inc/*.h",
        "ports/cortex_m33/gnu/inc/*.h",
    ]),
    copts = ["-w"],
    defines = [
        # Without this, tx_port.h never includes tx_user.h and every setting in it -- the tick
        # rate, TX_MAX_PRIORITIES, the timer-thread configuration -- silently reverts to the
        # ThreadX defaults.
        "TX_INCLUDE_USER_DEFINE_FILE",
        "TX_SINGLE_MODE_NON_SECURE",
    ],
    includes = [
        "common/inc",
        "ports/cortex_m33/gnu/inc",
    ],
    target_compatible_with = ["@flexptp_demo//toolchain:stm32h563"],
    deps = [":tx_user"],
)
