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

/* Settable, and zero unless a test says otherwise.
 *
 * ST's CMSIS-RTOS2 wrapper builds IS_IRQ_MODE() from __get_IPSR(), and this used to hard-code
 * 0 -- so on the host every API call looked like thread context and the ISR half of the "may
 * not block here" rules was unreachable. That was recorded as a permanent limitation in three
 * documents. It is not permanent; it is a property of the shim (finding TS-09).
 *
 * Setting host_fake_ipsr makes the WRAPPER's rule assertable:
 *
 *     if ((IS_IRQ_MODE() && (timeout != 0)) || ...) status = osErrorParameter;
 *
 * which is the exact line quoted at the top of Src/flexptp_nsd/ptp_tx_timestamp.c, and the
 * reason that file exists.
 *
 * BE PRECISE ABOUT WHAT THIS PROVES. It is not an interrupt. ThreadX's own kernel-entry paths
 * still believe they are in a thread, nothing is preempted, and no real exception frame exists.
 * It pins the wrapper's guard -- that a non-zero timeout from a context reporting IRQ mode is
 * refused -- and nothing about the silicon. The claim is therefore narrower than the
 * timer-thread tests next door, which run against a real kernel restriction in
 * txe_queue_send.c. */
extern uint32_t host_fake_ipsr;
static inline uint32_t __get_IPSR(void) { return host_fake_ipsr; }

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
