/* FreeRTOSConfig.h for the HOST context tests.
 *
 * Deliberately NOT the firmware's. That file configures the Cortex-M33 -- interrupt priorities,
 * a 30 KB heap sized for the board's RAM, a 130-word minimal stack -- none of which the POSIX
 * port has any use for, and the stack figure is far too small for a GoogleTest binary.
 *
 * The settings that ARE kernel behaviour are copied verbatim, because those are what the tests
 * are about: the tick rate, the priority scale, and above all configUSE_TIMERS with a timer task
 * that is an ORDINARY TASK. That last point is the whole subject of
 * BlockingEnqueueFromATimerCallbackSUCCEEDS -- FreeRTOS has no equivalent of ThreadX's
 * restriction on its timer thread, so a blocking call from a timer callback is legal here and
 * is not on ThreadX (finding F80).
 *
 * The same split, and the same reasoning, as the ThreadX project's host byte-pool sizes: share
 * what the kernel does, not how much RAM it is given.
 */
#ifndef FREERTOS_CONFIG_HOST_H
#define FREERTOS_CONFIG_HOST_H

/* --- copied from the firmware: kernel behaviour under test --------------------------------- */
#define configUSE_PREEMPTION            1
/* The wrapper's osKernelGetSysTimerFreq() divides by this. Nothing on the host consults the
   result, but it has to be defined for the file to compile. */
#define configCPU_CLOCK_HZ              ((unsigned long)250000000)
#define configTICK_RATE_HZ              ((TickType_t)1000)
#define configMAX_PRIORITIES            (56)
#define configUSE_TIMERS                1
#define configTIMER_TASK_PRIORITY       (2)
#define configTIMER_QUEUE_LENGTH        10
#define configUSE_MUTEXES               1
#define configUSE_RECURSIVE_MUTEXES     1
#define configUSE_COUNTING_SEMAPHORES   1
#define configUSE_16_BIT_TICKS          0

/* --- host-sized, and deliberately different -------------------------------------------------
 *
 * A GoogleTest binary is not a 2 KB CLI thread. Sizing these like the board's just makes
 * osThreadNew() fail at startup, which is what happened first time on the ThreadX side. */
#define configMINIMAL_STACK_SIZE        ((unsigned short)8192)
#define configTIMER_TASK_STACK_DEPTH    (configMINIMAL_STACK_SIZE)
#define configTOTAL_HEAP_SIZE           ((size_t)(4 * 1024 * 1024))

#define configSUPPORT_STATIC_ALLOCATION  0
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configUSE_IDLE_HOOK             0
#define configUSE_TICK_HOOK             0
#define configCHECK_FOR_STACK_OVERFLOW  0
#define configUSE_MALLOC_FAILED_HOOK    1
#define configASSERT(x)                 do { if (!(x)) { extern void vAssertCalled(const char *, int); vAssertCalled(__FILE__, __LINE__); } } while (0)
#define configUSE_TRACE_FACILITY        1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0

/* Every INCLUDE_* the CMSIS-RTOS2 wrapper #errors without. It checks them individually, so a
   missing one is a compile error naming the API it belongs to rather than a link failure. */
#define INCLUDE_eTaskGetState           1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xSemaphoreGetMutexHolder 1
#define INCLUDE_xTaskDelayUntil         1
#define INCLUDE_vTaskDelete             1
#define INCLUDE_vTaskDelay              1
#define INCLUDE_vTaskSuspend            1
#define INCLUDE_xTaskGetSchedulerState  1
#define INCLUDE_xTimerPendFunctionCall  1
#define INCLUDE_uxTaskPriorityGet       1
#define INCLUDE_vTaskPrioritySet        1
#define INCLUDE_xTaskGetCurrentTaskHandle 1

#endif /* FREERTOS_CONFIG_HOST_H */
