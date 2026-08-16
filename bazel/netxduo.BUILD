# Eclipse NetX Duo, as Middlewares/netxduo.cmake builds it.
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

label_flag(
    name = "nx_user",
    build_setting_default = "@flexptp_demo//projects/flexptp_threadx_netx_demo:nx_user",
)

# addons are opt-in file by file, not by directory: nxd_dhcp_client.c is needed, and its sibling
# nxd_dhcp_server.c is a server and is not built.
cc_library(
    name = "netxduo",
    srcs = glob(["common/src/*.c"]) + ["addons/dhcp/nxd_dhcp_client.c"],
    hdrs = glob([
        "common/inc/*.h",
        "addons/dhcp/*.h",
        "ports/cortex_m33/gnu/inc/*.h",
    ]),
    copts = ["-w"],
    defines = [
        # Without this, nx_port.h never includes nx_user.h and every setting in it reverts to
        # the defaults -- including NX_ENABLE_INTERFACE_CAPABILITY, whose absence removes the
        # per-packet field the PTP transmit timestamp request travels in. The failure would be a
        # working network stack that cannot timestamp.
        "NX_INCLUDE_USER_DEFINE_FILE",
    ],
    includes = [
        "addons/dhcp",
        "common/inc",
        "ports/cortex_m33/gnu/inc",
    ],
    target_compatible_with = ["@flexptp_demo//toolchain:stm32h563"],
    deps = [
        ":nx_user",
        "@threadx",
    ],
)
