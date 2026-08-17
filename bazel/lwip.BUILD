# lwIP, for the FreeRTOS project.
#
# The port -- sys_arch.c and arch/{cc,cpu,sys_arch}.h -- is NOT here: unlike FreeRTOS, whose
# Cortex-M33 port ships inside the kernel tree, lwIP's port is the integrator's and lives in the
# project, at lwip_port/.
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

label_flag(
    name = "lwip_config",
    build_setting_default = "@flexptp_demo//projects/flexptp_freertos_lwip_demo:lwip_config",
)

cc_library(
    name = "lwip",
    srcs = glob([
        "src/api/*.c",
        "src/core/*.c",
        "src/core/ipv4/*.c",
        "src/netif/*.c",
    ]),
    hdrs = glob(["src/include/**/*.h"]),
    copts = ["-w"],
    includes = ["src/include"],
    target_compatible_with = ["@flexptp_demo//toolchain:stm32h563"],
    deps = [":lwip_config"],
)
