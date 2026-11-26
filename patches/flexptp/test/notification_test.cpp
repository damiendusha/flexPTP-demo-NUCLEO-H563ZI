// flexPTP's PTP task after a burst it could not keep up with: finding U14.
//
// Every producer -- a received message, a message to transmit, its TX timestamp, a heartbeat --
// pushes its item onto its own FIFO and then a notification onto one shared FIFO, and the task
// takes one item per notification. The notification FIFO held 16 entries, the four item FIFOs 58
// between them. So a burst that leaves the task behind can fill the notification FIFO while an
// item FIFO still has room: the item goes in, its notification does not, and from then on the
// task takes each item one notification late -- for good, since nothing catches it up. A
// received message taken late enough has outlived its TTL in the receive buffer and is skipped,
// without a count or a log line.
//
// On a NUCLEO-H563ZI (ThreadX + NetX Duo), a 120 s saturation flood in gPTP left twelve received
// frames and three heartbeats queued without notifications, the task idle (read over SWD). A gPTP
// link without a Sync stream carries about two frames a second, so every Pdelay_Req was taken
// about five seconds late, past its 2 s TTL, and never answered; the host, with no peer delay to
// the board, stopped serving it, and the board stayed its own master until PTP was restarted.
//
// The test is that sequence, in flexPTP's own OS-less mode: the task is starved while heartbeats
// fire and the link peer's frames arrive, as during the flood; then it runs again, and the peer
// sends one Pdelay_Req a second, as ptp4l does. Each must be answered, with its own sequenceId, as
// soon as the task has run.

#include <gtest/gtest.h>

#include <string>

#include "ptp_wire.h"

extern "C" {
#include "fake_port.h"
#include <flexptp/logging.h>
#include <flexptp/ptp_core.h>
#include <flexptp/ptp_profile_presets.h>
#include <flexptp/settings_interface.h>
#include <flexptp/task_ptp.h>
}

using wire::Port;

extern "C" void MSG(const char *format, ...) { (void)format; }

namespace {

constexpr int64_t kTickNs = PTP_HEARTBEAT_TICKRATE_MS * 1000000LL;
constexpr int kTicksPerSecond = 1000 / PTP_HEARTBEAT_TICKRATE_MS;
constexpr uint8_t kGptp = 1;  // transportSpecific of 802.1AS
const Port kPeer{{0x00, 0xa0, 0xc9, 0xff, 0xfe, 0x26, 0x21, 0xe4}, 1};

class Peer : public ::testing::Test {
  protected:
    void SetUp() override {
        fake_reset();
        ASSERT_TRUE(reg_task_ptp());
        ptp_log_disable_all();
        const PtpProfile *gptp = ptp_profile_preset_get("gPTP");
        ASSERT_NE(gptp, nullptr);
        ptp_load_profile(gptp);
        task_ptp();  // the queued reset, and the profile with it
        ASSERT_EQ(gPtpCoreState.profile.delayMechanism, PTP_DM_P2P);
    }
    void TearDown() override { unreg_task_ptp(); }

    // A heartbeat, and -- unless the task is starved -- the task running after it.
    void heartbeat(bool task_runs = true) {
        fake_advance_ns(kTickNs);
        ptp_heartbeat_tmr_cb();
        if (task_runs) task_ptp();
    }

    void receive(const wire::Bytes &b) {
        const int64_t t = fake_now_ns();
        ptp_receive_enqueue(b.data(), (uint32_t)b.size(), (uint32_t)(t / 1000000000LL),
                            (uint32_t)(t % 1000000000LL), PTP_TP_802_3);
    }

    // The peer sends a Pdelay_Req; the task runs. Answered at once, and to this request?
    bool answered_on_time(uint16_t seq) {
        const uint32_t resp0 = g_fake.tx_by_type[wire::kPDelayResp];
        const uint32_t fup0 = g_fake.tx_by_type[wire::kPDelayRespFollowUp];
        receive(wire::pdelay_req(kPeer, seq, kGptp));
        task_ptp();
        return g_fake.tx_by_type[wire::kPDelayResp] == resp0 + 1 &&
               g_fake.last_tx_seq[wire::kPDelayResp] == seq &&
               g_fake.tx_by_type[wire::kPDelayRespFollowUp] == fup0 + 1 &&
               g_fake.last_tx_seq[wire::kPDelayRespFollowUp] == seq;
    }

    // Ten seconds of the peer's Pdelay_Reqs, one a second. How many are answered on time?
    int ten_seconds(uint16_t first_seq, std::string *late) {
        int on_time = 0;
        for (uint16_t seq = first_seq; seq < first_seq + 10; seq++) {
            if (answered_on_time(seq)) {
                on_time++;
            } else {
                *late += " " + std::to_string(seq) + "->" +
                         (g_fake.tx_by_type[wire::kPDelayResp]
                              ? std::to_string(g_fake.last_tx_seq[wire::kPDelayResp])
                              : std::string("none"));
            }
            for (int i = 0; i < kTicksPerSecond; i++) heartbeat();
        }
        return on_time;
    }
};

// Control: nothing has happened to the task; every request is answered as it arrives.
TEST_F(Peer, EveryPdelayReqIsAnsweredOnTime) {
    for (int i = 0; i < 4 * kTicksPerSecond; i++) heartbeat();
    std::string late;
    EXPECT_EQ(ten_seconds(100, &late), 10) << "requests answered late or not at all (seq->last answered):" << late;
}

// The PTP task is starved -- heartbeats keep firing, the peer's frames keep arriving -- and then
// runs again. Afterwards it must be exactly as responsive as before.
TEST_F(Peer, U14_AfterABurstEveryPdelayReqIsStillAnswered) {
    for (int i = 0; i < 4 * kTicksPerSecond; i++) heartbeat();

    // Starved: half a second of heartbeats, and eight frames from the peer.
    for (int i = 0; i < kTicksPerSecond / 2; i++) heartbeat(false);
    for (uint16_t seq = 0; seq < 8; seq++) receive(wire::pdelay_req(kPeer, seq, kGptp));

    task_ptp();  // the task runs again

    std::string late;
    EXPECT_EQ(ten_seconds(100, &late), 10)
        << "after the burst, requests answered late or not at all (seq->last answered):" << late
        << "\n  the task is taking each item a fixed number of notifications late: an item went "
           "onto its FIFO without its notification";
}

}  // namespace
