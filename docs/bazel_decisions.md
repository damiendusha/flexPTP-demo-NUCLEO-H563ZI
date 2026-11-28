# Bazel migration — decisions and rationale

Companion to [bazel_migration.md](bazel_migration.md). That document says **what to build and in
what order**; this one records **why**, and the evidence behind it.

The split exists because the two age differently. The plan churns while it is executed — phases
get reordered, acceptance criteria tighten, labels move. The rationale does not: an experiment
that was run, or an alternative that was weighed and rejected, stays true. Keeping them apart
means the plan can be edited freely without disturbing the record, and the record can be read
without wading through implementation detail.

Decisions are numbered D1–D18 and the table lives in
[bazel_migration.md §0](bazel_migration.md#0-decisions-taken). Sections here are cross-referenced
from it.

The migration is complete. This record was written while it ran, so its present tense — "`build.sh`
pins…", "as `Modules/CMakeLists.txt` does today" — describes the CMake build being replaced, and
those references are kept as written: they are the evidence the decisions were taken against.

## Contents

- [1. What was verified, and what it costs](#1-what-was-verified-and-what-it-costs)
- [2. Why these dependencies, from these places](#2-why-these-dependencies-from-these-places)
  - [2.1 Pinning is not availability](#21-pinning-is-not-availability) · [2.2 Not BCR's FreeRTOS](#22-why-not-bcrs-freertos-module) · [2.3 Not BCR's lwIP](#23-why-not-bcrs-lwip-module)
- [3. Why not Pigweed](#3-why-not-pigweed)
- [4. Which `cc_toolchain` API](#4-which-cc_toolchain-api)
- [5. Why not imu's `embedded_cc_binary`](#5-why-not-imus-embedded_cc_binary)
- [6. Why ST's OpenOCD, built from source](#6-why-sts-openocd-built-from-source)
- [7. Alternatives rejected, in one place](#7-alternatives-rejected-in-one-place)

---

## 1. What was verified, and what it costs

The following were established by building a throwaway Bazel module against this repository's
already-downloaded ARM toolchain. They are facts about this machine and this toolchain, not
projections, and several of them change the plan.

### 1.1 Bazel 9.2 has removed the native C++ rules

The installed Bazel is **9.2.0**. A `BUILD` file containing a bare `cc_library` fails:

```
This rule has been removed from Bazel. Please add a `load()` statement for it.
```

So `load("@rules_cc//cc:defs.bzl", "cc_library")` is mandatory in every `BUILD` file, and
`rules_cc` is a hard dependency. imu pins Bazel **8.0.0** in its `.bazelversion`, where the
native rules still autoload — so imu's `BUILD` files are not a syntactic template for this
repo unless this repo also pins 8.x. **Recommendation: pin `9.2.0`** and write the `load()`
statements. They are required on 8.x too, just not yet enforced.

### 1.2 A fully BCR-free build is not possible on Bazel 9

This was tested directly, by pointing `--registry` at a non-existent path:

```
ERROR: Error computing the main repository mapping: in module dependency chain
<root> -> bazel_tools@_ -> rules_java@9.1.0: module rules_java@9.1.0 not found in registries
```

`@bazel_tools` is built into Bazel and itself declares a `bazel_dep` on `rules_java`, which
must be resolved from *some* registry. A trivial `cc_library` resolves a module graph
including `rules_java`, `protobuf`, `rules_python`, `rules_kotlin`, `rules_jvm_external`,
`stardoc` and others.

The cost is smaller than it looks: resolution reads `MODULE.bazel` metadata, it does not fetch
those repositories. After a full build of the prototype, `external/` held **760 KB**, almost
all of it lazy marker files. Nothing Java was downloaded.

The practical consequence is that "avoid BCR" has to mean *avoid taking project dependencies
from BCR*, which is achievable and is what [plan §2](bazel_migration.md#1-dependency-policy) specifies. If genuine
zero-network hermeticity is wanted later, the mechanism is a vendored registry or
`bazel vendor --vendor_dir`, not a smaller `MODULE.bazel` — see
[the plan's open decisions](bazel_migration.md#12-open-decisions).

### 1.3 `git_override` does bypass BCR for the module it names

Verified: with a `git_override` on `rules_cc`, Bazel reported

```
INFO: Repo rules_cc+ defined by rule git_repository in @@bazel_tools//tools/build_defs/repo:git.bzl
```

and cloned from GitHub rather than consulting the registry. Two details matter:

- `git_override` takes **`commit`**, not `tag`. A tag name is passed to `git reset --hard` and
  fails with `unknown revision`. Every pin must be a full SHA.
- The overridden module's *transitive* `bazel_dep`s still resolve through the registry unless
  they are overridden too.

### 1.4 A Pigweed-free ARM toolchain works, and produces the right code

A `cc_toolchain` built from `toolchains/arm-gnu-toolchain-15.3.rel1-x86_64-arm-none-eabi.tar.xz`
compiled and linked a Cortex-M33 ELF. The object was checked with the vendor's own `readelf`:

```
architecture: armv8-m.main
Tag_CPU_name: "8-M.MAIN"
Tag_FP_arch: FPv5/FP-D16 for ARMv8
Tag_ABI_VFP_args: VFP registers
```

That is exactly what `cpu_PARAMS` in [`CMakeLists.txt`](../CMakeLists.txt) asks for
(`-mthumb -mcpu=cortex-m33 -mfpu=fpv5-sp-d16 -mfloat-abi=hard`); the corresponding multilib is
`thumb/v8-m.main+fp/hard`. Linking through `linkopts` also worked, including
`-Wl,--print-memory-usage` (which printed the FLASH/RAM table) and `-Wl,--build-id=sha1`,
both of which this project depends on.

The SHA-256 of the toolchain archive already in `toolchains/` is:

```
563bebb2b97d53382b956d6ee1fe61e2cae26699901417234a37df505ef9b5fa
```

### 1.5 Two traps found while prototyping

**The assembler must be in the toolchain filegroups.** With only `bin/**` and `libexec/**`
globbed, `arm-none-eabi-gcc` could not find its own `as` inside the sandbox and fell through
to the host assembler:

```
Assembler messages: Fatal error: invalid -march= option: `armv8-m.main+dsp+fp'
```

The fix is to include `arm-none-eabi/bin/**` — GCC's exec prefix — in both `compiler_files`
and `linker_files`. This failed loudly here. It is recorded because the same class of mistake
(a tool resolved from the host rather than the sandbox) is the one that does *not* always fail
loudly, and this project has a startup file, a linker script and an assembly-language RTOS
port that all pass through the assembler.

**`$(RULEDIR)` is not available in `linkopts`.** Attempting `-Wl,-Map=$(RULEDIR)/x.map` fails
at analysis time with `$(RULEDIR) not defined`. `cc_binary` has no way to declare the `.map`
file as an output, so `-Wl,-Map=` cannot be expressed through `linkopts` at all. The `.map` is
not decorative here — CI uploads it as an artifact and it is the tool for reading the memory
layout the linker script produces. See [plan §8.1](bazel_migration.md#71-the-map-file) for the fix.

---

---

## 2. Why these dependencies, from these places

D2 takes rule sets from BCR and every project dependency from git; D16 accepts BCR's
availability rather than mirroring it; D14 and D9 pick specific sources for FreeRTOS and lwIP.
The evidence for each is below.

### 2.1 Pinning is not availability

This is the substance behind open decision 1 in [the plan's open decisions](bazel_migration.md#12-open-decisions), and the mechanisms
were tested rather than assumed.

**What the plan already guarantees.** Every artifact is pinned by content: `sha256` on the
toolchain archive ([the plan's §3.1](bazel_migration.md#31-fetching)), full commit SHAs on every git repository
([the plan's §1.2](bazel_migration.md#12-from-git-everything-the-project-actually-compiles), and `git_override` accepts
nothing else — [§1.3](#13-git_override-does-bypass-bcr-for-the-module-it-names)), and
`MODULE.bazel.lock` carrying **163 `registryFileHashes`** entries. A substituted or tampered
artifact fails the build. **Supply-chain integrity is solved.**

**What it does not guarantee is that any of it can still be fetched.** Three results, each from
a direct experiment:

1. **A checked-in lockfile does not give offline resolution.** With a complete
   `MODULE.bazel.lock`, `--lockfile_mode=error`, an empty repository cache and
   `--registry=file:///nonexistent`:

   ```
   ERROR: ... <root> -> bazel_tools@_ -> buildozer@8.5.1:
   module buildozer@8.5.1 not found in registries
   ```

   The lockfile stores registry file *hashes*, not their *contents*, so a cold cache must still
   fetch all 163 `MODULE.bazel` files to verify them against it. The lockfile pins **what** and
   **integrity**; it does nothing for **availability**.

2. **`bazel vendor` alone does not either.** It vendors repository *content*, but module
   resolution still needs a registry:

   ```
   $ bazel build //:a --vendor_dir=... --registry=file:///nonexistent
   ERROR: ... <root> -> rules_cc@_ -> protobuf@27.0: module protobuf@27.0 not found
   ```

   A network-free build therefore needs **two** mechanisms — a vendor directory *and* a local
   registry — not one. This is the non-obvious part.

3. **Vendoring is expensive and mostly wasted.** `bazel vendor` on a *trivial* `cc_library`
   produced **95 MB**, dominated by `protobuf+` (51 MB) and `rules_kotlin+` (25 MB) — pulled in
   through `rules_cc` and `bazel_tools`, and irrelevant to compiling C for a microcontroller.
   For this project add ThreadX, NetX Duo, the HAL, OpenOCD's tree and a 153 MB toolchain.

**The options, therefore:**

| Option | Buys | Costs |
|---|---|---|
| **A. Lockfile only** (the plan's default) | Integrity; reproducible *versions* | Cold builds need BCR + GitHub + gitlab.arm.com |
| **B. + a checked-in local registry mirror** | Removes BCR as a runtime dependency | ~KB in-tree; a refresh step when deps change |
| **C. + a pre-populated `--repository_cache`** on CI and dev machines | Offline after first fetch | Not durable against machine loss; not in-tree |
| **D. Full `bazel vendor` + local registry, committed** | True airgap capability | 95 MB *before* this project's own dependencies; a firmware repo carrying 51 MB of protobuf |
| **E. Internal artifact mirror** + `--experimental_downloader_config` rewriting | The durable answer | Requires infrastructure |

**Chosen: A (D16).** Integrity is pinned and reproducibility of *versions* is guaranteed; the
availability of BCR, GitHub and gitlab.arm.com is accepted as a dependency. What that means in
practice:

- A cold clone needs network. CI mitigates this by caching the Bazel repository cache keyed on
  `MODULE.bazel.lock` ([plan §11](bazel_migration.md#10-ci)) — the same role `actions/cache` plays for the Arm
  toolchain today.
- A BCR outage blocks a cold build, and a warm one is unaffected. This is the same exposure the
  current CMake build already has to `gitlab.arm.com` and GitHub, so it is not a regression.
- **It is cheap to reverse.** B and C are additive: a `--registry` flag in `.bazelrc` plus a
  directory, and a cache path. Neither changes `MODULE.bazel`, any `BUILD` file, or the layout.
  Nothing about choosing A now forecloses choosing B, C or E later, which is what makes it a
  low-regret default rather than a bet.

**One targeted risk worth separating from all of this.** `epagris/flexPTP` and
`epagris/embfmt` are single-maintainer GitHub repositories that this firmware genuinely depends
on, and no amount of registry mirroring protects against one being deleted or force-pushed.
Forking both into an organisation you control is a few minutes' work and addresses more real
durability risk than options B–E combined. It is orthogonal to the Bazel decision and can be
done today. (EtherLib on `gitea.epagris.com` was the sharpest instance of this; D9 removed it
for unrelated reasons.)

### 2.2 Why not BCR's `freertos` module

Re-examined against the registry rather than from memory, because it is the one project
dependency with a plausible BCR case.

**The decisive fact: BCR does not have V11.3.0.** The `freertos` module has nine versions,
topping out at **`11.1.0.bcr.3`**. Choosing BCR therefore means moving the kernel from 11.3.0 to
11.1.0 — and the pin comment above says the CM33 port API is precisely the part this project
depends on. The downgrade would land on the sensitive area, not away from it.

**What BCR would otherwise give.** Its module is an *overlay*: the upstream release zip plus a
510-line `BUILD.bazel` supplying 56 targets — eight ports (CM0/CM3/CM4F/CM7/CM33_NTZ/Xtensa/
Posix/RP2040), five heaps, a `freertos_config` `label_flag`, and a `port_ARM_CM33_NTZ`
constraint value. That is genuinely useful work, and it includes the Posix port D15 needs.

**But less of it is load-bearing than it looks.**

- The overlay is maintained by a **single named BCR maintainer**, not by the FreeRTOS project.
  "Someone else maintains it" therefore means the same `BUILD` file we would write, written by
  a stranger — actively, on the evidence of nine versions and three `.bcr` revisions, but not
  upstream-official.
- Its `port_ARM_CM33_NTZ` constraint and `heap_N` targets exist to feed macros like imu's
  `freertos_cc_binary`. [plan §5.1](bazel_migration.md#41-platforms-and-constraints) declined that multiplexer, and
  this project already defines its own `mcu` `constraint_setting`, so the constraint plumbing is
  not a gap being filled.
- We need **two** of its eight ports and **one** of its five heaps.

**What writing it ourselves actually costs.** At V11.3.0 the required sources are: the six core
translation units (`tasks.c`, `queue.c`, `list.c`, `timers.c`, `event_groups.c`,
`stream_buffer.c`; `croutine.c` is optional), `portable/GCC/ARM_CM33_NTZ/non_secure/{port.c,
portasm.c}`, `portable/ThirdParty/GCC/Posix/{port.c, utils/}` for D15, `portable/MemMang/heap_4.c`
and `include/`. That is a `BUILD` file of roughly 60–80 lines — **smaller than the HAL,
ThreadX or NetX Duo files this plan already commits to writing**, and it covers exactly what is
used instead of eight ports that are not.

**The honest counterargument.** Phase 8 gives the FreeRTOS firmware its own HIL baseline, so it
is re-validated from scratch either way; "matching the validated version" is worth less than it
would be if a known-good binary were being preserved. The argument that survives is narrower and
still holds: starting from the kernel version the firmware was built against means a HIL failure
during bring-up points at *our* port rather than at a kernel delta. Fewer variables while
debugging is the benefit, not a guarantee of correctness.

**Middle path, if BCR's work is wanted without the version regression:** use the overlay
`BUILD.bazel` as a *reference* while writing `bazel/freertos.BUILD`. It is a reviewed, working
packaging of this exact kernel, and reading it costs nothing — the same relationship this plan
already has with Pigweed's `arm_gcc` toolchain ([§4.6](#4-which-cc_toolchain-api)).

### 2.3 Why not BCR's lwIP module

**There isn't one.** `https://bcr.bazel.build/modules/lwip/metadata.json` returns 404, as do
`lwip-tcpip` and `liblwip`, and no lwIP-shaped module appears in the registry's module list. So
unlike FreeRTOS ([above](bazel_migration.md#134-new-dependencies)) there is no trade-off to weigh — git is the
only option, and D2 holds without needing an argument.

**And even a hypothetical BCR module would be the wrong thing**, for the same reason it is the
wrong thing for FreeRTOS: a registry module packages *releases*, and this project's pin is
deliberately ahead of one. `3d896ba0` is **34 commits past `STABLE-2_2_1_RELEASE`**, which is
lwIP's newest release, and the tip commit is:

```
3d896ba0 tcp_in: Add bounds check for next option byte
```

with `nd6` option-parsing fixes, an mDNS invalid-domain fix and an SNMPv3 invalid-length fix in
the same range. That is not drift — it is the same judgement
[`Middlewares/netxduo.cmake`](../Middlewares/netxduo.cmake) records for rejecting ST's NetX Duo
fork, which was pinned at a release missing GHSA-tagged bounds checks on the IPv4 receive path.
Moving back to the 2.2.1 tag to satisfy a registry would drop those fixes from a stack that
parses untrusted frames.

---

## 3. Why not Pigweed

The brief said no Pigweed, and this plan complies. But the case against it is weaker than it
looks from the outside, and two common objections turn out to be **false**. Both were checked
against Pigweed's actual source in imu's Bazel cache rather than assumed.

**False objection 1: "Pigweed's Cortex-M toolchain is clang, so all our GCC measurements would
be invalidated."** It is not. `pw_toolchain/BUILD.bazel` resolves
`cc_toolchain_cortex-m33` through:

```python
alias(
    name = "cortex-m_cc_toolchain",
    actual = select({
        ":use_clang_for_cortex-m": "//pw_toolchain/arm_clang:arm_clang_toolchain_cortex-m",
        "//conditions:default":     "//pw_toolchain/arm_gcc:arm_gcc_toolchain_cortex-m",
    }),
)
```

**arm_gcc is the default**; clang is opt-in behind a flag.

**False objection 2: "It would be a different compiler from ours."** Pigweed fetches
`gcc_arm_none_eabi_toolchain`, and the binary in imu's cache reports:

```
arm-none-eabi-gcc (Arm GNU Toolchain 15.2.Rel1 (Build arm-15.86)) 15.2.1 20251203
```

against our `15.3.rel1`. Same vendor, same newlib, one point release apart. Adopting Pigweed
would not have meant a different compiler family, a different libc, or a rewrite of
`--specs=nosys.specs`.

**What Pigweed would genuinely buy:**

- A maintained Cortex-M toolchain covering m0/m0plus/m33/m55 etc., so a second MCU is nearly free.
- A ready-made ARM constraint hierarchy — the thing [plan §5.1](bazel_migration.md#41-platforms-and-constraints)
  hand-rolls in four lines.
- **It is already written against the rule-based `cc/toolchains` API** (`cc_args`, `cc_tool_map`
  — see [§4.6](#4-which-cc_toolchain-api)), so somebody else absorbs the churn that this plan's
  risk table rates Medium.

**What it would cost:**

- Size. Pigweed is a large module with a large transitive graph, taken on to use one toolchain
  for one MCU. imu sources it by `git_override` at a pinned commit, so this is not strictly a
  *BCR* cost — but it is a maintenance cost, because Pigweed moves fast and a pinned commit
  rots.
- Loss of control over the exact compiler. This project's recorded measurements — the
  `-Og`/`-O2`/`-Os` flash figures, the F62 UID disassembly — were taken with a specific
  toolchain that `build.sh` pins. Taking Pigweed's means the compiler version becomes
  Pigweed's choice, moving whenever the pin moves.
- Opinionated defaults to fight. The link line here is not generic: `--specs=nosys.specs`, the
  `-Wl,--start-group -lc -lm -lstdc++ -lsupc++ -Wl,--end-group` block, `-Wl,-z,max-page-size=8`,
  `-Wl,--build-id=sha1`. Layering these onto someone else's toolchain is not obviously less work
  than declaring them ([plan §4.3](bazel_migration.md#33-cc_toolchain_configbzl)).
- It solves none of the parts that actually cost effort. Pigweed does not produce the `.map`
  ([plan §8.1](bazel_migration.md#71-the-map-file)), the `.hex`/`.bin`/size steps, the linker-script wiring, or the
  configuration-header injection ([plan §5.2](bazel_migration.md#42-configuration-headers)).

**Verdict: hand-rolled, but for one reason rather than four.** The decisive argument is control
over the exact compiler the hardware evidence was gathered with — not "Pigweed is clang" and not
"Pigweed is BCR bloat", both of which are wrong. The honest counterweight is that Pigweed is on
the modern toolchain API and maintained, which is a real advantage this plan gives up and must
therefore pay for itself, in [§4.6](#4-which-cc_toolchain-api).

Reconsider if a second MCU appears. At that point Pigweed's multi-core coverage and constraint
hierarchy start earning their keep, and the cost stays flat.

---

## 4. Which `cc_toolchain` API

`rules_cc` 0.2.22 ships **two** ways to define a toolchain, and the choice matters more than it
appears:

1. **Legacy** — a Starlark rule returning `cc_common.create_cc_toolchain_config_info()` with
   hand-built `feature`/`flag_set`/`flag_group` structs. This is what nearly every tutorial
   shows, and **what the prototype in [§1.4](#14-a-pigweed-free-arm-toolchain-works-and-produces-the-right-code)
   used and verified working.**
2. **Rule-based** — `@rules_cc//cc/toolchains:{args,tool,tool_map,feature}.bzl`, where flags are
   `cc_args` targets composed declaratively. Present in `cc/toolchains/` alongside the legacy
   entry points, and **what Pigweed's `arm_gcc` toolchain is written against.**

**Recommendation: write it rule-based from the start**, using Pigweed's `pw_toolchain/arm_gcc`
as the reference implementation — it is a working example of precisely our case, arm-none-eabi
GCC 15.x targeting Cortex-M, and it can be read without depending on it. Rewriting a toolchain
after the fact is worse than writing it once, and the rule-based API is where `rules_cc` is
going; this is the mitigation that makes [§4.5](#3-why-not-pigweed)'s verdict pay for itself.

**Honest caveat:** the prototype validated the *legacy* path end-to-end (compile, link,
`readelf` confirmation). The rule-based port is **not** yet validated, and it is the one part of
phase 1 that is transcription plus genuine work. If it proves awkward, the legacy config is a
proven fallback and the flag content in [plan §4.3](bazel_migration.md#33-cc_toolchain_configbzl) is identical either
way — only its expression changes.

---

## 5. Why not imu's `embedded_cc_binary`

The question is not "are there two firmwares?" but "does any single vendored target need to
exist in two configurations *within one build*?"

[plan §14.1](bazel_migration.md#131-it-strengthens-the-case-for-migrating-and-reverses-one-recommendation) originally
claimed that imu's `app_config_transition` "becomes the mechanism" once there are two firmwares.
**That was wrong**, and checking imu's rule against this project's actual dependency graph is
what shows it. The question that decides whether a transition is needed is not "are there two
firmwares?" but "does any single vendored target need to exist in two configurations *within one
build*?"

| Vendored library | ThreadX project | FreeRTOS project | Host tests | Configurations |
|---|---|---|---|---|
| `@threadx` | ✓ `tx_user.h` | — | ✓ Linux port, larger pools | 2, **by platform** |
| `@cmsis_rtos_threadx` | ✓ | — | ✓ larger pools | 2, **by platform** |
| `@netxduo` | ✓ `nx_user.h` | — | — | 1 |
| `@freertos` | — | ✓ `FreeRTOSConfig.h` | ✓ Posix port (D15) | 2, **by platform** |
| `@lwip` | — | ✓ `lwipopts.h` | — | 1 |
| `@embfmt` | ✓ | ✓ | — | 1 (no config) |
| `@stm32h5_hal` | ✓ | ✓ | headers only | 1, possibly 2 |
| `@flexptp` | ✓ | ✓ | — | **2, by project** |

Two conclusions follow, and they point away from transitions.

**1. FreeRTOS and lwIP need no transition at all.** This is the case that prompted the question,
and it dissolves: under D11 the two projects are independent, so `@freertos` and `@lwip` have
exactly **one** firmware consumer each — the FreeRTOS project. Nothing else in the build wants a
different `FreeRTOSConfig.h` or `lwipopts.h` on the target. The `label_flag` can simply default
to that project's target and be depended on normally.

Where `@freertos` does need a second configuration is the **host** — D15's Posix-port context
tests, which want a different `FreeRTOSConfig.h` and a different port, exactly as
[`test/CMakeLists.txt`](../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/test/CMakeLists.txt) already gives ThreadX different byte-pool sizes
on the host. That divergence is driven by *platform*, so it is a `select()`:

```python
deps = select({
    "//toolchain:cortex_m33_cfg": [":freertos_config_target"],
    "//conditions:default":       [":freertos_config_host"],
})
```

No transition, no `_allowlist_function_transition`, and error messages that name a real target.

**2. The one library that genuinely needs a transition-or-equivalent is `@flexptp`** — both
projects use it, both on the same platform, with different `flexptp_options.h` (different NSD,
different network stack). A `select()` cannot separate them because the platform is identical.

**And that is precisely the case imu's rule does not handle.** Its `app_config_transition`
transitions exactly three settings — `@lwip//:lwip_config`, `@freertos//:freertos_config` and
`@freertos//:heap` — and the string `flexptp` does not appear in
`tools/build_rules/embedded.bzl` at all. imu's `bazel/flexptp.BUILD` gives `flexptp_config` a
hardcoded global default pointing at one experimental app:

```python
label_flag(
    name = "flexptp_config",
    build_setting_default = "@//experimental/ptp_demo_flexptp:flexptp_config",
)
```

So imu could not build two differently-configured flexPTP applications either. Copying its rule
wholesale would import machinery for the two libraries that need none here, and miss the one
that does.

### What to take from imu's `embedded_cc_binary`, and what to leave

| Part | Verdict |
|---|---|
| **Dependency validation** — `fail()` if a target depends on `@lwip` without `lwip_config`, or `@freertos` without `freertos_config`/`freertos_malloc` | **Take it.** Plain Starlark in a macro, no transition, and it converts "you forgot the config header" from a confusing error deep in a vendored tree into one sentence naming the target. Extend it to `@flexptp`. Fix the wart while copying: `_has_dep` substring-matches `str(dep)`, so a hypothetical `@lwip_extras` would satisfy the `@lwip` check. |
| **`platform_transition` / `embedded_multiplexer`** | **Leave it.** One MCU; [plan §5.1](bazel_migration.md#41-platforms-and-constraints) stands. |
| **`app_config_transition`** | **Leave it.** It covers the one-consumer libraries and misses the two-consumer one. |

### For `@flexptp`: instantiate per project, do not transition

Rather than a transition, give the main repo a macro — `//bazel/flexptp.bzl` — that each project
calls with its own configuration:

```python
flexptp_library(
    name = "flexptp",
    config = ":flexptp_options",      # this project's header
    port_srcs = ["ptp/ptp_port_stm32h563.c"],
    nsd_srcs = ["ptp/nsd_netxduo.c", ...],
)
```

creating a `cc_library` over `@flexptp//:core_srcs` plus the project's own sources. Two
advantages over a transition:

- **It is comprehensible.** Two projects, two `cc_library` instances, two names in a stack
  trace — instead of one target existing twice in configurations that differ by a flag value.
- **It resolves [plan §6.4](bazel_migration.md#54-flexptp) more faithfully.** That section splits flexPTP into
  `@flexptp//:flexptp_core` plus first-party `alwayslink = True` libraries, because an external
  repository cannot absorb main-repo sources. A macro *in the main repo* can, so the port and
  the NSD compile into the flexPTP library exactly as `FLEXPTP_HWPORT_SRC` and `FLEXPTP_NSD_SRC`
  do in [`Modules/CMakeLists.txt`](../Modules/CMakeLists.txt) today — and the `alwayslink`
  subtlety goes away.

  **Revisited 2026-11-28: it brought the `-w` with it.** Compiled into flexPTP's library, the
  port and the NSD got flexPTP's `-w`, which inhibits every warning wherever it sits on the
  command line — so they were outside the `-Werror` gate (D13) the whole time, and an unused
  variable added to `nsd_netxduo.c` built. The macro now makes two libraries: flexPTP's own
  sources with `-w`, and the project's port and NSD as first-party code, `alwayslink`. That is
  plan §5.4's shape after all; the `alwayslink` subtlety was the price of the warnings. Every
  object of both firmwares compiles byte-identically either way (`opt`), or identically but for
  the object path recorded in the debug info (`dbg`).

The same macro shape covers `@stm32h5_hal` if the two projects ever want different
`stm32h5xx_hal_conf.h` values (D12 currently assumes they do not).

**Transitions are not banned** — `stm32_firmware()` and `stm32_flash_binary` both use one, to
pin the platform ([plan §5.1](bazel_migration.md#41-platforms-and-constraints), [plan §9.2](bazel_migration.md#81-the-flash-rule--pure-starlark-no-new-bcr)).
That is a single, well-understood transition with an obvious purpose. What this section rejects
is a *second* transition layer over build settings, to solve a problem that per-project
instantiation solves more legibly.

---

## 6. Why ST's OpenOCD, built from source

### The fork is not optional

Non-negotiable, and confirmed rather than assumed:

- **Upstream OpenOCD does not support the STM32H5.** Its supported-STM32 list covers F1, F2/F4/F7,
  L1, L4/G4, H7, WB/WL, C0/G0 and U5 — not H5. This is the same point
  [`build.sh`](../build.sh) makes: `target/stm32h5x.cfg` declares `flash bank ... stm32h5x`,
  which names a *compiled-in* driver, so copying ST's `.cfg` files onto a distro build cannot
  work — the binary itself must have H5 support.
- **ST publishes no standalone prebuilt of the fork.** It ships inside CubeIDE/CubeCLT. A
  standing request to either upstream the support or publish binaries
  ([STMicroelectronics/OpenOCD#42](https://github.com/STMicroelectronics/OpenOCD/issues/42),
  opened August 2025) has no maintainer response.

So there is no `http_archive`-a-prebuilt option, which is what would otherwise have made this
trivial and hermetic. Bazel must either build the fork or use a locally provisioned one.

Alternatives considered and rejected: **probe-rs** (a single prebuilt static binary, would have
been fully hermetic with zero BCR additions) and **upstream OpenOCD + xpack builds** — both
rejected under D6. [`tools/hil/`](../tools/hil/README.md) is also written against OpenOCD's CLI,
so either would have meant rewriting the hardware suite's board control as well.

### Should it be built with `rules_foreign_cc`?

**Verdict: yes, and it should be the default.** This was evaluated against evidence rather than
on principle, because it is the one place in this plan that spends a BCR exception.

#### The evidence

**It demonstrably works, on this fork, for this part.** imu has already built ST's OpenOCD with
`rules_foreign_cc`, and the artifact is still in its Bazel cache. Interrogating that binary:

```
Open On-Chip Debugger 0.12.0+dev-snapshot (2026-08-07-16:45) [https://github.com/STMicroelectronics/OpenOCD]
stm32h5x        <- the compiled-in flash driver upstream lacks
st-link         <- the adapter the Nucleo's onboard debugger needs
```

That is the whole requirement. This is not a projection that it could work; it is a binary that
was built this way and has the two capabilities this board needs.

**The host prerequisites are satisfied here and are modest.** Checked on this machine:

| Prerequisite | Status |
|---|---|
| `libusb-1.0` (what ST-Link needs) | 1.0.29 ✓ |
| `autoconf` / `automake` / `libtoolize` / `aclocal` / `pkg-config` / `make` | all present ✓ |
| `libftdi1`, `hidapi`, `libjaylink` | **missing** — and irrelevant here (see below) |

The onboard ST-Link speaks USB via libusb; FTDI and CMSIS-DAP adapters are for external probes
this project does not use.

**imu's `./bootstrap` handling is already correct and worth copying.** ST's `bootstrap` normally
git-clones the `jimtcl` and `libjaylink` submodules — a network fetch *inside* a build action,
which is both a hermeticity violation and a proxy failure waiting to happen. imu avoids it by
passing `autogen_options = ["nosubmodule"]` and setting `init_submodules = True` on the
repository rule, so the submodules arrive declaratively during the fetch, cached and
content-addressed. That is the right structure and it transfers unchanged.

#### The costs, stated honestly

1. **It is not truly hermetic.** OpenOCD links host libusb and needs host autotools. imu's
   `configure_make` papers over this with a hardcoded absolute
   `PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig:...`, which is already reaching
   outside the sandbox. **Do not copy that line** — imu also defines a proper
   `native_tool_toolchain` for pkg-config in its `toolchains/BUILD.bazel`, and that is the
   mechanism to use.
2. **Adapter support varies silently with host dev packages.** libftdi1 and hidapi are absent
   here, so those adapters simply drop out of the build with no error. Harmless for this board,
   but it means "the build succeeded" does not imply "the binary can talk to your probe".
3. **The version stamp is lost.** imu's binary reports `0.12.0+dev-snapshot` where the
   `build.sh`-built one reports `0.12.0+dev-g49ef1d0`, because `guess-rev.sh` finds no git
   metadata in Bazel's fetched tree. Minor, but it removes a provenance signal.
4. Two BCR modules (`rules_foreign_cc`, `pkg-config`) and a slow first build.

#### Why it still wins

- **It pins the revision.** Today `build.sh` pins tag `openocd-cubeide-v2.1.0`, but the *built
  artifact* lives in gitignored `toolchains/` and nothing checks that it matches. Under Bazel
  the pin and the artifact are the same fact.
- **Fix imu's pin while copying it.** imu uses `branch = "openocd-cubeide-r7"`, which is not
  reproducible — a branch moves. Pin the commit instead:
  `49ef1d0107909045e31a728406fcfaeb6f1d5946`. That SHA is simultaneously tag
  `openocd-cubeide-v2.1.0` and, today, the head of `openocd-cubeide-r7`, so this matches what
  `build.sh` already builds — and it is the `g49ef1d0` in the local binary's banner, confirming
  the two agree right now and would drift later.
- **It lets `build.sh` die.** A clean clone can flash with no prior `--with-openocd` step, which
  is what makes D3's "delete CMake and `build.sh`" reachable rather than leaving a shell script
  behind purely to provision a debugger.
- Cost 1 is real but **flashing is inherently non-hermetic anyway** — it mutates a physical
  board over USB and depends on udev rules and device permissions. Hermeticity earns its keep
  where it determines build outputs; nothing OpenOCD does ends up in an artifact.

#### The escape hatch, and the test that makes both safe

Keep `--//tools/flash:openocd=source|local`, defaulting to **`source`**.

`local` probes `$OPENOCD`, then `toolchains/openocd/bin/openocd`, then `PATH`, and **verifies it
is ST's fork** by matching `STMicroelectronics` in `openocd --version` — the banner above shows
the signature is unambiguous. It exists for developers who already have a working OpenOCD, for
hosts where the source build fails, and as the bisection tool when flashing misbehaves.

Both modes are then covered by one smoke test, which is what converts costs 2 and 3 from silent
variability into a loud failure:

```
bazel test //tools/flash:openocd_capability_test
```

asserting that the selected binary reports `STMicroelectronics`, lists `stm32h5x` among its
flash drivers, and lists `st-link` among its adapters. Cheap, runs without a board attached,
and it is the check that a build which "succeeded" actually produced a usable flasher.

---

## 7. Alternatives rejected, in one place

A summary, so that a rejected option is not re-proposed as an obvious improvement. Each links to
the reasoning above.

| Rejected | In favour of | Why, in one line | Detail |
|---|---|---|---|
| Pigweed's toolchain | A hand-rolled `cc_toolchain` | Control over the exact compiler the hardware evidence was gathered with — *not* because it is clang (it defaults to GCC) or heavy | [§3](#3-why-not-pigweed) |
| The legacy `cc_toolchain_config` API | The rule-based `cc/toolchains` API | Rewriting a toolchain later is worse than writing it once, and the legacy path is what the prototype used | [§4](#4-which-cc_toolchain-api) |
| imu's `app_config_transition` | A per-project macro over `label_flag`s | It covers the two libraries that need one configuration here, and misses `@flexptp`, the one that needs two | [§5](#5-why-not-imus-embedded_cc_binary) |
| imu's `platform_transition` multiplexer | A single platform transition on `stm32_firmware()` | One MCU | [§5](#5-why-not-imus-embedded_cc_binary) |
| imu's PEX-packaged Python flasher | A pure-Starlark `stm32_flash_binary` | Costs `rules_python` + `rules_pex` to wrap one `subprocess.run` | [bazel_migration.md §9.1](bazel_migration.md#81-the-flash-rule--pure-starlark-no-new-bcr) |
| probe-rs; upstream OpenOCD + xpack | ST's OpenOCD fork | Upstream cannot program the STM32H5, and the HIL suite is written against OpenOCD's CLI | [§6](#6-why-sts-openocd-built-from-source) |
| BCR's `freertos` module | git `FreeRTOS-Kernel` @ V11.3.0 | BCR tops out at 11.1.0, and `master` pins 11.3.0 with "the CM33 port API is load-bearing" | [§2](#2-why-these-dependencies-from-these-places) |
| A BCR lwIP module | git `lwip-tcpip/lwip` | There is no lwIP module on BCR, and the pin is deliberately 34 commits past the newest release, tipped by a bounds-check fix | [§2](#2-why-these-dependencies-from-these-places) |
| `bazel vendor` / a local registry | Lockfile only (D16) | Vendoring costs 95 MB *before* this project's own dependencies, and neither mechanism alone gives an offline build | [§2](#2-why-these-dependencies-from-these-places) |
| imu's `ptp_demo_flexptp` as firmware source | `master` @ `5e25b7b` | imu's app is HAL_ETH-based; this project bypasses HAL_ETH to reach the DMA descriptors hardware timestamping needs | [bazel_migration.md §12.2](bazel_migration.md#132-where-the-freertos--lwip-firmware-comes-from) |
| A shared `lib/` from the outset | Duplicate, then unify (D11) | The shared/per-project boundary cannot be answered well with one application in front of you | [bazel_migration.md §12.3](bazel_migration.md#133-layout-two-independent-projects-first) |
| Porting `ETH_STACK` (D17) | Direct dependencies | Its `etherlib` value went with D9 and its `none` value was an S3–S5 bisection aid | — |
