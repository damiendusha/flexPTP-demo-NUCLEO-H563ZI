/* Which flexPTP message a pending transmit timestamp belongs to.
 *
 * lwIP let this project bolt `tag` and `tx_cb` onto struct pbuf through LWIP_PBUF_CUSTOM_DATA.
 * NX_PACKET has no such hook and no spare field, so the association has to live beside the
 * packet, keyed on its address.
 *
 * Extracted from eth_drv_netxduo.c so it can be tested. It is deliberately ignorant of
 * NX_PACKET -- it keys on `const void *` -- which is what lets the tests run without NetX Duo.
 */

#ifndef SRC_FLEXPTP_NSD_PTP_TX_REGISTRY_H
#define SRC_FLEXPTP_NSD_PTP_TX_REGISTRY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* flexPTP has at most a Sync and a Delay_Req outstanding, so four is ample and a linear scan
   of four is cheaper than anything cleverer. */
#define PTP_TX_REGISTRY_CAPACITY (4)

typedef struct {
    const void *key;  /* NX_PACKET *, as an opaque address. NULL means the slot is free */
    uint32_t uid;     /* flexPTP's message uid */
} PtpTxRegistryEntry;

typedef struct {
    PtpTxRegistryEntry entries[PTP_TX_REGISTRY_CAPACITY];
    uint32_t overflows; /* registrations refused because the table was full */
} PtpTxRegistry;

/** Empty the table. */
void ptp_tx_registry_reset(PtpTxRegistry *reg);

/** Associate `uid` with `key`. False if the table is full, or `key` is NULL.
 *
 *  Re-registering a key that is already present OVERWRITES it rather than adding a second
 *  entry: two live entries for one address could otherwise be taken in either order. */
bool ptp_tx_registry_add(PtpTxRegistry *reg, const void *key, uint32_t uid);

/** Forget `key`, if present. Must be called when a send fails, or the entry outlives the
 *  packet and the next allocation at the same address inherits its uid. */
void ptp_tx_registry_cancel(PtpTxRegistry *reg, const void *key);

/** Consume the entry for `key`: writes the uid and frees the slot.
 *
 *  TAKE, not read. Clearing on consumption is half of what makes the pointer key safe -- NetX
 *  Duo recycles packets, and an entry left behind would be matched by a later, unrelated
 *  packet allocated at the same address. */
bool ptp_tx_registry_take(PtpTxRegistry *reg, const void *key, uint32_t *uid);

/** Entries currently held. Diagnostics only. */
unsigned ptp_tx_registry_count(const PtpTxRegistry *reg);

#ifdef __cplusplus
}
#endif

#endif /* SRC_FLEXPTP_NSD_PTP_TX_REGISTRY_H */
