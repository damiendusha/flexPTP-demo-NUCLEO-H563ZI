// flexPTP's receive path against malformed and foreign messages: findings U12 and U13.
//
// The real engine -- flexPTP's own sources, as patched by //patches/flexptp -- runs here in its
// supported OS-less mode, slaved to a simulated master that sends Announce, Sync, Follow_Up and
// Delay_Resp the way ptp4l does. Each test interferes with that master's Sync cycles and counts
// how many still complete (PTP_UEV_FOLLOW_UP_RECVED), which is exactly what the board's HIL
// `fuzz` scenario measures on hardware.
//
//   U12  nothing compared the received length or versionPTP with the header: a short datagram
//        was parsed partly from the previous message's bytes, a PTPv1 message as PTPv2.
//   U13  the slave took Sync and Follow_Up from any sender, and a new Sync while it waited for a
//        Follow_Up was ignored rather than restarting the cycle.
//
// Interference is injected both BEFORE the master's Sync and BETWEEN the Sync and its
// Follow_Up. The two placements break different things -- before, without U13's restart; between,
// with it -- so each test fails without its own fix whichever of the other fixes is present.

#include <gtest/gtest.h>

#include <cstring>
#include <functional>

#include "ptp_wire.h"

extern "C" {
#include "fake_port.h"
#include <flexptp/logging.h>
#include <flexptp/ptp_core.h>
#include <flexptp/task_ptp.h>
}

using wire::Bytes;
using wire::Port;

extern "C" void MSG(const char *format, ...) { (void)format; }

namespace {

constexpr int64_t kTickNs = PTP_HEARTBEAT_TICKRATE_MS * 1000000LL;
constexpr int64_t kSyncPeriodNs = 1000000000LL;
constexpr int64_t kPathNs = 500;            // the simulated master is 500 ns away
const Port kMaster{{0x00, 0xa0, 0xc9, 0xff, 0xfe, 0x26, 0x21, 0xe4}, 1};
const Port kForeign{{0x02, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x01}, 1};
const Port kBetter{{0x00, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55}, 1};

uint32_t cycles() { return g_fake.events[PTP_UEV_FOLLOW_UP_RECVED]; }

// Syncs the slave ACCEPTED (one-step or two-step). Each interference test asserts this equals the
// number the master sent: the decisive check, because what a malformed message is parsed as
// depends on the receive slot's previous contents -- it may block a cycle, step the clock, or do
// neither visibly -- but if it was taken for a Sync at all, it is counted here.
uint32_t syncs() { return g_fake.events[PTP_UEV_SYNC_RECVED]; }

class Engine : public ::testing::Test {
  protected:
    void SetUp() override {
        fake_reset();
        ASSERT_TRUE(reg_task_ptp());
        ptp_log_disable_all();
        // reg_task_ptp() does not reset the core itself: it queues a PTP_CEV_RESET. Process it
        // now, or the first state check reads whatever the previous run left (SLAVE, to a
        // master that no longer exists) and the reset lands in the middle of the test.
        task_ptp();
        ASSERT_NE(gPtpCoreState.bmca.state, PTP_BMCA_SLAVE) << "the core was not reset";
    }
    void TearDown() override { unreg_task_ptp(); }

    // One heartbeat of virtual time, with Announces from `announcer` once a second.
    void tick() {
        fake_advance_ns(kTickNs);
        if ((announce_clock_ += kTickNs) >= 1000000000LL) {
            announce_clock_ = 0;
            rx(wire::announce(announcer_, ann_seq_++, announcer_prio_, announcer_.id));
        }
        ptp_heartbeat_tmr_cb();
        task_ptp();
        answer_delay_req();
    }

    void rx(const Bytes &b, int tp = PTP_TP_IPv4) {
        const int64_t t = fake_now_ns();
        ptp_receive_enqueue(b.data(), (uint32_t)b.size(), (uint32_t)(t / 1000000000LL),
                            (uint32_t)(t % 1000000000LL), tp);
        task_ptp();
    }

    // The master answers every Delay_Req the board sends, as ptp4l does.
    void answer_delay_req() {
        if (g_fake.tx_by_type[wire::kDelayReq] != delreqs_seen_) {
            delreqs_seen_ = g_fake.tx_by_type[wire::kDelayReq];
            if (foreign_delay_resp_first_)
                rx(wire::delay_resp(kForeign, g_fake.last_tx_seq[wire::kDelayReq],
                                    fake_now_ns() - 500000000LL, own_port()));
            rx(wire::delay_resp(announcer_, g_fake.last_tx_seq[wire::kDelayReq],
                                fake_now_ns() + kPathNs, own_port()));
        }
    }

    Port own_port() {
        Port p;
        std::memcpy(p.id.data(), &gPtpCoreState.hwoptions.clockIdentity, 8);
        p.number = 1;
        return p;
    }

    void run_ns(int64_t ns) {
        for (int64_t t = 0; t < ns; t += kTickNs) tick();
    }

    void become_slave() {
        for (int i = 0; i < 400 && gPtpCoreState.bmca.state != PTP_BMCA_SLAVE; i++) tick();
        ASSERT_EQ(gPtpCoreState.bmca.state, PTP_BMCA_SLAVE) << "never slaved to the simulated master";
    }

    // One two-step Sync cycle from the master, with optional interference before the Sync and
    // between the Sync and its Follow_Up. Returns the cycles completed during it.
    uint32_t cycle(const std::function<void()> &before = {}, const std::function<void()> &between = {},
                   const Port &from = kMaster) {
        const uint32_t c0 = cycles();
        if (before) before();
        const uint16_t seq = sync_seq_++;
        const int64_t t2 = fake_now_ns();
        rx(wire::sync(from, seq));
        if (between) between();
        rx(wire::follow_up(from, seq, t2 - kPathNs));
        run_ns(kSyncPeriodNs);
        return cycles() - c0;
    }

    uint32_t cycles_over(int n, const std::function<void()> &before = {},
                         const std::function<void()> &between = {}) {
        uint32_t done = 0;
        for (int i = 0; i < n; i++) done += cycle(before, between);
        return done;
    }

    void warm_up() {
        become_slave();
        for (int i = 0; i < 5; i++) cycle();      // a few clean cycles: path delay known, servo running
    }

    Port announcer_ = kMaster;
    uint8_t announcer_prio_ = 100;
    int64_t announce_clock_ = 0;
    uint16_t ann_seq_ = 0, sync_seq_ = 100;
    uint32_t delreqs_seen_ = 0;
    bool foreign_delay_resp_first_ = false;
};

// ---- positive controls: what must keep working ----------------------------------------------

TEST_F(Engine, SlavesToTheMasterAndCompletesEveryCycle) {
    become_slave();
    EXPECT_EQ(std::memcmp(&gPtpCoreState.bmca.masterProps.grandmasterClockIdentity, kMaster.id.data(), 8), 0);
    for (int i = 0; i < 5; i++) cycle();          // settling: path delay, servo start
    const uint32_t steps0 = g_fake.clock_steps, syncs0 = syncs();
    EXPECT_EQ(cycles_over(10), 10u);
    EXPECT_EQ(syncs() - syncs0, 10u);
    EXPECT_EQ(g_fake.clock_steps, steps0) << "a settled slave stepped its clock";
}

TEST_F(Engine, AcceptsVersion2Point0AndPaddedFrames) {
    warm_up();
    const uint32_t syncs0 = syncs();
    // versionPTP 0x02 (1588-2008) as well as 0x12, and a Sync padded the way an 802.3 frame is
    // padded to the 60-byte minimum: a datagram longer than messageLength is legal.
    uint32_t done = 0;
    for (int i = 0; i < 5; i++) {
        const uint16_t seq = sync_seq_++;
        const int64_t t2 = fake_now_ns();
        Bytes s = wire::sync(kMaster, seq);
        s[1] = 0x02;
        s.resize(46, 0);
        const uint32_t c0 = cycles();
        rx(s);
        Bytes f = wire::follow_up(kMaster, seq, t2 - kPathNs);
        f[1] = 0x02;
        rx(f);
        run_ns(kSyncPeriodNs);
        done += cycles() - c0;
    }
    EXPECT_EQ(done, 5u);
}

TEST_F(Engine, FollowsABetterMasterWhenOneAppears) {
    warm_up();
    announcer_ = kBetter;                         // a better master starts announcing
    announcer_prio_ = 50;
    run_ns(8 * kSyncPeriodNs);
    ASSERT_EQ(std::memcmp(&gPtpCoreState.bmca.masterProps.grandmasterClockIdentity, kBetter.id.data(), 8), 0);
    ASSERT_EQ(gPtpCoreState.bmca.state, PTP_BMCA_SLAVE);
    uint32_t done = 0;
    for (int i = 0; i < 5; i++) done += cycle({}, {}, kBetter);
    EXPECT_EQ(done, 5u) << "the new master's Syncs must be used";
}

// ---- U12: length and version -----------------------------------------------------------------

TEST_F(Engine, U12_AOneByteDatagramIsNotParsedFromStaleBytes) {
    // One byte, "Sync". The other 33 bytes of its header come from whatever the receive slot
    // held before -- here the twoStepFlag was clear there, so it was taken as a ONE-step Sync
    // with an originTimestamp of zero, and the servo was handed a 54-year error: no cycle lost,
    // the clock stepped instead. Both are checked.
    warm_up();
    const uint32_t steps0 = g_fake.clock_steps;
    const uint32_t syncs0 = syncs();
    const Bytes junk{0x00};
    EXPECT_EQ(cycles_over(10, [&] { rx(junk); }, [&] { rx(junk); }), 10u);
    EXPECT_EQ(syncs() - syncs0, 10u) << "the slave accepted a Sync that was not one of the master's";
    EXPECT_EQ(g_fake.clock_steps, steps0) << "a 1-byte datagram stepped the clock";
}

TEST_F(Engine, U12_AHeaderShorterThan34BytesIsDropped) {
    warm_up();
    const uint32_t steps0 = g_fake.clock_steps;
    const uint32_t syncs0 = syncs();
    auto junk = [&] { Bytes s = wire::sync(kMaster, 7); s.resize(33); rx(s); };
    EXPECT_EQ(cycles_over(10, junk, junk), 10u);
    EXPECT_EQ(syncs() - syncs0, 10u) << "the slave accepted a Sync that was not one of the master's";
    EXPECT_EQ(g_fake.clock_steps, steps0) << "interference stepped the clock";
}

TEST_F(Engine, U12_AMessageShorterThanItsMessageLengthIsDropped) {
    // The master's own Follow_Up, header only: messageLength says 44, 34 bytes arrived. Parsed,
    // its preciseOriginTimestamp comes from whatever the receive slot held before -- the previous
    // Follow_Up, a second old -- and the servo is fed a one-second error.
    warm_up();
    const uint32_t steps0 = g_fake.clock_steps;
    const uint32_t syncs0 = syncs();
    uint32_t done = 0;
    for (int i = 0; i < 10; i++) {
        const uint16_t seq = sync_seq_++;
        const int64_t t2 = fake_now_ns();
        const uint32_t c0 = cycles();
        rx(wire::sync(kMaster, seq));
        Bytes cut = wire::follow_up(kMaster, seq, t2 - kPathNs);
        cut.resize(34);
        rx(cut);
        rx(wire::follow_up(kMaster, seq, t2 - kPathNs));
        run_ns(kSyncPeriodNs);
        done += cycles() - c0;
    }
    EXPECT_EQ(done, 10u);
    EXPECT_EQ(g_fake.clock_steps, steps0) << "a truncated Follow_Up stepped the clock";
    EXPECT_EQ(syncs() - syncs0, 10u);
}

TEST_F(Engine, U12_APtpV1MessageIsDropped) {
    warm_up();
    const uint32_t steps0 = g_fake.clock_steps;
    const uint32_t syncs0 = syncs();
    auto v1 = [&] { Bytes s = wire::sync(kMaster, 7); s[1] = 0x01; rx(s); };
    EXPECT_EQ(cycles_over(10, v1, v1), 10u);
    EXPECT_EQ(syncs() - syncs0, 10u) << "the slave accepted a Sync that was not one of the master's";
    EXPECT_EQ(g_fake.clock_steps, steps0) << "interference stepped the clock";
}

TEST_F(Engine, U12_AMessageLengthTooShortForItsTypeIsDropped) {
    warm_up();
    const uint32_t steps0 = g_fake.clock_steps;
    const uint32_t syncs0 = syncs();
    auto bad = [&] { Bytes s = wire::sync(kMaster, 7); s[2] = 0; s[3] = 34; rx(s); };
    EXPECT_EQ(cycles_over(10, bad, bad), 10u);
    EXPECT_EQ(syncs() - syncs0, 10u) << "the slave accepted a Sync that was not one of the master's";
    EXPECT_EQ(g_fake.clock_steps, steps0) << "interference stepped the clock";
}

// ---- U13: the selected master only ------------------------------------------------------------

TEST_F(Engine, U13_AForeignSyncIsIgnored) {
    warm_up();
    const uint32_t steps0 = g_fake.clock_steps;
    const uint32_t syncs0 = syncs();
    auto foreign = [&] { rx(wire::sync(kForeign, 7)); };
    EXPECT_EQ(cycles_over(10, foreign, foreign), 10u);
    EXPECT_EQ(syncs() - syncs0, 10u) << "the slave accepted a Sync that was not one of the master's";
    EXPECT_EQ(g_fake.clock_steps, steps0) << "interference stepped the clock";
}

TEST_F(Engine, U13_AForeignFollowUpIsIgnored) {
    // A Follow_Up from another clock, with the master's sequenceId and a time one second off.
    // Accepted, it completes the cycle with the wrong T1: a coarse step to the wrong time.
    warm_up();
    const uint32_t steps0 = g_fake.clock_steps;
    const uint32_t syncs0 = syncs();
    uint32_t done = 0;
    for (int i = 0; i < 10; i++) {
        const uint16_t seq = sync_seq_++;
        const int64_t t2 = fake_now_ns();
        const uint32_t c0 = cycles();
        rx(wire::sync(kMaster, seq));
        rx(wire::follow_up(kForeign, seq, t2 - 1000000000LL));
        rx(wire::follow_up(kMaster, seq, t2 - kPathNs));
        run_ns(kSyncPeriodNs);
        done += cycles() - c0;
    }
    EXPECT_EQ(done, 10u);
    EXPECT_EQ(g_fake.clock_steps, steps0) << "a foreign Follow_Up stepped the clock";
    EXPECT_EQ(syncs() - syncs0, 10u);
}

TEST_F(Engine, U13_ASyncFromTheMastersOtherPortIsIgnored) {
    warm_up();
    const uint32_t steps0 = g_fake.clock_steps;
    const uint32_t syncs0 = syncs();
    Port other = kMaster;
    other.number = 2;
    auto wrong_port = [&] { rx(wire::sync(other, 7)); };
    EXPECT_EQ(cycles_over(10, wrong_port, wrong_port), 10u);
    EXPECT_EQ(syncs() - syncs0, 10u) << "the slave accepted a Sync that was not one of the master's";
    EXPECT_EQ(g_fake.clock_steps, steps0) << "interference stepped the clock";
}

TEST_F(Engine, U13_ANewSyncFromTheMasterRestartsTheCycle) {
    // Sync(n), Sync(n+1), Follow_Up(n+1): the first Follow_Up was lost. A slave must use the
    // newer Sync, not wait for a Follow_Up(n) that will never come.
    warm_up();
    uint32_t done = 0;
    for (int i = 0; i < 10; i++) {
        const uint16_t lost = sync_seq_++, seq = sync_seq_++;
        const uint32_t c0 = cycles();
        rx(wire::sync(kMaster, lost));
        fake_advance_ns(1000000);
        const int64_t t2 = fake_now_ns();
        rx(wire::sync(kMaster, seq));
        rx(wire::follow_up(kMaster, seq, t2 - kPathNs));
        run_ns(kSyncPeriodNs);
        done += cycles() - c0;
    }
    EXPECT_EQ(done, 10u);
}

TEST_F(Engine, U13_AForeignDelayRespIsIgnored) {
    // A Delay_Resp naming this clock as requester and answering its latest Delay_Req, but from
    // another clock and half a second off, arriving before the master's own: its
    // receiveTimestamp must not become the path delay.
    warm_up();
    const uint32_t steps0 = g_fake.clock_steps;
    foreign_delay_resp_first_ = true;
    EXPECT_EQ(cycles_over(10), 10u);
    EXPECT_EQ(g_fake.clock_steps, steps0) << "a foreign Delay_Resp corrupted the path delay";
}

}  // namespace
