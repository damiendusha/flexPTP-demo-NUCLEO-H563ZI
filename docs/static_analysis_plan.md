# Static analysis and sanitizers — a coverage plan

**Status: decided 2026-11-28. Phase 1 done and merged; phase 2 done the same day, awaiting
review before it merges. Phases 3–9 are not started.** §1 lists what was in place before phase 1. The measurements in §2 are from a spike run that day on `74c2702`; they were
not committed. The maintainer's decisions are in [§5](#5-decisions-taken) and are folded into the
phases.

What this covers: cppcheck, clang-tidy, the sanitizers (ASan, LSan, UBSan, MSan, TSan), GCC's
`-fanalyzer`, and the target-side equivalents — the checks that run on the board's own code
generation, which is the one place none of the host tools reach.

## Contents

- [The recommendation](#the-recommendation)
- [1. What is already in place](#1-what-is-already-in-place)
- [2. What a spike measured](#2-what-a-spike-measured)
- [3. The phases](#3-the-phases)
  - [Phase 1 — ASan, LSan and UBSan on the host suite, gated](#phase-1--asan-lsan-and-ubsan-on-the-host-suite-gated)
  - [Phase 2 — `-fanalyzer` on the code that only exists cross-compiled](#phase-2---fanalyzer-on-the-code-that-only-exists-cross-compiled)
  - [Phase 3 — compiler warnings: where they apply, where they are silenced, what more they could say](#phase-3--compiler-warnings-where-they-apply-where-they-are-silenced-what-more-they-could-say)
  - [Phase 4 — clang-tidy, as a Bazel aspect](#phase-4--clang-tidy-as-a-bazel-aspect)
  - [Phase 5 — cppcheck, as a Bazel aspect, with the real headers](#phase-5--cppcheck-as-a-bazel-aspect-with-the-real-headers)
  - [Phase 6 — MSan, with our own instrumented libc++](#phase-6--msan-with-our-own-instrumented-libc)
  - [Phase 7 — on the board: UBSan in trap mode, and stack bounds](#phase-7--on-the-board-ubsan-in-trap-mode-and-stack-bounds)
  - [Phase 8 — fuzzing the parsers (optional)](#phase-8--fuzzing-the-parsers-optional)
  - [Phase 9 — the ARM compiler: `15.3.rel1` to `15.3.rel2`](#phase-9--the-arm-compiler-153rel1-to-153rel2)
- [4. Not planned, and why](#4-not-planned-and-why)
- [5. Decisions taken](#5-decisions-taken)
- [6. What stays uncovered](#6-what-stays-uncovered)

---

## The recommendation

Do them in this order. The first is cheap, already proven to work on this suite, and has three
findings waiting for it.

| # | What | Effort | Gate? | Why this position |
|---|---|---|---|---|
| 1 | ASan + LSan + UBSan over `bazel test //...` | S | **yes** | Works today through features `rules_cc` already ships; 17 of 17 C/C++ test targets pass their assertions under it; **three real UB sites found** (§2) |
| 2 | `-fanalyzer` on the ARM build of first-party code | S | **yes** (see the phase) | 18 first-party files are only ever compiled for the board — both network drivers, the NetX Duo NSD, both `main.c` and `cmds.c`, the console, the runtime — and have never been through it. GI-03 assumed they could not be; the pinned ARM GCC runs it |
| 3 | Compiler warnings: close the holes, measure more flags, look under vendored `-w` | M | yes, as each flag is adopted | They have already cost real defects here: U9 is a comparison `-Wextra` calls always false, hidden by flexPTP's `-w`; the port and NSD were outside `-Werror` for the same reason |
| 4 | clang-tidy, curated, as a `rules_lint` aspect; clang-tidy from `toolchains_llvm` | M | yes, on a curated list | A second path-sensitive engine (the Clang Static Analyzer) plus the `bugprone-`/`cert-` families; the editor already runs it, CI does not |
| 5 | cppcheck as a `rules_lint` aspect, from BCR's `cppcheck` with a carried patch | M | yes (as now) | Pinned at last, sees the HAL and the kernels' headers it never has, and deletes two workarounds |
| 6 | MSan over the whole suite: `toolchains_llvm`, our own instrumented libc++ | L | yes, once clean | Uninitialised reads are the one class no current tool checks at run time |
| 7 | UBSan in trap mode on the `dbg` firmware; static stack bounds | M | HIL | The only items here that check the code the board actually runs |
| 8 | Fuzz the PTP and Ethernet parsers | M | no | Optional. Network-facing parsers are where the sanitizers pay most |

TSan is not planned; see [§4](#4-not-planned-and-why).

---

## 1. What is already in place

| Check | Covers | Gates | Where |
|---|---|---|---|
| `-Wall -Wextra -Werror` (D13) | first-party code, **ARM build only**, `dbg` and `opt` | Build job | `first_party_warnings` in `toolchain/BUILD.bazel` |
| `-Wall` | host builds, tests included — **no `-Wextra`**, which CMake had on every test, and no `-Werror` | no | `rules_cc`'s autoconfigured host toolchain (measured with `aquery`) |
| GCC `-fanalyzer` | first-party code compiled for the **host**: the NSD units, framer, link state, `mac_drv.c` via `host_mac_drv`, the Ethernet model, the FIFO, the CLI | warnings only | `HOST_ANALYZER`, [`lib/BUILD.bazel`](../projects/flexptp_demo_stm32h563/lib/BUILD.bazel) |
| cppcheck 2.19 (`warning,portability`) | both projects' directories and `lib/`, with CMSIS headers but **no HAL or kernel headers** | Static analysis job | [`ci.yml`](../.github/workflows/ci.yml) |
| clang-tidy | whatever file is open, in the editor, with clang-tidy's defaults — there is no `.clang-tidy` | no | `--clang-tidy` in `.vscode/settings.json` |
| Guard-page heap | use-after-free in flexPTP's teardown, on the FreeRTOS POSIX port | Tests job | `patches/flexptp/test/efence_heap.c` |
| C++ rules | no `operator new`/`delete`, `__cxa_throw` or RTTI in either linked image | Tests job | `//tools/ci:cxx_rules_test` |
| Mutation testing | that the tests can fail | by hand | [testing_architecture.md](../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md#mutation-testing-is-the-entry-fee) |
| RTOS run-time checks | ThreadX stack checking (`dbg` only), FreeRTOS `configCHECK_FOR_STACK_OVERFLOW 2`, NetX Duo packet debug info, the bounded `_sbrk` | on the board | `tx_user.h`, `FreeRTOSConfig.h`, `nx_user.h`, `runtime/sysmem.c` |
| Malformed PTP | the board's receive path against bad packets | HIL | `tools/hil/ptp_fuzz.py` |

What the docs say about the rest:

- **Sanitizers were deferred, deliberately.** [bazel_migration.md §1.1](bazel_migration.md#11-from-bcr-rule-sets-only)
  lists `toolchains_llvm` as not taken because "the sanitizer configs are not part of this
  migration". Nothing has picked them up since. `test_cli.cpp` notes that a one-byte overrun it
  pins "under ASan or Valgrind … would be caught; here the assertion is that nothing is lost".
- **GI-03** ([review_v1_opus_5p0.md](../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/review_v1_opus_5p0.md#gi-03--add-static-analysis-to-ci))
  asked for cppcheck *or* clang-tidy and noted the compile database. cppcheck and `-fanalyzer`
  were done; clang-tidy never was.
- **The guard-page heap is a hand-built ASan** for one defect class, written because heap_4
  hides a use-after-free by returning the block to its free list. It coexists with ASan (§2).

---

## 2. What a spike measured

`bazel test --features=asan --features=ubsan //...`, 2026-11-28. Results:

- **The firmware is untouched.** A `rules_cc` feature only takes effect in a toolchain that
  declares it. The host toolchain declares `asan`, `lsan`, `tsan` and `ubsan`; the ARM toolchain
  declares none of them. Both firmwares built and both C++-rules tests passed in the same
  invocation, so even a plain `bazel test --config=asan //...` cannot break them.
- **Everything ran.** That includes the parts most likely to break: the MAC driver tests with the
  `MAP_32BIT` arena and `-no-pie`, real ThreadX on pthreads, the FreeRTOS POSIX port, and the
  guard-page heap. Pass 1 (fatal UBSan, leak checking on) went 20/25, with the 5 failures listed
  below. Pass 2 (UBSan reporting and continuing, leak checking off) went **17/17 C/C++ test
  targets passing their own assertions, with zero ASan memory errors.**
- **Cost.** About 22 s to rebuild every C/C++ test and its dependencies with the sanitizers.
  Over `//...` it was 134 s, because the features also reach the *target-configuration* OpenOCD
  that `//tools/flash:openocd_capability_test` checks: it was rebuilt with ASan (and passed). The
  exec-configuration copy that `:flash` runs is not touched — `--features` does not reach the exec
  configuration.

The five failures, all explained:

| Test | Report | Whose | Verdict |
|---|---|---|---|
| `//projects/…/lib:sysmem_bounds_test` | `sysmem_bounds.h:65` negation of `INT64_MIN` | **ours** | **Real UB, on the host only.** `(uintptr_t)(-(intmax_t)incr)` negates in a *signed* type; on the target `intmax_t` is wider than `ptrdiff_t`, so it is safe there, but on x86-64 both are 64 bits. The test that fails is `TheMostNegativeIncrementDoesNotOverflowOnNegation`, which passed by two's-complement luck. The comment above the line already says "negate in an unsigned type": `0u - (uintptr_t)incr` |
| `//patches/flexptp/test:notification_test` | `msg_utils.c:214` left shift of 968000000 by 8 overflows `int` | flexPTP | **Real UB, on both targets.** `FLEXPTP_htonl` shifts its argument in the argument's own type; applied to `TimestampI.nanosec`, an `int32_t`, `<< 8` overflows for any value of 2²³ ns or more. A `uint32_t` cast inside the macro fixes it. A carried patch and an upstream issue, by the process in [patches/flexptp/README.md](../patches/flexptp/README.md) |
| `//patches/flexptp/test:rx_validation_test` | `msg_utils.c:246`, the same macro via `FLEXPTP_ntohl` | flexPTP | the same defect. A third site, the seconds shift at `:245` (an `int64_t`, overflowing whenever the last wire byte is `0x80` or more), was hidden behind it — UBSan stops at its first report — until U15's own test reached it. U15 fixes all three |
| `//patches/flexptp/test:teardown_test` | LSan: 104 B from `event_create` in `pxPortInitialiseStack`, 1 B from `prvMarkAsFreeRTOSThread` | FreeRTOS POSIX port | Per-thread bookkeeping still live at exit, one object each. Suppress |
| `//projects/…/flexptp_freertos_lwip_demo/test:test_rtos_context` | LSan: 1 B from `prvMarkAsFreeRTOSThread` | FreeRTOS POSIX port | the same; suppress |

---

## 3. The phases

Each phase has an acceptance check that includes a **non-vacuity probe**: a deliberate defect
that must turn it red. That is the same rule the cppcheck and `-fanalyzer` work followed, and for
the same reason — a check whose first clean run was the tool failing to start has happened here
before (GI-03).

### Phase 1 — ASan, LSan and UBSan on the host suite, gated

**What.** A `--config=asan` in `.bazelrc`, using the features rather than `--copt`:

```
# Host sanitizers. Features, not --copt: a feature only takes effect in a toolchain that declares
# it, the ARM toolchain declares none of these, and so the firmware is untouched.
build:asan --features=asan --features=ubsan
test:asan --test_env=ASAN_OPTIONS=detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1
```

`rules_cc`'s sanitizer features already add `-fno-sanitize-recover=all` and
`-fno-omit-frame-pointer`, so UBSan is fatal without any environment variable.

**Then:**

1. **Fix `sysmem_bounds.h:64`** as its own comment says. Target codegen is unaffected, because
   there `intmax_t` is already wider than `ptrdiff_t`.
2. **Carry the flexPTP fix as `0015-u15`**, with a host test that round-trips a nanosecond value
   of 2²³ or more. Prepare its upstream branch and pull request the way #37–#40 were, **for the
   maintainer's review before anything is pushed or opened**.
3. **Suppress the two FreeRTOS POSIX-port leaks in code, not in the environment.** A
   `__lsan_default_suppressions()` in the FreeRTOS host-support library, naming exactly
   `prvMarkAsFreeRTOSThread` and `event_create`, with the reasoning beside it. That way the
   suppression travels with the library that needs it and cannot be lost from a command line.
4. **A CI job** over the C/C++ tests:
   `bazel test --config=asan $(bazel query 'kind(cc_test, //...)')`. Not `//...`: that would also
   rebuild an ASan OpenOCD for the capability test, for nothing. Keep the job separate from
   `Tests`, so that a sanitizer finding and a functional failure read as different things.

**Phase 1b — make ASan see inside the arena.** The driver's descriptors and buffers come from
`host/host_arena.c`, which `mmap`s with `MAP_32BIT` and is invisible to ASan: no redzones, so a
DMA overrun inside the arena is not caught. That is the memory the driver tests exist for. Under
`__SANITIZE_ADDRESS__`, give each arena allocation a poisoned gap with
`ASAN_POISON_MEMORY_REGION`. The same applies, less urgently, to any custom pool a host test
exercises.

*Done, 2026-11-28, and measured before and after.* The arena poisons all it has not handed out,
with a 64-byte gap in front of each allocation, and the model gives the TX ring an allocation of
its own (only the state block and the RX ring must be adjacent). The probe: the driver's RX-ring
loop changed from `<` to `<=`, writing one descriptor past the ring. Before, every driver test
passed it, under ASan too — the write landed on the first TX descriptor, which the driver clears
a few lines later. After, all three report `use-after-poison`, a 4-byte write in
`ETHHW_InitPeripheral`. What it cannot reach: the frame buffers stay back to back, because the
driver strides through one block, so a frame overrunning into the next buffer is still invisible.

**Acceptance.** The CI job is green. These probes each turn it red: a heap overflow in a unit
test; the driver writing past the end of a descriptor ring (this one needs 1b); a signed
overflow in a first-party unit; and a leaked allocation in a first-party test. The firmware
build IDs are unchanged. *All met on 2026-11-28; see the phase's commits.*

**Risk.** `-fsanitize=address` uses the host's shadow memory below 2 GB, the same region
`MAP_32BIT` draws from. It worked in the spike; an arena large enough to collide would fail at
startup, not silently.

### Phase 2 — `-fanalyzer` on the code that only exists cross-compiled

**What.** The pinned `arm-none-eabi-gcc` 15.3 runs `-fanalyzer` (verified). Today it runs only on
the host build, so the 18 first-party files that are never compiled for the host have never
been analysed (measured with `aquery`, both firmwares against every host test):

- ThreadX firmware: `main.c`, `cmds.c`, `eth/eth_drv_netxduo.c`, `eth/ethernet_netxduo.c`,
  `ptp/nsd_netxduo.c`, `ptp/ptp_tx_timestamp.c`
- FreeRTOS firmware: `main.c`, `cmds.c`, `eth/eth_drv_lwip.c`, `eth/ethernet_lwip.c`
- `lib/`: `io/serial_io.c`, `io/standard_output.c`, `ptp/ptp_port_stm32h563.c`, and the runtime —
  `sysmem.c`, `syscall.c`, `build_id.c`, `system_stm32h5xx.c`, `cpp_support.cpp`

(`mac_drv.c`, `phy_common.c` and `cli.c` look cross-only but are not: the host compiles them
through their `_uut.c` wrappers.)

**How.** Two toolchain features on the ARM `cc_toolchain`. A command-line switch, `analyze`,
does nothing on its own. A `first_party_analysis` feature added to the `FIRST_PARTY` lists
`requires` that switch, so it is inert unless asked for, and when asked for it reaches only
first-party targets, never NetX Duo, ThreadX or the HAL. It adds `-fanalyzer -Wno-error`:
`-Wno-error` is acceptable because this is a separate advisory job, and the normal build job
still enforces D13.

**Acceptance.** `bazel build --config=opt --features=analyze` over both firmwares, with the
findings triaged. A NULL dereference added to `cmds.c` is reported.

**Cost.** `-fanalyzer` is slow, but only first-party files pay it.

*Done, 2026-11-28 — with three changes from the above, each for a reason found on the way.*

1. **The first finding was the plan's own premise.** `flexptp_library()` compiled the port and the
   NetX Duo NSD *into* flexPTP's library, `-w` and all, and `-w` silences every warning wherever
   it sits on the command line. So `ptp_port_stm32h563.c` and `nsd_netxduo.c` had been outside
   the `-Werror` gate (D13) all along — an unused variable in `nsd_netxduo.c` built — and
   `-fanalyzer` would have been silent on them too. The macro now builds them as a first-party
   library of their own. Every object compiles byte-identically either way (`opt`; in `dbg` only
   the object path recorded in the debug info differs), and nothing new warned.
2. **A gate, not advisory.** The ARM compiler is pinned by sha256, so a new analyzer diagnostic
   arrives only with a toolchain bump — D13's argument for `-Werror`, which applies here as it
   does not to the host's distro GCC. And an advisory run cannot work under Bazel: a cached
   compile prints nothing, so a finding would show in one CI run and vanish from every later
   one. It runs as the last step of each Build job, both firmwares × `dbg` and `opt`; the two
   optimisation levels find different things (only `-O0` reported the halt loops, only `-O2` the
   command-table copy), so both are needed.
3. **Triage.** 17 reports in all. 15 were `-Wanalyzer-infinite-loop` on the deliberate halts that
   end every fatal path — fault handlers, the stack-overflow and malloc hooks, asserts, `_exit`,
   `_sbrk`'s exhaustion halt — so that check is off for this build, with the list beside it. Two
   were `-Wanalyzer-overlapping-buffers` in `cli.c`: false positives, but one rested on an
   invariant held by another function, so both copies were rewritten rather than suppressed —
   and the compaction of the command table turned out to have no test at all (deleting the copy
   outright passed the whole suite). It has one now, and the history four.

Probes: a NULL dereference added to `cmds.c` or `nsd_netxduo.c` fails the analysis and not the
plain build. A lookup that can return NULL, dereferenced unchecked, is caught at both levels; a
dereference inside `if (p == NULL)` is caught at `-O2` only.

### Phase 3 — compiler warnings: where they apply, where they are silenced, what more they could say

**Why.** Warnings have already cost this project real defects, each time by their absence:

- **U9** (#34): flexPTP compares a short enum with `-1`, which the arm-none-eabi ABI makes always
  false, so GCC deletes the shutdown branch. `-Wextra`'s `-Wtype-limits` says exactly that —
  "comparison is always false due to limited range of data type", checked with the pinned
  compiler; `-Wall` alone does not — but flexPTP compiles with `-w`.
- **The port and the NetX Duo NSD** were outside the `-Werror` gate because they were compiled
  into flexPTP's library, under its `-w` (found in Phase 2, fixed in `4dfffe0`).
- **The host tests** compile with `-Wall` only. The CMake suite had `-Wall -Wextra`; the migration
  lost it, the same way it lost `-fanalyzer`.
- **The `dbg` image is built at `-O0`**, where the flow-dependent warnings — `-Wmaybe-uninitialized`,
  `-Wnull-dereference`, `-Warray-bounds`, `-Wstringop-*` — mostly do not run. Only the `opt` build
  sees them, and Phase 2 measured the same kind of difference for the analyzer.

**What.**

1. **Audit where warnings apply, and make it a check.** From `bazel aquery`, every compile of
   both firmwares (`dbg`, `opt`) and of the host tests, classified by origin and flags. A
   first-party compile without `-Werror` on the board, or without `-Wextra` on the host, is a
   hole; so is a vendored one without `-w`. A script over that output in CI fails on a hole.
   Probe: put `-w` back on the port's compile — it must go red.
2. **Measure more flags on first-party code**, each one counted on both firmwares in both
   configurations, then adopted (clean, or worth the fixes) or rejected with its numbers:
   `-Wcast-align=strict` (F62 was an access the silicon rejects), `-Wdouble-promotion` (the FPU
   is single-precision, so an accidental `double` is a software-emulated call), `-Wundef` (an
   `#if` on a misspelt configuration macro is silently 0), `-Wshadow`, `-Wformat=2`,
   `-Wformat-truncation=2` and `-Wformat-overflow=2` (the console is printf-shaped),
   `-Wstrict-prototypes` and `-Wmissing-prototypes`, `-Wnull-dereference`, `-Wduplicated-cond`,
   `-Wduplicated-branches`, `-Wlogical-op`, `-Wvla`, and `-Wconversion`/`-Wsign-conversion`
   (expected to be noisy on register code; measure before deciding, and consider them per
   library rather than everywhere). `-Wstack-usage=` belongs with Phase 7's stack bounds.
3. **Look under the vendored `-w`, once.** Compile flexPTP, NetX Duo, ThreadX, the HAL, lwIP,
   FreeRTOS and embfmt with `-Wall -Wextra`, without `-Werror`, and triage what appears. flexPTP
   first, since U9 is exactly this kind of finding and the upstream route exists. A report, not
   a gate: the warnings are not this project's to fix, but the defects behind them might be.
4. **The host.** Restore `-Wextra` on first-party host code, as warnings. Whether the host can
   gate is a separate decision: its compiler is the distro's and floats, which is the reason it
   does not today — Phase 4's pinned LLVM would remove that reason, if the host builds moved
   onto it.
5. **The linker and the assembler.** A link warning today is printed and ignored. Audit what the
   links say (`--print-memory-usage` aside), and consider `-Wl,--fatal-warnings`; likewise the
   startup assembly.
6. **The optimisation level of `dbg` and fastbuild**, because it decides which of the above can
   fire at all in a debug build. Measured separately; see the decision it leads to.

**Acceptance.** The audit runs in CI and goes red on the probe; every adopted flag is clean on
both firmwares in both configurations and has a probe of its own; the vendored survey is
triaged, with anything real taken upstream by the U-process.

### Phase 4 — clang-tidy, as a Bazel aspect

**What.** clang-tidy over first-party targets, run by `aspect_rules_lint`'s clang-tidy aspect with
the clang-tidy from a `toolchains_llvm` LLVM distribution, both from BCR and pinned. The aspect
analyses each target with that target's own compile context, so there is no
`compile_commands.json` step, and results are cached per target like any other action. Its
`clang-analyzer-*` checks are the Clang Static Analyzer: a second path-sensitive engine with
different strengths from `-fanalyzer`, so it is worth having both. Separate `scan-build` runs are
therefore unnecessary. `toolchains_llvm` supplies the binary only; it is not registered as a C++
toolchain here (MSan's opt-in config in Phase 6 is the one place that does).

**Checks**, in a root `.clang-tidy`. That file also configures the editor, which already runs
clang-tidy through clangd, so the editor and CI would finally agree:

- **On:** `clang-analyzer-*`, `bugprone-*`, `cert-*` (the C rules especially: `cert-err33-c` for
  unchecked return values, `cert-int*`, `cert-str*`), `misc-*`, `portability-*`, and
  `performance-*` for the C++ units.
- **Off, as noise on register-level code:** `bugprone-easily-swappable-parameters`,
  `readability-magic-numbers`, `readability-identifier-length`, `hicpp-signed-bitwise`, and
  `cppcoreguidelines-pro-type-cstyle-cast` (most of this tree is C).
- **Gate:** `WarningsAsErrors` on a curated subset, starting with `clang-analyzer-*` and the
  `bugprone-` checks that come up clean or get fixed. Suppressions are `NOLINT(check)` with a
  reason, the same rule as cppcheck's inline suppressions.

**The known obstacle — spike it first.** The board's targets compile with `arm-none-eabi-gcc`
flags. clangd copes through `--query-driver`, which asks the compiler for its built-in include
paths; clang-tidy cannot ask. It needs `--target=arm-none-eabi`, the newlib include directories
(from `arm-none-eabi-gcc -xc -E -v -`) as `-isystem`, and the GCC-only flags dropped, the way
`refresh_compile_commands.py` already drops `-fanalyzer`. Find out how much of that the aspect's
argument handling allows before choosing between linting the ARM build and linting each
first-party file's host build where one exists.

**Acceptance.** The CI job is green on the curated set. A NULL dereference and an unchecked
`snprintf` truncation, each added to a first-party file, are both reported.

### Phase 5 — cppcheck, as a Bazel aspect, with the real headers

**What.** `aspect_rules_lint`'s cppcheck aspect, with the cppcheck from BCR's `cppcheck` module
(2.20.0, built from source and pinned). Each target is analysed with its own include paths, so
cppcheck sees the HAL, ThreadX, NetX Duo, FreeRTOS and lwIP headers for the first time. That
deletes two workarounds in the current job: the symlink that recreates Bazel's
`standard_output/` include prefix, and the forced `FreeRTOSConfig.h`. The `apt` install and its
floating version go with them.

**BCR's `cppcheck` is broken as packaged, and needs a patch.** Measured 2026-11-28: it builds on
Bazel 9, prints its version, and then refuses to analyse anything — "Failed to load std.cfg. Your
Cppcheck installation is broken" — even when run from its own source directory, where all 51
`cfg/` files sit. The overlay compiles it with `FILESDIR="."` and attaches no `cfg/` data. This is
exactly the failure GI-03 met, where the first "clean run" was cppcheck not starting. Carry a
patch through `single_version_override`, as `patches/` already does for flexPTP and embfmt: add
`cfg/` as data and point `FILESDIR` at it through the runfiles. Then offer the same fix to the BCR
entry.

**To check in the spike:** as read, the aspect passes each target's include paths but not its
defines. If so, the defines the current job passes (`-DSTM32H563xx -DUSE_HAL_DRIVER …`) stay as
explicit arguments; otherwise cppcheck checks every `#ifdef` configuration, which is slower and
noisier.

**Optional, once green:** `--enable=style,performance` and `--check-level=exhaustive`, against a
baseline; and the MISRA addon as an advisory report, never a gate. The rule texts are licensed,
and the checks run with rule numbers only.

**Acceptance.** As today, plus two things: a defect that depends on a HAL macro is caught, which
the current job cannot see at all; and an out-of-bounds write is reported through the
Bazel-built binary. The second is the check that it actually started.

### Phase 6 — MSan, with our own instrumented libc++

**The gap.** No current tool checks uninitialised reads at run time. Statically there is
`-Wanalyzer-use-of-uninitialized-value` on the host units and cppcheck's `uninitvar`. GCC's
`-Wmaybe-uninitialized` only fires with optimisation, so only the `opt` ARM build sees it.

**What.** An opt-in `--config=msan` that uses `toolchains_llvm` (1.13.0, BCR) as the host C++
toolchain, through `--extra_toolchains` in that config only, with `--features=msan`. Everything
else keeps GCC. Since 1.13.0 its `msan` feature swaps in an instrumented libc++, supplied with
`libcxx_url` and `libcxx_sha256`, and keeps build tools uninstrumented, because exec builds use
`--host_features`. It fails loudly on an unsupported target rather than producing an
uninstrumented build.

**The instrumented libc++ is ours.** GCC has no MemorySanitizer, and no official prebuilt
instrumented libc++ exists; `toolchains_llvm`'s README points at a third-party tarball. Instead:

1. A script committed here builds libc++, libc++abi and libunwind from the LLVM source matching
   the pinned LLVM version, with `-DLLVM_USE_SANITIZER=MemoryWithOrigins` (the recipe
   `toolchains_llvm` documents), and archives `lib/` and `include/`.
2. The tarball is hosted as a release asset on this repository and pinned by sha256. Publishing
   it is an outward-facing step, confirmed with the maintainer when it happens.
3. Bumping LLVM means re-running the script. The sha256 pin makes a mismatch a fetch error, not
   a silent mix of versions.

GoogleTest, flexPTP, both RTOS host ports and first-party code are all compiled under it, which
is what MSan requires.

**Risks, to settle on the first run.** MSan is stricter about uninstrumented code than ASan:
anything reaching a non-instrumented library (glibc is intercepted; anything else is not) reports
falsely. The RTOS host ports' signal handling, and the `MAP_32BIT` arena with `-no-pie`, worked
under ASan but are untested under MSan. The arena should be fine — low addresses are application
memory in MSan's x86-64 layout — but measure it.

**Acceptance.** All C/C++ tests pass under `--config=msan`; an uninitialised read added to a
first-party unit, through a branch, is reported with its origin. The job gates once it is clean.

**Fallbacks**, if MSan cannot run part of the suite: Valgrind memcheck
(`--run_under=valgrind`) for that part, and `-ftrivial-auto-var-init=pattern`, which makes
uninitialised stack reads deterministic so existing assertions trip over them.

### Phase 7 — on the board: UBSan in trap mode, and stack bounds

[testing_architecture.md](../projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/docs/testing_architecture.md#known-gaps)'s
first known gap is that the host tools never see the ARM code generation. These checks do.

- **UBSan, trapping, on a `dbg` image.** `-fsanitize=undefined -fsanitize-trap=undefined` needs
  no runtime library: each check compiles to a `udf` instruction (verified, 2 traps in a probe
  and 0 `__ubsan` symbols), so undefined behaviour becomes a UsageFault (or a HardFault, if it
  escalates) and both handlers end in `fault_report()`, which prints the stacked PC — the trapping
  instruction. Run the HIL suite against it. Measure image growth first;
  there is room in 2 MB of flash, but measure rather than assume.
- **Static stack bounds.** `-fstack-usage` and `-fcallgraph-info=su` give each function's frame
  and the call graph, and from those a worst-case depth per thread to set against the configured
  stacks — the 2 KB CLI thread and the rest. Today stack sizing is runtime high-water marks
  (`osinfo`) and ThreadX's `dbg`-only stack checking; this adds the bound those cannot give. A
  small script over the `.su` and `.ci` files, or `-Wstack-usage=N` as the first, crude version.
- **`-fstack-protector-strong` on `dbg` images**, with `__stack_chk_guard` and
  `__stack_chk_fail` supplied in `runtime/`. Optional.

### Phase 8 — fuzzing the parsers (optional)

Run coverage-guided fuzzers under ASan and UBSan over the network-facing parsers: flexPTP's
receive path (`rx_validation_test`'s engine harness is most of a fuzz target already),
`eth_framer` and `ptp_ts_codec`. `tools/hil/ptp_fuzz.py` throws chosen malformed packets at the
board; a host fuzzer explores far more of the input space. `rules_fuzzing` is on BCR but is a
new module under D2 ([§0](bazel_migration.md#0-decisions-taken)), and libFuzzer wants clang —
which Phase 6's `toolchains_llvm` config provides.

---

### Phase 9 — the ARM compiler: `15.3.rel1` to `15.3.rel2`

Added 2026-11-28; its position in the order is still to be decided. Arm's newest release is
`15.3.rel2` (2026-10-12); there is no GCC 16 from Arm yet. The two source manifests differ in one
line: **binutils** (`e046e539…`, was `609397ff…`). GCC (`7909a9f8…`), newlib and glibc are the same
commits, so the compiler's code generation and diagnostics should not move; the assembler and
linker do. The x86-64 archive is published with sha256 `463df966…5d572d25215`, which is checked
against Arm's `.sha256asc` before it is pinned.

**What.** Bump the `http_archive` in `MODULE.bazel` (URL, `strip_prefix`, sha256). Then:

- every first-party object of both firmwares in `dbg` and `opt` should be byte-identical, because
  GCC is unchanged — measured the way Phase 2 measured the `flexptp_library` split; a difference
  is the finding;
- the linked images are expected to differ (a new linker): compare them function by function,
  and read the link output for new warnings (Phase 3, item 5);
- the `-Werror` and `-fanalyzer` gates, the C++ rules and `bazel test //...` in all three modes;
- the board: S8 on both firmwares, release and debug (`tools/hil/matrix.sh`), because a linker is
  part of what the image is.

**Acceptance.** All of the above green, the measurements recorded in the commit, and
testing_architecture.md's latest-verification section updated.

## 4. Not planned, and why

- **TSan.** The only multi-threaded host tests are the real-kernel ones, and both kernels' host
  ports emulate a single core with pthreads and signals, handing control between threads by
  design. TSan would report the kernels' own hand-offs, and it cannot model what concurrency on
  the board actually is: an interrupt preempting a thread on one core. A single
  `--features=tsan` run over the three real-kernel targets would confirm that and could be
  recorded here; it is not worth a job.
- **`scan-build` on its own.** It is the same analyser as clang-tidy's `clang-analyzer-*`
  (Phase 4).
- **CodeQL and Coverity.** Both trace the compiler, which Bazel's sandbox and cache get in the way
  of (it would need `--spawn_strategy=local` and no cache). Reconsider if the repository's CI
  moves to a setup they support natively.
- **`-Werror` on the host build.** D13 is safe because the ARM compiler is pinned by sha256; the
  host compiler is the distro's and moves underneath. `-Wextra` on host first-party targets is
  worth restoring (CMake had it), as a warning.

---

## 5. Decisions taken

Asked and answered on 2026-11-28.

1. **The analysers are pinned through BCR**, not installed from `apt`. That means
   `aspect_rules_lint` to run them, `toolchains_llvm` for clang-tidy, and BCR's `cppcheck` with a
   carried patch (Phase 5). This is **a change to D2**
   ([bazel_migration.md §0](bazel_migration.md#0-decisions-taken)), which allowed six modules, all
   rule sets except `googletest`. `aspect_rules_lint` and `toolchains_llvm` are rule sets, and
   `cppcheck` is a development dependency like `googletest`. D2's entry is amended when the first
   of them lands, not before.
2. **MSan over the whole suite, with an instrumented libc++ built here from the start**, rather
   than a third-party prebuilt or Valgrind first (Phase 6).
3. **The flexPTP UB becomes U15**: a carried patch with a host test, and an upstream branch and
   pull request prepared for review before anything is pushed or opened (Phase 1).
4. **Order:** this plan is committed, then Phase 1 starts. Each phase stops for review before
   anything merges to `main`.

## 6. What stays uncovered

- **Custom allocators hide memory errors from ASan**: ThreadX byte pools, NetX Duo packet pools
  and FreeRTOS heap_4, and inside the `MAP_32BIT` arena an overrun from one frame buffer into the
  next (Phase 1b guards the ends of each allocation, not the buffers within one). The guard-page
  heap covers one class in one test.
- **Codegen defects** such as F70 and F62 remain bench-only. Phase 7's UBSan image catches the
  UB-shaped subset on the real code generation, not the rest.
- **Concurrency**: interrupt-versus-thread races are a bench problem, as testing_architecture.md
  already says.
