// flexPTP's teardown on FreeRTOS: finding U11.
//
// unreg_task_ptp() is how an application stops PTP. It used to
//   - delete the message queues before clearing sPTP_operating, and leave the handles dangling,
//     while ptp_transmit_timestamp_cb() -- which a MAC driver calls when a frame's transmission
//     completes, possibly after the stop -- checked nothing and posted into them; and
//   - vTaskDelete() the PTP task from the caller, wherever it was: a task deleted while it holds
//     a mutex leaves the mutex held for ever. On the board that is lwIP's core lock inside a
//     transmit, or the application's console lock inside a log line.
//
// This runs flexPTP's real FreeRTOS binding on FreeRTOS's POSIX port with an "electric fence"
// heap (efence_heap.c): freed memory is made inaccessible, so a post into a deleted queue faults
// at the access, and the fault handler reports it as the use-after-free it is.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/wait.h>
#include <unistd.h>

extern "C" {
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#include "efence_heap.h"
#include "fake_port.h"
#include <flexptp/logging.h>
#include <flexptp/ptp_core.h>
#include <flexptp/task_ptp.h>
}

// ---- the application's console, as the firmware's MSG() is ------------------------------------
//
// One mutex around the output. While `g_gate_closed`, MSG() blocks -- holding the mutex -- the way
// the firmware's MSG() does when the console FIFO is full under load.

namespace {
SemaphoreHandle_t g_msgMtx, g_gate;
volatile bool g_gate_closed;
volatile TaskHandle_t g_blocked_task;
}  // namespace

extern "C" void MSG(const char *format, ...) {
    (void)format;
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING || g_msgMtx == nullptr) return;
    xSemaphoreTake(g_msgMtx, portMAX_DELAY);
    // Only the PTP task is held at the gate: the test itself prints too (ptp_log_enable() does),
    // and must not block on a console only it would reopen.
    if (g_gate_closed && std::strcmp(pcTaskGetName(nullptr), "ptp") == 0) {
        g_blocked_task = xTaskGetCurrentTaskHandle();
        xSemaphoreTake(g_gate, portMAX_DELAY);
        g_blocked_task = nullptr;
    }
    xSemaphoreGive(g_msgMtx);
}

namespace {

void open_gate_after(TickType_t delay) {
    static TickType_t s_delay;
    s_delay = delay;
    xTaskCreate(
        [](void *) {
            vTaskDelay(s_delay);
            g_gate_closed = false;
            xSemaphoreGive(g_gate);
            vTaskDelete(nullptr);
        },
        "gate", configMINIMAL_STACK_SIZE, nullptr, tskIDLE_PRIORITY + 2, nullptr);
}

void settle() { vTaskDelay(pdMS_TO_TICKS(300)); }   // timer task and idle task catch up

TEST(Teardown, StopAndRestartLeaveNothingBehind) {
    fake_reset();
    settle();
    const long base = efence_live_blocks();
    for (int round = 0; round < 3; round++) {
        ASSERT_TRUE(reg_task_ptp());
        vTaskDelay(pdMS_TO_TICKS(400));
        EXPECT_NE(gPtpCoreState.bmca.state, PTP_BMCA_INITIALIZING) << "round " << round << ": the PTP task is not running";
        unreg_task_ptp();
        settle();
        EXPECT_FALSE(is_flexPTP_operating());
        EXPECT_EQ(efence_live_blocks(), base) << "round " << round << ": a stop/start cycle leaked memory";
    }
}

TEST(Teardown, U11_StopWaitsForTheTaskToReleaseItsLocks) {
    fake_reset();
    g_msgMtx = xSemaphoreCreateMutex();          // fresh, and replaced at the end: if this test
    g_gate_closed = true;                        // fails, the old one is held by a deleted task
    ASSERT_TRUE(reg_task_ptp());
    ptp_log_enable(PTP_LOG_BMCA, true);          // the PTP task now logs every BMCA state change
    for (int i = 0; i < 200 && g_blocked_task == nullptr; i++) vTaskDelay(pdMS_TO_TICKS(50));
    ASSERT_NE(g_blocked_task, nullptr) << "the PTP task never logged";
    ASSERT_NE(g_blocked_task, xTaskGetCurrentTaskHandle());

    // The PTP task is inside MSG(), holding the console mutex. Stop PTP now; the console frees
    // up 300 ms later, as a draining FIFO would.
    open_gate_after(pdMS_TO_TICKS(300));
    unreg_task_ptp();
    settle();
    ptp_log_disable_all();
    // Asked of the mutex alone: TAKING it would follow its holder pointer into the deleted task's
    // freed control block (priority inheritance), which is the crash this defect causes.
    EXPECT_EQ(xSemaphoreGetMutexHolder(g_msgMtx), nullptr)
        << "the console mutex is still held -- by the PTP task, which unreg_task_ptp() deleted while it "
           "held it. Every later MSG() would block for ever, or walk into the freed task";
    g_msgMtx = xSemaphoreCreateMutex();          // later tests must not inherit a poisoned mutex
}

// The calls are made in a forked child. A post into a deleted queue faults inside
// taskENTER_CRITICAL(), where the POSIX port has every signal blocked, so no handler can report it
// in-process: the kernel kills the process. The child inherits the protected pages; the parent
// reads how it died. Fixed, flexPTP returns before touching FreeRTOS at all, and the child, which
// has no scheduler, makes no kernel call.
TEST(Teardown, U11_ATransmitTimestampAfterStopTouchesNoDeletedQueue) {
    fake_reset();
    ASSERT_TRUE(reg_task_ptp());
    vTaskDelay(pdMS_TO_TICKS(200));
    unreg_task_ptp();
    settle();
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // A frame sent just before the stop finishes transmitting after it: the MAC driver
        // reports its timestamp now. flexPTP must drop it, not post it into a deleted queue.
        ptp_transmit_timestamp_cb(1, 0, 0);
        const uint8_t sync[44] = {0x00, 0x02, 0x00, 44};
        ptp_receive_enqueue(sync, sizeof(sync), 0, 0, PTP_TP_IPv4);
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << (WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS)
                ? "USE AFTER FREE: a TX timestamp after the stop was posted into a queue unreg_task_ptp() had deleted"
                : "the child did not exit cleanly");
}

int g_result = 1;

void test_main(void *) {
    efence_unblock_faults_in_this_thread();
    g_result = RUN_ALL_TESTS();
    std::fflush(nullptr);
    std::exit(g_result);
}

}  // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    efence_install_fault_handler();
    g_msgMtx = xSemaphoreCreateMutex();
    g_gate = xSemaphoreCreateBinary();
    xTaskCreate(test_main, "tests", configMINIMAL_STACK_SIZE * 4, nullptr, tskIDLE_PRIORITY + 1, nullptr);
    vTaskStartScheduler();
    return 2;   // not reached
}
