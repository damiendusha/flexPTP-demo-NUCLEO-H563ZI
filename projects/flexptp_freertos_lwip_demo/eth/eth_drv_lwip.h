#ifndef ETHDRV_ETH_DRV_LWIP
#define ETHDRV_ETH_DRV_LWIP

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool init;
    bool up;
    uint16_t speed;
    bool duplex;
} LinkState;

/**
 * @brief Print the driver's link state and RX timestamp context counters, for `ethinfo`.
 *
 * The counterpart of the ThreadX firmware's eth_drv_report(). It reports less, and the comment
 * on the definition says exactly what is missing and why.
 */
void eth_drv_report(void);

#endif /* ETHDRV_ETH_DRV_LWIP */
