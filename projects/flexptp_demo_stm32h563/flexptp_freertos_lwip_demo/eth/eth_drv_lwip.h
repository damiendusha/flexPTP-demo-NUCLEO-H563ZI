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

/* Where raw-802.3 (EtherType 0x88F7) PTP frames went, as a histogram over distinct
 * destination MACs. The 802.3 destination is selected inside flexPTP and is otherwise only
 * observable with a packet capture, which needs root.
 *
 * eth_drv_ptp_l2_dst() returns false once `slot` is past the last used entry, so a caller
 * walks slots until it does. eth_drv_clear_ptp_l2_dst() resets the histogram, which is what
 * makes a before/after comparison across a profile change possible at all. */
bool eth_drv_ptp_l2_dst(unsigned slot, uint8_t *mac6, uint32_t *pCount);
void eth_drv_clear_ptp_l2_dst(void);
uint32_t eth_drv_ptp_l2_frames(void);

#endif /* ETHDRV_ETH_DRV_LWIP */
