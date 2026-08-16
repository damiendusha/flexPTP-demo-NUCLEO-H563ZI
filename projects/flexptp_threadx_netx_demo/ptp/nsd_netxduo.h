/* The NetX Duo network-stack driver's diagnostics, for the application to compose.
 *
 * WHY THIS HEADER EXISTS
 *
 * These counters used to be printed from eth_drv_report(), reached through four hand-written
 * `extern` declarations inside the function body. The instinct behind that was right -- the
 * DRIVER must not depend on the NSD, which sits above it -- but four unheadered prototypes are
 * four chances for a silent signature drift, which is precisely finding F48 (AR-03).
 *
 * The dependency does not need inverting; it needs to go to the layer that legitimately has
 * both. `ethinfo` is an application command, and Src/cmds.c may depend on the driver AND the
 * NSD AND flexPTP. So the driver reports the driver, this reports the NSD, and cmds.c calls
 * them in order.
 */
#ifndef FLEXPTP_NSD_NSD_NETXDUO_H
#define FLEXPTP_NSD_NSD_NETXDUO_H

#include <stdint.h>

/** Receive/transmit counters. Any pointer may be NULL. */
void ptp_nsd_stats(uint32_t *notify, uint32_t *packets, uint32_t *enqueued, uint32_t *tx);

/** Frames the 802.3 path refused: runts, and chained packets. Any pointer may be NULL. */
void ptp_nsd_reject_stats(uint32_t *runts, uint32_t *chained);

/** Histogram of received PTP message types, indexed by the low nibble of byte 0. 16 entries. */
void ptp_nsd_rx_types(uint32_t *out16);

/** The first 16 bytes of the last frame handed to flexPTP, and its full length. */
void ptp_nsd_last_rx(uint8_t *out, uint32_t *len);

/** Print all of the above. Thread context only -- it uses MSG(). */
void ptp_nsd_report(void);

#endif /* FLEXPTP_NSD_NSD_NETXDUO_H */
