#ifndef DRIVERS_ETHDRV_ETH_DRV_NETXDUO_H
#define DRIVERS_ETHDRV_ETH_DRV_NETXDUO_H

#include "nx_api.h"

/** NetX Duo driver entry point for the STM32H563's Ethernet MAC.
 *
 *  Passed to nx_ip_create(). Built on Drivers/EthDrv/mac_drv.c rather than on ST's
 *  HAL_ETH-based nx_stm32_eth_driver.c, because hardware timestamping needs the DMA
 *  descriptors the HAL abstracts away. */
VOID nx_h563_eth_driver(NX_IP_DRIVER *request);

/** Print driver state and counters. Bring-up and S6 diagnostic, reachable as `ethinfo`. */
void eth_drv_report(void);

#endif /* DRIVERS_ETHDRV_ETH_DRV_NETXDUO_H */
