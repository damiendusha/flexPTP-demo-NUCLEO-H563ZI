# ST's CMSIS-RTOS2 wrapper over ThreadX. One .c and no header of its own.
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

# The wrapper's single source, exposed for the host suite, which compiles it with different pool
# sizes and against the ThreadX Linux port.
exports_files(["cmsis_os2.c"])

filegroup(
    name = "cmsis_os2_src",
    srcs = ["cmsis_os2.c"],
)

cc_library(
    name = "cmsis_rtos_tx",
    srcs = ["cmsis_os2.c"],
    copts = ["-w"],
    defines = [
        # The wrapper #errors unless an allocation strategy is chosen. DYNAMIC takes the region
        # above __RAM_segment_used_end__ that _tx_initialize_low_level() publishes, which keeps
        # the sizing question in the linker script with the rest of the memory layout.
        "USE_DYNAMIC_MEMORY_ALLOCATION",
        "RTOS2_BYTE_POOL_STACK_SIZE=24576",
        "RTOS2_BYTE_POOL_HEAP_SIZE=8192",
    ],
    target_compatible_with = ["@flexptp_demo//toolchain:stm32h563"],
    deps = [
        "@flexptp_demo//third_party/CMSIS:cmsis",
        "@threadx",
    ],
)
