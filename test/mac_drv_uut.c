/* mac_drv.c under test, compiled for the host.
 *
 * This translation unit #includes the driver's .c file verbatim. That is deliberate, and it
 * is the whole reason no production code had to change to make the driver testable:
 *
 *   - 97 of mac_drv.c's 98 register accesses already go through its `ETH_TypeDef *eth`
 *     parameter, so they follow whatever pointer a test passes. The seam was already there.
 *   - The remaining few sites reach the peripherals by name -- `ETH` in ETHHW_RestoreRXDesc()
 *     (documented in place as a deliberate choice), and RCC/GPIOx/SBS inside ETHHW_Init()'s
 *     clock-and-pin preamble. Those macros are redirected below at host memory.
 *   - Including the .c rather than linking it also exposes the file-static helpers, which is
 *     what lets a test drive ETHHW_ProcessTx()'s ordering logic directly.
 *
 * The porting plan budgeted a stage for adding a seam to ETHHW_Init(). It turned out not to
 * need one: redirecting the peripheral macros covers it, and production code stays untouched.
 */

/* Real ST headers first, so the peripheral macros exist to be undefined. */
#include "stm32h5xx.h"
#include "stm32h5xx_hal.h"

#include "host_mmio.h"

uint32_t host_primask = 0U;

/* The shim's IPSR. Zero means thread context, which is what every test here wants
   except the ISR-context cases in test_rtos_context.cpp. */
uint32_t host_fake_ipsr = 0U;

/* Point every by-name peripheral reference at host memory. The include guards mean the
   driver's own re-inclusion of these headers cannot put the original definitions back. */
#undef ETH
#define ETH host_eth
#undef RCC
#define RCC host_rcc
#undef GPIOA
#define GPIOA host_gpioa
#undef GPIOB
#define GPIOB host_gpiob
#undef GPIOC
#define GPIOC host_gpioc
#undef GPIOG
#define GPIOG host_gpiog
#undef SBS
#define SBS host_sbs

#include "mac_drv.c"
