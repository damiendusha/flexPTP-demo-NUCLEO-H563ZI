/* Host shim for ARM's cmsis_compiler.h.
 *
 * mac_drv.c uses it for the interrupt-masking primitives around its critical section and
 * the two barriers on the descriptor-ownership handoff.
 *
 * The barriers are no-ops here and that is a real limitation, not an oversight: x86 has a
 * different memory model and the host build cannot say anything about whether the target's
 * __DSB() is correctly placed. Ordering bugs of that kind stay on the bench. What the host
 * CAN test is the logic either side of them.
 */
#ifndef HOST_SHIM_CMSIS_COMPILER_H
#define HOST_SHIM_CMSIS_COMPILER_H

#include <stdint.h>

/* Modelled so the driver's save/restore critical section behaves as it does on target:
   bfifo-style code reads PRIMASK, disables, then restores only if it had been enabled. */
extern uint32_t host_primask;

static inline uint32_t __get_PRIMASK(void) { return host_primask; }
static inline void __set_PRIMASK(uint32_t v) { host_primask = v; }
static inline void __disable_irq(void) { host_primask = 1U; }
static inline void __enable_irq(void) { host_primask = 0U; }

/* Always zero: the host has no exception context.
 *
 * ST's CMSIS-RTOS2 wrapper builds IS_IRQ_MODE() from this, so on the host every API call looks
 * like thread context. That is an honest limitation with a real consequence: the ISR half of
 * the "may not block here" rules -- the one ptp_tx_timestamp.c guards -- is NOT reachable from
 * a host test and stays on the bench. The TIMER-THREAD half is reachable, because ThreadX
 * enforces that in txe_queue_send.c by comparing against &_tx_timer_thread rather than by
 * asking the CPU anything. That is the half F80 lives in. */
static inline uint32_t __get_IPSR(void) { return 0U; }

static inline void __DSB(void) {}
static inline void __DMB(void) {}
static inline void __ISB(void) {}
static inline void __NOP(void) {}

/* ARM's cmsis_compiler.h defines these attribute aliases; the wrapper uses __WEAK on its
   default tx_application_define(). Nothing else in the shim needs them. */
#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif
#ifndef __USED
#define __USED __attribute__((used))
#endif
#ifndef __NO_RETURN
#define __NO_RETURN __attribute__((__noreturn__))
#endif
#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE static inline
#endif

#endif /* HOST_SHIM_CMSIS_COMPILER_H */
