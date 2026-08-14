#ifndef DRIVERS_ETHDRV_ETH_DRV_NETXDUO_H
#define DRIVERS_ETHDRV_ETH_DRV_NETXDUO_H

#include <stdint.h>

#include "nx_api.h"

/** NetX Duo driver entry point for the STM32H563's Ethernet MAC.
 *
 *  Passed to nx_ip_create(). Built on Drivers/EthDrv/mac_drv.c rather than on ST's
 *  HAL_ETH-based nx_stm32_eth_driver.c, because hardware timestamping needs the DMA
 *  descriptors the HAL abstracts away. */
VOID nx_h563_eth_driver(NX_IP_DRIVER *request);

/** Request a hardware transmit timestamp for this packet, tagged with flexPTP's message uid.
 *
 *  Called by the flexPTP NSD immediately before handing the packet to NetX Duo. The uid cannot
 *  travel in NX_PACKET -- there is no field for it, unlike lwIP where this project injected
 *  one via LWIP_PBUF_CUSTOM_DATA -- so it is recorded here and matched back when the driver
 *  sees the packet on its way out. */
void eth_drv_request_tx_timestamp(NX_PACKET *packet, uint32_t uid);

/** Withdraw a request made by eth_drv_request_tx_timestamp(). Must be called if the send
 *  fails, or the record outlives the packet and the next allocation at the same address
 *  inherits its uid. */
void eth_drv_cancel_tx_timestamp(NX_PACKET *packet);

/** Print driver state and counters. Bring-up and S6 diagnostic, reachable as `ethinfo`. */
void eth_drv_report(void);

#endif /* DRIVERS_ETHDRV_ETH_DRV_NETXDUO_H */
