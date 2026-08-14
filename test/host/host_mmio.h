/* Host-side stand-ins for the memory-mapped peripherals mac_drv.c touches.
 *
 * On target, `ETH`, `RCC`, `GPIOx` and `SBS` are macros expanding to fixed addresses in the
 * peripheral region. On x86 those are wild pointers, so mac_drv_uut.c redirects each macro
 * at one of the blocks below before including the driver.
 *
 * These are plain zeroed structs, not a hardware model: writing a register here has no
 * consequences. The *behaviour* of the DMA -- OWN bits flipping, timestamps appearing in
 * context descriptors, RBU on ring exhaustion -- is modelled explicitly and visibly in
 * eth_model.hpp, because a hardware model that is wrong is a test that passes for the wrong
 * reason, and it should be somewhere a reader will look.
 */
#ifndef HOST_MMIO_H
#define HOST_MMIO_H

#include "stm32h5xx.h"

#ifdef __cplusplus
extern "C" {
#endif

extern ETH_TypeDef *host_eth;
extern RCC_TypeDef *host_rcc;
extern GPIO_TypeDef *host_gpioa;
extern GPIO_TypeDef *host_gpiob;
extern GPIO_TypeDef *host_gpioc;
extern GPIO_TypeDef *host_gpiog;
extern SBS_TypeDef *host_sbs;

/** Zero every peripheral block. Call between tests. */
void host_mmio_reset(void);

/* --- observation points for the HAL calls the driver makes ------------------ */

typedef struct {
    unsigned gpio_init_calls;
    unsigned nvic_setpriority_calls;
    unsigned nvic_enable_calls;
    unsigned sbs_eth_iface_calls;
    uint32_t last_nvic_irqn;
    uint32_t last_nvic_priority;
    uint32_t last_sbs_iface;
} HostHalLog;

extern HostHalLog host_hal_log;

/** Captured MSG() output, so a test can assert that a fault path reported itself. */
const char *host_msg_text(void);
void host_msg_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* HOST_MMIO_H */
