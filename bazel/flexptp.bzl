"""Instantiate flexPTP for one project.

flexPTP compiles this project's hardware port and network-stack driver *into* its own library
-- that is what FLEXPTP_HWPORT_SRC and FLEXPTP_NSD_SRC do in Modules/CMakeLists.txt. An external
repository cannot reach main-repo sources, so the library is assembled here instead, where both
halves are visible.

A macro rather than a build-setting transition, because two projects need two differently
configured flexPTP libraries on the same platform and a macro gives them two names instead of
one target existing twice in configurations that differ by a flag value. See
docs/bazel_decisions.md §5.
"""

load("@rules_cc//cc:defs.bzl", "cc_library")

def flexptp_library(name, config, port_srcs, nsd_srcs, nsd_hdrs = [], deps = [], **kwargs):
    """A flexPTP library configured for one project.

    Args:
      name: target name.
      config: cc_library exporting this project's flexptp_options.h.
      port_srcs: the hardware port (FLEXPTP_HWPORT_SRC).
      nsd_srcs: the network stack driver (FLEXPTP_NSD_SRC).
      nsd_hdrs: headers belonging to the port and driver.
      deps: anything the port and driver need -- the HAL, the RTOS, the network stack.
      **kwargs: forwarded to cc_library.
    """
    cc_library(
        name = name,
        srcs = [
            "@flexptp//:core_srcs",
            "@flexptp//:servo_kalman_srcs",
        ] + port_srcs + nsd_srcs,
        hdrs = nsd_hdrs,
        # flexPTP is vendored and its warnings are not this project's to fix. The port and the
        # driver ARE this project's, and they are compiled here rather than in a first-party
        # library -- which is exactly the gap Modules/CMakeLists.txt papers over with
        # set_source_files_properties(... TARGET_DIRECTORY flexptp ...). They get their warnings
        # back through the per-target features below.
        copts = ["-w"],
        defines = [
            # flexPTP >= a60190f uses __PRI64_PREFIX, a glibc-internal macro, in cli_cmds.c,
            # master.c and slave.c. Newlib does not define it, so the library does not build for
            # arm-none-eabi as-is. Map it onto newlib's equivalent rather than hardcoding "ll",
            # so it follows whatever type the target uses for int64_t (finding F47).
            "__PRI64_PREFIX=__INT64",
        ],
        deps = deps + [
            config,
            "@flexptp//:headers",
        ],
        **kwargs
    )
