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

def flexptp_library(name, config, port_srcs, nsd_srcs, nsd_hdrs = [], deps = [], features = [], target_compatible_with = None, **kwargs):
    """A flexPTP library configured for one project.

    Two libraries, because one compile action has one set of flags. flexPTP itself -- its core,
    its servo, and an NSD taken from its examples, as the lwIP firmware does -- is vendored and
    compiled with -w: its warnings are not this project's to fix. The project's own port and NSD
    are first-party and compiled with `features`, the project's first-party warnings, like any
    other first-party code.

    That used to be one library, with -w in its copts and the features on top, on the theory
    that the features gave the port and NSD their warnings back. They did not: -w inhibits every
    warning wherever it appears on the command line, so ptp_port_stm32h563.c and nsd_netxduo.c
    were outside the -Werror gate (D13) -- an unused variable added to nsd_netxduo.c built --
    and would have been outside -fanalyzer's reach too.

    A source is vendored if its label is in another repository (starts with "@").

    Args:
      name: target name; the first-party half. The vendored half is `name + "_vendored"`.
      config: cc_library exporting this project's flexptp_options.h.
      port_srcs: the hardware port (FLEXPTP_HWPORT_SRC).
      nsd_srcs: the network stack driver (FLEXPTP_NSD_SRC).
      nsd_hdrs: headers belonging to the port and driver.
      deps: anything the port and driver need -- the HAL, the RTOS, the network stack.
      features: the first-party features, applied to the project's own sources only.
      target_compatible_with: applied to both halves.
      **kwargs: forwarded to the first-party cc_library.
    """
    srcs = port_srcs + nsd_srcs
    vendored = [s for s in srcs if s.startswith("@")]
    own = [s for s in srcs if not s.startswith("@")]

    cc_library(
        name = name + "_vendored",
        srcs = [
            "@flexptp//:core_srcs",
            "@flexptp//:servo_kalman_srcs",
        ] + vendored,
        copts = ["-w"],
        defines = [
            # flexPTP >= a60190f uses __PRI64_PREFIX, a glibc-internal macro, in cli_cmds.c,
            # master.c and slave.c. Newlib does not define it, so the library does not build for
            # arm-none-eabi as-is. Map it onto newlib's equivalent rather than hardcoding "ll",
            # so it follows whatever type the target uses for int64_t (finding F47). A define,
            # so it reaches the first-party half too, as it always has.
            "__PRI64_PREFIX=__INT64",
        ],
        target_compatible_with = target_compatible_with,
        deps = deps + [
            config,
            "@flexptp//:headers",
        ],
    )

    cc_library(
        name = name,
        srcs = own,
        hdrs = nsd_hdrs,
        features = features,
        target_compatible_with = target_compatible_with,
        # flexPTP calls into the port and the NSD, and they call into flexPTP. Linking this half's
        # objects whole means the vendored archive, read after it, finds them already there.
        alwayslink = True,
        deps = deps + [
            config,
            ":" + name + "_vendored",
            "@flexptp//:headers",
        ],
        **kwargs
    )
