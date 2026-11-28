# Host unit tests

Pure-logic tests that run on the development machine, with no board attached — and the mechanics
that let a register-level Ethernet driver and a real RTOS run there.

```bash
bazel test //...                                                     # everything, host and target
bazel test //projects/flexptp_demo_stm32h563/lib/...                 # the drivers and NSD units
bazel test //projects/flexptp_demo_stm32h563/flexptp_threadx_netx_demo/test:all
```

They run in seconds and gate every push in CI, which the hardware suite in
[`tools/hil/`](../../../../tools/hil/README.md) cannot. What every target pins, how the host layers
relate to the board layers, and how to run the whole verification are in
[`docs/testing_architecture.md`](../docs/testing_architecture.md).

## Where the tests live

| Package | Targets | Why there |
|---|---|---|
| `//projects/flexptp_demo_stm32h563/lib` | the MAC driver's RX and TX rings, link state, framer, harness; the four NSD logic units; `_sbrk()`'s bounds | beside the shared code they test, which both firmwares link |
| this directory | `test_blocking_fifo`, `test_cli`, `test_rtos_context` | they link **real ThreadX** (its Linux port), configured by this firmware's `tx_user.h` |
| `//projects/flexptp_demo_stm32h563/flexptp_freertos_lwip_demo/test` | `test_rtos_context` | the same context rules, re-derived for FreeRTOS's POSIX port |
| `//patches/flexptp/test` | `rx_validation_test`, `notification_test`, `teardown_test` | flexPTP's own engine, as patched — see [`patches/flexptp/README.md`](../../../../patches/flexptp/README.md#host-tests) |

Host and target are two platforms in one Bazel graph, so none of these needs a separate build
root. The firmware targets carry a transition onto the board's platform; the tests do not, and
build for the host.

## What is tested, and what is not

Measured against this repository's own finding log: **roughly half** of the findings in the code
the ThreadX port touched would have been caught here, a quarter become regression-lockable once
discovered on hardware, and a quarter never will be. The working is in
[`docs/threadx_porting_plan.md`](../docs/threadx_porting_plan.md) under *How far mocks actually
reach*.

Specifically **not** reachable, and no amount of test writing changes it:

- **Codegen defects.** F70 (a bounded wait the optimiser deleted) and F62 (a HardFault from
  byte loads the optimiser made of a correct word read) are properties of the optimised ARM
  build. These tests compile different code, with a different compiler, for a different
  instruction set. A green run here is *no evidence* about either — which is why the release
  image is verified on the board.
- **Memory ordering.** `__DSB`/`__DMB` are no-ops in the host shim.
- **Anything downstream of the timestamp capture**, the PHY, the clock tree, or the wire.

Static analysis rides along. The first-party code these targets compile — the NSD units, the
framer and link-state rules, the driver through `host_mac_drv` and the Ethernet model in `lib`,
the FIFO and the CLI here — is built with GCC's `-fanalyzer`. The GoogleTest files beside it are
not: analysing one takes minutes, and the time goes on gtest. A finding is a warning rather than a
failure, and it appears in the run whose compile produces it. Why, and what it cannot see:
`HOST_ANALYZER` in [`lib/BUILD.bazel`](../../lib/BUILD.bazel), and
[`docs/testing_architecture.md`](../docs/testing_architecture.md#known-gaps).

## How the driver is compiled for the host

`mac_drv.c` needed **no changes at all** to become testable, which is worth understanding before
adding to this suite. The wiring is `host_mac_drv` and its comments in
[`lib/BUILD.bazel`](../../lib/BUILD.bazel).

- **The seam was already there.** 97 of the driver's 98 register accesses go through its
  `ETH_TypeDef *eth` parameter, so they follow whatever pointer a test passes.
- **The rest is redirected by macro.** `lib/eth/mac_drv_uut.c` `#include`s the driver's `.c`
  verbatim after pointing `ETH`, `RCC`, `GPIOx` and `SBS` at host memory. Including the `.c` also
  exposes the file-static helpers.
- **The register definitions are ST's real ones.** `lib/host/` shadows exactly three ARM headers
  — `core_cm33.h`, `system_stm32h5xx.h`, `cmsis_compiler.h` — and must come first on the include
  path (ARM's `cmsis_gcc.h` emits `mrs r0, ipsr`, which x86 cannot assemble). Everything below
  that is ST's genuine CMSIS device header and HAL headers, from the pinned HAL. Nothing is
  hand-copied, so a wrong bit position cannot make a test lie, and a signature drift is a compile
  error: that already caught `HAL_GPIO_Init` taking a `const` pointer.

### The two 32-bit accommodations

The driver stores addresses in `uint32_t` fields because on a Cortex-M33 that is what a pointer
is. On x86-64 those casts truncate. Two things make the round-trip lossless:

| | |
|---|---|
| **`MAP_32BIT` arena** (`lib/host/host_arena.c`) | descriptors, buffers and the state block live below 2 GB, so `(uint32_t)ptr` survives |
| **`-no-pie`** (`lib/BUILD.bazel`) | puts `.text` low enough that function pointers survive `ETHHW_DescExt.tsCbPtr`, which is also a `uint32_t` |

`HostHarness.ArenaReturnsThirtyTwoBitAddresses` and `HostHarness.FunctionPointersFitInThirtyTwoBits`
guard both. If either regresses the driver tests would not fail cleanly — they would corrupt
memory or jump into nowhere — so those two fail first and say why.

Under `--config=asan` the arena also poisons everything it has not handed out, with a gap in
front of each allocation, and the model gives the TX ring an allocation of its own — the driver
takes it by its own pointer, so only the state block and the RX ring have to be adjacent. A
driver that runs off the end of either ring, or of the frame buffers, is then reported at the
access. Before, writing one descriptor past the RX ring landed on the first TX descriptor, which
initialisation clears a few lines later, and every test passed. The frame buffers themselves
stay back to back, because the driver strides through one block, so a frame overrunning into the
next buffer is still not caught.

`-m32` would be strictly better, matching the target's whole ABI rather than just pointer width,
but it needs `gcc-multilib`.

## The DMA model

`lib/eth/eth_model.hpp` plays the part of the Ethernet DMA: it clears OWN bits, fills lengths and
status flags, and places timestamps in context descriptors. It is deliberately small and
readable, and its **known omissions are listed at the top of the header** rather than left
implicit — because a hardware model that is wrong is a test that passes for the wrong reason.

The most important omission: **no RBU/suspend semantics.** The real engine, on reaching a
descriptor the application still owns, raises RBU and stops until the tail pointer is written
again. That is finding F71, and it is precisely the case where a naively written model would
have been wrong in the same way the code was. `RestoringWritesTheTailPointerToRestartTheEngine`
asserts the driver performs the write; it cannot prove the engine restarts.

**An omission that was found the hard way, and is worth the space:** the model originally wrote a
data descriptor and its context descriptor in one call, always both, always complete. That is the
ordinary case and it is not the only one — the DMA may release the data descriptor before it has
written the timestamp, which is why ST's HAL breaks out and retries. A model that cannot express
the other case makes the driver's handling of it untestable, and this one duly was: the driver
read the unwritten slot and reported `ts_ns = 1 080 035 208`, the buffer address, as a receive
timestamp (finding NX-01). Write-back ordering is now the *test's* choice —
`deliver_frame_ts()` for both, `deliver_frame_ts_pending()` plus `complete_pending_ts()` for the
split — rather than something the model decides on the hardware's behalf.

## The NSD logic units

`lib/ptp/ptp_{tx_registry,ts_codec,endpoint,nsd_lifecycle}` were extracted from
`eth/eth_drv_netxduo.c` and `ptp/nsd_netxduo.c` so they could be tested. They are C++ with
`extern "C"` boundaries, per the rules in `lib/runtime/cpp_support.cpp`, and they deliberately
depend on **nothing** — no ThreadX, no NetX Duo, not even the fakes in `lib/host/`. The registry
keys on `const void *` rather than `NX_PACKET *` for exactly that reason.

| Unit | The property that matters |
|---|---|
| `ptp_tx_registry` | **The ABA hazard.** NetX Duo recycles packets, so `take` clears the slot and the NSD cancels on send failure. Without both, a recycled packet inherits a stale uid and the servo is fed a real timestamp under the wrong message — silently, and only on colliding cycles |
| `ptp_ts_codec` | The 12-byte headroom precondition. Below it the three ULONGs were never written, so what is there is payload; returning zeroed-and-false beats a plausible wrong time (the F59 lesson) |
| `ptp_endpoint` | Five cases of transport × delay mechanism. A wrong destination presents as "no response received", not as a routing bug |
| `ptp_nsd_lifecycle` | **Transactional configuration.** A create or bind failure part-way through used to leave a socket on NetX Duo's created list, which the next attempt created again — `NX_PTR_ERROR` in a debug build, a corrupted list in release. The fake stack can be told to fail any single call, which is what makes those paths reachable |

## Context rules, on a real ThreadX kernel

`test_rtos_context.cpp` links **real ThreadX** (its Linux port) and **the real CMSIS-RTOS2
wrapper** — the same two components the firmware runs — and asserts the rule this port kept
breaking:

```
From a real osTimer callback (i.e. ThreadX's timer thread):
  osMessageQueuePut(q, &m, 0, osWaitForever)  ->  -2   (osErrorTimeout)   MUST fail
  osMessageQueuePut(q, &m, 0, 0U)             ->   0   (osOK)             MUST succeed
```

That is F80's exact mechanism, reproduced without a board. It works because the restriction
lives in ThreadX's `common/src/txe_queue_send.c` — `TX_WAIT_ERROR` when the caller is
`&_tx_timer_thread` — and `common/src` is identical on every port.

The firmware's `tx_user.h` is shared rather than copied, so the kernel under test is configured
exactly as the board's. The byte-pool sizes deliberately differ (GoogleTest needs more than a
2 KB CLI thread), and `TX_LINUX_MEMORY_SIZE` (`bazel/threadx.BUILD`) must stay larger than their
sum — overrunning it is not an allocation failure but a segfault inside `_tx_byte_pool_create()`.
These targets are `size = "medium"`: they enter a kernel that does not return until the test
ends.

**Both halves are covered, and they are not equally strong.** The host shim's `__get_IPSR()` is
settable, so a test can report IRQ mode and assert ST's wrapper refuses a blocking call — which is
the line `ptp/ptp_tx_timestamp.c` exists to work around. But that is the *wrapper's* guard, not an
interrupt: nothing is preempted and ThreadX still believes it is in a thread. The timer-thread half
runs against a real kernel restriction and is the stronger of the two.

## These tests are known to detect the bugs they name

Every finding pinned here was mutation-tested — the defect reintroduced, the suite re-run, the
source restored and verified byte-identical. The table of mutations and what each one turned red,
and the four lessons the exercise taught, are in
[`docs/testing_architecture.md`](../docs/testing_architecture.md#mutation-testing-is-the-entry-fee).
Do the same for any new characterisation test: break the thing, watch it go red, put it back.
