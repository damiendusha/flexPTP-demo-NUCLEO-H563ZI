#ifndef SRC_ETHERNET_ETHERNET_NETXDUO_H
#define SRC_ETHERNET_ETHERNET_NETXDUO_H

#include <stdbool.h>

#include "nx_api.h"

/** Create the packet pool, IP instance, ARP, ICMP, UDP and IGMP.
 *  Thread context only, after the kernel is running. False on any failure, having reported it. */
bool init_ethernet(void);

/** The IP instance, or NX_NULL before init_ethernet() has succeeded. Used by the flexPTP
 *  network stack driver, which needs it for sockets and for the link-layer PTP path. */
NX_IP *ethernet_ip(void);

/** The packet pool, or NX_NULL before init_ethernet() has succeeded. */
NX_PACKET_POOL *ethernet_packet_pool(void);

/** Report the DHCP-assigned address when it appears, and its loss. Call periodically from a
 *  thread; it prints through MSG(), which blocks on the console FIFO and so must not be
 *  called from a DHCP callback or an ISR. */
void ethernet_poll_dhcp(void);

#endif /* SRC_ETHERNET_ETHERNET_NETXDUO_H */
