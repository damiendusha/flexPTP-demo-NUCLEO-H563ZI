#include "ptp_endpoint.h"

#include <cstring>

namespace {

/* Host byte order. 224.0.1.129 and 224.0.0.107. */
constexpr uint32_t kIpv4Primary = 0xE0000181U;
constexpr uint32_t kIpv4PeerDelay = 0xE000006BU;

constexpr uint8_t kMacPrimary[6] = {0x01, 0x1B, 0x19, 0x00, 0x00, 0x00};
constexpr uint8_t kMacPeerDelay[6] = {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E};

}  // namespace

extern "C" {

void ptp_endpoint_ipv4_group_mac(uint32_t group, uint8_t *mac) {
    mac[0] = 0x01;
    mac[1] = 0x00;
    mac[2] = 0x5E;
    mac[3] = static_cast<uint8_t>((group >> 16) & 0x7FU); /* 23 bits, not 24 */
    mac[4] = static_cast<uint8_t>((group >> 8) & 0xFFU);
    mac[5] = static_cast<uint8_t>(group & 0xFFU);
}

void ptp_endpoint_resolve(PtpEndpointTransport tp, PtpEndpointDelay dm, PtpEndpoint *out) {
    std::memset(out, 0, sizeof(*out));

    const bool e2e = (dm == PTP_EP_DELAY_E2E);

    switch (tp) {
    case PTP_EP_TRANSPORT_IPV4:
        out->valid = true;
        out->is_ipv4 = true;
        out->ipv4_group = e2e ? kIpv4Primary : kIpv4PeerDelay;
        ptp_endpoint_ipv4_group_mac(out->ipv4_group, out->mac);
        break;

    case PTP_EP_TRANSPORT_802_3:
        out->valid = true;
        out->is_ipv4 = false;
        std::memcpy(out->mac, e2e ? kMacPrimary : kMacPeerDelay, sizeof(out->mac));
        out->ethertype = PTP_EP_ETHERTYPE;
        break;

    case PTP_EP_TRANSPORT_NONE:
    default:
        /* Left zeroed, valid == false. flexPTP calls ptp_nsd_init(-1, -1) to shut down, and a
           caller that ignores `valid` then transmits to 0.0.0.0 rather than doing nothing. */
        break;
    }
}

uint16_t ptp_endpoint_port(bool is_event) {
    return is_event ? PTP_EP_PORT_EVENT : PTP_EP_PORT_GENERAL;
}

}  // extern "C"
