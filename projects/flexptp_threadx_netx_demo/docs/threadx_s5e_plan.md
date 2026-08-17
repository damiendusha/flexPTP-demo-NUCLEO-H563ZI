# S5e — running the real stack on the host

> **Status: C and B are built and passing** (73 host tests). A remains undone, deliberately —
> see [Recommendation](#recommendation). This document is kept as the reasoning behind that
> split rather than as an outstanding to-do.

A plan for the one stage of the ThreadX/NetX Duo port that was never started: real ThreadX
(Linux port) + real NetX Duo + a RAM driver, so the flexPTP network stack driver can be
exercised in CI with no board attached.

It was marked optional in [threadx_porting_plan.md](threadx_porting_plan.md) and it stayed
optional. This document exists so the decision to do it or drop it can be made on evidence
rather than on the fact that it is the last unticked box.

---

## The honest value case, now that the port is finished

S5e was scoped when the NSD did not exist. It would have been a *development* tool. The port is
now complete and hardware-verified, so what remains is *regression* value — a different and
smaller thing.

The sharpest question is: **of the five defects that actually cost time in this port, how many
would S5e have caught?**

| Defect | Layer | Would S5e have caught it? |
|---|---|---|
| **F80** — heartbeat enqueued with `osWaitForever` from ThreadX's timer thread | flexPTP ↔ RTOS | **Yes.** `txe_queue_send.c` is in `common/src`, so the timer-thread restriction is identical on the Linux port. This one cost an entire bench session |
| Missing 2-byte RX alignment shim | Ethernet driver | No — the RAM driver replaces our driver entirely |
| `nx_interface_address_mapping_needed` left false | Ethernet driver | No — same reason |
| Checksum offload not declared | Ethernet driver ↔ MAC | No — there is no MAC on the host |
| `osThreadNew()` rejecting a zeroed priority | Application ↔ wrapper | Probably — it is wrapper behaviour, not hardware |

**One clear yes, one probable, three no.** The three misses are all driver-level, and that is
structural rather than bad luck: S5e's whole premise is *replacing* the driver with a RAM one.

That is not a reason to skip it. It is a reason to size it honestly — and it points at a much
cheaper option, below, that captures the "yes" without the rest.

---

## Feasibility — verified, not assumed

Every prerequisite was checked against the vendored trees:

| Requirement | State |
|---|---|
| ThreadX Linux port | ✅ `Modules/threadx/ports/linux/gnu/` with a complete `src/` |
| `_tx_initialize_unused_memory` on Linux | ✅ `tx_initialize_low_level.c:249` — `malloc(TX_LINUX_MEMORY_SIZE)`. So `USE_DYNAMIC_MEMORY_ALLOCATION` works unchanged and `Inc/tx_user.h` needs no host variant |
| NetX Duo RAM driver | ✅ `Modules/netxduo/common/src/nx_ram_network_driver.c` — part of the shipped library, not a test fixture |
| Multiple peers in one process | ✅ `NX_MAX_RAM_INTERFACES 4`, and the driver broadcasts between registered instances — so a master and a slave can be two IP instances in one test binary |
| CMSIS-RTOS2 wrapper on Linux | Unverified. It touches only `_tx_initialize_kernel_setup`, `_tx_initialize_unused_memory` and `_tx_thread_created_ptr`, all in `common/`, so it should port — but this is the one thing to spike first |

Nothing here is a blocker. The capability is better than the plan assumed: two IP instances
talking over a RAM driver means a real PTP exchange, not just "does the NSD compile".

## The obstacles that actually matter

1. **flexPTP needs a hardware clock.** `Inc/flexptp_options.h` wires `PTP_HW_INIT`,
   `PTP_SET_CLOCK`, `PTP_SET_ADDEND` and `PTP_HW_GET_TIME` to `ptp_port_stm32h563.c`, which
   programs ETH registers. The host needs a software clock behind the same four macros. Real
   work, but bounded — and NetX Duo ships a worked example of the idea in
   `nx_ptp_client_soft_clock_callback`.

2. **Real threads, real time.** The Linux port uses pthreads and the wall clock. A sync test
   takes real seconds and can flake under CI load. Mitigate by asserting *state* (SLAVE reached,
   t3 present, path delay computed) rather than *accuracy*, and by keeping timeouts generous.

3. **It is a simulator, not a unit test.** The existing `test/` tree compiles individual files
   against small fakes and runs in a fraction of a second. This would link two real middleware stacks and run
   for seconds. It should live in its own target, not alongside them, or it will make the fast
   suite slow and the slow suite look flaky.

4. **Second definition of the world.** `nx_user.h` and `tx_user.h` are currently one
   configuration each. A host build adds a second set of assumptions that can drift from the
   target's. Share the headers, override only what the host must.

---

## The three options

### Option A — the full tier, as originally scoped

Real ThreadX + real NetX Duo + RAM driver + the real `nsd_netxduo.c`, with a second IP instance
acting as a PTP peer.

| Step | Work |
|---|---|
| A1 | Spike: ThreadX Linux port + CMSIS-RTOS2 wrapper, one thread, one queue. Proves the riskiest assumption in an hour |
| A2 | `test/host_stack/` CMake target: ThreadX (linux port), NetX Duo, RAM driver, shared `tx_user.h`/`nx_user.h` |
| A3 | Software PTP clock behind the four `PTP_HW_*` macros |
| A4 | Bring up one IP instance; assert `ptp_nsd_init()` creates and binds both sockets, and joins the right group per delay mechanism |
| A5 | Second IP instance as a peer; assert a Sync/Follow_Up/Delay_Req/Delay_Resp round trip and that `ptp_receive_enqueue()` sees each |
| A6 | Error paths that are painful on hardware: packet-pool exhaustion, socket-create failure, a chained receive packet, `ptp_nsd_init(-1,-1)` teardown and re-init |
| A7 | CI job, separate from the fast suite |

**Estimate: 3–5 days.** The variance is A3 and A5.

### Option B — the context-rule test *(recommended)*

The single defect class that has bitten this port three times is **"a blocking call from a
context that may not block"**: the transmit-timestamp callback from an ISR, the heartbeat from
ThreadX's timer thread, and my own wrong inference in `tx_user.h` that the timer thread was
safe. F80 alone cost a bench session.

That class does not need NetX Duo, a RAM driver, a software clock, or a PTP exchange. It needs
**real ThreadX on the host and the CMSIS-RTOS2 wrapper**, and a test that calls the same APIs
from the same contexts flexPTP does:

```
Given a real osTimer callback (i.e. ThreadX's timer thread)
  osMessageQueuePut(q, &m, 0, osWaitForever)  MUST fail        <- reproduces F80
  osMessageQueuePut(q, &m, 0, 0U)             MUST succeed     <- pins the fix
```

and the same pair for the paths `ptp_tx_timestamp.c` guards.

| Step | Work |
|---|---|
| B1 | Spike A1 (shared) |
| B2 | `test/host_rtos/` target: ThreadX linux port + the wrapper + GoogleTest |
| B3 | The context-rule tests above, plus a mutation check that each fails when the rule is violated |
| B4 | CI job — seconds, not minutes |

**Estimate: 1–1.5 days**, and it makes F80 a permanently guarded property rather than a comment.

---

### Option C — the NSD's own logic, on the existing harness

**This was in the S6 plan and never got built.** S6 shipped in C and was verified on hardware
only, so at the time of writing the three units it named had no automated coverage at all —
and neither did anything else in `Src/flexptp_nsd/`. The 25 tests then in `test/` covered
`mac_drv.c` and nothing above it.

*(Since built. The three units were extracted to `Src/flexptp_nsd/ptp_{tx_registry,ts_codec,
endpoint}` and the suite now stands at 73 `ctest` entries — see
[testing_architecture.md](testing_architecture.md).)*

| Unit | Why it is worth a test |
|---|---|
| **the pending-timestamp table** (`tx_ts_pending` in `eth_drv_netxduo.c`) | **The ABA hazard.** NetX Duo recycles packets, so a stale entry hands the servo a *wrong* uid — silently, intermittently, and only on the Delay_Req cycles unlucky enough to collide. Two things prevent it: the entry is taken rather than read, and the NSD cancels on send failure. Neither is tested, and neither is the sort of thing a bench proves |
| **the RX timestamp codec** | The 12-byte `nx_packet_data_start` convention, plus the head-room precondition. The F48 class exactly |
| **the endpoint decision table** | Transport × delay mechanism → destination address, port, EtherType, multicast group. Five cases including teardown; a wrong destination presents as "no response received" |

Needs **no new infrastructure**: no ThreadX, no NetX Duo, no RAM driver. The fakes in
`test/host/` already cover what these touch. **Estimate: ~1 day.**

## How the three compose

They are not alternatives. **B is literally A's first two steps** — A1 and B1 are the same
spike, and A2 is B2 plus NetX Duo — so doing B first makes A cheaper later rather than
duplicating it. C is independent of both and shares the existing harness.

What each *uniquely* covers, once the others are done:

| | Unique remainder |
|---|---|
| **B** | The "blocking call from a context that may not block" class — the only one of the five defects a host test would have caught, and the one that cost a bench session |
| **C** | NSD and driver *logic*: the ABA hazard, the codec precondition, the endpoint table |
| **A** | Real socket lifecycle, packet-pool exhaustion, chained receive packets, and board-free end-to-end regression |

The honest consequence: **after B and C, A's marginal value is much smaller than its standalone
value**, because B has taken the biggest prize and C the second. A's remainder is error paths
that are awkward on hardware — real, but the narrowest of the three.

## Recommendation

**Do C, then B. Leave A until a trigger fires.** Roughly 2.5 days for both, against 3–5 for A
alone.

C first, because it is the cheapest, needs nothing new, and closes a gap that should not have
been left open — the S6 plan called for exactly these tests and the stage shipped without them.
Then B, because it guards the one class this port keeps getting wrong.

The reasoning is the table at the top. Option A's unique reach over the existing suites is
narrow — the driver-level defects are invisible to it by construction, and the hardware suite
already proves the happy path end to end, twice, on both build configurations. What it adds is
error-path coverage and board-free regression, which are real but not worth 3–5 days on a demo
project that has a working bench.

B is a fifth of A's cost and covers the one class this port demonstrably keeps getting wrong —
including on a future flexPTP or ThreadX bump, which is exactly when nobody will be watching.

**Is A worth doing as well?** Eventually, and it composes cleanly — nothing in B or C is thrown
away. But it should be pulled by a need rather than pushed by completeness, because after B and
C its remainder is narrow and its maintenance surface is the largest of the three.

**What would change the recommendation:** if this port ever loses reliable access to the board,
or if the middleware submodules start being bumped routinely, Option A's board-free regression
value rises sharply and it becomes worth the days.
