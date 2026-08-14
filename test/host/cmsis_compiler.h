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

static inline void __DSB(void) {}
static inline void __DMB(void) {}
static inline void __ISB(void) {}
static inline void __NOP(void) {}

#endif /* HOST_SHIM_CMSIS_COMPILER_H */
