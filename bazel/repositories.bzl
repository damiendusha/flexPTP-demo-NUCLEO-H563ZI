"""The vendored trees, fetched from git.

These were git submodules until phase 2 of the Bazel migration. Every pin below is the commit
the corresponding submodule pointed at when it was converted, so the migration changed the build
system without changing a line of vendored source.

Two properties that the submodule arrangement did not have:

  - the patches in //patches are applied *by the fetch*, keyed by content in the repository
    cache. There is no longer a working tree that can be pristine, patched, or -- the case
    build.sh's apply_submodule_patches() had to detect and refuse -- neither. That is what
    retired the --check-patches CI gate, which existed because the flexPTP enqueue patch was
    silent: omit it and the firmware builds, links, gets an address and never syncs (F80).
    Upstream took all three of the original flexPTP patches, then five of the seven that
    replaced them; each remaining one goes away as its pull request merges.
  - a clone does not need `git submodule update --init --recursive` before it can build.

See docs/bazel_decisions.md for why these come from git rather than from BCR.
"""

load("@bazel_tools//tools/build_defs/repo:git.bzl", "new_git_repository")

def _vendored_impl(_mctx):
    # ST's HAL for the STM32H5. Configured by the project's stm32h5xx_hal_conf.h, reached
    # through a label_flag so a second project can supply its own. At main head: the five
    # commits since v1.7.0-8 (two ETH, GPIO, RTC, I3C) are all in code this firmware does not
    # link -- the images are byte-identical across the bump (same build_id on both).
    new_git_repository(
        name = "stm32h5_hal",
        build_file = "//bazel:stm32h5_hal.BUILD",
        commit = "deaf9d1da8a7b2958b5e0f0937bf6676ca0cd307",  # v1.7.0-13-gdeaf9d1, upstream main head (2026-10-23)
        remote = "https://github.com/STMicroelectronics/stm32h5xx-hal-driver.git",
    )

    # Eclipse ThreadX, not ST's stm32-mw-threadx fork -- see
    # projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_netxduo_feasibility.md, "Choosing the supply chain".
    new_git_repository(
        name = "threadx",
        build_file = "//bazel:threadx.BUILD",
        commit = "b2868c3d2849aab95d04cedcf8885d3f81798c86",  # v6.5.2.202603b_rel = master head
        remote = "https://github.com/eclipse-threadx/threadx.git",
    )

    # Eclipse NetX Duo. ST's fork is pinned at 6.4.0 and is missing the bounds checks tagged
    # GHSA-cf2g-j6vv-m8c5 and GHSA-c9pq-93jp-w649 in _nx_ipv4_packet_receive(), both on the
    # IPv4/DHCP receive path this firmware runs on every frame.
    new_git_repository(
        name = "netxduo",
        build_file = "//bazel:netxduo.BUILD",
        commit = "c690f59f9a5195c465153e90ef56b865337a8a62",  # v6.5.2.202603b_rel = master head
        remote = "https://github.com/eclipse-threadx/netxduo.git",
    )

    # ST's CMSIS-RTOS2 wrapper over ThreadX. Ships a single .c and no header; cmsis_os2.h is
    # ARM's and lives in //third_party/CMSIS.
    new_git_repository(
        name = "cmsis_rtos_threadx",
        build_file = "//bazel:cmsis_rtos_threadx.BUILD",
        commit = "3b0b28a45c6eca4daaf6d640fefaa7a67aeead54",  # v1.4.1
        remote = "https://github.com/STMicroelectronics/stm32-mw-cmsis-rtos-tx.git",
    )

    # flexPTP.
    #
    # RC1.0-26. Of the seven fixes this repository filed after reviewing RC1.0-17, five are
    # merged there -- #21 (logid config bit), #22 (stale pbuf tx_cb), #23 (transport-type
    # reset), #24 (sticky P2P 802.3 destinations) and #25 (orphan [LOG-LCKD] prefix) -- so
    # their patches are gone. #21 and #25 landed with the comments trimmed; the code is ours.
    #
    # Two are still open and carried here until they merge:
    #
    #   0001  #20  the Announce body is the only place a clock identity is byte-swapped on
    #              receive. Not cosmetic: it inverts the BMCA tie-break between two masters,
    #              independently reproduced by another user in issue #32, and folded into
    #              their PR #33.
    #   0006  #26  comment only: the PDelay pcbs are receive-only.
    #
    # Delete each as its pull request merges. //patches/flexptp/README.md has the commands to
    # check that a fix really is in a new revision before deleting its patch.
    new_git_repository(
        name = "flexptp",
        build_file = "//bazel:flexptp.BUILD",
        commit = "fe4b071d72c1f6e489f13e350e0e4c8e157de4b8",  # RC1.0-26
        patch_args = ["-p1"],
        patches = [
            "//patches/flexptp:0001-u1-gm-clock-identity-byte-order.patch",
            "//patches/flexptp:0006-u6-pdelay-pcb-comment.patch",
        ],
        remote = "https://github.com/epagris/flexPTP.git",
    )

    new_git_repository(
        name = "embfmt",
        build_file = "//bazel:embfmt.BUILD",
        commit = "670a34997010e8a564053f35511ff9cf56fd7523",
        patch_args = ["-p1"],
        patches = ["//patches/embfmt:0001-formatting-fixes.patch"],
        remote = "https://github.com/epagris/embfmt.git",
    )

    # FreeRTOS, for the second project (D14). git rather than BCR: the registry tops out at
    # 11.1.0. Was V11.3.0 (9b777ae), a release-branch commit that is not on main; now main head.
    # The CM33 port changed substantially on the way (#1385 adds SMP to the Armv8-M ports, behind
    # configNUMBER_OF_CORES > 1, so inert here) and the port API is load-bearing for this project,
    # so this bump was validated on the board: full S8 with the flood, and gPTP.
    new_git_repository(
        name = "freertos",
        build_file = "//bazel:freertos.BUILD",
        commit = "83a45bf290024de16ca94df1f7262bcde6c82ffe",  # main head (2026-11-17); V11.3.0 was 9b777ae
        remote = "https://github.com/FreeRTOS/FreeRTOS-Kernel.git",
    )

    # lwIP, built from phase 8 for the FreeRTOS project. master head, 104 commits past
    # STABLE-2_2_1_RELEASE -- ahead of the newest release on purpose, for the bounds checks that
    # land on master first (the previous pin's tip was "tcp_in: Add bounds check for next option
    # byte"), which is also why a registry module would be the wrong source even if one existed.
    # It does not; lwIP is not on BCR.
    new_git_repository(
        name = "lwip",
        build_file = "//bazel:lwip.BUILD",
        commit = "3aa95579e578b97b42d7871fe480f2601313066e",  # master head (2026-11-24), STABLE-2_2_1_RELEASE-104
        remote = "https://github.com/lwip-tcpip/lwip.git",
    )

vendored = module_extension(implementation = _vendored_impl)
