# Bazel migration plan

Status: **plan only — nothing here has been landed.** The toolchain design in
[§3](#3-the-toolchain) was prototyped and verified working before this plan was written;
everything else is design.

This document sets out how to move this project from CMake
([`CMakeLists.txt`](../CMakeLists.txt) + [`build.sh`](../build.sh)) to Bazel, without Pigweed,
with dependencies fetched by Bazel's own git repository rules rather than git submodules.

**The rationale lives next door.** [`bazel_decisions.md`](bazel_decisions.md) holds the evidence
behind D1–D18 — the experiments that were run, the alternatives weighed and rejected, and why.
This document says *what to build and in what order*; that one says *why*. They are split
because they age differently: the plan churns while it is executed, the record does not. If you
are about to argue for an option this plan does not take, check there first — it has probably
been measured.

## Contents

- [0. Decisions taken](#0-decisions-taken)
- [1. Dependency policy](#1-dependency-policy)
- [2. Repository layout after the migration](#2-repository-layout-after-the-migration)
- [3. The toolchain](#3-the-toolchain)
- [4. Platforms and the configuration model](#4-platforms-and-the-configuration-model)
- [5. The target graph](#5-the-target-graph)
- [6. Host tests](#6-host-tests)
- [7. What does not translate cleanly](#7-what-does-not-translate-cleanly)
- [8. Flashing](#8-flashing)
- [9. Phases](#9-phases)
- [10. CI](#10-ci)
- [11. Risks](#11-risks)
- [12. Open decisions](#12-open-decisions)
- [13. Target state: the two-firmware monorepo](#13-target-state-the-two-firmware-monorepo)

---

## 0. Decisions taken

Eighteen decisions were taken before this plan was written. They are recorded here because the
rest of the document depends on them and because reversing one is cheaper than discovering it
was never made.

| # | Decision | Chosen | Consequence |
|---|----------|--------|-------------|
| D1 | Relationship to `~/git/imu` | **Copy what is useful; stay self-contained** | The patterns in imu's `tools/build_rules/`, `tools/flash/` and `bazel/repositories.bzl` are ported into this repo. No cross-repo dependency; this repo clones and builds on its own. Some duplication is accepted. |
| D2 | BCR policy | **Minimal BCR: rule sets only** | `rules_cc`, `platforms`, `bazel_skylib`, `googletest` come from BCR, pinned, with a checked-in lockfile. Every *project* dependency comes from git. No Pigweed, no `rules_pex`, no `rules_python`. `rules_foreign_cc` + `pkg-config` are admitted for exactly one purpose — building OpenOCD (D6) — as the deliberate "unless you really have to" exception. Six modules total. |
| D3 | CMake | **Coexist, then delete** | Bazel lands alongside CMake. CMake is removed in one commit once the Bazel image is accepted on hardware. |
| D4 | First milestone | **Toolchain + firmware + flashing + host tests** | Phases 0–6 below are the first landing. Format, coverage and the second project follow. |
| D5 | Flashing | **In scope, and a priority** | `bazel run //P:flash` is part of the first landing, not a later phase. Its design is [§8](#8-flashing). |
| D6 | Flashing tool | **ST's OpenOCD fork, built by `rules_foreign_cc`** | Upstream OpenOCD cannot program this part and no prebuilt of ST's fork exists, so Bazel must build it. Evaluated against evidence in [decisions §6](bazel_decisions.md#6-why-sts-openocd-built-from-source); a `local` escape hatch remains. probe-rs and upstream+xpack rejected. |
| D8 | `-c opt` optimisation level | **`-O2`, not CMake's `-Og`** | `opt` means what a Bazel user expects. Costs +56 B of flash and the debuggability of release images; `-O2` is already hardware-verified. Opens one verification gap that phase 4 must close — see [§3.3](#33-cc_toolchain_configbzl). |
| D10 | Long-term shape | **A two-firmware monorepo: FreeRTOS + lwIP *and* ThreadX + NetX Duo** | Changes what the configuration model must express, and reverses [§4.1](#41-platforms-and-constraints)'s "skip the transition machinery". See [§13](#13-target-state-the-two-firmware-monorepo). |
| D17 | `ETH_STACK` | **Deleted, not ported** | D9 removed its `etherlib` value and D11 makes stack choice mean *which project you build*; the residual `none` was an S3–S5 bisection aid. The ThreadX project depends on NetX Duo directly. Removes a `string_flag`, its `config_setting`s and `select()`s, two acceptance criteria and a CI job. |
| D18 | Documentation | **Per project, moved in phase 8** | `projects/<name>/docs/` holds that project's documents; repo-level ones stay in `docs/`. Moved when the second project arrives, not in phase 0 — see [§2.1](#21-documentation). |
| D16 | Offline/mirroring | **Option A: lockfile only** | Integrity is pinned; availability is not. Cold builds fetch from BCR, GitHub and gitlab.arm.com. No registry mirror, no vendoring. Reversible later without restructuring — see [decisions §2](bazel_decisions.md#2-why-these-dependencies-from-these-places). |
| D14 | FreeRTOS source | **git, `FreeRTOS/FreeRTOS-Kernel` @ V11.3.0** | Not BCR. Consistent with D2, and V11.3.0 is the version this firmware was validated with. Costs writing the port/heap selection ourselves. See [§13.4](#134-new-dependencies). |
| D15 | FreeRTOS context tests | **Yes — but re-derived, not translated** | The ISR half ports directly; the timer-thread half has no FreeRTOS analogue and must pin FreeRTOS's own rule instead. See [§13.6](#136-what-gets-harder). |
| D13 | First-party warnings | **`-Werror`, on by default, first-party targets only** | Stricter than today's CI-only log-grep. Safe because the toolchain is pinned by sha256, so new diagnostics arrive in the commit that bumps it. Escape hatch: `--features=-first_party_werror`. See [§7.3](#73-the-first-party-warnings-gate). |
| D12 | Layout inside a project | **`eth/`, `ptp/`, `cli/`, `standard_output/`, `blocking_io/`, `startup/`, `test/`; vendored ARM/ST headers to `third_party/CMSIS/`** | Fixed in phase 0, so `BUILD` files reference it from the start. Host tests live in `//P/test` — there is no root `test/` package after phase 0. See [§2](#2-repository-layout-after-the-migration). |
| D11 | Structure and order | **`projects/<name>/` from the start; the two projects mostly independent, unified afterwards** | The tree is restructured in **phase 0**, before any Bazel work, so `BUILD` files are written once in their final home. Duplication between the projects is accepted initially and factored out in phase 9. See [§13.3](#133-layout-two-independent-projects-first) and [§13.8](#138-unification-afterwards). |
| D9 | EtherLib and lwIP | **Drop EtherLib; keep the lwIP pin** | The vendored tree goes, along with the first-party glue that selected it. lwIP is retained as a git repository because it is wanted again later; imu already has a working `lwip.BUILD` to start from. |
| D7 | `build.sh` | **Deleted once the migration is complete** | It survives only for the duration of the coexistence period. Bazel absorbs all three of its jobs — toolchain provisioning ([§3.1](#31-fetching)), OpenOCD provisioning ([decisions §6](bazel_decisions.md#6-why-sts-openocd-built-from-source)) and patch application ([§1.3](#13-the-four-patches)). Deleted in phase 7; the HIL dependency on it must be removed first ([§8.3](#83-the-hil-suite-must-stop-hardcoding-the-openocd-path)). |

Note on D1: imu's toolchain is Pigweed-based (`@pigweed//pw_toolchain:cc_toolchain_cortex-m33`)
and is exactly the part being replaced. What is worth copying from imu is the *structure* —
the module extension that declares git repositories, the `label_flag` configuration-injection
pattern in its `bazel/flexptp.BUILD` and `bazel/lwip.BUILD`, the split-transition
`embedded_cc_binary` macro, and the flash rule — not the toolchain itself.

---

## 1. Dependency policy

Per D2, dependencies fall into two classes with two different mechanisms.

### 1.1 From BCR: rule sets only

```python
bazel_dep(name = "rules_cc", version = "0.2.22")
bazel_dep(name = "platforms", version = "1.0.0")
bazel_dep(name = "bazel_skylib", version = "1.8.2")
bazel_dep(name = "googletest", version = "1.15.2", dev_dependency = True)
```

Four modules. `rules_cc` is unavoidable ([decisions §1.1](bazel_decisions.md#11-bazel-92-has-removed-the-native-c-rules));
`platforms` supplies the `@platforms//cpu:armv8-m` and `@platforms//os:none` constraints;
`bazel_skylib` supplies `string_flag`/`bool_flag` for the configuration model in
[§4](#4-platforms-and-the-configuration-model); `googletest` is the host test framework and is
already pinned to v1.15.2 by [`test/CMakeLists.txt`](../projects/flexptp_threadx_netx_demo/test/CMakeLists.txt), so this is not a
new dependency, only a new way of fetching the same commit.

`MODULE.bazel.lock` is **checked in**. Without it the "pinned" versions above are still
resolved against a live registry.

Plus two more for OpenOCD only (D6), which are the deliberate exception D2 carves out and are
justified in [decisions §6](bazel_decisions.md#6-why-sts-openocd-built-from-source):

```python
bazel_dep(name = "rules_foreign_cc", version = "0.14.0")
bazel_dep(name = "pkg-config", version = "0.29.2.bcr.1")
```

Six modules total. Explicitly *not* taken from BCR, all of which imu uses: `pigweed` (replaced
by [§3](#3-the-toolchain)), `freertos` (this project runs ThreadX), `rules_python`/`rules_pex`
(imu needs them to package its Python flasher as a PEX; the flash rule here is pure Starlark
and needs neither — see [§8.1](#81-the-flash-rule--pure-starlark-no-new-bcr)), `toolchains_llvm`
(the sanitizer configs are not part of this migration).

### 1.2 From git: everything the project actually compiles

The submodules in [`.gitmodules`](../.gitmodules) become git repositories declared by a module
extension, in the shape of imu's `bazel/repositories.bzl`. Pins are the SHAs the submodules
point at **today**, so the migration changes the build system without changing a single line
of vendored source:

| Repo | Remote | Pin | Description tag |
|---|---|---|---|
| `stm32h5_hal` | `github.com/STMicroelectronics/stm32h5xx-hal-driver` | `0fff6e2a2e2fa40ba335ee739dd0cb64c509f521` | `v1.7.0-8-g0fff6e2` |
| `threadx` | `github.com/eclipse-threadx/threadx` | `44d7c95c582d415c4ad84527180b29c93c3bf664` | `v6.5.0.202601_pre-48` |
| `netxduo` | `github.com/eclipse-threadx/netxduo` | `473d192822babbd831dc5907b9e279b340a3925b` | `v6.5.0.202601_pre-28` |
| `cmsis_rtos_threadx` | `github.com/STMicroelectronics/stm32-mw-cmsis-rtos-tx` | `3b0b28a45c6eca4daaf6d640fefaa7a67aeead54` | `v1.4.1` |
| `flexptp` | `github.com/epagris/flexPTP` | `76aaf67050dd497d09af1d60e1cc752bb93d9af4` | `RC1.0-11` |
| `embfmt` | `github.com/epagris/embfmt` | `670a34997010e8a564053f35511ff9cf56fd7523` | — |
| `lwip` | `github.com/lwip-tcpip/lwip` | `3d896ba0a37ff3ce73270ca5e230707fe47f60e3` | `STABLE-2_2_1_RELEASE-34` |

Seven repositories. None is a Bazel module, so each is a `new_git_repository` with a
`build_file` from `//bazel/`.

**EtherLib is dropped entirely (D9).** It was compile-only and has never been run — see
`docs/design_doc.md`, "Remaining work to enable EtherLib". Removing it takes with it the
`//P:etherlib_config` target, the `ETHERLIB` half of CI's alternatives matrix, and the 39-file
vendored tree. It also strands five
first-party glue files that nothing will build: `Src/etherlib_options.c`,
`Inc/etherlib_options.h`, [`Src/ethernet/ethernet_etherlib.c`](../Src/ethernet/ethernet_etherlib.c)
(and its header), and [`Drivers/EthDrv/eth_drv_etherlib.c`](../Drivers/EthDrv/eth_drv_etherlib.c)
(and its header). **Deleting those is a source change, not a build-system change** — it should
be its own commit, before or after the migration, not smuggled into a `BUILD` file. The plan
assumes they stay on disk and simply go unreferenced until then.

**lwIP is kept and is expected to come back (D9).** It is pinned but not currently built — the
`ETH_STACK=LWIP` path `FATAL_ERROR`s in [`Modules/CMakeLists.txt`](../Modules/CMakeLists.txt),
which is where lwIP was removed in S3 of the ThreadX port. Because it is wanted again later,
carry the pin *and* plan for a `bazel/lwip.BUILD`: imu already has a working one that globs
`src/api`, `src/core`, `src/core/ipv4` and `src/netif` and injects `lwipopts.h` through an
`lwip_config` `label_flag` — the same pattern as [§4.2](#42-configuration-headers). Reviving
lwIP then means restoring `ETH_LWIP` to the flag's `values` and writing the port glue, not
rediscovering how to package lwIP for Bazel.

`Drivers/CMSIS/` (35 files) stays **in-tree**. It is not a submodule today, it holds ARM's
`cmsis_os2.h` and ST's device headers, and there is nothing to gain from moving it.

### 1.3 The four patches

[`patches/`](../patches/) exists because the parent repo records only a submodule pointer, so
working-tree edits inside `Modules/` are invisible to a clone. `build.sh` applies them and CI
verifies them independently — see [`patches/flexptp/README.md`](../patches/flexptp/README.md).

Bazel's git repository rules take `patches` and `patch_args` directly:

```python
new_git_repository(
    name = "flexptp",
    remote = "https://github.com/epagris/flexPTP",
    commit = "76aaf67050dd497d09af1d60e1cc752bb93d9af4",
    build_file = "//bazel:flexptp.BUILD",
    patches = [
        "//patches/flexptp:0002-servo-nan-latch-fix.patch",
        "//patches/flexptp:0003-generic-netstack-igmp-addresses.patch",
        "//patches/flexptp:0004-cmsis-os2-event-enqueue-from-timer-thread.patch",
    ],
    patch_args = ["-p1"],
)
```

The patch files are standard `git diff` output with `a/`/`b/` prefixes, so `-p1` is correct
and they need no regeneration. A `BUILD.bazel` with `exports_files()` must be added under
`patches/flexptp/` and `patches/embfmt/` to make them addressable as labels.

**This is a genuine improvement, and it retires a whole class of failure.** The patches become
part of the fetch, inside the repository cache, keyed by content. There is no
"neither pristine nor patched" state to detect, no `apply_submodule_patches()`, and no way to
build an unpatched tree — which is what `--check-patches` exists to prevent, having been added
after patch 0004 (a *silent* defect: the firmware builds, links, gets an address and never
syncs) went unverified. The `--check-patches` CI step becomes unnecessary rather than being
ported; [§10](#10-ci) covers what replaces it.

---

## 2. Repository layout after the migration

Per D10/D11 the tree is restructured into per-project directories **before** the Bazel work
begins, so that every `BUILD` file is written once in its final home. Phase 0
([§9](#9-phases)) does that move under CMake. This is the agreed shape (D12):

```
projects/
  flexptp_threadx_netx_demo/    the firmware that exists today
    BUILD.bazel
    main.c  cmds.c  syscall.c  sysmem.c  cpp_support.cpp  system_stm32h5xx.c
    tx_user.h  nx_user.h  flexptp_options.h  stm32h5xx_hal_conf.h
    cli/  standard_output/  blocking_io/
    eth/          mac_drv.c  phy_drv/  eth_drv_netxduo.c  ethernet_netxduo.c
                  eth_framer.cpp  eth_link_state.cpp
    ptp/          ptp_port_stm32h563.c  nsd_netxduo.c  ptp_*.cpp
    startup/      startup_stm32h563zitx.s  tx_initialize_low_level.S
                  stm32h563zitx_FLASH.ld
    test/         this project's host tests (no root test/ package after phase 0)
  flexptp_freertos_lwip_demo/   added in phase 8, from master (§14.2)

third_party/
  CMSIS/                        ARM + ST headers, vendored in-tree (was Drivers/CMSIS)

bazel/
  repositories.bzl              module extension declaring the git repos
  stm32h5_hal.BUILD  threadx.BUILD  netxduo.BUILD  cmsis_rtos_threadx.BUILD
  flexptp.BUILD  embfmt.BUILD
  lwip.BUILD                    added in phase 8 (D9)

toolchain/
  BUILD.bazel                   toolchain(), platform(), the mcu constraint (§5.1)
  cc_toolchain_config.bzl       the toolchain definition (§4)
  arm_gnu.BUILD                 injected into @arm_gnu_toolchain

tools/build_rules/firmware.bzl  stm32_firmware(): elf + map + hex + bin + size
tools/flash/                    stm32_flash_binary (§9.2)

.bazelversion  .bazelrc  MODULE.bazel  MODULE.bazel.lock  BUILD.bazel
patches/*/BUILD.bazel           exports_files() for the patch labels
```

**Label shorthand.** `//P` abbreviates `//projects/flexptp_threadx_netx_demo` throughout the
rest of this document, because the full package path is long and appears often. The second
project is `//projects/flexptp_freertos_lwip_demo` and gets no abbreviation, since it appears
only in [§13](#13-target-state-the-two-firmware-monorepo).

**There is deliberately no shared `lib/` yet.** D11 makes the two projects mostly independent
first and unifies afterwards, with duplication accepted in between — see
[§13.3](#133-layout-two-independent-projects-first) for why, and
[§13.8](#138-unification-afterwards) for what unification then targets.

`Modules/` and `Drivers/stm32h5xx-hal-driver/` are **not** moved by phase 0. They are submodules
that phase 2 replaces with git repositories, so disturbing them early would create work that
phase 2 throws away. `Modules/blocking_io/` is the exception — it is first-party, not a
submodule, and moves with the rest.

`.gitignore` gains `bazel-*`. `toolchains/` stays ignored; Bazel fetches its own copy of the
ARM archive into the repository cache and does not use the one `build.sh` extracts.

### 2.1 Documentation

**D18: documentation is per project**, in `projects/<name>/docs/`. Most of `docs/` describes
*this* firmware rather than the repository — `architecture.md` opens "How this firmware fits
together: the module stack, the threads, the interrupts"; `design_doc.md` is the H743→H563 port's
design record; `testing_architecture.md` describes tiers that include the ThreadX host tests.
Once a second project exists, an unqualified `docs/` is ambiguous.

| Destination | Documents |
|---|---|
| `projects/flexptp_threadx_netx_demo/docs/` | `architecture.md`, `design_doc.md`, `testing_architecture.md`, `ptp_clock.md`, `ptp_profiles.md`, `host_ptp_setup.md`, `implementation_plan.md`, `threadx_porting_plan.md`, `threadx_netxduo_feasibility.md`, `threadx_s5e_plan.md`, `port_review.md`, `review_v1_opus_5p0.md`, `reviews/` |
| `docs/` (repo level) | `bazel_migration.md`, `bazel_decisions.md` |

**Unification, later.** Three of those are hardware- or protocol-level rather than
firmware-architecture — `ptp_clock.md` (the H563 MAC timestamp clock), `ptp_profiles.md`
(flexPTP's profile presets) and `host_ptp_setup.md` (`linuxptp` on the host) — and none of their
content changes with the RTOS or the network stack. They are the documentation counterpart of
[§13.8](#138-unification-afterwards)'s tier 1, and the natural candidates to lift back to a
shared `docs/` once both projects exist and the overlap is visible rather than predicted. Until
then they live with the only project that has them, which is the same duplicate-then-unify rule
D11 applies to code.

#### Why phase 8 and not phase 0

The code moves in phase 0 because `BUILD` files must reference final paths — write once, not
twice ([§13.7](#137-sequencing)). **No `BUILD` file references a document**, so that argument
does not transfer, and the link arithmetic argues the other way:

| Link class | Count |
|---|---|
| `docs/` → source paths (`Src/`, `Inc/`, `Drivers/`, …) | 279 |
| `docs/` → `docs/` | 150 |
| `docs/reviews/` → parent paths | 333 |
| `README.md` → `docs/` | 30 |

Moving sources *and* documents together means editing all four classes at once — roughly 790
links — with one gate at the end. Splitting them keeps each move independently verifiable:
phase 0 fixes the 279 source links and proves "same firmware, new source paths"; phase 8 fixes
the rest and proves "same documents, new home". [`tools/ci/check_docs_links.py`](../tools/ci/check_docs_links.py)
catches every break in both, so the risk is tedium rather than silent rot — but 790 edits behind
a single gate is how tedium turns into a rushed sweep.

---

## 3. The toolchain

This replaces both `cmake/gcc-arm-none-eabi.cmake`
([`cmake/gcc-arm-none-eabi.cmake`](../cmake/gcc-arm-none-eabi.cmake)) and Steps 2–3 of
`build.sh`, and it replaces Pigweed's `cc_toolchain_cortex-m33` in the imu design.

### 3.1 Fetching

The same URL `build.sh` uses, so the Bazel and CMake builds compile with the identical
compiler during the coexistence period:

```python
http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

http_archive(
    name = "arm_gnu_toolchain",
    urls = ["https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel1/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi.tar.xz"],
    sha256 = "563bebb2b97d53382b956d6ee1fe61e2cae26699901417234a37df505ef9b5fa",
    strip_prefix = "arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi",
    build_file = "//toolchain:arm_gnu.BUILD",
)
```

The `sha256` is the one measured in [decisions §1.4](bazel_decisions.md#14-a-pigweed-free-arm-toolchain-works-and-produces-the-right-code)
from the archive already in `toolchains/`. `build.sh` fetches this archive with **no integrity
check at all** today, so pinning the digest is a small independent gain.

This is `x86_64-linux` only, matching `build.sh`. Adding aarch64 or macOS means a second
`http_archive` and a `select()` on the exec platform; not planned, but the structure allows it.

### 3.2 `arm_gnu.BUILD`

Filegroups over the extracted archive. The `arm-none-eabi/bin/**` entries are the fix from
[decisions §1.5](bazel_decisions.md#15-two-traps-found-while-prototyping) and must not be dropped:

```python
filegroup(
    name = "compiler_files",
    srcs = glob([
        "bin/arm-none-eabi-gcc*", "bin/arm-none-eabi-g++", "bin/arm-none-eabi-cpp",
        "bin/arm-none-eabi-as",
        "libexec/**", "lib/gcc/**",
        "arm-none-eabi/bin/**",        # GCC's exec prefix: as, ld. Required.
        "arm-none-eabi/include/**",
    ], allow_empty = True),
)
# linker_files: as above, with arm-none-eabi/lib/** instead of include/**
# ar_files / objcopy_files / strip_files: the single binary plus libexec/**
```

The `cc_toolchain` and its config are declared **inside this file**, i.e. inside the external
repository, so that `tool_paths` are simple repo-relative strings (`bin/arm-none-eabi-gcc`)
rather than paths that have to reach across the `external/` mangling. The `.bzl` is loaded
back out of the main module:

```python
load("@flexptp_demo//toolchain:cc_toolchain_config.bzl", "cc_toolchain_config")
load("@rules_cc//cc/toolchains:cc_toolchain.bzl", "cc_toolchain")
```

`cxx_builtin_include_directories` must list the three directories GCC searches, verified with
`gcc -E -Wp,-v`:

```
lib/gcc/arm-none-eabi/15.3.1/include
lib/gcc/arm-none-eabi/15.3.1/include-fixed
arm-none-eabi/include
```

These embed the compiler version `15.3.1`. A toolchain bump changes them, and the symptom of
getting them wrong is Bazel's "this file is a header, but it wasn't declared" strictness error
rather than a miscompile.

### 3.3 `cc_toolchain_config.bzl`

Loads that Bazel 9 requires (`CcToolchainConfigInfo` and `cc_common` are no longer globals —
this was the second failure hit while prototyping):

```python
load("@rules_cc//cc:action_names.bzl", "ACTION_NAMES")
load("@rules_cc//cc:cc_toolchain_config_lib.bzl", "feature", "flag_group", "flag_set", "tool_path")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/toolchains:cc_toolchain_config_info.bzl", "CcToolchainConfigInfo")
```

The features to define, and where each comes from in the current build:

| Feature | Flags | Source |
|---|---|---|
| `default_flags` (always on) | `-mthumb -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard` on compile **and** link | `cpu_PARAMS` |
| | `-fdata-sections -ffunction-sections` | `FLAGS` in the CMake toolchain file |
| | `-fno-rtti -fno-exceptions -fno-threadsafe-statics` (C++ only) | `CPP_FLAGS` |
| | `--specs=nosys.specs`, `-Wl,--gc-sections`, `-Wl,-z,max-page-size=8` on link | `target_link_options` |
| | `-Wl,--start-group -lc -lm -lstdc++ -lsupc++ -Wl,--end-group` | `target_link_options` |
| `dbg` | `-O0 -g3 -ggdb` | `$<$<CONFIG:Debug>:...>` |
| `opt` | `-O2 -g0` | **changed from CMake's `-Og` by D8** — see the note below |
| `first_party_warnings` (off by default) | `-Wall -Wextra -Wno-unused-parameter` | `target_compile_options` |
| `asm_with_cpp` | `-x assembler-with-cpp` | the ASM genex |

**`opt` means `-O2`, which is a deliberate change from the CMake build's `-Og` (D8).** The
mapping is `--compilation_mode=fastbuild|dbg|opt` → `-O0 -g` / `-O0 -g3 -ggdb` / `-O2 -g0`.
`-c opt` now means what a Bazel user expects it to mean.

The long comment in [`CMakeLists.txt`](../CMakeLists.txt) is the evidence base for what this
costs, and it is reassuring on the two points that matter:

- **Size: +56 bytes.** The recorded measurements are `-Og` 149 400 B, `-O2` 149 456 B, `-Os`
  147 400 B of FLASH. So "a larger binary is OK" is barely being cashed in — 56 B on a 2 MB
  part, with flash at 7% utilisation.
- **`-O2` is hardware-known-good.** All three levels were built *and run*, the full 12-check S8
  suite passing 12/12 on each, reaching SLAVE with `|offset|` ≤ 14 895 ns at `-O2`. The comment
  states plainly that "all three levels are known-good".

What is given up is debuggability of release images, which the comment says "on this project has
repeatedly mattered". That is the trade D8 accepts.

**One verification gap this opens, and it must be closed in phase 4.** The same comment records
that the F62 regression — the STM32H563's UID region must be read 32 bits at a time, which
`UNIQUE_DEV_ID_BASE` in `Inc/stm32h5xx_hal_conf.h` forces with a `volatile const uint32_t *` —
was re-checked at the disassembly level **only at `-Og`**, where the UID is read with `ldr.w`
and the bytes extracted with `ubfx`. It was explicitly *not* re-checked at `-O2`, because
`drv_initialize()` is inlined away at that level and the UID base register was not tracked
through the inlined code. F62 was a HardFault, so the passing `-O2` hardware run does show the
byte access is absent — but only on the paths S8 exercises.

Moving the shipping level to `-O2` promotes that from a documented gap in a level nobody ships
to a documented gap in the level everybody ships. It is cheap to close: disassemble the `opt`
image, find where `0x08FFF800` is materialised, and assert that the accesses through it are
`ldr`/`ldr.w` and never `ldrb`/`ldrh`. Worth scripting as `//tools/ci:uid_access_test`, in the
same spirit as [`tools/ci/check_cxx_rules.sh`](../tools/ci/check_cxx_rules.sh) — a rule about
what the firmware contains, asked of the firmware.

The `-Wall -Wextra` set is a **non-default feature**, enabled per-target on first-party
libraries only. This is how the vendored/first-party split in the CI warnings gate
([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)) is expressed structurally instead
of by grepping a log — see [§10](#10-ci). It also fixes, for free, the gap that
[`Modules/CMakeLists.txt`](../Modules/CMakeLists.txt) documents at length: `ptp_port_stm32h563.c`
is first-party but gets compiled into flexPTP's library and so inherited its `-w`, needing a
`set_source_files_properties(... TARGET_DIRECTORY flexptp ...)` incantation to warn at all.
In Bazel it is simply a first-party `cc_library` with the feature on ([§5.4](#54-flexptp)).

### 3.4 Registration

```python
toolchain(
    name = "arm_none_eabi_cortex_m33",
    target_compatible_with = ["@platforms//cpu:armv8-m", "@platforms//os:none"],
    toolchain = "@arm_gnu_toolchain//:cortex_m33_cc_toolchain",
    toolchain_type = "@rules_cc//cc:toolchain_type",
)
```

The host toolchain stays Bazel's autodetected local one, which is what the host tests want:
[`test/CMakeLists.txt`](../projects/flexptp_threadx_netx_demo/test/CMakeLists.txt) compiles with the system gcc/g++ today.

---

## 4. Platforms and the configuration model

### 4.1 Platforms and constraints

Three constraint values describe the board, declared in `//toolchain/BUILD.bazel`:

```python
constraint_setting(name = "mcu")
constraint_value(name = "stm32h563", constraint_setting = ":mcu")

platform(
    name = "cortex_m33",
    constraint_values = [
        "@platforms//cpu:armv8-m",   # from BCR's platforms module
        "@platforms//os:none",       # bare metal
        ":stm32h563",                # ours
    ],
)
```

**Why a project-local `constraint_setting` rather than the two stock ones alone.**
`@platforms//cpu:armv8-m` does not distinguish a Cortex-M33 from a Cortex-M23, and says nothing
about the FPU or the float ABI — yet `-mfpu=fpv5-sp-d16 -mfloat-abi=hard` is baked into the
toolchain. With only `cpu` and `os`, a second Armv8-M part added later would match this
toolchain and be compiled with the wrong FPU flags, and toolchain resolution would give no
warning. Four lines now make that impossible. This is the same job Pigweed does with
`@pigweed//pw_build/constraints/arm:cortex-m33`, scoped to one project — see
[decisions §3](bazel_decisions.md#3-why-not-pigweed).

**The constraint's real payoff is `target_compatible_with`.** Firmware-only libraries carry:

```python
cc_library(name = "app", target_compatible_with = ["//toolchain:stm32h563"], ...)
```

which is what makes a *single* `bazel build //...` and `bazel test //...` do the right thing
across host and target: the firmware libraries are skipped as incompatible when the host
platform is in effect, rather than being attempted with the host compiler and failing. Without
it, the host/target split that [§6](#6-host-tests) describes as "a real simplification" does not
actually work at the command line — a wildcard build would try to compile `mac_drv.c` for x86
and fall over. This is the mechanism that retires the two-CMake-roots arrangement, so it is not
optional polish.

**The toolchain also needs `exec_compatible_with`:**

```python
toolchain(
    name = "arm_none_eabi_cortex_m33",
    exec_compatible_with = ["@platforms//os:linux", "@platforms//cpu:x86_64"],
    target_compatible_with = ["@platforms//cpu:armv8-m", "@platforms//os:none", ":stm32h563"],
    ...
)
```

because [§3.1](#31-fetching) fetches only the `x86_64-linux` archive. Omitting it lets Bazel
select this toolchain on an aarch64 or macOS host and fail at exec time with a confusing error
instead of a clear "no toolchain found".

**Selection without flags.** `stm32_firmware()` carries a transition onto
`//toolchain:cortex_m33`, exactly like the flash rule in
[§8.1](#81-the-flash-rule--pure-starlark-no-new-bcr). So `bazel build //P:flexptp-demo` produces
a Cortex-M33 image whatever `--platforms` says, and cannot silently produce a host build. The
`--config=target` line in `.bazelrc` below then exists only for building *libraries* directly,
not for the firmware.

Unlike imu, there is no `@freertos//:port_*` constraint — that is FreeRTOS's, and this project
runs ThreadX. ThreadX needs no constraint of its own because there is only one port in play;
were the host-side ThreadX of [§6](#6-host-tests) ever to become ambiguous, the `mcu` setting is
where that would be expressed.

**Recommendation: skip imu's split-transition multiplexer** (`embedded_cc_binary` /
`embedded_multiplexer`) **for now** — but see the caveat below. It exists so
`bazel build //...` can build one source tree for several MCUs at once. This repository targets
exactly one part, and the single-platform transition on `stm32_firmware()` above already gives
flag-free builds. A plain `--platforms` flag in `.bazelrc` under a named config is simpler and
does not need the function-transition
allowlist. The macro can be added later if a second target appears.

**Caveat:** that reasoning holds for *platforms*, and D10's two-firmware monorepo raises a
separate question about *configuration* — two binaries needing different RTOS and stack
configurations in one invocation, which a global flag cannot express. The answer turns out
**not** to be imu's `app_config_transition` either:
[decisions §5](bazel_decisions.md#5-why-not-imus-embedded_cc_binary) works through which vendored
library actually needs two configurations at once, and concludes that a per-project macro over
the [§4.2](#42-configuration-headers) `label_flag`s does the job with less machinery. What is
worth taking from imu's rule is its *validation*, not its transitions.

```
# .bazelrc
build:target --platforms=//toolchain:cortex_m33
build:dbg --compilation_mode=dbg
build:opt --compilation_mode=opt
```

### 4.2 Configuration headers

This is the pattern to copy from imu (`bazel/lwip.BUILD`, `bazel/flexptp.BUILD`), and it is
the single most important structural idea in the migration. Five vendored libraries here are
configured by a header that lives in this repository:

| Library | Header | Mechanism |
|---|---|---|
| HAL | `Inc/stm32h5xx_hal_conf.h` | include path |
| ThreadX | `Inc/tx_user.h` | `TX_INCLUDE_USER_DEFINE_FILE` |
| NetX Duo | `Inc/nx_user.h` | `NX_INCLUDE_USER_DEFINE_FILE` |
| flexPTP | `Inc/flexptp_options.h` | include path |

In CMake this works because `target_include_directories(... ${CMAKE_SOURCE_DIR}/Inc)` is
applied to each vendored target from the parent scope. Bazel has no parent scope, and an
external repository cannot name a label in the main repository directly. The `label_flag`
indirection solves it:

```python
# bazel/threadx.BUILD
label_flag(
    name = "tx_user",
    build_setting_default = "@flexptp_demo//projects/flexptp_threadx_netx_demo:tx_user",
)
cc_library(
    name = "threadx",
    ...
    deps = [":tx_user"],
    defines = ["TX_INCLUDE_USER_DEFINE_FILE", "TX_SINGLE_MODE_NON_SECURE"],
)
```

**This indirection is what makes D10 reachable.** With one firmware a `label_flag` looks like
ceremony around a constant — the default is the only value. With two firmwares each needing its
own `tx_user.h`/`FreeRTOSConfig.h` and `nx_user.h`/`lwipopts.h`, the flag is the seam a
per-binary transition writes through. Build it properly now rather than hardcoding the paths and
retrofitting later; see [§13.1](#131-it-strengthens-the-case-for-migrating-and-reverses-one-recommendation).

with `//P:tx_user` a `cc_library` exporting just that header. The flag makes the host test
build able to point ThreadX at a different configuration without forking the `BUILD` file —
though note that [`test/CMakeLists.txt`](../projects/flexptp_threadx_netx_demo/test/CMakeLists.txt) deliberately shares the
*firmware's* `tx_user.h` ("sharing the header means a test that passes here is a test of the
kernel the board actually runs"), so the default is what both use, and the flag exists for the
pool sizes that genuinely do differ.

---

## 5. The target graph

Roughly one `cc_library` per current CMake library or `add_subdirectory`, plus a split of the
first-party code that CMake glues into the vendored trees.

### 5.1 Vendored libraries

| Target | Sources | Notes |
|---|---|---|
| `@stm32h5_hal//:hal` | 124 `Src/*.c` | The full list, as [`Drivers/CMakeLists.txt`](../Drivers/CMakeLists.txt) does — modules disabled in `stm32h5xx_hal_conf.h` compile to empty TUs. `copts = ["-w"]`. |
| `@threadx//:threadx` | 185 `common/src/*.c` + 16 `ports/cortex_m33/gnu/src/*` | `glob(exclude = ["*secure_stack*"])`, per `threadx.cmake`. |
| `@threadx//:threadx_linux` | 185 common + 8 `ports/linux/gnu/src/*.c` | Host tests only ([§6](#6-host-tests)). Two **named targets** rather than one target with a `select()` on the port — the ports differ in sources, not just flags, and two names debug better. The *configuration* difference [decisions §5](bazel_decisions.md#5-why-not-imus-embedded_cc_binary) describes (byte-pool sizes) is the `select()` part. |
| `@netxduo//:netxduo` | 511 `common/src/*.c` + `addons/dhcp/nxd_dhcp_client.c` | Addons stay opt-in file by file, per `netxduo.cmake`. |
| `@cmsis_rtos_threadx//:cmsis_rtos_tx` | 1 `.c` | Needs `//third_party/CMSIS:cmsis_os2_hdrs`. |
| `@flexptp//:flexptp_core` | 21 `src/flexptp/*.c` + `servo/kalman_filter.c` + `port/osless/fifo.c` | See [§5.4](#54-flexptp). |
| `@embfmt//:embfmt` | 1 `.c` | |

All get `copts = ["-w"]`, which is what the CMake build effectively does and what keeps the
warnings gate meaningful.

The `startup_stm32h563xx.s` fix imu needs in its `stm32cubeh5` genrule (`sed 's/\.thumb_set /\.set /g'`)
is **not** needed here: this project has its own
[its own `startup/startup_stm32h563zitx.s`](../projects/flexptp_threadx_netx_demo/startup/startup_stm32h563zitx.s), not CubeH5's.

### 5.2 First-party libraries

| Target | Sources |
|---|---|
| `//P:hal_conf`, `:tx_user`, `:nx_user`, `:flexptp_config` | header-only, one per [§4.2](#42-configuration-headers) |
| `//third_party/CMSIS:cmsis` | headers only; `Device/ST/STM32H5xx/Include`, `Include`, `CMSIS_RTOS_V2` |
| `//P/eth:mac_drv` | `mac_drv.c`, `phy_drv/phy_common.c` — always built |
| `//P/eth:eth_drv` | `eth_drv_netxduo.c`, `eth_framer.cpp`, `eth_link_state.cpp` |
| `//P/eth:eth_framer`, `:eth_link_state` | separate, because the host tests link them alone |
| `//P/blocking_io:blocking_io` | `blocking_fifo.c` — in-tree, not a submodule |
| `//P/standard_output`, `//P/cli`, `//P/eth:ethernet` | as the current subdirectories |
| `//P/ptp:port` | `ptp_port_stm32h563.c` |
| `//P/ptp:nsd` | `select()`: `nsd_netxduo.c` + `ptp_tx_timestamp.c` + the four `.cpp` units / `nsd_stub.c` |
| `//P/ptp:ptp_units` | the four `.cpp` units alone, for the host tests |
| `//P/startup:startup` | `startup_stm32h563zitx.s` |
| `//P/startup:tx_low_level` | `tx_initialize_low_level.S` |
| `//P:app` | `main.c`, `syscall.c`, `sysmem.c`, `system_stm32h5xx.c`, `cmds.c`, `cpp_support.cpp` |

Every first-party target carries the `first_party_warnings` feature; no vendored target does.

### 5.3 The firmware binary

```python
stm32_firmware(
    name = "flexptp-demo",
    deps = ["//P:app", "//P/startup:startup", "@flexptp//:flexptp_core", ...],
    linker_script = "//P/startup:stm32h563zitx_FLASH.ld",
)
```

a macro in `tools/build_rules/firmware.bzl` wrapping `cc_binary` plus the post-build steps
that [`CMakeLists.txt`](../CMakeLists.txt) does with `add_custom_command(POST_BUILD)`:
`arm-none-eabi-size`, `objcopy -O ihex`, `objcopy -O binary`. Those are ordinary genrules over
the toolchain's `objcopy`.

`LINK_DEPENDS` has no Bazel equivalent and needs none: the linker script is an
`additional_linker_inputs` entry, so editing it invalidates the link action. The CMake build
needed an explicit property because `-T` on the link line told CMake nothing — and
[`CMakeLists.txt`](../CMakeLists.txt) records that P3.8's section reorder appeared to do
nothing until the ELF was deleted by hand. **That bug is structurally impossible in Bazel.**

### 5.4 flexPTP

flexPTP is the one dependency whose CMake integration does not map one-to-one, because
[`Modules/CMakeLists.txt`](../Modules/CMakeLists.txt) compiles *this repository's* sources
into flexPTP's static library via `FLEXPTP_HWPORT_SRC` and `FLEXPTP_NSD_SRC`. An external
Bazel repository cannot absorb main-repo sources.

The split:

- `@flexptp//:flexptp_core` — the library's own sources, plus a `label_flag` for
  `flexptp_options.h`. Does **not** contain the port or the NSD.
- `//P/ptp:port` and `//P/ptp:nsd` — first-party `cc_library` targets,
  `alwayslink = True`, depending on `@flexptp//:flexptp_core` for its headers.
- The firmware binary depends on all three.

This works because flexPTP reaches the port and the NSD through declared functions resolved at
link time, not through anything requiring one archive. `alwayslink = True` is what stops the
linker discarding objects whose symbols are only referenced from the vendored library.

Two flags to carry over verbatim:

- `__PRI64_PREFIX=__INT64`. flexPTP uses a glibc-internal macro that newlib does not define;
  [`Modules/CMakeLists.txt`](../Modules/CMakeLists.txt) maps it onto newlib's `__INT64` rather
  than hardcoding `"ll"` so it follows the target's `int64_t` (finding F47). Goes in
  `defines` on `@flexptp//:flexptp_core`.
- `-lm`. `target_link_libraries(flexptp m)` — becomes `linkopts = ["-lm"]`, though the
  `--start-group ... -lm ...` block in the toolchain's link flags already covers it.

---

## 6. Host tests

13 test binaries (plus `eth_model.cpp`, a support library), currently a **separate CMake root**
([`test/CMakeLists.txt`](../projects/flexptp_threadx_netx_demo/test/CMakeLists.txt)) because the top-level CMake includes the ARM
toolchain file unconditionally and there is no flag that makes it produce a host binary.

In Bazel this separation disappears: host and target are two platforms in one graph, and
`bazel test //...` builds the host tests while `bazel build --config=target //P:flexptp-demo`
builds for the board. That is a real simplification, and it removes the "second configuration
of the world" hazard that `test/CMakeLists.txt` calls out.

Three tiers, matching the current structure:

**Tier 1 — pure logic, no fakes.** `test_ptp_tx_registry`, `test_ptp_ts_codec`,
`test_ptp_endpoint`, `test_ptp_nsd_lifecycle`, `test_eth_framer`, `test_eth_link_state`,
`test_sysmem_bounds`. These depend on nothing but `stdint`, which is the point of extracting
them. Straight `cc_test` + `@googletest//:gtest_main`. Port these first.

**Tier 2 — the driver against a software DMA model.** `test_host_harness`, `test_rx_ring`,
`test_tx_ring`, via `host_mac_drv` and `eth_model`. Two constraints that must survive:

- Include order: `test/host` must come **first** so its `core_cm33.h`, `system_stm32h5xx.h`
  and `cmsis_compiler.h` shadow ARM's. In Bazel, `includes = ["host"]` on the `host_shims`
  target combined with careful ordering of `deps`. This is the most fragile part of the port —
  Bazel's include path ordering across `deps` is not as directly controllable as CMake's, and
  it may need `copts = ["-Itest/host"]` to be certain.
- `-no-pie` / `-fno-pie`, so `.text` lands low enough that function pointers still fit in the
  `uint32_t` `ETHHW_DescExt.tsCbPtr` field. `features = ["-pie"]` plus explicit
  `copts`/`linkopts`, applied to the test binaries and not just the library — the current
  CMake marks them `PUBLIC` for exactly this reason.

**Tier 3 — real ThreadX on the host.** `test_blocking_fifo`, `test_cli`, `test_rtos_context`.
These link `@threadx//:threadx_linux` and `@cmsis_rtos_threadx`, with
`TX_LINUX_MEMORY_SIZE=4194304` and the larger host byte-pool sizes. `test_rtos_context` enters
a kernel that never returns, so it must not use test sharding or any discovery that runs the
binary to enumerate — the CMake build notes the same about `gtest_discover_tests`.

`gtest_discover_tests` has no Bazel analogue and needs none: `cc_test` runs the binary and
Bazel reads the result.

**The timeout gets 6× looser, and that should be said plainly.** CMake sets `TIMEOUT 10`, and
[`test/CMakeLists.txt`](../projects/flexptp_threadx_netx_demo/test/CMakeLists.txt) records why it is not boilerplate: some defects
these tests pin present as a *hang* rather than a wrong value — reintroducing F57 makes
`ETHHW_ProcessTx()`'s drain loop spin forever. Bazel's `timeout` attribute has fixed buckets and
its smallest, `short`, is **60 s**. So the fail-fast window goes from 10 s to 60 s. A hang still
fails the suite; it just takes 50 s longer to say so. `--test_timeout=10` can tighten it for a
run, but the committed floor is 60 s. Bazel's alternative — no timeout at all — is not
acceptable here.

**Where these live, and the FreeRTOS counterpart.** Under D11 the tests are
`//P/test`, not a root `test/` package. D15 adds a FreeRTOS equivalent of tier 3 in the second
project, and [§13.6](#136-what-gets-harder) is explicit that it is re-derived rather than
translated: only the ISR half of the rule ports across.

`-fanalyzer` on first-party host targets becomes another non-default feature, or simply
`copts`. Keep it non-`-Werror`, for the reason already recorded: an analyzer that blocks the
suite on a false positive is one somebody disables.

---

## 7. What does not translate cleanly

Six things. None is a blocker; all are places where a careless port silently loses a property
this project currently has.

### 7.1 The `.map` file

`$(RULEDIR)` is not available in `linkopts` ([decisions §1.5](bazel_decisions.md#15-two-traps-found-while-prototyping)),
and `cc_binary` cannot declare extra outputs, so `-Wl,-Map=` cannot be expressed.

The fix is a small Starlark rule in `tools/build_rules/firmware.bzl` that performs the link
itself via `cc_common.link()`, declaring both the ELF and the `.map` as outputs and adding
`-Wl,-Map=<declared path>` to `user_link_flags`. This is the same rule that already has to
exist for `.hex`/`.bin`/`size`, so it is not extra machinery — but it must be built, and it is
the one piece of the plan with no working prototype behind it.

Fallback if that proves awkward: keep `cc_binary` and add a genrule that re-links from the
same inputs purely to produce the map. Wasteful and it can drift from the real link; not
recommended, recorded only so the option is on the table.

### 7.2 The build-id check

`-Wl,--build-id=sha1` works through `linkopts` (verified). The property that depends on it —
`osinfo` printing the ID and [`tools/hil/`](../tools/hil/README.md) comparing it against the
ELF, so the hardware suite cannot report a pass for an image it did not test (finding GI-01) —
is unaffected by the build system. It only needs the flag not to be dropped in the port. Worth
an explicit acceptance check in phase 4.

### 7.3 The first-party warnings gate

Today: grep `build.log` for `warning:` and filter to first-party paths (`/projects/` since
phase 0; `/(Src|Inc|Drivers/EthDrv)/` before it).

In Bazel this becomes structural — `-Wall -Wextra` is a feature enabled only on first-party
targets, vendored targets keep `-w` — so the *scoping* no longer needs a regex. But making
warnings **fail** still needs a mechanism, because Bazel does not fail on warnings either.

**D13: `-Werror`, in the `first_party_warnings` feature, on by default rather than CI-only.**

The usual objection to always-on `-Werror` is that a compiler upgrade turns a new diagnostic
into everybody's broken build overnight. **That objection largely does not apply here, and the
reason is this migration.** Today the compiler is whatever `build.sh` last extracted onto
`PATH`; under Bazel it is pinned by sha256 in [§3.1](#31-fetching) and cannot change without a
deliberate commit editing that pin. A new warning therefore arrives *in the commit that bumps
the toolchain*, which is exactly where it should be dealt with — not on an unrelated Tuesday.

That removes the reason to split behaviour between local and CI, so there is one rule rather
than two, and "it passed locally" means the same thing everywhere.

Two consequences worth stating:

- **Scope is first-party only.** The feature is off for every vendored target
  ([§5.1](#51-vendored-libraries) keeps `copts = ["-w"]`), preserving today's deliberate
  distinction: a warning in NetX Duo or the HAL is not this project's to fix, and turning it
  into a build failure would be how the gate gets disabled.
- **There is an escape hatch, and it should be used sparingly.** `--features=-first_party_werror`
  turns it off for a build — for bisecting or a mid-refactor tree — without editing a `BUILD`
  file. It is deliberately a flag rather than a per-target attribute, so it cannot be committed
  by accident.

This is stricter than today, where warnings only fail in CI.

### 7.4 The C++ rules check

[`tools/ci/check_cxx_rules.sh`](../tools/ci/check_cxx_rules.sh) runs `nm` over the linked image
looking for `operator new`, `__cxa_throw` and RTTI vtables. It takes an ELF path and an
optional `nm` path, so it ports as-is — but it should become a `sh_test` with the ELF and the
toolchain's `nm` as data dependencies, rather than a CI step with a hardcoded path into
`toolchains/`. Then it runs on every `bazel test`, not only in CI.

The script's fallback path to `toolchains/arm-gnu-toolchain-15.3.rel1-.../bin/arm-none-eabi-nm`
must be updated or bypassed — under Bazel that directory is not where the compiler lives.

### 7.5 cppcheck

The static-analysis job passes `-I Inc -I Src -I Drivers -I Modules` and a fixed define list.
Nothing about it depends on CMake, so it can stay a plain CI step over the source tree. Making
it a Bazel `sh_test` would be tidier but gains little; not planned.

### 7.6 OpenOCD and flashing

Flashing is a required feature of the Bazel build (D5), and ST's OpenOCD fork is the required
tool (D6). It has its own section — see [§8](#8-flashing).

---

## 8. Flashing

`bazel run //P:flash` must put the firmware on the board. This section is the design.

### 8.1 The flash rule — pure Starlark, no new BCR

imu's `stm32_flash_binary` wraps a Python flasher packaged as a PEX, which costs `rules_python`
*and* `rules_pex`. That layer is not earning its place: `flasher_lib.py` builds an argv and
calls `subprocess.run`, and imu's rule already writes a shell script with `ctx.actions.write()`
anyway. Dropping the Python entirely keeps the BCR footprint in [§1.1](#11-from-bcr-rule-sets-only)
at four modules.

```python
stm32_flash_binary(
    name = "flash",
    elf_image = ":flexptp-demo.elf",
    platform = "//toolchain:cortex_m33",
)
```

The rule:

- carries imu's **platform transition** on `elf_image`, so `bazel run //P:flash` builds the ELF
  for Cortex-M33 whatever `--platforms` is currently set to. This is the one piece of imu's
  design worth copying wholesale — it removes a class of "flashed the host build" mistake.
- takes the OpenOCD binary and its scripts directory as **explicit labels**. imu's version
  recovers them by string-splitting a runfile's `short_path`
  (`repo_base = tcl_file.split("/")[0] + "/" + tcl_file.split("/")[1]`) and its `flash.sh`
  searches the runfiles tree with `find`. Both break quietly when repository-name mangling
  changes; explicit labels do not.
- emits `-s <scripts> -f board/st_nucleo_h5.cfg -c "program <elf> verify reset exit"`, matching
  what [`build.sh`](../build.sh) prints today and what `tools/hil/hil.py` runs.

A sibling `//:debug` target (`-c "init; halt"`, leaving the GDB server up) costs almost nothing
once this exists and replaces a command people currently paste from the README.

### 8.2 Consequences for `build.sh` and the HIL suite

With `source` as the default, `build.sh --with-openocd` becomes redundant: Bazel builds the same
fork, from the same commit, and the `local` mode can still consume the binary it produces for
anyone who prefers it. `build.sh` therefore has no surviving responsibility once CMake is
deleted, which **closes** open decision 4 in [§12](#12-open-decisions) — it can go entirely
rather than being kept alive as a provisioning script.

### 8.3 The HIL suite must stop hardcoding the OpenOCD path

`tools/hil/hil.py` hardcodes

```python
DEFAULT_OPENOCD = REPO_ROOT / "toolchains" / "openocd" / "bin" / "openocd"
```

and `tools/hil/run.py` constructs **two independent** `OpenOcd` instances on that default — one
for `--flash`, one inside `Board(port=args.port)` for `reset()`. `Board.__init__` already takes
an `openocd=` parameter, so the library plumbing exists; only the CLI flag and the default are
wrong. See [`tools/hil/README.md`](../tools/hil/README.md) for what the suite does.

**To be clear about what this is and is not.** It is *not* a GI-01-style hazard.
`check_build_id` compares the ELF's `.note.gnu.build-id` against what `osinfo` prints and fails
whether or not `--flash` was passed, so it already prevents a confident pass for an image that
was never loaded — and it does so regardless of which OpenOCD did the flashing. Flashing with
one ST-fork build and resetting with another is not a correctness problem. There are two other
problems, and the first is a certainty rather than a risk.

**Problem 1: phase 7 breaks the suite outright.** Deleting `build.sh` leaves nothing that
creates `toolchains/openocd/bin/openocd`, and `toolchains/` is gitignored. Every HIL run then
dies on the existence check with

```
OpenOCD not found at .../toolchains/openocd/bin/openocd.
Build it with: ./build.sh --with-openocd
```

— an error naming a script that no longer exists, pointing at a path nothing populates. The
hardware suite stops working at the exact moment the migration completes.

**Problem 2: the acceptance gate depends on an unpinned binary.** Once Bazel pins OpenOCD at
`49ef1d0…`, `toolchains/openocd` stays whatever that developer last built.
`//tools/flash:openocd_capability_test` covers the Bazel-selected binary only; HIL's copy is
verified by nothing.

**The fix, in two parts:**

1. **Add `--openocd` to `run.py`** and thread it into *both* construction sites — the `--flash`
   call and the `Board(...)` call. Missing the second reproduces the mismatch inside a single
   run.
2. **Replace the fixed `DEFAULT_OPENOCD` with a resolver**: `$OPENOCD`, then
   `toolchains/openocd/bin/openocd`, then `PATH`. This is deliberately the same search order
   the `local` repository rule uses in [decisions §6](bazel_decisions.md#6-why-sts-openocd-built-from-source),
   so the two agree by construction, and it is what removes the dead-path failure above.

The Bazel flash target passes `--openocd` explicitly, so Bazel is authoritative when you drive
from Bazel, while HIL still runs standalone — which matters, because it is a bench tool run
repeatedly while chasing a fault, and `bazel run` indirection is friction exactly there.

**Rejected: running HIL under Bazel** (`bazel run //tools/hil:run -- --stage S8`, OpenOCD via
runfiles). It is the tidiest answer on paper and would make problem 2 vanish, but it costs
`rules_python` *plus* a pip dependency for `pyserial` — a larger BCR expansion than the OpenOCD
build this plan already argues for, spent on a smaller problem. Revisit only if HIL grows other
reasons to be a Bazel target.

---

## 9. Phases

Each phase is independently landable and leaves the CMake build working (D3). Phase 0 is a
source move under CMake; phases 1–6 are the first landing (D4); phase 7 removes CMake; phases
8–9 build out the monorepo (D10, D11).

### Phase 0 — Restructure into `projects/`, under CMake

A pure source move, no build-system change: today's firmware becomes
`projects/flexptp_threadx_netx_demo/` in the shape of [§2](#2-repository-layout-after-the-migration),
with `CMakeLists.txt`, the seven nested ones, `Middlewares/*.cmake` and `test/CMakeLists.txt`
updated to the new paths, and the **279** `docs/`→source links fixed. The documents themselves
stay in `docs/` until phase 8 (D18, [§2.1](#21-documentation)).

`Modules/` and `Drivers/stm32h5xx-hal-driver/` stay where they are — phase 2 replaces them with
git repositories, so moving them now creates work that phase 2 discards.

*Accepts when:*
1. `./build.sh` builds Debug and Release from the moved tree.
2. `./build.sh --tests-only` passes.
3. `python3 tools/ci/check_docs_links.py` is clean.
4. Section sizes match the pre-move image, allowing only for `__FILE__`-length changes where
   assertion macros embed the path. This is the check that the move changed paths and nothing
   else.
5. The full 12-check S8 HIL suite passes.

Doing this first is what lets every later `BUILD` file be written once, and removes a confound
from phase 4 — see [§13.7](#137-sequencing).

### Phase 1 — Toolchain

`MODULE.bazel`, `.bazelversion`, `.bazelrc`, `toolchain/`, `bazel/repositories.bzl` with the
`http_archive` only.

Also the constraints and `target_compatible_with` wiring of
[§4.1](#41-platforms-and-constraints), and the API choice in
[decisions §4](bazel_decisions.md#4-which-cc_toolchain-api).

*Accepts when:* a throwaway `cc_library` in the repo compiles for `//toolchain:cortex_m33` and
`readelf -A` reports `8-M.MAIN` / `FPv5/FP-D16` / `VFP registers`; and a `cc_library` marked
`target_compatible_with = ["//toolchain:stm32h563"]` is **skipped**, not failed, by a plain
`bazel build //...` on the host.

The flag content is proven
([decisions §1.4](bazel_decisions.md#14-a-pigweed-free-arm-toolchain-works-and-produces-the-right-code)), but this is not
pure transcription: the prototype used the legacy toolchain API and
[decisions §4](bazel_decisions.md#4-which-cc_toolchain-api) recommends the rule-based one, which is unvalidated.

### Phase 2 — Vendored dependencies as git repositories

The seven `new_git_repository` declarations and their `BUILD` files, with the three flexPTP
patches and the embfmt patch attached. The `label_flag` config injection. lwIP is declared but
gets no `BUILD` file yet (D9).

*Accepts when:* `bazel build @threadx//:threadx @netxduo//:netxduo @stm32h5_hal//:hal
@flexptp//:flexptp_core @cmsis_rtos_threadx//:cmsis_rtos_tx @embfmt//:embfmt --config=target`
succeeds, **and** the patched files in the fetched flexPTP match the patched submodule
byte-for-byte. The second half is the replacement for `--check-patches` and must be run once
explicitly; after that the content-addressed fetch guarantees it.

### Phase 3 — First-party libraries

The `//P/...` packages of [§5.2](#52-first-party-libraries) and `//third_party/CMSIS`.

*Accepts when:* every first-party `cc_library` builds, with
`-Wall -Wextra` on and zero warnings — matching what CI asserts today.

### Phase 4 — The firmware binary

`stm32_firmware()`, the linker script, the post-build outputs.

*Accepts when:*
1. `flexptp-demo.elf` links.
2. `.map`, `.hex`, `.bin` are produced and `size` runs.
3. `check_cxx_rules.sh` passes on the Bazel ELF.
4. The build-id is present.
5. **Section sizes are compared against a CMake image built at a matched optimisation level,
   and any difference is explained.** D8 moved `opt` to `-O2` while CMake `Release` is still
   `-Og`, so comparing the two as they stand measures the flag change, not the build system.
   Build CMake with `-O2` for this comparison, or expect the recorded +56 B. Not
   "byte-identical" either — object order differs between build systems and `__FILE__` paths
   differ, which [`CMakeLists.txt`](../CMakeLists.txt) notes affects image size. A
   `text`/`data`/`bss` delta beyond a few hundred bytes means a flag was lost, and the one most
   likely to be lost silently is `-Wl,--gc-sections`, whose removal grew the Release image from
   167 424 B to 233 924 B (finding H-3).
6. **`//tools/ci:uid_access_test` passes on the `opt` image** — the F62 re-check D8 requires.

**Expect the PTP offset to move, and do not read it as a regression.** D8 ships `-O2` where
CMake shipped `-Og`, and [`CMakeLists.txt`](../CMakeLists.txt) records the hardware figures for
both: `|offset| <= 14 895 ns` at `-O2` against `11 036 ns` at `-Og`. That is a ~35% larger
number from a deliberate flag change, not a defect. Mechanically it trips nothing —
`offset_ns_absmax` has no threshold in `tools/hil/checks.py` and is not among the two keys in
`run.py`'s `TOLERANCE`, so `--compare` checks only PASS→not-PASS — but it is conspicuous in the
output and someone will otherwise spend an afternoon on it.

Note that the *decisive* check on this phase's output — the HIL suite — cannot run until
phase 5 exists. Until then this image has been verified by inspection only, and inspection has
not historically been sufficient on this project.

### Phase 5 — Flashing

[§8](#8-flashing) in full: the `rules_foreign_cc` OpenOCD build pinned at
`49ef1d0107909045e31a728406fcfaeb6f1d5946`, the `local` escape hatch, the pure-Starlark
`stm32_flash_binary` with its platform transition, and the HIL changes in
[§8.3](#83-the-hil-suite-must-stop-hardcoding-the-openocd-path).

*Accepts when:*
1. `bazel test //tools/flash:openocd_capability_test` passes in **both** modes — the built
   binary and a locally provisioned one — reporting `STMicroelectronics`, `stm32h5x` and
   `st-link`.
2. `bazel run //P:flash` programs the board from a clean output base, with no prior `build.sh`
   invocation.
3. `bazel run //P:flash` from an unset or host `--platforms` still flashes a Cortex-M33 image —
   i.e. the transition works and cannot flash a host build.
4. `tools/hil/run.py --openocd <path>` is honoured by **both** the `--flash` path and the
   `reset()` path — verified by pointing it at a deliberately broken binary and confirming
   *both* fail, not just the first.
5. With `toolchains/openocd` **deleted**, `tools/hil/run.py --stage S0` still finds an OpenOCD
   via the resolver. This is the check that phase 7 will not break the suite, and it must be
   run before phase 7, not after.
6. **The full 12-check S8 HIL suite passes on a Bazel-built, Bazel-flashed image.** This is the
   real gate for phases 1–5 together, and §8.2's build-id check is what makes it trustworthy:
   without it the suite can pass against whatever was previously on the board (finding GI-01).

### Phase 6 — Host tests

Tiers 1, 2, 3 from [§6](#6-host-tests), in that order.

*Accepts when:* `bazel test //...` runs all 13 binaries green, and mutation checks still
bite — specifically, reintroducing F57 must make `test_tx_ring` **fail on timeout** rather than
hang the runner, and adding a `std::vector` to an NSD unit must fail the C++ rules test.
A green suite that no longer detects these is not a ported suite.

Independent of phases 4–5 and can be done in parallel by a second pair of hands.

### Phase 7 — Delete CMake

Once the HIL suite has passed on Bazel-built images across a few sessions: delete
`CMakeLists.txt`, the seven nested `CMakeLists.txt` files, `cmake/`, `Middlewares/*.cmake`,
`test/CMakeLists.txt`, `build.sh` (D7), `patches/` and `.gitmodules`, and rewrite the README's
build section. [§8.3](#83-the-hil-suite-must-stop-hardcoding-the-openocd-path) must have landed
in phase 5, or this takes the hardware suite with it.

Also here, not before: `bazel run //:format`, coverage and complexity — the remaining reusable
parts of imu's tooling.

### Phases 8–9 — The second project, then unification

Out of scope for this plan's detail, specified in
[§13.7](#137-sequencing) and [§13.8](#138-unification-afterwards):
**phase 8** adds `projects/flexptp_freertos_lwip_demo/` from `master` @ `5e25b7b` with its own
HIL baseline and CI row, and moves the documents per D18; **phase 9** factors the duplication
out — code and the three hardware/protocol documents alike — a layer at a time, only where a
test pins the behaviour first.

---

## 10. CI

The five jobs in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml) map as follows.

| Current job | After |
|---|---|
| `build` (Debug, Release) | `bazel build --config=dbg` / `--config=opt`. The `set -o pipefail` guard against tee swallowing failures (finding N16) is no longer needed — there is no pipeline. |
| Arm toolchain cache | Bazel repository cache, keyed on `MODULE.bazel.lock`. |
| First-party warnings gate | Retired as a separate step: `-Werror` is on by default in the `first_party_warnings` feature, so the build itself fails (D13, [§7.3](#73-the-first-party-warnings-gate)). |
| `--check-patches` | Retired; replaced by the one-time phase 2 comparison ([§1.3](#13-the-four-patches)). |
| C++ rules check | `bazel test //tools/ci:cxx_rules_test` ([§7.4](#74-the-c-rules-check)). |
| `eth-stack-alternatives` (sed-rewrites `CMakeLists.txt`) | **Retired.** `ETHERLIB` goes with D9 and `NONE` was an S3–S5 bisection aid, not a configuration anyone ships (D17). The job's purpose — proving the stack switch still means something — is served in the monorepo by both projects building. |
| `static-analysis` (cppcheck) | Unchanged. |
| `host-tests` | `bazel test //...`. Tests live in `//P/test`, per D11 — there is no root `test/` package once phase 0 lands. |
| *(new)* | `bazel test //tools/flash:openocd_capability_test` — no board needed, so it gates every push ([decisions §6](bazel_decisions.md#6-why-sts-openocd-built-from-source)). |
| `docs` | Unchanged — but note `check_docs_links.py` requires `submodules: recursive` today; once the submodules are gone, any doc linking into `Modules/` breaks. Audit before phase 7's deletion step. |

During coexistence both build systems run, which roughly doubles CI time. That is the cost of
D3 and it is bounded by the length of the migration.

One new CI cost: the `source` OpenOCD build needs `libusb-1.0-0-dev`, `pkg-config` and
autotools on the runner, and takes several minutes cold. Cache it on `MODULE.bazel.lock` like
the toolchain, and keep it out of the per-push jobs — the capability test above is what needs
to run every push, not a rebuild.

---

## 11. Risks

| Risk | Severity | Mitigation |
|---|---|---|
| A compiler or link flag is silently dropped, changing generated code | **High** | Phase 4 acceptance compares section sizes against CMake at a matched optimisation level and requires the HIL suite. The specific flags with recorded evidence behind them — `-Wl,--gc-sections` (H-3, +66 KB when lost) and the `volatile const uint32_t *` UID access (F62, a HardFault) — are called out in [§3.3](#33-cc_toolchain_configbzl). |
| F62 recurs at `-O2`, the level D8 now ships | Medium | The UID 32-bit-access property was verified by disassembly at `-Og` only; at `-O2` `drv_initialize()` is inlined away. Phase 4 adds the `//tools/ci:uid_access_test` image check ([§3.3](#33-cc_toolchain_configbzl)). |
| Host `as` used instead of the toolchain's | Medium | Found and fixed in the prototype ([decisions §1.5](bazel_decisions.md#15-two-traps-found-while-prototyping)); `arm-none-eabi/bin/**` in the filegroups. Fails loudly. |
| Host test include-order shadowing breaks | Medium | Tier 2/3 depend on `test/host` headers shadowing ARM's. Port tier 1 first; treat tier 2 as the risky one. `test_rtos_context` failing to build is obvious; a *silently* unshadowed `cmsis_compiler.h` would emit `mrs r0, ipsr` and fail to assemble, so this too fails loudly. |
| `.map` rule is more work than estimated | Low | [§7.1](#71-the-map-file) has a documented fallback. |
| OpenOCD source build fails on a host missing autotools or libusb | Medium | The `local` mode is the escape hatch, and the capability test tells you which mode you are on. Prerequisites are listed in [decisions §6](bazel_decisions.md#6-why-sts-openocd-built-from-source); `libusb-1.0` is the only non-obvious one and it is what ST-Link needs. |
| A built OpenOCD silently lacks the adapter or flash driver needed | Medium | Proven possible: libftdi1 and hidapi are absent on this host, so those adapters drop out of the build with no error. `//tools/flash:openocd_capability_test` asserts `stm32h5x` and `st-link` are present rather than trusting a green build. |
| Phase 7 deletes `build.sh` and the HIL suite stops finding OpenOCD | **High**, and certain if unaddressed | Not a risk so much as a scheduled breakage: `DEFAULT_OPENOCD` points into gitignored `toolchains/`, which nothing would populate. Fixed by the resolver in [§8.3](#83-the-hil-suite-must-stop-hardcoding-the-openocd-path), and phase 5 acceptance criterion 5 proves it *before* phase 7 rather than discovering it after. |
| BCR pins drift | Low | `MODULE.bazel.lock` checked in. |
| Bazel 9 churn in `rules_cc` toolchain APIs | Medium | `CcToolchainConfigInfo` and `cc_common` already moved to explicit loads. Mitigated by writing the toolchain against the rule-based `cc/toolchains` API ([decisions §4](bazel_decisions.md#4-which-cc_toolchain-api)) — the same one Pigweed uses — rather than the legacy config the prototype validated. Pin `rules_cc` exactly and treat bumps as deliberate work. |
| The rule-based toolchain API proves awkward in phase 1 | Low | The legacy config is validated and is a drop-in fallback; only the expression of the flags changes, not the flags ([decisions §4](bazel_decisions.md#4-which-cc_toolchain-api)). |
| The two projects' duplicated code drifts before phase 9 unifies it | Medium | Inherent to D11's duplicate-then-unify order, and the divergence table in [§13.2](#132-where-the-freertos--lwip-firmware-comes-from) is what it looks like unwatched. Bounded by seeding the second project from current code rather than `master` ([§13.3](#133-layout-two-independent-projects-first)), and by phase 9 being a scheduled phase rather than an aspiration. |
| Two build systems drift during coexistence | Medium | Source lists must be changed in both. Bounded by keeping the migration short; this is the argument for not choosing "coexist permanently". |

---

## 12. Open decisions

Not needed to start phase 1, but each will come up:

1. ~~**Zero-network hermeticity.**~~ **Closed by D16: option A, lockfile only.** The evidence
   and the four alternatives remain recorded in [decisions §2](bazel_decisions.md#2-why-these-dependencies-from-these-places),
   because A accepts a known exposure rather than eliminating one, and the escape routes should
   stay documented. Residual, and independent of Bazel: forking `epagris/flexPTP` and
   `epagris/embfmt` into an organisation you control addresses the durability risk A accepts,
   and can be done at any time.

2. ~~**`--compilation_mode=opt` → `-Og`.**~~ **Closed by D8: `opt` is `-O2`.** The residual
   work is the F62 disassembly re-check at `-O2`, which
   [§3.3](#33-cc_toolchain_configbzl) specifies and phase 4 gates.

3. ~~**`etherlib` and `lwip` pins.**~~ **Closed by D9: EtherLib goes, lwIP stays.** The
   residual work is deleting the five stranded first-party EtherLib glue files as a separate
   source commit — see [§1.2](#12-from-git-everything-the-project-actually-compiles).

4. ~~**Whether `build.sh` survives.**~~ **Closed: it does not — see D7.** It did three things
   — toolchain provisioning, OpenOCD provisioning, patch verification — and Bazel absorbs all
   three. The one thing that must happen *before* it is deleted is
   [§8.3](#83-the-hil-suite-must-stop-hardcoding-the-openocd-path), because the HIL suite
   currently reaches into the directory `build.sh` populates and its error message names
   `build.sh` as the remedy. Phase 5 acceptance criterion 5 is the proof that phase 7 is safe.

5. ~~**How FreeRTOS is sourced.**~~ **Closed by D14: git, at V11.3.0.**

6. ~~**Whether the FreeRTOS app gets RTOS-context tests.**~~ **Closed by D15: yes**, with the
   important qualification in [§13.6](#136-what-gets-harder) that they are re-derived rather
   than translated.

7. ~~**The internal layout of a project directory.**~~ **Closed by D12**, as proposed in
   [§2](#2-repository-layout-after-the-migration). Phase 0 fixes it; host tests go to
   `//P/test` and no root `test/` package survives.

8. ~~**The first-party warnings gate becomes `-Werror`.**~~ **Closed by D13: yes, and on by
   default rather than CI-only** — the pinned toolchain is what makes always-on safe. See
   [§7.3](#73-the-first-party-warnings-gate).

9. **Whether to revisit D1.** **Deferred, deliberately** — not now, but expected to come back.
   A second firmware *is* arriving (D10), so the duplication between this repo and imu will
   grow rather than shrink, and that is the argument for the third-repo option D1 declined.
   `toolchain/` and `tools/build_rules/` are the directories designed to stay extractable;
   keeping them free of project-specific assumptions is what preserves the option. Natural
   point to reconsider: after phase 9, when unification has shown what is genuinely common.

---

## 13. Target state: the two-firmware monorepo

A stated goal is that this repository eventually carry **both** firmwares — FreeRTOS + lwIP and
ThreadX + NetX Duo — for the same board. This section records what that changes. It is written
as a forward-looking target state, not as work in the first landing (D4).

### 13.1 It strengthens the case for migrating, and reverses one recommendation

**CMake, as this project has it, cannot do this.** The top-level
[`CMakeLists.txt`](../CMakeLists.txt) has a single `add_executable(${CMAKE_PROJECT_NAME})` that
every subdirectory accumulates `target_sources` into, a single global `set(ETH_STACK ...)`, and
an unconditional `include(cmake/gcc-arm-none-eabi.cmake)`. Two firmwares in one tree means two
CMake roots or a substantial refactor. The monorepo goal is therefore an argument *for* this
migration rather than a complication of it.

**But it reverses [§4.1](#41-platforms-and-constraints)'s recommendation to skip imu's
transition machinery.** That recommendation rested on "this repository targets exactly one
part" — true of the *platform*, but the multiplexer's real value is per-target
**configuration**, and with two firmwares that becomes load-bearing:

- `ETH_STACK` as a global `string_flag` + `select()` **cannot build both
  firmwares in one `bazel build //...`**. A global flag has one value per build. Two apps that
  differ in RTOS and stack are not two values of a flag; they are two targets.
- The vendored libraries are configured by headers that now differ per app — `FreeRTOSConfig.h`
  vs `tx_user.h`, `lwipopts.h` vs `nx_user.h`, and two different `flexptp_options.h`. A
  `label_flag` with one `build_setting_default` cannot serve both.

What that requires is *per-project configuration of shared vendored libraries* — but **not
necessarily imu's transition**. [decisions §5](bazel_decisions.md#5-why-not-imus-embedded_cc_binary)
works the dependency graph through and finds that only `@flexptp` genuinely needs two
configurations on the same platform, that FreeRTOS and lwIP need none, and that imu's rule
happens not to cover the one case that matters. The [§4.2](#42-configuration-headers)
`label_flag` pattern is still what makes per-project configuration possible; the mechanism on
top of it is a macro rather than a transition.

`ETH_STACK` itself dissolves, and D17 deletes it outright rather than porting it. Its `netxduo`
and (returning) `lwip` values become *which project you build*; its `none` value was a bisection
aid for stages S3–S5 of the ThreadX port with no role in a two-project world. The ThreadX
project simply depends on NetX Duo.

### 13.2 Where the FreeRTOS + lwIP firmware comes from

**Decided: this repository's `master` branch. imu's app is not used as a source of firmware
code**, though its tooling and Bazel packaging are fair game (D1).

`master` @ `5e25b7b` is exactly the right commit, and better than any point on the ThreadX
line. Verified:

- **It is the fork point.** `git merge-base bazel master` is `5e25b7b`, which is also `master`'s
  HEAD — the ThreadX work branched from it. So "when master was branched" is unambiguous.
- **It is the intact FreeRTOS + lwIP firmware**, with `set(ETH_STACK "LWIP")` and every deleted
  file present: `Inc/FreeRTOSConfig.h`, `Inc/lwipopts.h`, `Modules/lwip_port/`
  (`sys_arch.c`, `arch/cc.h`, `arch/cpu.h`, `arch/sys_arch.h`), `Src/ethernet/ethernet_lwip.c`
  and `Drivers/EthDrv/eth_drv_lwip.c`, plus their headers.
- **It carries the P1–P6 remediation.** Its HEAD is "docs: mark the P1–P5 remediation rows
  verified in the consolidated reviews", so it is the reviewed FreeRTOS firmware, not a
  half-ported intermediate. A commit *inside* the ThreadX branch would already carry S1/S2
  porting work.
- **It shares this project's driver architecture.** `master`'s `mac_drv.c` has 8
  `DMACRDLAR`/`DMACTDLAR` references and 2 `HAL_ETH_` — the same descriptor-level design as the
  current one. This is the property imu's app lacks: its `ethernetif.cpp` is HAL_ETH-based (19
  references), the abstraction [`Middlewares/netxduo.cmake`](../Middlewares/netxduo.cmake) says
  was deliberately bypassed to reach the DMA descriptors hardware timestamping needs. Importing
  it would put two incompatible Ethernet architectures in one repository and share almost
  nothing.

**Why this is more natural than it sounds: the project is CMSIS-RTOS2 throughout.**
[`Middlewares/CMakeLists.txt`](../Middlewares/CMakeLists.txt) records that "the application
never called FreeRTOS in any quantity — it is CMSIS-RTOS2 throughout — so swapping the kernel is
largely a matter of swapping which implementation of that API gets linked". The monorepo is that
observation made permanent: one `cmsis_os2.h`, two implementations —
`@cmsis_rtos_threadx` for one app, and ST's FreeRTOS-backed `cmsis_os2.c` (which `master` still
has in `Drivers/CMSIS/CMSIS_RTOS_V2/`, alongside `freertos_os2.h` and `freertos_mpool.h`) for
the other. `cli.c`, `cmds.c` and `serial_io.c` are shareable *because* of this.

#### The shared code has moved on, and that is the real work

`master` is not a drop-in. The files that **both projects will carry** — and that phase 9 will
eventually unify ([§13.8](#138-unification-afterwards)) — diverged substantially on the ThreadX
line, carrying the port review's fixes:

| Shared file | `master` → `bazel` |
|---|---|
| `Drivers/EthDrv/mac_drv.c` | +99 / −18 |
| `Src/cmds.c` | +338 |
| `Src/sysmem.c` | +114 (plus new `sysmem_bounds.h`) |
| `Src/standard_output/serial_io.c` | +63 / −18 |
| `Src/flexptp_port/ptp_port_stm32h563.c` | +17 / −6 |
| `Src/cliutils/cli.c` | +5 |
| `Src/cpp_support.cpp` | new on the ThreadX line |

**The shared libraries therefore take the `bazel`-line versions**, and the restored app is
adapted to them — not the other way round. Taking `master`'s copies would silently revert
F57, F62, GI-01/GI-02 and the rest of the P1–P6 work.

#### The adaptation is small, and this was measured

The concern is that `eth_drv_lwip.c` and `ethernet_lwip.c` were written against `master`'s
driver and will not compile against the current one. Checking the *API surface* rather than the
implementations:

| Shared header | Change | Breaking? |
|---|---|---|
| `Drivers/EthDrv/mac_drv.h` | +10 / −0, one new function `ETHHW_GetRxCtxCounters()` | **No** — purely additive |
| `Src/cliutils/cli.h` | unchanged | No |
| `Src/flexptp_port/ptp_port_stm32h563.h` | unchanged | No |
| `Modules/blocking_io/blocking_fifo.h` | unchanged | No |
| `Src/standard_output/serial_io.h` | +9 / −1 — `serial_io_init()` signature changed | **Yes**, and it is the only one |

So the restore is a copy plus one known fix at the driver and utility boundaries, not a port.
That materially de-risks phase 9. It says nothing about `main.c` or the FreeRTOS-flavoured half
of `cmds.c`, which are per-app by nature and will need real work.

### 13.3 Layout: two independent projects first

**D11: the two firmwares start out mostly independent, and unification comes afterwards.**

```
projects/flexptp_threadx_netx_demo/     exists today, moved here by phase 0
projects/flexptp_freertos_lwip_demo/    added in phase 8, from master (§14.2)
```

Each carries its own copy of everything it needs. There is **no shared `lib/` at the outset**,
and the duplication that implies is deliberate.

**Why this is right, having initially proposed the opposite.** An earlier draft of this section
proposed factoring `lib/eth`, `lib/ptp`, `lib/cli` and so on up front. That is the wrong order,
for a reason the divergence data in [§13.2](#132-where-the-freertos--lwip-firmware-comes-from)
makes concrete: the shared/per-app boundary is a genuine design question, and it cannot be
answered well with only one application in front of you. Guessing it early means either
over-sharing — pulling ThreadX assumptions into a "shared" library that the FreeRTOS app then
has to fight — or under-sharing and refactoring anyway. Duplicating first and unifying against
two real applications replaces a guess with evidence.

It also decouples the schedule. Two independent projects can be migrated, built and flashed
without either blocking the other, and a mistake in one cannot break the other.

**The cost, stated plainly:** until unification, a fix in genuinely common code must land twice,
and the two copies will drift if that is not watched. This is the standard
duplicate-then-unify trade, and it is only acceptable because [§13.8](#138-unification-afterwards)
is an actual planned phase rather than an aspiration.

#### One thing the duplication must *not* do

The obvious reading of "independent" is that
`projects/flexptp_freertos_lwip_demo/` is populated wholesale from `master`. **It must not be.**
[§13.2](#132-where-the-freertos--lwip-firmware-comes-from) measured what that would cost:
`master`'s copies of the common files are behind by `mac_drv.c` +99/−18, `cmds.c` +338,
`sysmem.c` +114, `serial_io.c` +63/−18 and more — changes that include the F57, F62, GI-01 and
GI-02 fixes and the rest of the P1–P6 remediation.

Seeding the new project from `master` wholesale would ship a second firmware with those fixes
silently reverted, and unification would then have to rediscover them. So:

- **common files** — seeded from the **current** (`bazel`-line) versions;
- **only the FreeRTOS/lwIP-specific glue** comes from `master`: `FreeRTOSConfig.h`,
  `lwipopts.h`, `lwip_port/` (`sys_arch.c`, `arch/cc.h`, `arch/cpu.h`, `arch/sys_arch.h`),
  `ethernet_lwip.c`, `eth_drv_lwip.c`, ST's FreeRTOS-backed `cmsis_os2.c` with `freertos_os2.h`
  and `freertos_mpool.h`, and the lwIP-flavoured `flexptp_options.h`;
- plus the one known fix at the boundary, `serial_io_init()` — the only breaking signature
  change across the shared headers.

"Independent" means *separately buildable*, not *separately stale*.

### 13.4 New dependencies

**FreeRTOS returns, from git (D14).** The CMake build used FreeRTOS-Kernel **V11.3.0** with
`FREERTOS_PORT=GCC_ARM_CM33_NTZ_NONSECURE` and `FREERTOS_HEAP=4`. That pin is deliberate:
`master`'s `Middlewares/CMakeLists.txt` carries it as

```cmake
GIT_TAG V11.3.0 # pinned: the CM33 port API is load-bearing for this project
```

So:

| Repo | Remote | Pin | Tag |
|---|---|---|---|
| `freertos` | `github.com/FreeRTOS/FreeRTOS-Kernel` | `9b777ae5c5b8e9e456065a00294d1e5f5f9facf5` | `V11.3.0` |

`FreeRTOSConfig.h` reaches the kernel through the same `label_flag` pattern as everything else
([§4.2](#42-configuration-headers)).

**lwIP needs its `BUILD` file.** D9 kept the pin and deferred the `BUILD` file; this is when it
lands.

**One thing that is already settled:** `master` and `bazel` pin the *same* lwIP commit
(`3d896ba0`), so unlike the first-party files in
[§13.2](#132-where-the-freertos--lwip-firmware-comes-from) there is nothing to reconcile.

**What the `BUILD` file has to do.** imu's `bazel/lwip.BUILD` is a ~25-line reference: it globs
`src/api`, `src/core`, `src/core/ipv4` and `src/netif`, and injects `lwipopts.h` through an
`lwip_config` `label_flag` — the [§4.2](#42-configuration-headers) pattern again. Two things to
check rather than copy blindly:

- **The IPv6 question.** imu's glob covers `src/core/ipv4` only. Whether this project needs
  `src/core/ipv6` follows from `master`'s `lwipopts.h` (`LWIP_IPV6`), and getting it wrong fails
  at link time rather than silently.
- **The port is ours, not upstream's.** Unlike FreeRTOS, where the Cortex-M33 port ships inside
  the kernel tree, lwIP's port — `sys_arch.c` and `arch/{cc.h,cpu.h,sys_arch.h}` — is
  first-party and comes from `master` @ `5e25b7b`
  ([§13.2](#132-where-the-freertos--lwip-firmware-comes-from)), not from imu, whose port is
  built on a different Ethernet driver architecture.

### 13.5 What does *not* change

Worth stating, because it is most of the plan:

- **The toolchain** ([§3](#3-the-toolchain)). Same MCU, same compiler, same flags.
- **Platforms and constraints** ([§4.1](#41-platforms-and-constraints)). Both apps target
  `//toolchain:cortex_m33`; the `mcu` constraint and `target_compatible_with` work unchanged,
  and the per-binary transition already exists.
- **Flashing** ([§8](#8-flashing)). `stm32_flash_binary` takes an `elf_image` label, so two
  flash targets fall out for free: `bazel run //P:flash` and
  `bazel run //projects/flexptp_freertos_lwip_demo:flash`. No rule change.
- **The vendored git repositories** ([§1.2](#12-from-git-everything-the-project-actually-compiles)),
  the patches, the HAL, flexPTP, ThreadX and NetX Duo.

### 13.6 What gets harder

- **Host tests (D15).** The stack-agnostic ones (`ptp_units`, `eth_framer`, `eth_link_state`,
  `sysmem_bounds`, the ring tests) serve both projects unchanged. The RTOS ones do not, and
  **they must not simply be translated** — reading what
  [`test/test_rtos_context.cpp`](../projects/flexptp_threadx_netx_demo/test/test_rtos_context.cpp) actually asserts, its four
  context tests pin *two different rules from two different layers*:

  1. **The ISR half** — ST's CMSIS-RTOS2 wrapper rejecting a blocking call from interrupt
     context (`if ((IS_IRQ_MODE() && (timeout != 0)) || ...) status = osErrorParameter;`). This
     is the wrapper's rule, not the kernel's, and the FreeRTOS project uses ST's
     *FreeRTOS-backed* `cmsis_os2.c` from the same family. **This half ports directly**, and it
     is the one guarding why `Src/flexptp_nsd/ptp_tx_timestamp.c` exists at all.
  2. **The timer-thread half** — ThreadX's *own* restriction, `txe_queue_send.c` returning
     `TX_WAIT_ERROR` when the caller is `&_tx_timer_thread`. **This half has no FreeRTOS
     analogue**: FreeRTOS's timer service task is an ordinary task that may block, so F80's
     failure mode does not exist there. Translating the test would assert a rule FreeRTOS does
     not have.

  So the FreeRTOS project gets context tests built on `portable/ThirdParty/GCC/Posix`
  (confirmed present at V11.3.0), pinning **FreeRTOS's** rules — the `configASSERT` on
  non-`FromISR` API use from an ISR, and the `xTimerPendFunctionCall` boundary — plus the ISR
  half above verbatim. The test file should record that F80 has no FreeRTOS counterpart, because
  "we ported the tests and they pass" would otherwise imply a guarantee that was never checked.
- **HIL.** [`tools/hil/baseline.json`](../tools/hil/README.md) is one baseline, and `checks.py`
  asserts firmware-specific facts — the registered CLI command count is asserted *exactly* (25),
  and it would differ between apps. Expect a baseline per app and a way to say which app a run
  is testing. The `build_id` check already keys on the ELF, so it needs nothing.
- **CI.** The matrix becomes per-app. The payoff is that `bazel build //...` builds both
  firmwares in one invocation, which is the thing CMake cannot do today.

### 13.7 Sequencing

The restructure comes **first**, not last (D11). Full order, with the phase numbers used in
[§9](#9-phases):

| Phase | What | Verified by |
|---|---|---|
| **0** | Move today's firmware into `projects/flexptp_threadx_netx_demo/`, **under CMake** | CMake still builds; HIL S8 passes; sections match modulo `__FILE__` |
| 1–6 | The Bazel migration, writing `BUILD` files into their final home | as [§9](#9-phases) |
| 7 | Delete CMake and `build.sh` (D3, D7) | |
| **8** | Add `projects/flexptp_freertos_lwip_demo/` from `master`, plus the FreeRTOS and lwIP `BUILD` files and per-project configuration; **and move the documents into `projects/<name>/docs/`** (D18) | its own HIL baseline; `check_docs_links.py` clean |
| **9** | Unification ([§13.8](#138-unification-afterwards)), incrementally | both firmwares keep passing |

**Why phase 0 goes first rather than last.** An earlier draft put the reorganisation after the
migration. Moving it to the front is strictly better:

- **`BUILD` files get written once.** Migrating the flat tree and then moving it means rewriting
  every label and every `BUILD` file — the migration's entire output — for no gain.
- **It removes a confound from phase 4.** Moving files changes `__FILE__`, which
  [`CMakeLists.txt`](../CMakeLists.txt) notes affects image size where assertion macros embed
  it. Doing the move first means phase 4's section-size comparison is Bazel-vs-CMake *in the
  same layout*, measuring only the build system.
- **It is verifiable on its own terms.** A pure `git mv` plus path updates, with CMake and the
  HIL suite as the gate. "Same firmware, new paths" is a claim that can be checked before any
  build-system change muddies it.

**The one real cost:** phase 0 must update the CMake files it is about to obsolete, because D3
requires CMake to keep working through coexistence. That is mechanical path editing in
`CMakeLists.txt`, the seven nested ones, `Middlewares/*.cmake` and `test/CMakeLists.txt`.

**And a docs sweep.** 279 relative links across 15 Markdown files point into `Src/`, `Inc/`,
`Drivers/`, `Startup/`, `Modules/` and `test/`. Every one breaks on the move. This is tedious
rather than risky: [`tools/ci/check_docs_links.py`](../tools/ci/check_docs_links.py) finds all
of them and the `docs` CI job fails until they are fixed. Note that `docs/reviews/` is an
archive of reviews written against the old paths — its `REMOVED_BY_THREADX_PORT` allow-list in
the checker is the existing precedent for how this project handles historical paths, and the
same judgement applies: rewriting an archive to hide a move falsifies the record.

### 13.8 Unification, afterwards

Once both projects build and both pass HIL, factor the common code out — driven by observed
duplication rather than by prediction. Rough order, easiest and safest first:

1. **The pure-logic units.** `ptp_endpoint`, `ptp_ts_codec`, `ptp_tx_registry`,
   `ptp_nsd_lifecycle`, `eth_framer`, `eth_link_state`, `blocking_fifo`, `sysmem_bounds.h`.
   They depend on nothing but `stdint` — that is precisely why they were extracted — and the
   host tests already pin their behaviour. Near-zero risk.
2. **The hardware layer.** `mac_drv.c` + `phy_drv/`, and `ptp_port_stm32h563.c`. Same silicon,
   same registers, and `mac_drv.h` is stack-agnostic by design. Covered by the ring/harness host
   tests plus HIL.
3. **The CMSIS-RTOS2 layer.** `cli.c`, `serial_io.c`, `standard_output/`, and the shared half of
   `cmds.c`. These are portable *because* the project is CMSIS-RTOS2 throughout
   ([§13.2](#132-where-the-freertos--lwip-firmware-comes-from)) — one API, two implementations
   underneath. Note `cmds.c` is not wholly common: `ethinfo`/`ethstat` and any stack-specific
   command stay per-project.
4. **The runtime odds and ends.** `syscall.c`, `sysmem.c`, `cpp_support.cpp`,
   `system_stm32h5xx.c`, `startup_stm32h563zitx.s` and the linker script.
5. **Judgement calls, deliberately last.** `stm32h5xx_hal_conf.h` carries `HAL_ETH_USE_PTP` and
   `ETH_RX_DESC_CNT`; share it only if both projects genuinely want the same values, otherwise
   leave it per-project behind a `label_flag` like the others
   ([§4.2](#42-configuration-headers)).

**Never unified:** `main.c`, the config headers, `ethernet_*.c`, `eth_drv_*.c`, `nsd_*.c`, and
the RTOS/stack `BUILD` wiring. Those *are* the difference between the two projects.

**The rule that keeps this honest:** unify a file only when a test pins its behaviour first. The
host suite covers tier 1 and much of tier 2; HIL covers the rest. A file with no test over it is
one where "the two copies were identical" is an assertion nobody checked — and the divergence
table in [§13.2](#132-where-the-freertos--lwip-firmware-comes-from) is what that looks like when
it goes unwatched.
