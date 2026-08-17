#include "eth_framer.h"

#include <cstring>

namespace {

/* The wire values. Not taken from nx_link.h on purpose: this unit deliberately depends on
   nothing, so that a test of it is a test of the framing and not of NetX Duo's headers. The
   driver's switch is what maps NetX Duo's commands onto EthFramerPayload, and that mapping is
   one line per case where a reader can see it. */
constexpr uint16_t kEtherTypeIpv4 = 0x0800U;
constexpr uint16_t kEtherTypeIpv6 = 0x86DDU;
constexpr uint16_t kEtherTypeArp = 0x0806U;
constexpr uint16_t kEtherTypeRarp = 0x8035U;

}  // namespace

extern "C" {

uint16_t eth_framer_ethertype(EthFramerPayload payload) {
    switch (payload) {
    case ETH_FRAMER_PAYLOAD_IPV6:
        return kEtherTypeIpv6;
    case ETH_FRAMER_PAYLOAD_ARP:
        return kEtherTypeArp;
    case ETH_FRAMER_PAYLOAD_RARP:
        return kEtherTypeRarp;
    case ETH_FRAMER_PAYLOAD_IPV4:
    default:
        return kEtherTypeIpv4;
    }
}

void eth_framer_write_header(uint8_t *out, uint32_t dst_msw, uint32_t dst_lsw,
                             uint32_t src_msw, uint32_t src_lsw, uint16_t ethertype) {
    /* Byte by byte, big-endian, written out rather than memcpy'd from a struct: the whole
       point is that the msw/lsw split is visible and checkable. */
    out[0] = static_cast<uint8_t>(dst_msw >> 8);
    out[1] = static_cast<uint8_t>(dst_msw);
    out[2] = static_cast<uint8_t>(dst_lsw >> 24);
    out[3] = static_cast<uint8_t>(dst_lsw >> 16);
    out[4] = static_cast<uint8_t>(dst_lsw >> 8);
    out[5] = static_cast<uint8_t>(dst_lsw);
    out[6] = static_cast<uint8_t>(src_msw >> 8);
    out[7] = static_cast<uint8_t>(src_msw);
    out[8] = static_cast<uint8_t>(src_lsw >> 24);
    out[9] = static_cast<uint8_t>(src_lsw >> 16);
    out[10] = static_cast<uint8_t>(src_lsw >> 8);
    out[11] = static_cast<uint8_t>(src_lsw);
    out[12] = static_cast<uint8_t>(ethertype >> 8);
    out[13] = static_cast<uint8_t>(ethertype);
}

uint32_t eth_framer_append(uint8_t *dst, uint32_t cap, uint32_t offset,
                           const uint8_t *src, uint32_t len) {
    if ((offset >= cap) || (len == 0U) || (src == nullptr)) {
        return (offset > cap) ? cap : offset;
    }

    const uint32_t room = cap - offset;
    const uint32_t take = (len < room) ? len : room;

    std::memcpy(dst + offset, src, take);
    return offset + take;
}

}  // extern "C"
