# Host unit tests

Pure-logic tests that run on the development machine, with no board attached.

```bash
cmake -S test -B build/test && cmake --build build/test -j
ctest --test-dir build/test --output-on-failure
```

They run in well under a second and gate every push in CI, which the hardware suite in
[`tools/hil/`](../tools/hil/README.md) cannot.

## What is tested, and what is not

Set out in full in [`docs/threadx_porting_plan.md`](../docs/threadx_porting_plan.md) under
*How far mocks actually reach*. The short version, measured against this repository's own
finding log: **roughly half** of the findings in the code this port touches would have been
caught here, a quarter become regression-lockable once discovered on hardware, and a quarter
never will be.

Specifically **not** reachable, and no amount of test writing changes it:

- **Codegen defects.** F70 (a bounded wait optimised away above `-Og`) and F62 (an
  optimisation-dependent HardFault from a byte access the silicon rejects) are properties of
  the ARM build. These tests compile different code, with a different compiler, for a
  different instruction set. A green run here is *no evidence* about either.
- **Memory ordering.** `__DSB`/`__DMB` are no-ops in the host shim.
- **Anything downstream of the timestamp capture**, the PHY, the clock tree, or the wire.

## Why this is a separate CMake root

The top-level `CMakeLists.txt` includes `cmake/gcc-arm-none-eabi.cmake` unconditionally —
there is no flag that makes it produce a host binary. So `test/` is its own `project()`.

## How the driver is compiled for the host

`mac_drv.c` needed **no changes at all** to become testable, which is worth understanding
before adding to this suite.

- **The seam was already there.** 97 of the driver's 98 register accesses go through its
  `ETH_TypeDef *eth` parameter, so they follow whatever pointer a test passes.
- **The rest is redirected by macro.** `mac_drv_uut.c` `#include`s the driver's `.c` verbatim
  after pointing `ETH`, `RCC`, `GPIOx` and `SBS` at host memory. Including the `.c` also
  exposes the file-static helpers.
- **The register definitions are ST's real ones.** `test/host/` shadows exactly three ARM
  headers — `core_cm33.h`, `system_stm32h5xx.h`, `cmsis_compiler.h` — and everything below
  that is ST's genuine CMSIS device header and HAL headers. Nothing is hand-copied, so a
  wrong bit position cannot make a test lie, and the tests track the pinned HAL submodule
  automatically. It also means a signature drift is a compile error: that already caught
  `HAL_GPIO_Init` taking a `const` pointer.

### The two 32-bit accommodations

The driver stores addresses in `uint32_t` fields because on a Cortex-M33 that is what a
pointer is. On x86-64 those casts truncate. Two things make the round-trip lossless:

| | |
|---|---|
| **`MAP_32BIT` arena** (`host/host_arena.c`) | descriptors, buffers and the state block live below 2 GB, so `(uint32_t)ptr` survives |
| **`-no-pie`** (`CMakeLists.txt`) | puts `.text` at ~`0x400000`, so function pointers survive `ETHHW_DescExt.tsCbPtr`, which is also a `uint32_t` |

`HostHarness.ArenaReturnsThirtyTwoBitAddresses` and
`HostHarness.FunctionPointersFitInThirtyTwoBits` guard both. If either regresses the driver
tests would not fail cleanly — they would corrupt memory or jump into nowhere — so those two
fail first and say why.

`-m32` would be strictly better, matching the target's whole ABI rather than just pointer
width, but it needs `gcc-multilib`. If you have it, use it and the arena becomes unnecessary.

## The DMA model

`eth_model.hpp` plays the part of the Ethernet DMA: it clears OWN bits, fills lengths and
status flags, and places timestamps in context descriptors. It is deliberately small and
readable, and its **known omissions are listed at the top of the header** rather than left
implicit — because a hardware model that is wrong is a test that passes for the wrong reason.

The most important omission: **no RBU/suspend semantics.** The real engine, on reaching a
descriptor the application still owns, raises RBU and stops until the tail pointer is written
again. That is finding F71, and it is precisely the case where a naively written model would
have been wrong in the same way the code was. `RestoringWritesTheTailPointerToRestartTheEngine`
asserts the driver performs the write; it cannot prove the engine restarts.

## These tests are known to detect the bugs they name

Characterisation tests that cannot fail are worse than no tests. Both findings pinned here
were mutation-tested — the defect reintroduced into `mac_drv.c`, the suite re-run, the source
restored and verified byte-identical:

| Mutation | Result |
|---|---|
| Drop `!(bd)->ext.inFlight` from `ETHHW_DESC_NEEDS_PROCESSING` (**F61**) | `RxRing.DoesNotRedeliverAFrameStillInFlight` **fails** |
| Remove the `DMACRDTPR` write from `ETHHW_RestoreRXDesc` (**F71**) | `RxRing.RestoringWritesTheTailPointerToRestartTheEngine` **fails** |

Do the same for any new characterisation test: break the thing, watch it go red, put it back.
