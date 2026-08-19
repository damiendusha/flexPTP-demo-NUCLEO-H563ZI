/* Ethernet framing: the part of drv_send() that can be silently wrong.
 *
 * Finding TS-04. drv_send() has the highest defect density of anything in this port -- three of
 * the five NetX Duo bring-up defects were in or immediately around it -- and had no automated
 * coverage, because every line of it touches NX_PACKET or NX_IP_DRIVER.
 *
 * These are the parts that could be lifted out without inventing an abstraction: the
 * EtherType mapping, the msw/lsw address split, and the saturating append that flattens a
 * packet chain. Each has the same character -- a mistake produces a frame that is perfectly
 * formed and wrong, which no counter anywhere reports. The measured symptom during bring-up
 * was "tx: 16 sent, 0 dropped" against a host that received nothing.
 *
 * Ownership, release and the cancel-on-every-drop-path invariant stay in the driver and stay
 * on the bench; they are not framing.
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "eth_framer.h"
}

namespace {

/* The board's own MAC, in NetX Duo's split form: 56:00:1e:00:04:51. */
constexpr uint32_t kSrcMsw = 0x5600U;
constexpr uint32_t kSrcLsw = 0x1e000451U;

/* PTP's E2E link-layer group, 01:1b:19:00:00:00 -- the one gPTP actually sends to. */
constexpr uint32_t kDstMsw = 0x011bU;
constexpr uint32_t kDstLsw = 0x19000000U;

}  // namespace

TEST(EthFramer, EtherTypesAreTheWireValues) {
    EXPECT_EQ(eth_framer_ethertype(ETH_FRAMER_PAYLOAD_IPV4), 0x0800u);
    EXPECT_EQ(eth_framer_ethertype(ETH_FRAMER_PAYLOAD_IPV6), 0x86DDu);
    EXPECT_EQ(eth_framer_ethertype(ETH_FRAMER_PAYLOAD_ARP), 0x0806u);
    EXPECT_EQ(eth_framer_ethertype(ETH_FRAMER_PAYLOAD_RARP), 0x8035u);
}

TEST(EthFramer, WritesTheHeaderExactlyAsItGoesOnTheWire) {
    /* The whole test in one expected array. The msw/lsw split is the thing being checked: get
     * it wrong and the frame is perfectly formed with the wrong addresses, which is measured
     * as "tx sent, 0 dropped" against a host that receives nothing. */
    uint8_t h[ETH_FRAMER_HEADER_SIZE];
    std::memset(h, 0xAA, sizeof(h));

    eth_framer_write_header(h, kDstMsw, kDstLsw, kSrcMsw, kSrcLsw,
                            eth_framer_ethertype(ETH_FRAMER_PAYLOAD_IPV4));

    const uint8_t expected[ETH_FRAMER_HEADER_SIZE] = {
        0x01, 0x1b, 0x19, 0x00, 0x00, 0x00,  /* destination */
        0x56, 0x00, 0x1e, 0x00, 0x04, 0x51,  /* source      */
        0x08, 0x00,                          /* EtherType   */
    };
    EXPECT_EQ(std::memcmp(h, expected, sizeof(expected)), 0);
}

TEST(EthFramer, TheBroadcastAddressSurvivesTheSplit) {
    /* ff:ff:ff:ff:ff:ff is what ARP requests and DHCP discovery go to, so a sign-extension or
       width mistake in the split would break address resolution before anything else. */
    uint8_t h[ETH_FRAMER_HEADER_SIZE];
    eth_framer_write_header(h, 0xFFFFU, 0xFFFFFFFFU, kSrcMsw, kSrcLsw, 0x0806U);

    for (int i = 0; i < 6; i++) {
        EXPECT_EQ(h[i], 0xFFu) << "destination byte " << i;
    }
    EXPECT_EQ(h[12], 0x08u);
    EXPECT_EQ(h[13], 0x06u);
}

TEST(EthFramer, DoesNotWritePastTheHeader) {
    uint8_t buf[ETH_FRAMER_HEADER_SIZE + 4];
    std::memset(buf, 0x5A, sizeof(buf));

    eth_framer_write_header(buf, kDstMsw, kDstLsw, kSrcMsw, kSrcLsw, 0x0800U);

    for (unsigned i = ETH_FRAMER_HEADER_SIZE; i < sizeof(buf); i++) {
        EXPECT_EQ(buf[i], 0x5Au) << "byte " << i << " past the header was written";
    }
}

TEST(EthFramer, AppendConcatenatesAChain) {
    uint8_t out[64];
    std::memset(out, 0, sizeof(out));

    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {4, 5};
    const uint8_t c[] = {6};

    uint32_t off = 0;
    off = eth_framer_append(out, sizeof(out), off, a, sizeof(a));
    off = eth_framer_append(out, sizeof(out), off, b, sizeof(b));
    off = eth_framer_append(out, sizeof(out), off, c, sizeof(c));

    EXPECT_EQ(off, 6u);
    const uint8_t expected[] = {1, 2, 3, 4, 5, 6};
    EXPECT_EQ(std::memcmp(out, expected, sizeof(expected)), 0);
}

TEST(EthFramer, AppendSaturatesAtTheBufferRatherThanOverrunning) {
    /* The property that matters: a chain whose declared length exceeds the transmit buffer
     * must fill it and stop. The driver's buffer is 1528 bytes and NetX Duo's MTU keeps frames
     * below that, so this is defence -- but it is defence against a chain built by a stack
     * this project does not control. */
    uint8_t out[8];
    std::memset(out, 0, sizeof(out));
    const std::vector<uint8_t> big(32, 0xC3);

    const uint32_t off = eth_framer_append(out, sizeof(out), 0, big.data(),
                                           static_cast<uint32_t>(big.size()));

    EXPECT_EQ(off, sizeof(out));
    for (unsigned i = 0; i < sizeof(out); i++) {
        EXPECT_EQ(out[i], 0xC3u);
    }
}

TEST(EthFramer, AppendToAFullBufferCopiesNothing) {
    uint8_t out[4] = {1, 2, 3, 4};
    const uint8_t more[] = {9, 9, 9};

    EXPECT_EQ(eth_framer_append(out, sizeof(out), sizeof(out), more, sizeof(more)), sizeof(out));

    const uint8_t unchanged[] = {1, 2, 3, 4};
    EXPECT_EQ(std::memcmp(out, unchanged, sizeof(unchanged)), 0);
}

TEST(EthFramer, AppendFillsTheLastByteExactly) {
    /* Off-by-one in the direction that truncates a frame by one byte: the FCS would then be
       computed over the wrong length and the frame silently discarded by the receiver. */
    uint8_t out[4] = {};
    const uint8_t src[] = {7, 7, 7, 7};

    EXPECT_EQ(eth_framer_append(out, sizeof(out), 0, src, sizeof(src)), 4u);
    EXPECT_EQ(out[3], 7u);
}

TEST(EthFramer, AppendIgnoresDegenerateInput) {
    uint8_t out[4] = {1, 2, 3, 4};
    const uint8_t src[] = {9};

    EXPECT_EQ(eth_framer_append(out, sizeof(out), 0, src, 0), 0u);
    EXPECT_EQ(eth_framer_append(out, sizeof(out), 2, nullptr, 4), 2u);
    EXPECT_EQ(eth_framer_append(out, sizeof(out), 99, src, 1), sizeof(out))
        << "an offset past the buffer must clamp, not wrap";

    const uint8_t unchanged[] = {1, 2, 3, 4};
    EXPECT_EQ(std::memcmp(out, unchanged, sizeof(unchanged)), 0);
}
