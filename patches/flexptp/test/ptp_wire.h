// PTPv2 messages as bytes on the wire (IEEE 1588-2019 clause 13), for feeding flexPTP in tests.
#ifndef FLEXPTP_TEST_PTP_WIRE_H
#define FLEXPTP_TEST_PTP_WIRE_H

#include <array>
#include <cstdint>
#include <vector>

namespace wire {

using Bytes = std::vector<uint8_t>;
using ClockId = std::array<uint8_t, 8>;

enum Type : uint8_t {
    kSync = 0x0, kDelayReq = 0x1, kPDelayReq = 0x2, kPDelayResp = 0x3,
    kFollowUp = 0x8, kDelayResp = 0x9, kPDelayRespFollowUp = 0xA, kAnnounce = 0xB,
};

struct Port {
    ClockId id;
    uint16_t number = 1;
};

struct Header {
    uint8_t transport_specific = 0;   // 1 for gPTP (802.1AS)
    uint8_t type = kSync;
    uint8_t version = 0x12;     // ptp4l 4.x sends 2.1: minorVersionPTP 1, versionPTP 2
    uint16_t length = 44;       // messageLength
    uint8_t domain = 0;
    bool two_step = true;
    Port source;
    uint16_t seq = 0;
    uint8_t control = 0;
    int8_t log_interval = 0;
};

inline void put16(Bytes &b, uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xFF); }
inline void put32(Bytes &b, uint32_t v) { put16(b, v >> 16); put16(b, v & 0xFFFF); }

inline Bytes header(const Header &h) {
    Bytes b;
    b.push_back(static_cast<uint8_t>((h.transport_specific << 4) | (h.type & 0x0F)));
    b.push_back(h.version);
    put16(b, h.length);
    b.push_back(h.domain);
    b.push_back(0);                                   // minorSdoId
    put16(b, h.two_step ? 0x0200 : 0x0000);           // flagField: twoStepFlag
    for (int i = 0; i < 8; i++) b.push_back(0);       // correctionField
    put32(b, 0);                                      // messageTypeSpecific
    b.insert(b.end(), h.source.id.begin(), h.source.id.end());
    put16(b, h.source.number);
    put16(b, h.seq);
    b.push_back(h.control);
    b.push_back(static_cast<uint8_t>(h.log_interval));
    return b;
}

inline void timestamp(Bytes &b, int64_t ns) {
    const uint64_t s = static_cast<uint64_t>(ns / 1000000000LL);
    put16(b, static_cast<uint16_t>(s >> 32));
    put32(b, static_cast<uint32_t>(s));
    put32(b, static_cast<uint32_t>(ns % 1000000000LL));
}

inline Bytes sync(const Port &from, uint16_t seq, bool two_step = true, int64_t origin_ns = 0) {
    Header h;
    h.type = kSync; h.length = 44; h.source = from; h.seq = seq; h.two_step = two_step;
    h.control = 0;
    Bytes b = header(h);
    timestamp(b, origin_ns);
    return b;
}

inline Bytes follow_up(const Port &from, uint16_t seq, int64_t precise_origin_ns) {
    Header h;
    h.type = kFollowUp; h.length = 44; h.source = from; h.seq = seq; h.two_step = false;
    h.control = 2;
    Bytes b = header(h);
    timestamp(b, precise_origin_ns);
    return b;
}

inline Bytes delay_resp(const Port &from, uint16_t seq, int64_t receive_ns, const Port &requester) {
    Header h;
    h.type = kDelayResp; h.length = 54; h.source = from; h.seq = seq; h.two_step = false;
    h.control = 3;
    Bytes b = header(h);
    timestamp(b, receive_ns);
    b.insert(b.end(), requester.id.begin(), requester.id.end());
    put16(b, requester.number);
    return b;
}

inline Bytes pdelay_req(const Port &from, uint16_t seq, uint8_t transport_specific = 0) {
    Header h;
    h.transport_specific = transport_specific;
    h.type = kPDelayReq; h.length = 54; h.source = from; h.seq = seq; h.two_step = false;
    h.control = 5;
    Bytes b = header(h);
    timestamp(b, 0);                                  // originTimestamp
    for (int i = 0; i < 10; i++) b.push_back(0);      // reserved
    return b;
}

inline Bytes announce(const Port &from, uint16_t seq, uint8_t priority1, const ClockId &gm,
                      uint8_t clock_class = 248) {
    Header h;
    h.type = kAnnounce; h.length = 64; h.source = from; h.seq = seq; h.two_step = false;
    h.control = 5; h.log_interval = 1;
    Bytes b = header(h);
    timestamp(b, 0);
    put16(b, 37);                                     // currentUtcOffset
    b.push_back(0);                                   // reserved
    b.push_back(priority1);
    b.push_back(clock_class);
    b.push_back(0xFE);                                // clockAccuracy: unknown
    put16(b, 0xFFFF);                                 // offsetScaledLogVariance
    b.push_back(128);                                 // priority2
    b.insert(b.end(), gm.begin(), gm.end());
    put16(b, 0);                                      // stepsRemoved
    b.push_back(0xA0);                                // timeSource: internal oscillator
    return b;
}

}  // namespace wire

#endif
