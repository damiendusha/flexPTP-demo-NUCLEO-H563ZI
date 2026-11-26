#ifndef FLEXPTP_TEST_EFENCE_HEAP_H
#define FLEXPTP_TEST_EFENCE_HEAP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Blocks allocated and not yet freed. */
long efence_live_blocks(void);

/* Report a fault in freed memory as a use-after-free, and exit non-zero. */
void efence_install_fault_handler(void);

/* Let a fault in the calling FreeRTOS task reach that handler (the POSIX port blocks signals). */
void efence_unblock_faults_in_this_thread(void);

#ifdef __cplusplus
}
#endif

#endif
