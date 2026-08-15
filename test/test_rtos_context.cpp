/* Context rules, against a REAL ThreadX kernel.
 *
 * WHY THIS EXISTS
 *
 * One defect class has bitten this port three separate times, always in flexPTP's CMSIS-RTOS2
 * binding, always the same shape: **a blocking call made from a context that may not block.**
 *
 *   1. ptp_transmit_timestamp_cb() enqueues with osWaitForever, and the MAC delivers transmit
 *      timestamps from ETH_IRQHandler. Worked around by Src/flexptp_nsd/ptp_tx_timestamp.c.
 *   2. ptp_event_enqueue() does the same, and its highest-frequency caller is the heartbeat
 *      timer callback, which CMSIS-RTOS2 runs on the RTOS's timer thread. That is F80: every
 *      heartbeat rejected, the BMCA stranded in INITIALIZING, no master ever selected. It cost
 *      a full bench session.
 *   3. A comment of mine in Inc/tx_user.h that reasoned (2) was safe *because* it was not (1).
 *      ThreadX rejects both, for different reasons. That wrong inference is what sent the S6
 *      debugging down the receive path first.
 *
 * A comment cannot stop the fourth. These tests can, because the restriction is enforced in
 * ThreadX's own common/src/txe_queue_send.c -- so the Linux port behaves identically to the
 * Cortex-M33 one and the rule is reachable without a board.
 *
 * WHAT THIS DOES NOT COVER
 *
 * Case 1. IS_IRQ_MODE() is built from __get_IPSR(), which the host shim hard-codes to 0
 * because Linux has no exception context. The ISR half stays on the bench. Case 2 is the one
 * that cost the time, and it is fully reachable here.
 */

#include <gtest/gtest.h>

#include <atomic>

extern "C" {
#include "cmsis_compiler.h"   /* the host shim: __get_IPSR() */
#include "cmsis_os2.h"
#include "tx_api.h"
}

/* The wrapper's osKernelGetSysTimerFreq() reports the CPU clock, which on the target comes
   from ST's CMSIS device header. Nothing here reads it; it just has to link. Same value as the
   board so the two cannot disagree if a test ever does look. */
extern "C" uint32_t SystemCoreClock = 250000000U;

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

TEST_F(RtosContext, BlockingEnqueueFromATimerCallbackIsRejected) {
    /* F80, reproduced. ThreadX refuses to suspend its timer thread:
     * common/src/txe_queue_send.c returns TX_WAIT_ERROR when the caller is &_tx_timer_thread
     * and the wait option is not TX_NO_WAIT. The wrapper reports that as an error status.
     *
     * If this ever starts PASSING as osOK, either ThreadX has relaxed the rule or the wrapper
     * has started masking it -- and flexPTP's unpatched binding would silently work again.
     * Either way somebody should look. */
    ASSERT_TRUE(run_probe()) << "the timer callback never ran";

    EXPECT_NE(g_probe.blocking_put, osOK)
        << "a blocking osMessageQueuePut succeeded from the timer thread. This is the "
           "assumption that produced F80 -- if it now holds, re-check patches/flexptp/0004.";
}

TEST_F(RtosContext, NonBlockingEnqueueFromATimerCallbackSucceeds) {
    /* The other half, and the one that makes the fix a fix rather than a workaround: the
       timeout-0 form must actually deliver, or flexPTP's heartbeat would be dropped by the
       patch instead of by the kernel. */
    ASSERT_TRUE(run_probe()) << "the timer callback never ran";

    EXPECT_EQ(g_probe.nonblocking_put, osOK)
        << "the non-blocking form failed too -- patch 0004 would not have fixed F80";
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
