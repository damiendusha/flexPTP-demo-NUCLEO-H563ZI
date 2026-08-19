"""The vendored trees, fetched from git.

These were git submodules until phase 2 of the Bazel migration. Every pin below is the commit
the corresponding submodule pointed at when it was converted, so the migration changed the build
system without changing a line of vendored source.

Two properties that the submodule arrangement did not have:

  - the patches in //patches are applied *by the fetch*, keyed by content in the repository
    cache. There is no longer a working tree that can be pristine, patched, or -- the case
    build.sh's apply_submodule_patches() had to detect and refuse -- neither. That is what
    retires the --check-patches CI gate, which existed because patch 0004 is silent: omit it and
    the firmware builds, links, gets an address and never syncs (finding F80).
  - a clone does not need `git submodule update --init --recursive` before it can build.

See docs/bazel_decisions.md for why these come from git rather than from BCR.
"""

load("@bazel_tools//tools/build_defs/repo:git.bzl", "new_git_repository")

def _vendored_impl(_mctx):
    # ST's HAL for the STM32H5. Configured by the project's stm32h5xx_hal_conf.h, reached
    # through a label_flag so a second project can supply its own.
    new_git_repository(
        name = "stm32h5_hal",
        build_file = "//bazel:stm32h5_hal.BUILD",
        commit = "0fff6e2a2e2fa40ba335ee739dd0cb64c509f521",  # v1.7.0-8-g0fff6e2
        remote = "https://github.com/STMicroelectronics/stm32h5xx-hal-driver.git",
    )

    # Eclipse ThreadX, not ST's stm32-mw-threadx fork -- see
    # projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/threadx_netxduo_feasibility.md, "Choosing the supply chain".
    new_git_repository(
        name = "threadx",
        build_file = "//bazel:threadx.BUILD",
        commit = "44d7c95c582d415c4ad84527180b29c93c3bf664",  # v6.5.0.202601_pre-48
        remote = "https://github.com/eclipse-threadx/threadx.git",
    )

    # Eclipse NetX Duo. ST's fork is pinned at 6.4.0 and is missing the bounds checks tagged
    # GHSA-cf2g-j6vv-m8c5 and GHSA-c9pq-93jp-w649 in _nx_ipv4_packet_receive(), both on the
    # IPv4/DHCP receive path this firmware runs on every frame.
    new_git_repository(
        name = "netxduo",
        build_file = "//bazel:netxduo.BUILD",
        commit = "473d192822babbd831dc5907b9e279b340a3925b",  # v6.5.0.202601_pre-28
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

    # flexPTP, with the three patches that were working-tree edits under the submodule
    # arrangement and are part of the fetch here.
    new_git_repository(
        name = "flexptp",
        build_file = "//bazel:flexptp.BUILD",
        commit = "76aaf67050dd497d09af1d60e1cc752bb93d9af4",  # RC1.0-11
        patch_args = ["-p1"],
        patches = [
            "//patches/flexptp:0002-servo-nan-latch-fix.patch",
            "//patches/flexptp:0003-generic-netstack-igmp-addresses.patch",
            "//patches/flexptp:0004-cmsis-os2-event-enqueue-from-timer-thread.patch",
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
    # 11.1.0 and this firmware was validated against V11.3.0, whose pin master carries as
    # "the CM33 port API is load-bearing for this project".
    new_git_repository(
        name = "freertos",
        build_file = "//bazel:freertos.BUILD",
        commit = "9b777ae5c5b8e9e456065a00294d1e5f5f9facf5",  # V11.3.0
        remote = "https://github.com/FreeRTOS/FreeRTOS-Kernel.git",
    )

    # lwIP, built from phase 8 for the FreeRTOS project. The pin is 34 commits past
    # STABLE-2_2_1_RELEASE and its tip is "tcp_in: Add bounds check for next option byte" --
    # ahead of the newest release on purpose, which is also why a registry module would be the
    # wrong source for it even if one existed. It does not; lwIP is not on BCR.
    new_git_repository(
        name = "lwip",
        build_file = "//bazel:lwip.BUILD",
        commit = "3d896ba0a37ff3ce73270ca5e230707fe47f60e3",  # STABLE-2_2_1_RELEASE-34
        remote = "https://github.com/lwip-tcpip/lwip.git",
    )

vendored = module_extension(implementation = _vendored_impl)
