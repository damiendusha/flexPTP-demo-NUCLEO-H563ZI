#ifndef HW_PORT_PTP_PORT_STM32H563_H_
#define HW_PORT_PTP_PORT_STM32H563_H_

/**
 * flexPTP hardware port for the STM32H563 (NUCLEO-H563ZI).
 *
 * flexPTP ships no STM32H5 port, so this one lives in the project rather than in the
 * submodule. It is derived from the bundled ptp_port_stm32h743_lwip.c.
 *
 * The timestamp unit is driven from HCLK; see docs/ptp_clock.md.
 */

#include <flexptp/timeutils.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize PTP hardware.
 *
 * @param increment PTP hardware clock tick increment in nanoseconds
 * @param addend initial frequency tuning code word
 */
void ptphw_init(uint32_t increment, uint32_t addend);

/**
 * Get time right from the hardware clock.
 *
 * NOTE: the argument is TimestampU, not TimestampI. The published flexPTP porting guide
 * says TimestampI, but every call site in the core passes TimestampU. The guide is wrong.
 *
 * @param pTime pointer to a TimestampU object.
 */
void ptphw_gettime(TimestampU *pTime);

#ifdef __cplusplus
}
#endif

#endif /* HW_PORT_PTP_PORT_STM32H563_H_ */
