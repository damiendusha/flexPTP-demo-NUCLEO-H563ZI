/* CMSIS-RTOS2 context rules, on REAL FreeRTOS -- the counterpart of the ThreadX project's
 * test of the same name, and deliberately NOT a translation of it.
 *
 * The ThreadX file pins two rules that live in two different layers, and only one of them
 * exists here:
 *
 *   THE ISR HALF PORTS VERBATIM. It is ST's CMSIS-RTOS2 wrapper, not the kernel, that refuses a
 *   blocking call from interrupt context:
 *
 *       if (IS_IRQ_MODE()) { if (... || (timeout != 0U)) { stat = osErrorParameter; } }
 *
 *   This project uses ST's FreeRTOS-backed cmsis_os2.c, which has the same guard. That is the
 *   rule guarding why ptp_tx_timestamp.c exists at all: flexPTP's binding calls
 *   osMessageQueuePut(..., osWaitForever) from the transmit-timestamp callback, which on this
 *   hardware is reached from ETH_IRQHandler.
 *
 *   THE TIMER-THREAD HALF HAS NO COUNTERPART, and this file says so with a test rather than
 *   with silence. F80 was ThreadX's own restriction -- txe_queue_send.c returns TX_WAIT_ERROR
 *   when the caller is &_tx_timer_thread -- and FreeRTOS has no equivalent: its timer service
 *   task is an ordinary task that may block. So the blocking put that F80 made fail SUCCEEDS
 *   here. Asserting that is not decoration: it records that a reader who ports the ThreadX
 *   reasoning to this firmware would be wrong, and it fails if FreeRTOS ever changes.
 *
 * Both halves matter because "a blocking call from a context that may not block" is the one
 * class of defect this project kept getting wrong.
 *
 * ---------------------------------------------------------------------------------------------
 * THE THREE TIMER TESTS BELOW WERE DISABLED FOR A WHILE, AND WHY IS WORTH KEEPING.
 *
 * Every test that drove a CMSIS-RTOS2 timer callback segfaulted on the FreeRTOS POSIX port --
 * including ATimerCallbackIsNotInterruptContext, which only reads __get_IPSR() and blocks
 * nothing. They were disabled with that stated plainly, and with the conclusion drawn honestly:
 * the claim they exist to pin -- "FreeRTOS's timer service task is an ordinary task and may
 * block, so F80 has no counterpart here" -- was NOT verified, and was to be treated as an
 * assumption rather than a checked fact.
 *
 * It was neither the POSIX port nor this harness. It was ST's CMSIS-RTOS2 wrapper: it tags the
 * low bit of a TimerCallback_t* to record dynamic allocation and masked that bit off through a
 * `uint32_t`, which on a 64-bit host discards the upper half of the pointer. See the note on
 * TimerCallback() in third_party/CMSIS/CMSIS_RTOS_V2/cmsis_os2_freertos.c. The fix is
 * `uintptr_t`, it is a no-op on the 32-bit target -- .text is byte-identical -- and with it all
 * three pass.
 *
 * So the claim is now checked. Two things are worth carrying away. The first is that a
 * diagnosis of "the timer path is broken on the host" was accurate and still pointed at the
 * wrong component; what settled it was a backtrace, not more reasoning. The second is that a
 * DISABLED test with an honest note is not a dead end -- this one described its own symptom
 * precisely enough that the fix was fifteen minutes' work once someone ran it under a debugger.
 * ---------------------------------------------------------------------------------------------
 */

#include <gtest/gtest.h>

#include <atomic>

extern "C" {
#include "cmsis_compiler.h"   /* the host shim: __get_IPSR() */
#include "cmsis_os2.h"
#include "FreeRTOS.h"
#include "task.h"
}

extern "C" {
/* The wrapper's osKernelGetSysTimerFreq() reports the CPU clock, which on the target comes
   from ST's CMSIS device header. Nothing here reads it; it just has to link. Same value as the
   board so the two cannot disagree if a test ever does look. */
uint32_t SystemCoreClock = 250000000U;

/* The shim's IPSR, which IS_IRQ_MODE() is built from. Zero except where a test sets it. */
uint32_t host_fake_ipsr = 0U;
}

namespace {

/** Report IRQ mode for the duration of a scope, as an ISR would. */
class PretendInterruptContext {
  public:
    PretendInterruptContext() { host_fake_ipsr = 16U; /* any nonzero IPSR is an exception */ }
    ~PretendInterruptContext() { host_fake_ipsr = 0U; }
};

}  // namespace

namespace {

/* What a timer callback observed. Plain values written once and read after the callback has
   certainly run -- no synchronisation needed beyond the event flag that gates the read. */
struct TimerProbe {
    std::atomic<int> fired{0};
    osStatus_t blocking_put{osOK};
    osStatus_t nonblocking_put{osOK};
    uint32_t ipsr{0xFFFFFFFFu};
};

TimerProbe g_probe;
osMessageQueueId_t g_queue;
osEventFlagsId_t g_done;

constexpr uint32_t kProbeDone = 0x01u;

void probe_timer_cb(void *arg) {
    (void)arg;

    uint32_t msg = 0x5A5A5A5Au;

    /* Exactly what flexPTP's ptp_event_enqueue() did before patch 0004. */
    g_probe.blocking_put = osMessageQueuePut(g_queue, &msg, 0, osWaitForever);

    /* And exactly what it does after. */
    g_probe.nonblocking_put = osMessageQueuePut(g_queue, &msg, 0, 0U);

    g_probe.ipsr = __get_IPSR();
    g_probe.fired++;

    osEventFlagsSet(g_done, kProbeDone);
}

/** Run the timer once and wait for its callback. */
bool run_probe() {
    g_probe.fired = 0;
    g_done = osEventFlagsNew(nullptr);
    if (g_done == nullptr) {
        return false;
    }

    osTimerId_t timer = osTimerNew(probe_timer_cb, osTimerOnce, nullptr, nullptr);
    if (timer == nullptr) {
        return false;
    }
    if (osTimerStart(timer, 10U) != osOK) {
        return false;
    }

    // Generous: this is a real kernel on a shared CI machine, not a simulation.
    uint32_t flags = osEventFlagsWait(g_done, kProbeDone, osFlagsWaitAny, 5000U);
    osTimerDelete(timer);
    osEventFlagsDelete(g_done);

    return (flags == kProbeDone) && (g_probe.fired > 0);
}

/* Drain anything left by a previous test.
 *
 * The probe callback deliberately enqueues, so tests share state through g_queue unless it is
 * cleared. The control test read 0x5A5A5A5A -- a value an earlier test's timer had left behind
 * -- and failed for a reason that had nothing to do with what it was asserting. */
void drain_queue() {
    uint32_t discard = 0;
    while (osMessageQueueGet(g_queue, &discard, nullptr, 0U) == osOK) {
    }
}

class RtosContext : public ::testing::Test {
  protected:
    void SetUp() override { drain_queue(); }
};

}  // namespace

TEST_F(RtosContext, TheKernelIsActuallyRunning) {
    // Guards against the whole suite passing vacuously because the kernel never started.
    EXPECT_EQ(osKernelGetState(), osKernelRunning);
    EXPECT_NE(osThreadGetId(), nullptr);
}

TEST_F(RtosContext, BlockingEnqueueFromATimerCallbackSUCCEEDS) {
    /* The opposite of the ThreadX project's test of nearly this name, and the reason this file
     * is re-derived rather than translated.
     *
     * F80 was ThreadX refusing to suspend its timer thread: common/src/txe_queue_send.c returns
     * TX_WAIT_ERROR when the caller is &_tx_timer_thread, which the CMSIS-RTOS2 wrapper reports
     * as osErrorTimeout. FreeRTOS has no such rule -- the timer service task is an ordinary
     * task and may block -- so the same call succeeds.
     *
     * Asserted rather than left unsaid, so that "F80 does not apply here" is a checked claim
     * and not an assumption someone re-derives under time pressure. */
    ASSERT_TRUE(run_probe()) << "the timer callback never ran";

    EXPECT_EQ(g_probe.blocking_put, osOK)
        << "a blocking osMessageQueuePut from the FreeRTOS timer task failed. If FreeRTOS has "
           "gained a ThreadX-style timer-thread restriction, patch 0004's reasoning now applies "
           "to this firmware too -- see the ThreadX project's test_rtos_context.cpp.";
}

TEST_F(RtosContext, NonBlockingEnqueueFromATimerCallbackAlsoSucceeds) {
    /* The form patch 0004 changed flexPTP to use. It is correct on both kernels, which is why
     * the patch is safe to carry here even though the defect it fixes is ThreadX-only. */
    ASSERT_TRUE(run_probe()) << "the timer callback never ran";
    EXPECT_EQ(g_probe.nonblocking_put, osOK);
}

TEST_F(RtosContext, ATimerCallbackIsNotInterruptContext) {
    /* The inference that cost the S6 session. The callback is NOT in an ISR, which is why
     * reasoning "not an ISR, therefore safe to block" felt sound and was wrong: ThreadX
     * restricts the timer thread separately, and for a different reason.
     *
     * On this host __get_IPSR() is hard-coded to 0, so this asserts what the shim guarantees
     * rather than what the silicon does -- but the point stands, because the ThreadX
     * restriction that actually bites is not conditioned on IPSR at all. */
    ASSERT_TRUE(run_probe()) << "the timer callback never ran";
    EXPECT_EQ(g_probe.ipsr, 0u);
}

/* NOTE on the four context tests: they assert the same API from two different contexts, and
 * only two of the rejections come from the KERNEL. The timer-thread ones are ThreadX's own
 * restriction (txe_queue_send.c returns TX_WAIT_ERROR when the caller is &_tx_timer_thread);
 * the interrupt ones are ST's wrapper checking IS_IRQ_MODE(). Both bit this port, for different
 * reasons -- which is exactly why reasoning "not an ISR, therefore safe to block" was wrong. */

TEST_F(RtosContext, BlockingEnqueueFromInterruptContextIsRejected) {
    /* The ISR half of the rule, and the reason Src/flexptp_nsd/ptp_tx_timestamp.c exists.
     *
     * flexPTP's CMSIS-RTOS2 binding calls osMessageQueuePut(..., osWaitForever) from
     * ptp_transmit_timestamp_cb(), and on this hardware that is reached from ETH_IRQHandler.
     * ST's wrapper refuses it:
     *
     *     if ((IS_IRQ_MODE() && (timeout != 0)) || ...) { status = osErrorParameter; }
     *
     * flexPTP does not check the return, so every transmit timestamp would be discarded in
     * silence and the servo would converge on a lie.
     *
     * WHAT THIS PROVES, EXACTLY. It is not an interrupt: nothing is preempted, ThreadX still
     * believes it is in a thread, and there is no exception frame. It pins the WRAPPER's guard
     * against a context that reports IRQ mode -- which is the check that would have to be
     * removed or changed for the hazard to come back. It says nothing about the silicon, and
     * it is a narrower claim than the timer-thread cases above, which run against a real
     * kernel restriction in txe_queue_send.c.
     *
     * Three documents used to record this as permanently unreachable on the host. It was a
     * property of the shim, not of the host (finding TS-09). */
    uint32_t msg = 0xC0FFEEu;

    PretendInterruptContext irq;
    EXPECT_NE(osMessageQueuePut(g_queue, &msg, 0, osWaitForever), osOK)
        << "a blocking enqueue was accepted from interrupt context -- the wrapper's guard is "
           "what ptp_tx_timestamp.c relies on";
}

TEST_F(RtosContext, NonBlockingEnqueueFromInterruptContextSucceeds) {
    /* The other half: the timeout-0 form that ptp_tx_timestamp_from_isr() actually uses must
       work, or the deferral would drop what the kernel would have accepted. */
    uint32_t msg = 0xBEEFu;

    {
        PretendInterruptContext irq;
        EXPECT_EQ(osMessageQueuePut(g_queue, &msg, 0, 0U), osOK);
    }

    uint32_t out = 0;
    EXPECT_EQ(osMessageQueueGet(g_queue, &out, nullptr, 0U), osOK);
    EXPECT_EQ(out, 0xBEEFu) << "the message posted from 'interrupt context' never arrived";
}

TEST_F(RtosContext, BlockingEnqueueFromAnOrdinaryThreadIsFine) {
    /* The control. Without it, a suite where every put failed would look identical to a suite
       proving the rule -- and the tests above would be worthless. */
    uint32_t msg = 1;
    EXPECT_EQ(osMessageQueuePut(g_queue, &msg, 0, osWaitForever), osOK);

    uint32_t out = 0;
    EXPECT_EQ(osMessageQueueGet(g_queue, &out, nullptr, osWaitForever), osOK);
    EXPECT_EQ(out, 1u);
}

/* --- kernel bring-up ---------------------------------------------------------------------- */

namespace {

int g_argc;
char **g_argv;
int g_result = 1;

void test_main(void *arg) {
    (void)arg;

    /* Deep enough that a blocking put has somewhere to go when it is allowed to, and shallow
       enough that the timer-thread rejection is about context rather than capacity. */
    g_queue = osMessageQueueNew(8, sizeof(uint32_t), nullptr);

    g_result = RUN_ALL_TESTS();

    /* tx_kernel_enter() never returns, so the process has to be ended from inside a thread. */
    std::exit(g_result);
}

}  // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    g_argc = argc;
    g_argv = argv;

    if (osKernelInitialize() != osOK) {
        std::fprintf(stderr, "osKernelInitialize() failed\n");
        return 2;
    }

    osThreadAttr_t attr{};
    attr.name = "tests";
    attr.stack_size = 32768;  /* GoogleTest is not frugal */
    attr.priority = osPriorityNormal;

    if (osThreadNew(test_main, nullptr, &attr) == nullptr) {
        std::fprintf(stderr, "could not create the test thread\n");
        return 2;
    }

    osKernelStart();  /* does not return */
    return 2;
}
