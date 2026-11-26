/* An "electric fence" FreeRTOS heap for the teardown test, plus the kernel hooks the host
 * FreeRTOSConfig.h asks for.
 *
 * Every pvPortMalloc() gets its own pages; vPortFree() makes them inaccessible and never hands
 * them out again. So a use-after-free -- flexPTP posting into a queue unreg_task_ptp() has deleted
 * -- faults at the access instead of silently writing into memory someone else may own, and the
 * fault handler names it. heap_4, which the firmware uses, would hide it: the freed block simply
 * goes back on the free list, still writable.
 *
 * The POSIX port gives each task its own pthread stack and keeps only bookkeeping in the FreeRTOS
 * stack buffer, which it stops touching once the thread is cancelled and joined -- before the
 * buffer is freed. So protecting freed task memory is safe.
 */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "FreeRTOS.h"
#include "task.h"

#include "efence_heap.h"

#define MAX_BLOCKS 8192

static struct {
    void *p;
    size_t size;
    int live;
} s_blocks[MAX_BLOCKS];
static size_t s_count;
static long s_live;

static size_t page_round(size_t n) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    return (n + page - 1) / page * page;
}

static void lock(void) {
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) vTaskSuspendAll();
}
static void unlock(void) {
    if (xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED) (void)xTaskResumeAll();
}

void *pvPortMalloc(size_t wanted) {
    if (wanted == 0) return NULL;
    const size_t size = page_round(wanted);
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    lock();
    if (s_count < MAX_BLOCKS) {
        s_blocks[s_count].p = p;
        s_blocks[s_count].size = size;
        s_blocks[s_count].live = 1;
        s_count++;
        s_live++;
    }
    unlock();
    return p;
}

void vPortFree(void *p) {
    if (p == NULL) return;
    lock();
    for (size_t i = 0; i < s_count; i++) {
        if (s_blocks[i].p == p && s_blocks[i].live) {
            s_blocks[i].live = 0;
            s_live--;
            mprotect(p, s_blocks[i].size, PROT_NONE);
            break;
        }
    }
    unlock();
}

long efence_live_blocks(void) { return s_live; }

static int freed_block_at(const void *a) {
    for (size_t i = 0; i < s_count; i++) {
        const uint8_t *b = (const uint8_t *)s_blocks[i].p;
        if (!s_blocks[i].live && (const uint8_t *)a >= b && (const uint8_t *)a < b + s_blocks[i].size) return 1;
    }
    return 0;
}

static void on_segv(int sig, siginfo_t *info, void *ctx) {
    (void)sig;
    (void)ctx;
    static const char used[] = "\n*** USE AFTER FREE: something touched memory vPortFree() had released "
                               "-- a queue or task that unreg_task_ptp() deleted. Test failed.\n";
    static const char other[] = "\n*** SIGSEGV outside any freed block. Test failed.\n";
    if (freed_block_at(info->si_addr)) {
        (void)!write(2, used, sizeof(used) - 1);
    } else {
        (void)!write(2, other, sizeof(other) - 1);
    }
    _exit(3);
}

void efence_unblock_faults_in_this_thread(void) {
    /* The POSIX port blocks every signal in its task threads. A synchronous fault that is
       blocked does not reach a handler: the kernel kills the process outright. */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGSEGV);
    sigaddset(&s, SIGBUS);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
}

void efence_install_fault_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

/* ---- kernel hooks named by the host FreeRTOSConfig.h ---- */

void vAssertCalled(const char *file, int line) {
    fprintf(stderr, "FreeRTOS configASSERT failed: %s:%d\n", file, line);
    _exit(4);
}
void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
void vApplicationMallocFailedHook(void) {
    fprintf(stderr, "FreeRTOS: pvPortMalloc failed\n");
    _exit(5);
}
void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    fprintf(stderr, "FreeRTOS: stack overflow in %s\n", name ? name : "?");
    _exit(6);
}
