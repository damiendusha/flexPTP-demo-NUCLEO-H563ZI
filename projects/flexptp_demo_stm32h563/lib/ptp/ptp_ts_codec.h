/* Reading a receive timestamp back out of a NetX Duo packet buffer.
 *
 * NetX Duo does not carry the timestamp in a struct field. nx_link.c writes it over the front
 * of the packet buffer -- the Ethernet header it has already consumed:
 *
 *     ((ULONG *)packet -> nx_packet_data_start)[0] = time -> nano_second;
 *     ((ULONG *)packet -> nx_packet_data_start)[1] = time -> second_low;
 *     ((ULONG *)packet -> nx_packet_data_start)[2] = time -> second_high;
 *
 * That is an undocumented layout convention carrying a timestamp through three protocol
 * layers, which is the same class of unversioned contract as the lwIP tx_cb signature that
 * drifted in F48. Extracted here so the convention -- and its precondition -- can be tested.
 */

#ifndef SRC_FLEXPTP_NSD_PTP_TS_CODEC_H
#define SRC_FLEXPTP_NSD_PTP_TS_CODEC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Three ULONGs. The driver must leave at least this much between nx_packet_data_start and the
   payload, or NetX Duo's write lands on live data. eth_drv_netxduo.c satisfies it by copying
   the frame at data_start + 2, so the 14-byte Ethernet header covers +2..+15. */
#define PTP_TS_HEADROOM_BYTES (12U)

typedef struct {
    uint32_t sec;
    uint32_t nanosec;
} PtpTsCodecResult;

/**
 * Read the timestamp NetX Duo stashed at the start of a packet buffer.
 *
 * @param data_start  the packet's nx_packet_data_start
 * @param headroom    bytes between data_start and the payload -- what the caller guarantees
 * @return false, leaving *out zeroed, if the headroom is too small for the convention to hold.
 *
 * Returning false rather than reading anyway is the point: a short buffer means the three
 * ULONGs were never written there, so what is present is payload, and a plausible-looking
 * wrong timestamp is worse than none. Same reasoning as zeroing an unstamped frame's fields
 * in ETHHW_ProcessRx().
 */
bool ptp_ts_codec_extract(const void *data_start, uint32_t headroom, PtpTsCodecResult *out);

#ifdef __cplusplus
}
#endif

#endif /* SRC_FLEXPTP_NSD_PTP_TS_CODEC_H */
