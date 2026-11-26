/* flexPTP options for the host teardown test: the FreeRTOS binding, as the FreeRTOS + lwIP
 * firmware uses it, on FreeRTOS's POSIX port. Same port fakes and servo as the OS-less build.
 */
#ifndef FLEXPTP_TEST_OPTIONS_FREERTOS
#define FLEXPTP_TEST_OPTIONS_FREERTOS

#include <stdio.h>
#include <stdlib.h>

#define FLEXPTP_FREERTOS (1)
#define FLEXPTP_TASK_PRIORITY (configMAX_PRIORITIES / 2)
#include "fake_port.h"

/* A Cortex-M port function; the POSIX port has no interrupts for flexPTP to be inside. */
#define xPortIsInsideInterrupt() (0)

#ifdef __cplusplus
extern "C" {
#endif
void MSG(const char *format, ...);
#ifdef __cplusplus
}
#endif

#define FLEXPTP_SNPRINTF(...) snprintf(__VA_ARGS__)

#define PTP_ADDEND_INTERFACE
#define PTP_MAIN_OSCILLATOR_FREQ_HZ (250000000)
#define PTP_INCREMENT_NSEC (5)
#define PTP_HW_INIT(increment, addend) fake_ptphw_init(increment, addend)
#define PTP_SET_CLOCK(s, ns) fake_set_clock((int64_t)(s), (int64_t)(ns))
#define PTP_SET_ADDEND(addend) fake_set_addend(addend)
#define PTP_HW_GET_TIME(pt) fake_get_time(pt)

#include <flexptp/servo/kalman_filter.h>
#define PTP_SERVO_INIT() kalman_filter_init()
#define PTP_SERVO_DEINIT() kalman_filter_deinit()
#define PTP_SERVO_RESET() kalman_filter_reset()
#define PTP_SERVO_RUN(d, pscd) kalman_filter_run(d, pscd)

#define CLILOG(en, ...)       \
    {                         \
        if (en)               \
            MSG(__VA_ARGS__); \
    }

#define PTP_ENABLE_MASTER_OPERATION (1)
#define PTP_CLOCK_PRIORITY1 (128)
#define PTP_CLOCK_PRIORITY2 (128)
#define PTP_BEST_CLOCK_CLASS (PTP_CC_DEFAULT)
#define PTP_WORST_ACCURACY (PTP_CA_UNKNOWN)
#define PTP_TIME_SOURCE (PTP_TSRC_INTERNAL_OSCILLATOR)

#include <flexptp/event.h>
#ifdef __cplusplus
extern "C" {
#endif
void fake_user_event_cb(PtpUserEventCode uev);
#ifdef __cplusplus
}
#endif
#define PTP_USER_EVENT_CALLBACK fake_user_event_cb

#endif
