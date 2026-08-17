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

# The Linux port, for the host suite.
#
# Two named targets rather than one with a select() on the port: the ports differ in SOURCES,
# not just flags, and two names debug better. What differs by configuration -- the byte-pool
# sizes -- is on the consumer, not here.
#
# This is what makes the context tests possible at all: the restriction they pin lives in
# common/src/txe_queue_send.c, so the Linux port enforces it identically to the Cortex-M33 one.
cc_library(
    name = "threadx_linux",
    srcs = glob([
        "common/src/*.c",
        "ports/linux/gnu/src/*.c",
    ]),
    hdrs = glob([
        "common/inc/*.h",
        "ports/linux/gnu/inc/*.h",
    ]),
    copts = ["-w"],
    defines = [
        "TX_INCLUDE_USER_DEFINE_FILE",
        # The Linux port hands ThreadX a single malloc() of this size as its unused memory, and
        # the CMSIS-RTOS2 wrapper carves BOTH byte pools out of it. The default 64000 is smaller
        # than the pools, and overrunning it is not an allocation failure --
        # _tx_byte_pool_create() writes its first block header past the end and segfaults.
        "TX_LINUX_MEMORY_SIZE=4194304",
    ],
    includes = [
        "common/inc",
        "ports/linux/gnu/inc",
    ],
    linkopts = ["-lpthread"],
    visibility = ["//visibility:public"],
    deps = [":tx_user"],
)
