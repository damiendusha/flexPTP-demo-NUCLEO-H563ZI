# flexPTP.
#
# Exposed as SOURCES rather than as a finished cc_library, because this project supplies
# flexPTP's hardware port and network-stack driver from its own tree -- FLEXPTP_HWPORT_SRC and
# FLEXPTP_NSD_SRC in Modules/CMakeLists.txt -- and an external repository cannot absorb main-repo
# sources. //bazel/flexptp.bzl has the macro that puts them together; see docs/bazel_decisions.md
# §5 for why that is a macro and not a transition.
load("@rules_cc//cc:defs.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "core_srcs",
    srcs = [
        "src/flexptp/bmca.c",
        "src/flexptp/cli_cmds.c",
        "src/flexptp/clock_utils.c",
        "src/flexptp/common.c",
        "src/flexptp/config.c",
        "src/flexptp/event.c",
        "src/flexptp/format_utils.c",
        "src/flexptp/logging.c",
        "src/flexptp/master.c",
        "src/flexptp/msg_buf.c",
        "src/flexptp/msg_utils.c",
        "src/flexptp/port/osless/fifo.c",
        "src/flexptp/profiles.c",
        "src/flexptp/ptp_core.c",
        "src/flexptp/ptp_defs.c",
        "src/flexptp/ptp_profile_presets.c",
        "src/flexptp/settings_interface.c",
        "src/flexptp/slave.c",
        "src/flexptp/stats.c",
        "src/flexptp/task_ptp.c",
        "src/flexptp/timeutils.c",
        "src/flexptp/tlv.c",
    ],
)

# FLEXPTP_SERVO=KALMAN.
filegroup(
    name = "servo_kalman_srcs",
    srcs = ["src/flexptp/servo/kalman_filter.c"],
)

cc_library(
    name = "headers",
    hdrs = glob(["src/flexptp/**/*.h"]),
    includes = [
        "src",
        "src/flexptp",
    ],
)
