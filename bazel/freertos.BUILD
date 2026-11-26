# FreeRTOS-Kernel, `main` head (83a45bf). The previous pin was V11.3.0 (9b777ae), which sits on a
# release branch; main does not carry the release-bump commits, so the kernel built from this
# pin reports itself as "V11.1.0+" even though it is newer than V11.3.0.
#
# From git rather than BCR (D14): the registry's newest is 11.1.0, and master pinned V11.3.0 with
# "the CM33 port API is load-bearing for this project" -- so the downgrade a registry module
# would force lands exactly on the sensitive part.
#
# The cost of not using BCR's module is this file: it ships eight ports and five heaps, and here
# the port and the heap are selected by hand. That is ~60 lines for the two ports and one heap
# actually used, against 510 lines of coverage that is not.
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

label_flag(
    name = "freertos_config",
    build_setting_default = "@flexptp_demo//projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo:freertos_config",
)

cc_library(
    name = "headers",
    hdrs = glob(["include/*.h"]),
    includes = ["include"],
    deps = [":freertos_config"],
)

# The kernel proper. croutine.c is deprecated upstream and configUSE_CO_ROUTINES is 0, so it is
# not built.
FREERTOS_CORE = [
    "event_groups.c",
    "list.c",
    "queue.c",
    "stream_buffer.c",
    "tasks.c",
    "timers.c",
]

# The Cortex-M33 non-TrustZone port, matching FREERTOS_PORT=GCC_ARM_CM33_NTZ_NONSECURE in the
# CMake build this replaces. mpu_wrappers_v2_asm.c is for the MPU variant and is not used.
cc_library(
    name = "freertos",
    srcs = FREERTOS_CORE + [
        "portable/GCC/ARM_CM33_NTZ/non_secure/port.c",
        "portable/GCC/ARM_CM33_NTZ/non_secure/portasm.c",
        "portable/MemMang/heap_4.c",  # FREERTOS_HEAP=4
    ],
    hdrs = glob([
        "portable/GCC/ARM_CM33_NTZ/non_secure/*.h",
    ]),
    copts = ["-w"],
    includes = ["portable/GCC/ARM_CM33_NTZ/non_secure"],
    target_compatible_with = ["@flexptp_demo//toolchain:stm32h563"],
    deps = [":headers"],
)

# The POSIX port, for the host-side context tests (D15). The rule those tests pin lives in the
# kernel's common sources, so a host build enforces it the same way the target does.
cc_library(
    name = "freertos_posix",
    srcs = FREERTOS_CORE + [
        "portable/MemMang/heap_4.c",
        "portable/ThirdParty/GCC/Posix/port.c",
        "portable/ThirdParty/GCC/Posix/utils/wait_for_event.c",
    ],
    hdrs = glob([
        "portable/ThirdParty/GCC/Posix/*.h",
        "portable/ThirdParty/GCC/Posix/utils/*.h",
    ]),
    copts = ["-w"],
    includes = [
        "portable/ThirdParty/GCC/Posix",
        "portable/ThirdParty/GCC/Posix/utils",
    ],
    linkopts = ["-lpthread"],
    deps = [":headers"],
)

# The POSIX port with NO heap: the caller supplies pvPortMalloc()/vPortFree(). For the flexPTP
# teardown test (//patches/flexptp/test), whose allocator makes freed memory inaccessible so that
# a use-after-free faults where it happens instead of passing silently.
cc_library(
    name = "freertos_posix_noheap",
    testonly = True,
    srcs = FREERTOS_CORE + [
        "portable/ThirdParty/GCC/Posix/port.c",
        "portable/ThirdParty/GCC/Posix/utils/wait_for_event.c",
    ],
    hdrs = glob([
        "portable/ThirdParty/GCC/Posix/*.h",
        "portable/ThirdParty/GCC/Posix/utils/*.h",
    ]),
    copts = ["-w"],
    includes = [
        "portable/ThirdParty/GCC/Posix",
        "portable/ThirdParty/GCC/Posix/utils",
    ],
    linkopts = ["-lpthread"],
    deps = [":headers"],
)
