/* Where a PTP message goes. Five cases, and a wrong one presents as "no response received"
 * rather than as anything resembling a routing bug -- which is how F42 cost days when the
 * cause turned out to be a switch discarding Delay_Resp.
 */

#include <gtest/gtest.h>

#include <cstring>

extern "C" {
#include "ptp_endpoint.h"
}

namespace {

::testing::AssertionResult MacIs(const uint8_t *got, std::initializer_list<uint8_t> want) {
    size_t i = 0;
    for (uint8_t b : want) {
        if (got[i] != b) {
            return ::testing::AssertionFailure()
                   << "byte " << i << ": got 0x" << std::hex << int(got[i])
                   << ", want 0x" << int(b);
        }
        i++;
    }
    return ::testing::AssertionSuccess();
}

}  // namespace

TEST(Endpoint, Ipv4E2eIsThePrimaryGroup) {
    PtpEndpoint ep{};
    ptp_endpoint_resolve(PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E, &ep);

    EXPECT_TRUE(ep.valid);
    EXPECT_TRUE(ep.is_ipv4);
    EXPECT_EQ(ep.ipv4_group, 0xE0000181u) << "224.0.1.129 in host byte order";
    EXPECT_TRUE(MacIs(ep.mac, {0x01, 0x00, 0x5E, 0x00, 0x01, 0x81}));
}

TEST(Endpoint, Ipv4P2pIsThePeerDelayGroup) {
    PtpEndpoint ep{};
    ptp_endpoint_resolve(PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_P2P, &ep);

    EXPECT_EQ(ep.ipv4_group, 0xE000006Bu) << "224.0.0.107 in host byte order";
    EXPECT_TRUE(MacIs(ep.mac, {0x01, 0x00, 0x5E, 0x00, 0x00, 0x6B}));
}

TEST(Endpoint, Ipv4GroupMacMasksBitTwentyThree) {
    /* 01:00:5E plus the low 23 bits. 32 groups share each MAC, and carrying bit 23 produces a
     * MAC the NIC never accepts while the IP address still looks right.
     *
     * Tested through ptp_endpoint_ipv4_group_mac() rather than through resolve(), and with an
     * address whose third octet has the top bit SET. An earlier version asserted
     * `mac[3] & 0x80 == 0` on 224.0.1.129 -- whose third octet is 0x00, so it passed whether
     * the mask was 0x7F or 0xFF. Mutation testing caught it; the assertion was true by
     * accident rather than by the code being right. */
    uint8_t mac[6] = {0};

    // 239.129.1.1 -- third octet 0x81, so the masked bit is genuinely in play.
    ptp_endpoint_ipv4_group_mac(0xEF810101u, mac);
    EXPECT_EQ(mac[3], 0x01u) << "bit 23 was carried into the MAC instead of being masked";
    EXPECT_EQ(mac[4], 0x01u);
    EXPECT_EQ(mac[5], 0x01u);

    // And the fixed prefix.
    EXPECT_EQ(mac[0], 0x01u);
    EXPECT_EQ(mac[1], 0x00u);
    EXPECT_EQ(mac[2], 0x5Eu);
}

TEST(Endpoint, Ethernet802_3E2eIsThePrimaryMac) {
    PtpEndpoint ep{};
    ptp_endpoint_resolve(PTP_EP_TRANSPORT_802_3, PTP_EP_DELAY_E2E, &ep);

    EXPECT_TRUE(ep.valid);
    EXPECT_FALSE(ep.is_ipv4);
    EXPECT_TRUE(MacIs(ep.mac, {0x01, 0x1B, 0x19, 0x00, 0x00, 0x00}));
    EXPECT_EQ(ep.ethertype, 0x88F7u);
    EXPECT_EQ(ep.ipv4_group, 0u) << "no IP address on the L2 transport";
}

TEST(Endpoint, Ethernet802_3P2pIsTheReservedPeerDelayMac) {
    /* 01:80:C2:00:00:0E is in the 802.1D reserved range that conformant bridges must not
       forward. That is exactly why gPTP needs a direct cable, and why sending E2E traffic to
       it -- or peer-delay traffic to the primary address -- fails silently on both ends. */
    PtpEndpoint ep{};
    ptp_endpoint_resolve(PTP_EP_TRANSPORT_802_3, PTP_EP_DELAY_P2P, &ep);

    EXPECT_TRUE(MacIs(ep.mac, {0x01, 0x80, 0xC2, 0x00, 0x00, 0x0E}));
    EXPECT_EQ(ep.ethertype, 0x88F7u);
}

TEST(Endpoint, TheFourCombinationsAreAllDistinct) {
    PtpEndpoint a{}, b{}, c{}, d{};
    ptp_endpoint_resolve(PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E, &a);
    ptp_endpoint_resolve(PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_P2P, &b);
    ptp_endpoint_resolve(PTP_EP_TRANSPORT_802_3, PTP_EP_DELAY_E2E, &c);
    ptp_endpoint_resolve(PTP_EP_TRANSPORT_802_3, PTP_EP_DELAY_P2P, &d);

    EXPECT_NE(a.ipv4_group, b.ipv4_group);
    EXPECT_NE(0, std::memcmp(a.mac, b.mac, 6));
    EXPECT_NE(0, std::memcmp(c.mac, d.mac, 6));
    EXPECT_NE(0, std::memcmp(a.mac, c.mac, 6));
}

TEST(Endpoint, TeardownIsNotValidAndIsZeroed) {
    /* flexPTP calls ptp_nsd_init(-1, -1) to shut down. A caller that ignores `valid` should
       find zeroes rather than a stale or plausible destination. */
    PtpEndpoint ep{};
    ptp_endpoint_resolve(PTP_EP_TRANSPORT_NONE, PTP_EP_DELAY_E2E, &ep);

    EXPECT_FALSE(ep.valid);
    EXPECT_EQ(ep.ipv4_group, 0u);
    EXPECT_EQ(ep.ethertype, 0u);
    EXPECT_TRUE(MacIs(ep.mac, {0, 0, 0, 0, 0, 0}));
}

TEST(Endpoint, ResolveOverwritesAPreviousResult) {
    /* The NSD reuses one PtpEndpoint across profile switches. A stale field surviving a
       re-resolve is how gPTP would keep sending to an IPv4 group. */
    PtpEndpoint ep{};
    ptp_endpoint_resolve(PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E, &ep);
    ASSERT_NE(ep.ipv4_group, 0u);

    ptp_endpoint_resolve(PTP_EP_TRANSPORT_802_3, PTP_EP_DELAY_P2P, &ep);
    EXPECT_EQ(ep.ipv4_group, 0u) << "the IPv4 group survived a switch to 802.3";
}

TEST(Endpoint, EventAndGeneralPorts) {
    EXPECT_EQ(ptp_endpoint_port(true), 319u);
    EXPECT_EQ(ptp_endpoint_port(false), 320u);
}
