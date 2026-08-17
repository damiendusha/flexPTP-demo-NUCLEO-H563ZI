/* Host-harness glue for the FreeRTOS context tests.
 *
 * Everything here exists because a FreeRTOS binary needs symbols that a firmware provides from
 * its startup code and its application, and a test binary otherwise does not. None of it is
 * under test; it is the scaffolding that lets the parts that ARE under test -- the CMSIS-RTOS2
 * wrapper's context rules -- run on a host.
 *
 * Kept in one file with this note rather than scattered, so that a reader who finds an empty
 * vApplicationIdleHook() does not have to wonder whether it was meant to do something.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

/* PRIMASK, for the cmsis_compiler.h shim. On the target this is the real interrupt mask; here
   it is a variable, which is enough for code that only saves and restores it. */
uint32_t host_primask = 0;

/* The FreeRTOS POSIX port drives its tick from a host timer and never calls this, but ST's
   cmsis_os2.c defines a SysTick_Handler that references it. */
void xPortSysTickHandler(void);
void xPortSysTickHandler(void) {}

/* configASSERT. A failed assertion in the KERNEL is a test failure, not something to continue
   past -- abort() so it surfaces as a crash with a message rather than as a later, stranger
   symptom. */
void vAssertCalled(const char *file, int line);
void vAssertCalled(const char *file, int line) {
    fprintf(stderr, "FreeRTOS configASSERT failed: %s:%d\n", file, line);
    abort();
}

/* The hooks. Declared by FreeRTOS whenever the corresponding configUSE_* is set, and the POSIX
   port references some regardless. */
void vApplicationIdleHook(void) {}

void vApplicationTickHook(void) {}

void vApplicationMallocFailedHook(void) {
    fprintf(stderr, "FreeRTOS malloc failed -- raise configTOTAL_HEAP_SIZE in "
                    "test/host/FreeRTOSConfig.h\n");
    abort();
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name);
void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    fprintf(stderr, "FreeRTOS stack overflow in task '%s' -- raise "
                    "configMINIMAL_STACK_SIZE\n", name ? name : "?");
    abort();
}
