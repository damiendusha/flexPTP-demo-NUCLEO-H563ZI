/* Where a PTP message goes, as a function of transport and delay mechanism.
 *
 * Five cases -- two transports x two delay mechanisms, plus teardown -- and getting one wrong
 * presents as "no response received" rather than as anything resembling a routing bug. It was
 * a nest of ternaries spread across nsd_lwip.c; here it is one table with tests.
 *
 * Owns its own enums rather than taking flexPTP's. PtpTransportType and PtpDelayMechanism come
 * from ptp_types.h, which pulls in flexptp_options.h and with it the CLI, embfmt, the MSG()
 * backend and the H563 hardware port -- none of which a decision table needs, and all of which
 * would put this unit out of reach of a host test. The NSD maps across at one call site.
 */

#ifndef SRC_FLEXPTP_NSD_PTP_ENDPOINT_H
#define SRC_FLEXPTP_NSD_PTP_ENDPOINT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PTP_EP_TRANSPORT_NONE = 0, /* torn down */
    PTP_EP_TRANSPORT_IPV4,
    PTP_EP_TRANSPORT_802_3
} PtpEndpointTransport;

typedef enum {
    PTP_EP_DELAY_E2E = 0,
    PTP_EP_DELAY_P2P
} PtpEndpointDelay;

/* IEEE 1588 event/general split: event messages are the timestamped ones. */
#define PTP_EP_PORT_EVENT   (319U)
#define PTP_EP_PORT_GENERAL (320U)

#define PTP_EP_ETHERTYPE (0x88F7U)

typedef struct {
    bool valid;          /* false when the transport is NONE */
    bool is_ipv4;

    uint32_t ipv4_group; /* host byte order, e.g. 0xE0000181 for 224.0.1.129. 0 if not IPv4 */
    uint8_t mac[6];      /* L2 destination. Also the IGMP-derived group MAC when IPv4 */
    uint16_t ethertype;  /* 0x88F7 when 802.3, 0 otherwise */
} PtpEndpoint;

/**
 * Resolve the destination for a transport and delay mechanism.
 *
 * The two addresses per transport are fixed by the standard:
 *   IPv4   224.0.1.129 (E2E)            224.0.0.107 (peer delay)
 *   802.3  01:1B:19:00:00:00 (E2E)      01:80:C2:00:00:0E (peer delay)
 *
 * The peer-delay MAC is in the 802.1D reserved range that conformant bridges must not forward,
 * which is why gPTP needs a direct cable -- see docs/ptp_profiles.md.
 */
void ptp_endpoint_resolve(PtpEndpointTransport tp, PtpEndpointDelay dm, PtpEndpoint *out);

/**
 * Map an IPv4 multicast group (host byte order) to its Ethernet MAC, per RFC 1112:
 * 01:00:5E followed by the low **23** bits of the address.
 *
 * Exposed rather than kept private purely so the mask can be tested. ptp_endpoint_resolve()
 * only ever produces 224.0.1.129 and 224.0.0.107, and both have a third octet of 0x00 -- so
 * through that API the 23-bit mask is indistinguishable from a 24-bit one, and a test written
 * against it passes either way. Mutation testing caught exactly that.
 *
 * The bit matters: 32 groups share each MAC, and carrying bit 23 produces a MAC the NIC never
 * accepts while the IP address still looks correct.
 */
void ptp_endpoint_ipv4_group_mac(uint32_t group, uint8_t *mac);

/** UDP port for a message class. `is_event` selects 319 over 320. */
uint16_t ptp_endpoint_port(bool is_event);

#ifdef __cplusplus
}
#endif

#endif /* SRC_FLEXPTP_NSD_PTP_ENDPOINT_H */
