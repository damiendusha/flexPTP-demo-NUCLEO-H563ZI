// flexPTP's timestamp byte order: finding U15.
//
// Writing and reading a timestamp shifted signed values into their sign bit, which is undefined
// behaviour, in two places. FLEXPTP_htonl() shifts its argument in the argument's own type, and
// TimestampI.nanosec is an int32_t: `<< 8` overflows for any value of 2^23 ns or more. And the
// seconds are read by copying six bytes into the low end of an int64_t and shifting it left by 16,
// which overflows whenever the last byte on the wire is 0x80 or more -- half of all timestamps.
// GCC happens to produce the right bytes, which is why nothing has ever looked wrong; UBSan
// reports all three sites.
//
// So these pass without U15 in a plain build, as the firmware's behaviour always has. Under
// --config=asan, which carries UBSan and stops at its first report, they fail without it -- that
// is the configuration that keeps the defect out.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

extern "C" {
#include <flexptp/msg_utils.h>
#include <flexptp/timeutils.h>
}

// The application's console. Nothing here logs, but the engine links against it.
extern "C" void MSG(const char *format, ...) { (void)format; }

namespace {

constexpr int kHeader = PTP_HEADER_LENGTH;

// Nanosecond values that matter: either side of 2^23, where `<< 8` starts to overflow an int32,
// and the largest a timestamp may carry.
constexpr int32_t kNanoseconds[] = {0, 1, 127, 128, (1 << 23) - 1, 1 << 23, 968000000, 999999999};

TEST(ByteOrder, U15_ATimestampIsWrittenBigEndian) {
    uint8_t msg[kHeader + 10] = {};
    const TimestampI ts = {0x123456789aLL, 999999999};  // 999999999 = 0x3b9ac9ff
    ptp_write_binary_timestamps(msg, &ts, 1);

    const uint8_t seconds[6] = {0x00, 0x12, 0x34, 0x56, 0x78, 0x9a};
    const uint8_t nanoseconds[4] = {0x3b, 0x9a, 0xc9, 0xff};
    EXPECT_EQ(std::memcmp(msg + kHeader, seconds, 6), 0) << "48-bit seconds, network order";
    EXPECT_EQ(std::memcmp(msg + kHeader + 6, nanoseconds, 4), 0) << "nanoseconds, network order";
}

TEST(ByteOrder, U15_ATimestampSurvivesTheRoundTrip) {
    for (const int32_t ns : kNanoseconds) {
        uint8_t msg[kHeader + 10] = {};
        const TimestampI out = {1764300000, ns};
        ptp_write_binary_timestamps(msg, &out, 1);

        TimestampI in = {};
        ptp_extract_timestamps(&in, msg, 1);
        EXPECT_EQ(in.sec, out.sec) << "nanosec " << ns;
        EXPECT_EQ(in.nanosec, ns);
    }
}

TEST(ByteOrder, U15_ATimestampIsReadFromNetworkOrder) {
    // 968000000 = 0x39b28200, the value UBSan first reported; and seconds whose last byte, 0xe0,
    // has its top bit set -- the case that overflowed the seconds shift.
    uint8_t msg[kHeader + 10] = {};
    const uint8_t wire[10] = {0x00, 0x00, 0x69, 0x29, 0x22, 0xe0, 0x39, 0xb2, 0x82, 0x00};
    std::memcpy(msg + kHeader, wire, sizeof wire);

    TimestampI ts = {};
    ptp_extract_timestamps(&ts, msg, 1);
    EXPECT_EQ(ts.sec, 0x692922e0);
    EXPECT_EQ(ts.nanosec, 968000000);
}

}  // namespace
