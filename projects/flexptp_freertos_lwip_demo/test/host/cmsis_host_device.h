/* Host stand-in for the CMSIS device header, for the FreeRTOS context tests.
 *
 * ST's FreeRTOS-backed cmsis_os2.c reaches for Cortex-M hardware that x86 does not have:
 * NVIC_SetPriority(SVCall_IRQn, 0) when the kernel starts, and SysTick->{CTRL,LOAD,VAL} in
 * osKernelGetTickCount() and friends. The ThreadX project's core_cm33.h shim deliberately
 * provides none of this, with a comment saying nothing under test touches it -- true there,
 * because ST's THREADX wrapper does not. Its FreeRTOS wrapper does, which is a difference
 * between the two vendored wrappers rather than between the two kernels.
 *
 * These stubs exist so the wrapper compiles and links on the host. They are deliberately inert:
 * the POSIX port drives the tick from a host timer and never consults SysTick, so nothing the
 * tests assert depends on these values. If a test ever DID depend on one, it would be testing
 * the stub rather than the firmware -- so the SysTick registers read as zero rather than as
 * something plausible, to make that failure loud instead of subtle.
 *
 * Reached via -DCMSIS_device_header, which is exactly the indirection that header exists for.
 */
#ifndef CMSIS_HOST_DEVICE_H
#define CMSIS_HOST_DEVICE_H

#include <stdint.h>

/* IRQn_Type is NOT declared here, and NOT taken as the parameter type either.
 *
 * ST's real stm32h563xx.h provides that enum, but freertos_os2.h includes CMSIS_device_header
 * (this file) at line 30 and cmsis_os2.c includes stm32h5xx.h at line 37 -- so this header is
 * read FIRST, before the enum exists. int32_t takes the enum by promotion and does not care
 * about the ordering. */

static inline void NVIC_SetPriority(int32_t irq, uint32_t priority) {
    (void)irq;
    (void)priority;
}

typedef struct {
    volatile uint32_t CTRL;
    volatile uint32_t LOAD;
    volatile uint32_t VAL;
    volatile uint32_t CALIB;
} SysTick_Type;

/* Zeroed, and never written by anything on the host. See the note above on why these are inert
   rather than plausible. */
extern SysTick_Type cmsis_host_systick;
#define SysTick (&cmsis_host_systick)

#endif /* CMSIS_HOST_DEVICE_H */
