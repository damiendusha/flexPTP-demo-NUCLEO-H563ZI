/* ThreadX configuration for this project.
 *
 * Reached from ports/cortex_m33/gnu/inc/tx_port.h, which includes this file only when
 * TX_INCLUDE_USER_DEFINE_FILE is defined -- see Middlewares/CMakeLists.txt.
 *
 * This is the ThreadX counterpart of Inc/FreeRTOSConfig.h. Every setting below that differs
 * from the ThreadX default is a decision with a reason; the reasons are here rather than in
 * the porting document, because this is the file someone will be looking at when it matters.
 */

#ifndef TX_USER_H
#define TX_USER_H

/* --- Required by the CMSIS-RTOS2 wrapper ---------------------------------------------- */

/* ST's wrapper has a hard #error on anything else (cmsis_os2.c:134). It maps CMSIS
   priorities as tx_priority = osPriorityISR - cmsis_priority, and osPriorityISR is 56, so
   the ThreadX scale has to be at least that deep. osPriorityNormal (24) becomes 32. */
#define TX_MAX_PRIORITIES 64

/* --- Tick rate ------------------------------------------------------------------------ */

/* 1000 Hz, matching the FreeRTOS configTICK_RATE_HZ this port replaces.
 *
 * NOT the ThreadX default of 100. flexPTP's heartbeat is PTP_HEARTBEAT_TICKRATE_MS = 31 ms
 * and FLEXPTP_MS_TO_TICKS derives its message TTLs from it; at a 10 ms tick the heartbeat
 * quantises to 40 ms and every TTL in the stack shifts with it. Keeping 1000 Hz makes the
 * kernel swap invisible to flexPTP's timing.
 *
 * tx_initialize_low_level.S derives the SysTick reload from SYSTEM_CLOCK and this value;
 * both are passed to the assembler from Middlewares/CMakeLists.txt. */
#define TX_TIMER_TICKS_PER_SECOND 1000

/* --- Timer thread --------------------------------------------------------------------- */

/* TX_TIMER_PROCESS_IN_ISR is deliberately NOT defined, and this is load-bearing rather than
 * a default left alone.
 *
 * flexPTP's heartbeat callback calls ptp_event_enqueue(), which in the FLEXPTP_CMSIS_OS2
 * binding does osMessageQueuePut(..., osWaitForever). CMSIS-RTOS2 forbids a non-zero timeout
 * from interrupt context and ST's wrapper enforces it, returning osErrorParameter -- which
 * flexPTP does not check. Defining TX_TIMER_PROCESS_IN_ISR would run that callback in the
 * SysTick ISR and silently stop the PTP heartbeat.
 *
 * See docs/threadx_netxduo_feasibility.md, finding 1: the same defect on the transmit path
 * is the sharpest hazard in this whole port. */
/* #define TX_TIMER_PROCESS_IN_ISR */

#define TX_TIMER_THREAD_STACK_SIZE 1024
#define TX_TIMER_THREAD_PRIORITY   0    /* highest; ThreadX counts 0 as most urgent */

/* --- Stack checking -------------------------------------------------------------------- */

/* Complements, and does not replace, the hardware trap. The Cortex-M33 port programs PSPLIM
 * on every context switch (ports/cortex_m33/gnu/src/tx_thread_schedule.S), so an overflow
 * faults on the offending push and is caught by the UsageFault handler in Src/main.c long
 * before any software pattern check would run. This adds the software check on top, in Debug
 * only, because it also catches a stack that came close without faulting. */
#ifdef DEBUG
#define TX_ENABLE_STACK_CHECKING
#endif

/* --- Required by the CMSIS-RTOS2 wrapper, continued -------------------------------------- */

/* The wrapper stores each thread's detached/joinable state in a ThreadX user extension, and
   #errors without it (cmsis_os2.c:126). It must be exactly this field, by this name: the
   wrapper reads thread_ptr->tx_thread_detached_joinable directly. */
#define TX_THREAD_USER_EXTENSION ULONG tx_thread_detached_joinable;

/* --- Error checking --------------------------------------------------------------------- */

/* TX_DISABLE_ERROR_CHECKING is NOT defined, in either configuration.
 *
 * The intent was to mirror configASSERT() -- checked in Debug, compiled out in Release -- but
 * the wrapper forbids it outright (cmsis_os2.c:130). It relies on ThreadX validating object
 * pointers and returning error codes it can translate into osStatus_t; with checking removed,
 * a bad handle is undefined behaviour inside the kernel instead of osErrorParameter at the
 * API boundary.
 *
 * The cost is a little code size and a few cycles per call in Release. Accepted: this
 * firmware sits at 10% of a 2 MB flash, and the checks are what turn a wrapper-level misuse
 * into a diagnosable return value. */

/* --- Interrupt masking ------------------------------------------------------------------ */

/* TX_PORT_USE_BASEPRI is deliberately NOT defined: the port's default is PRIMASK, which
 * masks everything inside a kernel critical section.
 *
 * This is a real difference from FreeRTOS, where configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
 * was 5 and interrupts at 0-4 were never masked but also could not call the kernel. Under
 * ThreadX that partition disappears: any ISR may call ThreadX services, and all of them are
 * masked in critical sections. Nothing regresses here because all three of this project's
 * interrupts already sit at 7 and 12 -- both below the old masking level -- but
 * docs/architecture.md's "FreeRTOS priority constraint" section describes a rule that no
 * longer applies and is rewritten in S8.
 *
 * If an interrupt with hard latency requirements is ever added, define TX_PORT_USE_BASEPRI
 * and TX_PORT_BASEPRI together and re-derive the priorities. */
/* #define TX_PORT_USE_BASEPRI */

#endif /* TX_USER_H */
