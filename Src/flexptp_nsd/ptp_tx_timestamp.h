#ifndef SRC_FLEXPTP_NSD_PTP_TX_TIMESTAMP_H
#define SRC_FLEXPTP_NSD_PTP_TX_TIMESTAMP_H

#include <stdbool.h>
#include <stdint.h>

/** Create the deferral queue and thread. Idempotent. */
bool ptp_tx_timestamp_init(void);

/** Hand a captured transmit timestamp over from the Ethernet ISR.
 *
 *  Signature matches the MAC driver's ETHHW_DescExt.tsCbPtr contract: (sec, nsec, tag), with
 *  the tag carrying flexPTP's message uid. Safe from interrupt context -- that is its purpose.
 *  See the file header of ptp_tx_timestamp.c for why calling flexPTP directly is not. */
void ptp_tx_timestamp_from_isr(uint32_t ts_s, uint32_t ts_ns, void *tag);

/** Counters for the `ptp diag ts` CLI command. Any pointer may be NULL. */
void ptp_tx_timestamp_stats(uint32_t *posted, uint32_t *delivered, uint32_t *dropped);

#endif /* SRC_FLEXPTP_NSD_PTP_TX_TIMESTAMP_H */
