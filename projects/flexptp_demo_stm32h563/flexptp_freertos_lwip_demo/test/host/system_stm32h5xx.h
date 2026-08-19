/* Host shim for ST's system header. The device header includes it for SystemCoreClock;
   nothing under test reads it, but it must exist and be a real symbol. */
#ifndef HOST_SHIM_SYSTEM_STM32H5XX_H
#define HOST_SHIM_SYSTEM_STM32H5XX_H
#include <stdint.h>
extern uint32_t SystemCoreClock;
#endif
