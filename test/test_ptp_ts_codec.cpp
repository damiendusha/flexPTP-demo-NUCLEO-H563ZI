/* Reading a receive timestamp back out of NetX Duo's undocumented layout convention.
 *
 * The convention -- three ULONGs written over the consumed Ethernet header -- is not a struct
 * field and is not versioned. It is the same class of contract as the lwIP tx_cb signature
 * that silently drifted in F48, so it is pinned here.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

extern "C" {
#include "ptp_ts_codec.h"
}

namespace {

/* A buffer laid out the way nx_link.c leaves one: nano_second, second_low, second_high. */
std::vector<uint8_t> stamped_buffer(uint32_t ns, uint32_t sec_low, uint32_t sec_high = 0,
                                    size_t total = 64) {
    std::vector<uint8_t> buf(total, 0xAB);
    std::memcpy(buf.data() + 0, &ns, 4);
    std::memcpy(buf.data() + 4, &sec_low, 4);
    std::memcpy(buf.data() + 8, &sec_high, 4);
    return buf;
}

}  // namespace

TEST(TsCodec, ExtractsSecondsAndNanoseconds) {
    auto buf = stamped_buffer(987654321u, 0x1234u);

    PtpTsCodecResult ts{};
    ASSERT_TRUE(ptp_ts_codec_extract(buf.data(), 16, &ts));
    EXPECT_EQ(ts.nanosec, 987654321u);
    EXPECT_EQ(ts.sec, 0x1234u);
}

TEST(TsCodec, AZeroTimestampIsValid) {
    /* An unstamped frame reaches here with zeroes rather than junk, because ETHHW_ProcessRx()
       zeroes the event for exactly that reason. Zero must therefore be reported as a real
       reading, not confused with failure. */
    auto buf = stamped_buffer(0, 0);

    PtpTsCodecResult ts{};
    EXPECT_TRUE(ptp_ts_codec_extract(buf.data(), 16, &ts));
    EXPECT_EQ(ts.sec, 0u);
    EXPECT_EQ(ts.nanosec, 0u);
}

TEST(TsCodec, HandlesTheFullNanosecondRange) {
    auto buf = stamped_buffer(999999999u, 0xFFFFFFFFu);

    PtpTsCodecResult ts{};
    ASSERT_TRUE(ptp_ts_codec_extract(buf.data(), 16, &ts));
    EXPECT_EQ(ts.nanosec, 999999999u);
    EXPECT_EQ(ts.sec, 0xFFFFFFFFu) << "the seconds field must not be truncated";
}

TEST(TsCodec, IgnoresSecondHigh) {
    /* The third word is the top half of a 48-bit seconds field. The H563's timestamp unit has
       a 32-bit seconds register and flexPTP's TimestampI is 32-bit seconds, so carrying it
       would imply a range neither end has. Deliberately dropped -- pinned so that stays a
       decision rather than becoming a surprise. */
    auto buf = stamped_buffer(1u, 2u, 0xFFFFFFFFu);

    PtpTsCodecResult ts{};
    ASSERT_TRUE(ptp_ts_codec_extract(buf.data(), 16, &ts));
    EXPECT_EQ(ts.sec, 2u);
}

TEST(TsCodec, RejectsInsufficientHeadroom) {
    /* THE PRECONDITION. Below 12 bytes the three ULONGs were never written there, so what is
       present is payload. Reading it anyway yields a plausible-looking wrong time, which is
       strictly worse than none -- the F59 lesson, where a NaN printed as "0.0000". */
    auto buf = stamped_buffer(123u, 456u);

    PtpTsCodecResult ts{};
    EXPECT_FALSE(ptp_ts_codec_extract(buf.data(), PTP_TS_HEADROOM_BYTES - 1, &ts));
    EXPECT_EQ(ts.sec, 0u) << "a rejected read must leave the output zeroed, not partial";
    EXPECT_EQ(ts.nanosec, 0u);
}

TEST(TsCodec, AcceptsExactlyTheMinimumHeadroom) {
    auto buf = stamped_buffer(7u, 8u);

    PtpTsCodecResult ts{};
    ASSERT_TRUE(ptp_ts_codec_extract(buf.data(), PTP_TS_HEADROOM_BYTES, &ts));
    EXPECT_EQ(ts.nanosec, 7u);
}

TEST(TsCodec, RejectsANullBuffer) {
    PtpTsCodecResult ts{};
    EXPECT_FALSE(ptp_ts_codec_extract(nullptr, 64, &ts));
}

TEST(TsCodec, ReadsFromAnUnalignedBuffer) {
    /* nx_link.c writes through a ULONG*, so the words are aligned there. This side is handed
       whatever the driver chose for data_start -- and on this port that is a buffer the frame
       was copied into at +2. Reading it back as aligned words would be an assumption about
       someone else's allocator, so the codec uses memcpy; this pins that. */
    std::vector<uint8_t> raw(80, 0);
    uint8_t *unaligned = raw.data() + 1;
    ASSERT_NE(reinterpret_cast<uintptr_t>(unaligned) % 4, 0u);

    const uint32_t ns = 0x11223344u, sec = 0x55667788u;
    std::memcpy(unaligned + 0, &ns, 4);
    std::memcpy(unaligned + 4, &sec, 4);

    PtpTsCodecResult ts{};
    ASSERT_TRUE(ptp_ts_codec_extract(unaligned, 16, &ts));
    EXPECT_EQ(ts.nanosec, ns);
    EXPECT_EQ(ts.sec, sec);
}
