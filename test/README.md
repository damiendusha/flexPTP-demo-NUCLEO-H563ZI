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

## The NSD logic units

`Src/flexptp_nsd/ptp_{tx_registry,ts_codec,endpoint}` were extracted from `eth_drv_netxduo.c`
and `nsd_netxduo.c` so they could be tested. They are C++ with `extern "C"` boundaries, per the
rules in `Src/cpp_support.cpp`, and they deliberately depend on **nothing** — no ThreadX, no
NetX Duo, not even the fakes in `host/`. The registry keys on `const void *` rather than
`NX_PACKET *` for exactly that reason.

| Unit | The property that matters |
|---|---|
| `ptp_tx_registry` | **The ABA hazard.** NetX Duo recycles packets, so `take` clears the slot and the NSD cancels on send failure. Without both, a recycled packet inherits a stale uid and the servo is fed a real timestamp under the wrong message — silently, and only on colliding cycles |
| `ptp_ts_codec` | The 12-byte headroom precondition. Below it the three ULONGs were never written, so what is there is payload; returning zeroed-and-false beats a plausible wrong time (the F59 lesson) |
| `ptp_endpoint` | Five cases of transport × delay mechanism. A wrong destination presents as "no response received", not as a routing bug |

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

`Inc/tx_user.h` is shared with the firmware rather than copied, so the kernel under test is
configured exactly as the board's. The byte-pool sizes deliberately differ (GoogleTest needs
more than a 2 KB CLI thread), and `TX_LINUX_MEMORY_SIZE` must stay larger than their sum —
overrunning it is not an allocation failure but a segfault inside `_tx_byte_pool_create()`.

**What it cannot cover:** the ISR half of the same rule. `IS_IRQ_MODE()` is built from
`__get_IPSR()`, which the host shim hard-codes to 0 because Linux has no exception context. The
transmit-timestamp path that `ptp_tx_timestamp.c` guards stays on the bench. The timer-thread
half is the one that cost a session, and it is fully covered.

## These tests are known to detect the bugs they name

Characterisation tests that cannot fail are worse than no tests. Both findings pinned here
were mutation-tested — the defect reintroduced into `mac_drv.c`, the suite re-run, the source
restored and verified byte-identical:

| Mutation | Result |
|---|---|
| Drop `!(bd)->ext.inFlight` from `ETHHW_DESC_NEEDS_PROCESSING` (**F61**) | `RxRing.DoesNotRedeliverAFrameStillInFlight` **fails** |
| Remove the `DMACRDTPR` write from `ETHHW_RestoreRXDesc` (**F71**) | `RxRing.RestoringWritesTheTailPointerToRestartTheEngine` **fails** |
| Hoist `found` out of `ETHHW_ProcessTx`'s retry loop (**F57**) | six `TxRing` tests **time out** — see below |
| Widen the ordering delta to `int32_t` (the wrap bug) | `TxRing.DispatchesInOrderAcrossTheSixteenBitCounterWrap` **fails** |
| `ptp_tx_registry_take()` reads instead of clearing (**the ABA hazard**) | **5 tests fail** |
| `ptp_tx_registry_cancel()` does nothing (the send-failure leak) | 1 test fails |
| Drop the codec's headroom precondition | 1 test fails |
| Endpoint teardown stops zeroing | 1 test fails |
| Widen the RFC 1112 mask from 23 to 24 bits | 1 test fails — **but only after the test was fixed; see below** |
| Assert the timer-thread blocking put SUCCEEDS (**F80 inverted**) | `RtosContext.BlockingEnqueueFromATimerCallbackIsRejected` **fails**, reporting −2 vs 0 — proving the kernel really rejects it rather than the test being tautological |

Do the same for any new characterisation test: break the thing, watch it go red, put it back.

**One test was vacuous and mutation testing is what exposed it.** The RFC 1112 mask check
originally asserted `mac[3] & 0x80 == 0` on the resolved endpoint for 224.0.1.129 — whose third
octet is `0x00`, so it passed whether the mask was `0x7F` or `0xFF`. It was true by accident,
not because the code was right. The mask is now exercised through
`ptp_endpoint_ipv4_group_mac()` with 239.129.1.1, whose third octet is `0x81` and therefore
actually puts the bit in play. That function is public purely so the test can reach it, and the
header says so.

Three things this exercise taught, all of which changed the code above:

- **F57 presents as a hang, not a wrong value.** With the per-pass count hoisted, `found`
  never reaches zero and the drain loop spins forever. Without a timeout the suite wedges and
  CI waits for its job limit instead of reporting anything. That is why `add_host_test()` sets
  `TIMEOUT 10` — 250× headroom over the 0.04 s the whole suite takes, and it turns the hang
  into six clean failures.

- **The first version of the wrap test could not fail.** Four timestamped frames straddling
  the wrap are ordered identically by the correct modular arithmetic and by the buggy widened
  subtraction: the `delta == 1` fast path takes the first, and the rest are all post-wrap. A
  discriminating case needs two candidates pending *simultaneously* on opposite sides of the
  wrap with neither at delta 1 — which is why that test now sends untimestamped filler frames
  to open a gap. It passes on the real driver and fails on the mutation; the original passed
  on both, and would have been worse than no test at all.
