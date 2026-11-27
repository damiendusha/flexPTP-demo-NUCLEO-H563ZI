# Testing Architecture

How this repository is tested, how to repeat that testing, what each layer is *for*, and — the
part that matters most — what each layer cannot tell you. A green suite trusted beyond its reach
is worse than no suite, because it turns an unknown into a false certainty.

This covers **both firmwares** (ThreadX + NetX Duo, FreeRTOS + lwIP), the shared code in
`//projects/flexptp_demo_stm32h563/lib`, the flexPTP patches the build carries, and the tools
that test them. It lives beside the ThreadX firmware for historical reasons only.

## Table of Contents

- [The layers](#the-layers)
- [Build configurations: what "debug" and "release" mean](#build-configurations-what-debug-and-release-mean)
- [The bench](#the-bench)
- [Procedures](#procedures)
  - [1. Every change](#1-every-change)
  - [2. Full verification: the matrix](#2-full-verification-the-matrix)
  - [3. A flexPTP fix](#3-a-flexptp-fix)
  - [4. A dependency bump](#4-a-dependency-bump)
  - [5. Proving a fix: before and after](#5-proving-a-fix-before-and-after)
  - [6. Characterisation and soak](#6-characterisation-and-soak)
- [The layers in detail](#the-layers-in-detail)
- [Reading results](#reading-results)
- [What is reachable, measured](#what-is-reachable-measured)
- [Mutation testing is the entry fee](#mutation-testing-is-the-entry-fee)
- [What CI gates](#what-ci-gates)
- [Known gaps](#known-gaps)
- [Latest full verification](#latest-full-verification)

Companion documents: [architecture.md](architecture.md) (what is being tested) ·
[port_review.md](port_review.md) (the finding log these tests are calibrated against) ·
[`test/README.md`](../test/README.md) (how the driver is compiled for the host) ·
[`tools/hil/README.md`](../../../../tools/hil/README.md) (the hardware harness, its checks and
scenarios) · [`patches/flexptp/README.md`](../../../../patches/flexptp/README.md) (the patches and
their host tests) · [`docs/flexptp_upstream_review.md`](../../../../docs/flexptp_upstream_review.md)
(every campaign's results, dated)

---

## The layers

| # | Layer | Where it runs | Needs | Wall clock | Gates |
|---|---|---|---|---|---|
| 1 | **Host unit tests** — the MAC driver's rings, link state, framer, the NSD logic units, console FIFO, CLI, sbrk bounds | dev machine / CI, host compiler | nothing | seconds | every push |
| 2 | **Real kernels on the host** — ThreadX's Linux port and FreeRTOS's POSIX port, under the real CMSIS-RTOS2 wrappers | dev machine / CI | nothing | seconds | every push |
| 3 | **flexPTP's real engine on the host** — its own sources, patched as the firmware fetches them | dev machine / CI | nothing | seconds | every push |
| 4 | **Checks on the linked image** — the C++ rules, the per-project file split | dev machine / CI | nothing | seconds | every push |
| 5 | **Tool tests** — the HIL parsers, the SWD readers, OpenOCD discovery and capability, the compile-commands rewrite | dev machine / CI | nothing | seconds | every push |
| 6 | **HIL checks** — `tools/hil/run.py`, 14 checks from build ID to a saturation flood | the board, over serial + SWD | board, cable, host NIC | ~6 min | manual |
| 7 | **HIL scenarios** — `tools/hil/scenario.py`, 20 long scenarios: fix regressions, floods, malformed PTP, stacks, health | the board, the host's ptp4l units | as 6, plus the installed bench | ~55 min per image | manual |
| 8 | **SWD inspection** — `tools/hil/swd.py`: heap, stacks, faults, the console thread, flexPTP's queues | the board, halted for milliseconds | ST-Link | seconds | inside 7, and by hand |

Layers 1–5 are one command, `bazel test //...` — 25 targets, host and target in one graph, so
there is no separate test root to configure. Layers 6–8 need the board.

**The split is not a maturity ladder.** The board layers are not slower versions of the host
ones; they catch disjoint classes of defect. Roughly a quarter of this port's findings were
reachable *only* on hardware ([below](#what-is-reachable-measured)), and so were all three of the
November 2026 FreeRTOS wedges and U14 — none of which any host test could have found, because
each needed real interrupt timing, real memory pressure or a real flood.

---

## Build configurations: what "debug" and "release" mean

Three compilation modes, and **only two of them are what ships**:

| Mode | How | Flags | `DEBUG` | Text (ThreadX / FreeRTOS) | Use |
|---|---|---|---|---|---|
| fastbuild | `bazel build` (no `--config`) | `-O0 -g` | no | 211 120 / 224 432 B | the developer default; what most bench work is done on |
| **debug** | `--config=dbg` | `-O0 -g3 -ggdb` | **yes** | 221 840 / 224 432 B | the Debug configuration |
| **release** | `--config=opt` | `-O2 -g0` | no | 148 640 / 162 656 B | the Release configuration |

**`DEBUG` is what makes a debug image a debug image**, not the `-O0`. It is defined for every
compile action in `dbg` mode (`toolchain/BUILD.bazel`), vendored libraries included, and two
headers react to it — both ThreadX-side:

| Switch | Debug | Release | Consequence |
|---|---|---|---|
| `TX_ENABLE_STACK_CHECKING` (`tx_user.h`) | on | off | ThreadX's software stack check. The PSPLIM hardware trap is the primary mechanism in both |
| `NX_DISABLE_ERROR_CHECKING` (`nx_user.h`) | off | **on** | Debug links NetX Duo's `_nxe_*` API checks (26 of them); release calls `_nx_*` directly, so a misuse that returns `NX_PTR_ERROR` in debug is undefined behaviour in release (finding NX-02) |
| `NX_ENABLE_PACKET_DEBUG_INFO` (`nx_user.h`) | on | off | 12 bytes per `NX_PACKET`: owner, file and line for a packet that never came back |

Nothing on the FreeRTOS side reads `DEBUG`, so its debug image is fastbuild's code with more debug
information — verified byte-identical in every loaded section. ThreadX's debug image is genuinely
different: 10.7 KB more code. The Bazel migration lost `DEBUG` (CMake applied it as
`$<$<CONFIG:Debug>:DEBUG>`), and from then until 2026-11-27 `--config=dbg` built neither check; it
was found writing this document, by comparing the symbol tables.

**Both shipped configurations are verified on hardware, every time.** They fail differently:

- **Release** is the only mode in which the compiler optimises, and two of this port's defects
  existed only there: F62, a HardFault because the optimiser narrowed a correct word read of the
  UID registers into the byte loads the silicon rejects — the release build had never booted, and
  the `-O0` build was fine — and F70, a bounded transmit wait the optimiser deleted. A host test
  compiles different code with a different compiler for a different instruction set, and
  fastbuild is `-O0` — so release on the board is the *only* place such a defect can show.
- **Debug** is the only mode with the error checks compiled in. A NetX Duo misuse, a thread stack
  that came close to overflowing without faulting, a leaked packet — debug reports them; release
  carries on with undefined behaviour.

fastbuild sits between the two and is neither, which is why a passing run on a fastbuild image —
what `bazel build` gives you — is a good development signal and **not** a verification.

The host suites are also run in all three modes: the host compiler at `-O2` and at `-O0` genuinely
builds different code, and undefined behaviour one optimiser tolerates the other may not.

---

## The bench

What the board layers need, set up once.

**Hardware.** A NUCLEO-H563ZI (MB1404) with **JP6 on** (it connects PB15 to RMII_TXD1); its
ST-Link USB to the host, which gives `/dev/ttyACM0` and SWD; and an Ethernet cable **direct** to a
host NIC with a PTP hardware clock. This bench uses an Intel i226 (`igc`) on `enp5s0`. Not a
switch: one silently discarded every Delay_Resp here (F42).

**Host software.**

| | |
|---|---|
| Bazel | the version in `.bazelversion`; everything else the build needs it fetches |
| `pyserial` | `pip install pyserial` |
| linuxptp | `sudo apt install linuxptp` — ptp4l 4.x |
| ST's OpenOCD | built by Bazel; see below |
| udev rule | non-root access to the ST-Link |

The harness finds OpenOCD at `$OPENOCD`, then `toolchains/openocd/bin/openocd`, then on `PATH`,
and it must be ST's fork — upstream 0.12.0 cannot program an STM32H5. Bazel builds the fork for
the flash target; copy it out once for the harness:

```bash
bazel build //tools/flash:openocd_binary
mkdir -p toolchains && cp -rL --no-preserve=mode bazel-bin/external/+openocd+openocd_src/openocd_build toolchains/openocd
chmod +x toolchains/openocd/bin/openocd
sudo cp "$(bazel info output_base)/external/+openocd+openocd_src/contrib/60-openocd.rules" /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

**The bench services, installed once** — DHCP for the board and the three ptp4l roles, so that
afterwards nothing needs sudo:

```bash
sudo tools/hil/privileged/install.sh
```

That installs `flexptp-hil-dhcp`, `flexptp-ptp4l-default`, `flexptp-ptp4l-gptp` and
`flexptp-ptp4l-slave`, a polkit rule letting one user start and stop exactly those four, and a
NetworkManager profile pinning `192.168.2.1/24` on the interface. The scenarios start the unit
each needs and restore the default grandmaster afterwards. See
[`tools/hil/README.md`](../../../../tools/hil/README.md#network-checks-need-the-host-set-up-once).

**Check the bench before trusting it.**

```bash
systemctl start flexptp-hil-dhcp flexptp-ptp4l-default
tools/hil/run.py --flash bazel-bin/projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp-demo.elf --stage S0
```

S0 passing means the board flashes, boots, answers on the console, gets an address, pings, and
synchronises to the host. A failure here is almost always the bench, and the
[symptom table in host_ptp_setup.md](host_ptp_setup.md#if-nothing-happens) is the place to start.

---

## Procedures

Each procedure says what to run and what must hold. Results are tied to images by **build ID**:
every image carries a SHA-1 of its contents, `osinfo` prints it, and both runners compare it with
the ELF they were given — a run against a board holding a different image fails, and a scenario
run refuses to start. Record the build ID with every result.

### 1. Every change

```bash
bazel test //...
bazel build //projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo:flexptp-demo \
            //projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo:flexptp-demo-freertos
```

If the change touches firmware, also run S8 on each affected firmware, on the image just built:

```bash
tools/hil/run.py --flash bazel-bin/projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/flexptp-demo.elf --stage S8
tools/hil/run.py --firmware freertos --flash bazel-bin/projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo/flexptp-demo-freertos.elf --stage S8
```

and the scenarios that exercise what changed (`tools/hil/scenario.py --list`). Code in
`//projects/flexptp_demo_stm32h563/lib` is in **both** firmwares; test both.

### 2. Full verification: the matrix

Before a release, before pushing a firmware or dependency change, and after anything that could
plausibly change timing or memory: **both firmwares × release and debug**, each through the host
suites, S8 and every scenario. One command:

```bash
tools/hil/matrix.sh
```

[`tools/hil/matrix.sh`](../../../../tools/hil/matrix.sh) does, in order:

1. `bazel test //...` in fastbuild, `--config=opt` and `--config=dbg`;
2. builds the four images, copies each to `build/matrix-<date>/<config>-<firmware>.elf`, and
   records its build ID and size in `images.txt`;
3. for each image: `run.py --flash <elf> --elf <elf> --stage S8`, then
   `scenario.py --elf <elf> all` — skipping the scenarios for an image that failed S8;
4. writes every console log, every JSON report and a `summary.txt`, and exits 0 only if
   everything passed.

About **4½ hours** with the board attached: release ThreadX ~55 min, release FreeRTOS ~65, the
same again for debug. Options run a subset — `--configs opt`, `--firmwares threadx`,
`--stage S0`, `--scenarios fixes` (a preset, or scenario names, or `""` for none), `--build-only`,
`--skip-host`. A quick confidence pass of the script itself:

```bash
tools/hil/matrix.sh --skip-host --configs opt --firmwares threadx --stage S0 --scenarios health
```

**What must hold:**

| Step | Pass |
|---|---|
| host suites, all three modes | every target passes |
| S8, ThreadX | 14/14, exit 0 |
| S8, FreeRTOS | 13/14 with exactly one SKIP, `threads` (FreeRTOS's `osinfo` does not enumerate threads); the runner exits 3, "incomplete", and `matrix.sh` accepts that one skip and no other |
| scenarios, ThreadX | 18 scenarios (`u4` and `pup` are lwIP-only), 0 FAIL |
| scenarios, FreeRTOS | 20 scenarios, 0 FAIL |

`OBSERVED` lines are recorded, not judged: they are behaviour measured on every image including
the pre-session baseline — the CLI starved during a saturation flood, ptp4l losing the board
during one — and asserting them would make the suite permanently red on healthy firmware. What is
asserted is what must hold *afterwards*. See
[`tools/hil/README.md`](../../../../tools/hil/README.md#pass-fail-and-observed).

Keep the results directory. The ELFs are what `swd.py` needs if anything has to be read back
later, and the build IDs are what the board's results mean.

### 3. A flexPTP fix

The firmware carries upstream fixes as patches, applied by the Bazel fetch
([`patches/flexptp/README.md`](../../../../patches/flexptp/README.md)). A new one is tested at
four levels:

1. **On the host, with and without it.** A test in `//patches/flexptp/test` that drives flexPTP's
   real engine into the defect, and fails without the fix:

   ```bash
   C=~/git/flexPTP-fixes                     # the working clone; see tools/upstream/README.md
   git -C $C checkout series/rc26~1          # the stack without the newest fix
   bazel test --override_repository=flexptp=$C //patches/flexptp/test:all    # its test fails
   git -C $C checkout series/rc26
   bazel test --override_repository=flexptp=$C //patches/flexptp/test:all    # all pass
   ```

   Then on the PR branch alone (one commit on bare upstream): it must pass its own test and fail
   only the other fixes' tests.
2. **The patch stack applies exactly:** regenerated from the series, applied with plain
   `patch -p1` to bare upstream with no offset and no fuzz, the patched tree identical to the
   series, and `git patch-id --stable` matching each series commit to its PR branch.
3. **On the board, before and after** ([§5](#5-proving-a-fix-before-and-after)): the scenario or
   probe that shows the defect, on an image without the fix and the same tree with it.
4. **The matrix** ([§2](#2-full-verification-the-matrix)) on the final images.

Rebuilding after a change to a patch's commit message only must give the same build IDs — that is
the proof the message edit changed nothing that runs.

### 4. A dependency bump

1. Move the pin in `bazel/repositories.bzl` and run `bazel mod deps --lockfile_mode=update` —
   the build runs with `--lockfile_mode=error`, so any change to that file, comments included,
   needs it.
2. `bazel test //...`.
3. Compare the images with the previous pin's. If the build IDs are **identical** the bump
   changed nothing this firmware links — the HAL bump of November 2026 was exactly that — and S8
   on each firmware is enough. Otherwise run the matrix.
4. For flexPTP, also rebase the patch series onto the new pin and check each patch that should
   have merged really did (`git apply -R --check`), as
   [`patches/flexptp/README.md`](../../../../patches/flexptp/README.md#when-a-pull-request-merges)
   describes.

### 5. Proving a fix: before and after

A claim that something is fixed is a measurement, not an argument. The method used for every fix
in this repository:

- **Two images from the same tree**, differing only by the change, each identified by build ID.
- **The same test on both**, chosen to show the defect: a scenario, a check, or a probe written for
  it. It must fail on the first image and pass on the second; a test that passes on both proves
  nothing about the fix.
- **More than one trial** where the defect is timing-dependent — U14 was 0/5 without and 5/5 with.
- **A host test where one is possible**, mutation-tested ([below](#mutation-testing-is-the-entry-fee)),
  so the fix cannot silently regress.
- **Read the board when the console cannot tell you.** A defect that leaves the board alive but
  wrong is often only visible over SWD: U14 was found by reading flexPTP's queues
  (`swd.py ptpq`), the three FreeRTOS wedges by reading the stack, the heap and the console thread.

### 6. Characterisation and soak

The matrix asserts behaviour; it does not characterise accuracy. Two longer measurements exist
for that, and are worth repeating after anything that touches the servo, the timestamp path or
the clock tree:

- **Layer 4 accuracy**: a 15-minute steady-state run per firmware against the hardware-timestamping
  host, RMS and p99 offset — method and results in [l4_performance.md](l4_performance.md).
- **Long floods**: `tools/hil/run.py --elf <elf> --checks dhcp,ptp_flood --flood-seconds 600`,
  and the converged gPTP figure from the `gptp` scenario. Note that the converged gPTP offset
  moves by a factor of four from run to run on the same image; a single run says little.

---

## The layers in detail

### 1. Host unit tests

Pure logic, compiled by the host compiler. The drivers and the NSD logic units live in
`//projects/flexptp_demo_stm32h563/lib`, beside the code they test; the console FIFO and the CLI
tests are in the ThreadX project's `test/` because they link a real kernel.

| Target | Cases | What it pins |
|---|---|---|
| `lib:mac_drv_rx_ring_test` | 13 | Receive descriptor state machine: re-delivery (**F61**), the RBU tail-pointer restart (**F71**), copy-pointer advance (**F58**), and the data/context descriptor pairing rules (**NX-01**) |
| `lib:mac_drv_tx_ring_test` | 10 | Transmit completion ordering, including across the 16-bit counter wrap; dropped-vs-sent accounting (**F75**); the per-pass count reset (**F57**) |
| `lib:eth_link_state_test` | 7 | The PHY transition rules (**CX-06**), including the first poll not announcing `DOWN` before auto-negotiation has finished |
| `lib:eth_framer_test` | 9 | The framing decisions lifted out of `drv_send()` (**TS-04**): EtherType mapping, the msw/lsw address split, the saturating chain append |
| `lib:host_harness_test` | 6 | The harness itself — 32-bit arena and function-pointer round-trips. These fail *first* and say why, because if they regress the driver tests corrupt memory rather than failing cleanly |
| `lib:ptp_tx_registry_test` | 14 | **The ABA hazard.** NetX Duo recycles packets, so an entry left behind after consumption would be claimed by a later, unrelated packet at the same address — feeding the servo a real timestamp under the wrong message uid |
| `lib:ptp_ts_codec_test` | 8 | The 12-byte headroom precondition. Below it the three ULONGs were never written, so what is there is payload; returning zeroed-and-false beats a plausible wrong time (the **F59** lesson) |
| `lib:ptp_endpoint_test` | 9 | Five cases of transport × delay mechanism. A wrong destination presents as "no response received", not as a routing bug |
| `lib:ptp_nsd_lifecycle_test` | 14 | **Transactional configuration** (**NX-02**): every socket created is deleted, a failure part-way leaves nothing behind, a failed configure leaves the transport unset |
| `lib:sysmem_bounds_test` | 11 | `_sbrk()`'s window (**NX-03**): the newlib heap bounded at `__RAM_segment_used_end__`, not at the stack, so it cannot grow into the RTOS's memory |
| `flexptp_threadx_netx_demo/test:test_blocking_fifo` | 13 | **F67.** Occupancy is a modular difference of two free-running totals, so a full FIFO reads as full and the arithmetic survives the 2³² wrap |
| `flexptp_threadx_netx_demo/test:test_cli` | 15 | **F74.** A token of exactly `MAX_TOK_LEN` is terminated; a full-length line does not overrun its copy buffer; `ptp servo st` and `ptp servo stm` dispatch to different handlers |

`mac_drv.c` needed **no changes at all** to become testable — 97 of its 98 register accesses go
through its `ETH_TypeDef *` parameter — and the register definitions are ST's real ones, so a wrong
bit position cannot make a test lie. How that works, the 32-bit accommodations and the DMA model's
known omissions are in [`test/README.md`](../test/README.md).

### 2. Real kernels on the host

One defect class bit this port three times, always the same shape: **a blocking call from a
context that may not block** — the transmit timestamp from `ETH_IRQHandler`, the heartbeat from
ThreadX's timer thread (**F80**: every heartbeat rejected, no master ever selected), and a comment
that reasoned one was safe because it was not the other. A comment cannot stop the fourth. These
can, because they run the real kernels:

| Target | Kernel | Cases | What it proves |
|---|---|---|---|
| `flexptp_threadx_netx_demo/test:test_rtos_context` | ThreadX `ports/linux/gnu` + ST's CMSIS-RTOS2 wrapper, configured by the firmware's own `tx_user.h` | 7 | From a real `osTimer` callback, `osMessageQueuePut(..., osWaitForever)` **must fail** (−2) and the non-blocking put must succeed — ThreadX's own `txe_queue_send.c`, identical on every port. And the wrapper refuses a blocking call from a context reporting IRQ mode |
| `flexptp_freertos_lwip_demo/test:test_rtos_context` | FreeRTOS POSIX port + the CMSIS-RTOS2 wrapper | 7 | The same questions re-derived for FreeRTOS, where the answers differ: a timer callback is *not* interrupt context and may block; only the ISR half of the rule ports across |
| `patches/flexptp/test:teardown_test` | FreeRTOS POSIX port + flexPTP's FreeRTOS binding, with an "electric fence" heap | 3 | **U11**: a TX timestamp after `unreg_task_ptp()` touches no deleted queue; a stop while the PTP task holds a lock leaves nothing held; stop/start cycles leak nothing |

Be precise about the IRQ half: the host shim's `__get_IPSR()` is settable, so a test can make the
wrapper *believe* it is in an interrupt and assert its guard. Nothing is preempted and there is no
exception frame; it proves the wrapper's guard, not the silicon.

### 3. flexPTP's real engine on the host

`//patches/flexptp/test` builds flexPTP from its own sources — the patched tree the firmware uses
— with a fake hardware port and network-stack driver, mostly in flexPTP's own OS-less mode, where
the engine is synchronous and a test can assert exactly what it did with each message:

| Target | Cases | What it proves |
|---|---|---|
| `rx_validation_test` | 13 | **U12, U13**: ten interference patterns — truncated, wrong-version, foreign and out-of-order messages — each placed before a Sync and between it and its Follow_Up, judged by completed cycles, Syncs accepted and clock steps; three controls. The two slave controls also guard **U1** |
| `notification_test` | 2 | **U14**: the PTP task starved while heartbeats and a peer's Pdelay_Reqs arrive, then every later Pdelay_Req answered at once, with its own sequenceId |
| `teardown_test` | 3 | **U11**, above |

Each fix's tests fail without it and pass with it, on the patch stack and on each PR branch alone;
the override recipe is in [`patches/flexptp/README.md`](../../../../patches/flexptp/README.md#host-tests).

### 4. Checks on the linked image

| Target | Asserts |
|---|---|
| `tools/ci:cxx_rules_test`, `cxx_rules_freertos_test` | the linked image contains no `operator new`/`delete`, no `__cxa_throw`, no RTTI (finding CX-07) — checked against the image, not the sources |
| `tools/ci:seeding_test` | every file both projects have is either identical or explicitly per-project, so a fix cannot exist in one firmware and be silently missing from the other |

The build itself is a check too: first-party code compiles with `-Wall -Wextra -Werror`, against a
compiler pinned by sha256, so a warning fails the build rather than a log scan (decision D13).

### 5. Tool tests

`tools/hil:scenarios_test` (10) pins the parsers that decide PASS or FAIL on the board — a
`ptp info` reply cut off mid-line once read `00a0c9` as a clock identity — and the malformed-PTP
packet builders. `tools/hil:swd_test` (15) drives the SWD readers over synthetic target memory laid
out by hand, including every corruption actually found: a stack overflowed into its own heap header,
a looping free list, a stacked PC of `0xfffffe00` with T clear, and flexPTP's queues as the stuck
ThreadX board had them. `tools/hil:find_openocd_test` (6) pins the OpenOCD search order, and
`tools/flash:openocd_capability_test` asserts the selected OpenOCD is ST's fork with the
`stm32h5x` driver — a build that "succeeded" does not imply a usable flasher.

### 6. HIL checks

`tools/hil/run.py` drives the board's serial CLI over pyserial, with OpenOCD for flash and reset.
Fourteen named checks; S8 runs all of them, in this order:

| Check | Asserts |
|---|---|
| `build_id` | **the board is running the image this run was given** |
| `boot` | PHY identified, link up |
| `cli` | command table populated (26 of 64 slots on both firmwares) |
| `osinfo` | kernel version and free memory |
| `cpp_ctor` | static C++ constructors ran before `main` |
| `threads` | the documented thread set and **every** priority (**TS-05**); ThreadX only |
| `irq_prio` | exception and interrupt priorities match the documented table, per firmware (**NX-04**) |
| `ptp_start` | addend programmed to the computed nominal |
| `dhcp` | address acquired |
| `ping` | round trip from the host, loss and latency |
| `ptp_ts` | T2 and T3 both advancing — before `ptp_sync`, because a servo missing T3 can still look converged |
| `ptp_sync` | `SLAVE`, settled for 60 s, offset sampled over 30 Syncs |
| `counters` | every drop and refusal counter zero, read from `ethstat`'s `key=value` lines (**GI-02**) |
| `ptp_flood` | 120 s saturation flood (~76 000 pps): afterwards the CLI answers, ping is clean, PTP is back in SLAVE; `ptp.drop_txts` ≤ 16 (**U5**) |

`--firmware freertos` selects the per-firmware expectations — the interrupt table differs because
the two kernels require different things of SysTick and SVCall. The earlier stages (S0, S1, S3–S7)
are the subsets that were meaningful at each stage of the ThreadX port; S0 is the quick bench check.
`--compare tools/hil/baseline.json` diffs against values captured from the pre-port firmware.

### 7. HIL scenarios

`tools/hil/scenario.py` runs the long tests, each minutes long, driving the host's ptp4l units,
flooding the link or speaking PTP at the board. Presets:

| Preset | Scenarios | What it covers |
|---|---|---|
| `fixes` | `u1` `u2` `u3` `u4` `pup` `gptp` `smss` `stopstart` `stopstart_busy` `master_delresp` | one regression test per upstream finding, gPTP convergence, SLAVE → MASTER → SLAVE, stop/start |
| `load` | `gptp_flood` `master_flood` `stopstart_flood` `cli_flood` `cli_load` `stopstart_load` `console_flood` | saturation and 20 kpps floods as slave, as master, during stop/start, with CLI traffic, and silent |
| `robust` | `fuzz` | nine classes of malformed and foreign PTP traffic (**U12**, **U13**) |
| `finish` | `stacks` `health` | stack headroom over SWD after the deepest commands; the end-of-run gate |
| `all` | all of the above | |

With `--elf`, a scenario run refuses to start on a build-ID mismatch, and the SWD parts run: the
console thread after `console_flood`, every stack in `stacks`, the heap walk and fault decode in
`health`, and flexPTP's queues after `gptp_flood` and `master_flood` (**U14**). The full table, the
fuzz classes and the reasoning are in [`tools/hil/README.md`](../../../../tools/hil/README.md#scenarios-the-long-tests).

### 8. SWD inspection

`tools/hil/swd.py --elf <elf> {snapshot,heap,stacks,fault,console,ptpq,all}` reads the target from
symbols alone — it needs no debug info, so it works on a release image — in one short halt per
operation, and always resumes the core. It is the tool for a board that is alive but wrong, or dead:
what to run for each symptom is in
[`tools/hil/README.md`](../../../../tools/hil/README.md#reading-a-wedged-board).

---

## Reading results

| Runner | Exit status |
|---|---|
| `run.py` | `0` all passed · `1` something failed or regressed · `2` board unreachable · `3` passed but incomplete |
| `scenario.py` | `0` no assertion failed · non-zero otherwise, including a build-ID refusal |
| `matrix.sh` | `0` every step passed · `1` otherwise |

**A `SKIP` is not a pass.** "We could not test it" and "it works" are different claims, and
conflating them is how F42 survived for days. With no `--elf` and no `--flash`, `build_id` SKIPs
and the run is incomplete: no claim was made about which image was tested, so none is checked.

Bench faults that look like firmware faults, all seen here:

| Looks like | Was |
|---|---|
| `ptp_sync` or `health` finds no Sync samples just after a flash | the board had not had time to select a master and settle; S8 waits for that, a bare `health` does not |
| the board elects itself master (`56001e…`) in the default profile | the host is running the **gPTP** ptp4l unit, left by an interrupted script: `systemctl start flexptp-ptp4l-default` |
| Sync arrives, Delay_Resp never does | a switch in the path — use the direct cable (F42) |
| a frozen tick, no ping, no console | the core left halted by an SWD script that died between `halt` and `resume`; `swd.py` always resumes, ad-hoc OpenOCD sessions may not |

---

## What is reachable, measured

Derived against this repository's own finding log rather than estimated. Of the ~23 findings in
[port_review.md](port_review.md) that fall in code the ThreadX port wrote or rewrote:

| Reach | Share | Example findings |
|---|---|---|
| Catchable by a host test | ~50% | F57, F61, F58, F75, F54, F69, F67, F74, F59 |
| Regression-lockable, but discovery needed hardware | ~25% | F71, F76, F52, F68, F72, F73 |
| Irreducibly hardware | ~25% | F70, F62, F51, F53, F42, and the clock-tree findings |

The full working is in [threadx_porting_plan.md](threadx_porting_plan.md) under *How far mocks
actually reach*. The findings since bear it out: of the November 2026 set, U12–U14 became
host-testable once found, while the three FreeRTOS wedges — a CLI stack overflow under preemption,
a stale `pbuf->tx_cb` called from the ETH ISR, a console thread one completion flag ahead — and
U14's discovery all needed a flooded board.

> **The asymmetry matters more than the ratio.** Mocks are excellent at *preventing recurrence*
> and poor at *first discovery*. F71 is the clearest case: the RX ring's RBU latch needs a
> tail-pointer write to restart, and a model would only reproduce that if its author already
> knew. The model would have been wrong in exactly the way the code was wrong, and the suite
> would have been green.

---

## Mutation testing is the entry fee

A characterisation test that cannot fail is worse than no test — it costs the same to maintain
and buys a false sense of coverage. Every finding pinned by a host test was mutation-tested: the
defect reintroduced, the suite re-run, the source restored and verified byte-identical. For the
flexPTP fixes the mutation is the fix's absence, and the override recipe above runs it.

| Mutation | Result |
|---|---|
| Drop `!(bd)->ext.inFlight` from `ETHHW_DESC_NEEDS_PROCESSING` (**F61**) | `RxRing.DoesNotRedeliverAFrameStillInFlight` fails |
| Remove the `DMACRDTPR` write from `ETHHW_RestoreRXDesc` (**F71**) | `RxRing.RestoringWritesTheTailPointerToRestartTheEngine` fails |
| Hoist `found` out of `ETHHW_ProcessTx`'s retry loop (**F57**) | six `TxRing` tests **time out** |
| Widen the ordering delta to `int32_t` | `TxRing.DispatchesInOrderAcrossTheSixteenBitCounterWrap` fails |
| `ptp_tx_registry_take()` reads instead of clearing | 5 tests fail |
| `ptp_tx_registry_cancel()` does nothing | 1 test fails |
| Drop the codec's headroom precondition | 1 test fails |
| Remove the registry's stale-entry eviction (**AR-02**) | 1 test fails |
| Announce `DOWN` on the first PHY poll (**CX-06**) | 1 `LinkState` test fails |
| Publish the link to the stack only on `UP` | 2 `LinkState` tests fail |
| Evict at `MAX_AGE` 1 instead of 16 | 3 tests fail — the bound has to be wrong in *both* directions to be worth having |
| Swap the msw/lsw halves of the destination MAC (**TS-04**) | 1 `EthFramer` test fails |
| Off-by-one in the saturating chain append | 2 `EthFramer` tests fail |
| Transpose the ARP and RARP EtherTypes | 1 `EthFramer` test fails |
| Derive FIFO occupancy from the two indices (**F67/N1**) | 4 `Fifo` tests fail — one of them by *timing out*, because a full FIFO reporting empty stalls the blocking push |
| `strncpy` without the terminator (**F74/N12**) | 1 `Cli` test fails |
| Shrink the CLI line-copy buffer by one (**F74/N8**) | 1 `Cli` test fails |
| Match commands on a prefix rather than the whole token | 1 `Cli` test fails |
| Endpoint teardown stops zeroing | 1 test fails |
| Widen the RFC 1112 mask from 23 to 24 bits | 1 test fails — *but only after the test was fixed* |
| Assert the timer-thread blocking put SUCCEEDS (**F80** inverted) | `RtosContext.BlockingEnqueueFromATimerCallbackIsRejected` fails, reporting −2 vs 0 |
| Drop the context descriptor's `OWN` check (**NX-01**) | 2 `RxRing` tests fail — a frame is delivered stamped `1 080 035 208 ns`, the buffer address |
| Drop the stray-context-descriptor skip (**NX-01**) | `RxRing.DoesNotMistakeAStrayContextDescriptorForAFrame` fails |
| Drop the `CTXT` test on the context slot (**NX-01**) | `RxRing.DoesNotReadTheNextFrameAsATimestamp` fails — **and this test exists only because that mutation survived the first round** |
| Mark the sockets created only once the whole sequence succeeded (**NX-02**) | 3 `Lifecycle` tests fail, reporting a socket the stack still holds |
| Leave the transport set across a teardown (**NX-02**) | 4 `Lifecycle` tests fail |
| Remove `0011` (**U11**) | both `Teardown.U11_*` tests fail: a SIGSEGV on the deleted `sTxCbFIFO`, a console mutex left held |
| Remove `0012` (**U12**) | the five `Engine.U12_*` tests fail |
| Remove `0013` (**U13**) | the five `Engine.U13_*` tests fail |
| Remove `0014` (**U14**) | `Peer.U14_AfterABurstEveryPdelayReqIsStillAnswered` fails: 0/10 answered on time |
| Remove `0001` (**U1**) | the two slave controls in `rx_validation_test` fail: the BMCA records a byte-reversed grandmaster |

Do the same for any new test: break the thing, watch it go red, put it back.

What this exercise taught, each lesson of which changed the code:

- **F57 presents as a hang, not a wrong value.** With the per-pass count hoisted, `found` never
  reaches zero and the drain loop spins forever. A test without a timeout wedges the suite instead
  of failing it — which is what Bazel's test `size` and its timeout are for.
- **A third NX-01 branch survived the first mutation round.** Checking that the context slot is
  application-owned is not sufficient, because an ordinary frame in that slot is
  application-owned too — so `if (1)` in place of the `CTXT` test passed the whole suite. The
  discriminating case needs a TSA frame with a *normal* frame behind it.
- **The first wrap-ordering test could not fail.** Four timestamped frames straddling the wrap are
  ordered identically by correct modular arithmetic and by the buggy widened subtraction. A
  discriminating case needs two candidates pending *simultaneously* on opposite sides of the wrap,
  which is why that test sends untimestamped filler frames to open a gap.
- **One test was vacuous.** The RFC 1112 mask check originally asserted against 224.0.1.129, whose
  third octet is `0x00` — so it passed whether the mask was `0x7F` or `0xFF`. It now uses
  239.129.1.1, whose third octet puts the bit in play.
- **A host test that depends on leftover state is not a test.** The first `rx_validation_test`
  results changed with test order: `reg_task_ptp()` queues a reset that a later test then
  processed. Each fixture now processes it in `SetUp()`, and asserts the core really was reset.

---

## What CI gates

`.github/workflows/ci.yml`, on every push and pull request:

| Job | Asserts |
|---|---|
| **Build** (both firmwares × `dbg`, `opt`) | all four images compile and link, with **zero first-party warnings** (`-Werror`) |
| **Tests** | `bazel test //...`: layers 1–5, including building ST's OpenOCD from source — a clean clone can flash |
| **Static analysis** | `cppcheck` over each firmware project's own directory |
| **Docs cross-references** | every relative link and in-document anchor in the Markdown resolves |

**The board is not in CI.** Layers 6–8 are run by hand, by the procedures above, and their results
are recorded with build IDs in [`docs/flexptp_upstream_review.md`](../../../../docs/flexptp_upstream_review.md)
and in the [latest run](#latest-full-verification) below.

---

## Known gaps

Stated plainly, because a gap that is written down is a decision and a gap that is not is an
accident.

- **Codegen defects are invisible to the host layers.** F70 and F62 are properties of the
  optimised ARM build. The host tests compile different code, with a different compiler, for a
  different instruction set. A green host run is *no evidence* about either — which is why the
  release image is verified on the board every time.
- **Memory ordering is not modelled.** `__DSB`/`__DMB` are no-ops in the host shim.
- **`-fanalyzer` did not survive the Bazel migration.** The CMake host suite compiled the
  first-party host targets with GCC's interprocedural analyser; the plan said to carry it over as a
  non-`-Werror` copt ([bazel_migration.md §6](../../../../docs/bazel_migration.md#6-host-tests)),
  and it was not. Restoring it is one copt on the `lib` host targets.
- **cppcheck no longer reaches the shared code.** CI analyses each project's directory; the driver,
  the NSD units, the CLI and the console moved to `//projects/flexptp_demo_stm32h563/lib` in phase 9
  and are outside both scans.
- **FreeRTOS's `threads` check is a permanent SKIP.** Its `osinfo` does not enumerate tasks, so the
  documented FreeRTOS task priorities are not asserted on the board.
- **All three receive drop counters are instrumented and none has ever fired** under ordinary
  load: 53 000 UDP frames at line rate, and again with the pool cut from 8 to 2; 70 600 frames of
  1 400 B in 8 s, 0 dropped. A counter that has never incremented is a counter whose increment has
  never been tested.
- **The `ETHHW_Init()` GPIO/RCC/NVIC preamble has no seam.** It is the one unparameterised block
  in `mac_drv.c`; the descriptor state machine was the part with the finding history.
- **The real network stack never runs on the host.** Real NetX Duo over a RAM driver was planned
  as S5e option A and deliberately skipped — see [threadx_s5e_plan.md](threadx_s5e_plan.md). The
  board layers cover it instead.
- **The CLI is starved during a saturation flood**, on both firmwares and on every image measured,
  and on FreeRTOS + lwIP down to 20 kpps. Recorded as OBSERVED, not asserted; what is asserted is
  that it comes back.
- **Timing, preemption and races stay on the bench**, as does everything downstream of the
  timestamp capture itself — the PHY, the clock tree and the wire.

---

## Latest full verification

2026-11-27, tree `b742e43` with the `DEBUG` fix to `toolchain/BUILD.bazel`, all nine flexPTP
patches, every dependency at upstream head. Host suites: 24/24 targets in fastbuild, `dbg` and
`opt`. On the board:

| Image | Build ID | Text | S8 | Scenarios |
|---|---|---|---|---|
| release ThreadX | `1363dd8b` | 148 640 B | 14/14 | 18, **105/105** (51 min) |
| release FreeRTOS | `208f134e` | 162 656 B | 13/14, `threads` SKIP | 20, **113/113** (58 min) |
| debug ThreadX | `0f906149` | 221 840 B | 14/14 | 18, **105/105** (51 min) |
| debug FreeRTOS | `d0e1f0e3` | 224 432 B | 13/14, `threads` SKIP | 20, **113/113** (58 min) |

Every SWD check passed on all four: flexPTP's queues in step after both flood scenarios, the
console thread in step, every stack with at least 256 B untouched, and the FreeRTOS heap
consistent. Earlier campaigns, and the before/after evidence for each fix, are in
[`docs/flexptp_upstream_review.md`](../../../../docs/flexptp_upstream_review.md).
