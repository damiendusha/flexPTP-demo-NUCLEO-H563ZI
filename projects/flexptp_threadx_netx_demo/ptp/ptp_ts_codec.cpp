#include "ptp_ts_codec.h"

#include <cstring>

extern "C" bool ptp_ts_codec_extract(const void *data_start, uint32_t headroom,
                                     PtpTsCodecResult *out) {
    out->sec = 0;
    out->nanosec = 0;

    if ((data_start == nullptr) || (headroom < PTP_TS_HEADROOM_BYTES)) {
        return false;
    }

    /* memcpy rather than a ULONG* cast.
     *
     * nx_link.c writes through a ULONG*, so the words are naturally aligned there -- but this
     * side is handed whatever the driver chose for data_start, and on this port that is a
     * buffer the frame was copied into at +2. Reading it back as aligned words would be an
     * assumption about someone else's allocator. memcpy costs nothing here and cannot trap.
     *
     * Only the first two words are read. The third is second_high, the top half of a 48-bit
     * seconds field; the H563's timestamp unit has a 32-bit seconds register and flexPTP's
     * TimestampI is 32-bit seconds, so it is always zero and carrying it would imply a range
     * neither end has. */
    uint32_t words[2];
    std::memcpy(words, data_start, sizeof(words));

    out->nanosec = words[0];
    out->sec = words[1];
    return true;
}
