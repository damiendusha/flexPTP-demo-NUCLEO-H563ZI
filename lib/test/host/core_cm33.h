/* Host shim for ARM's core_cm33.h.
 *
 * ST's CMSIS device header (stm32h5xx.h -> stm32h563xx.h) includes this for exactly five
 * things: the __IO/__IM/__OM/__I/__O access qualifiers, and __NVIC_PRIO_BITS. Everything
 * else it needs -- the IRQn enum, every peripheral struct, every bit mask -- it defines
 * itself, so shadowing this one header is enough to compile the REAL device header on x86.
 *
 * That matters: the alternative is hand-copying register layouts and bit positions into a
 * fake, where a single wrong bit makes a test lie while staying green. Nothing here is
 * copied from the device -- it is all pulled from ST's own headers at compile time, and it
 * tracks the pinned HAL submodule automatically.
 *
 * Deliberately does NOT provide NVIC/SCB/SysTick. Nothing under test touches them, and a
 * stub for a core peripheral would be a silent lie rather than a link error.
 */
#ifndef HOST_SHIM_CORE_CM33_H
#define HOST_SHIM_CORE_CM33_H

#include <stdint.h>

#define __I   volatile const
#define __O   volatile
#define __IO  volatile
#define __IM  volatile const
#define __OM  volatile
#define __IOM volatile

#ifndef __NVIC_PRIO_BITS
#define __NVIC_PRIO_BITS 4
#endif

#endif /* HOST_SHIM_CORE_CM33_H */
