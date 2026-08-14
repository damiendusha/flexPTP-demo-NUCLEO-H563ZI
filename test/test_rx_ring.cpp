/* Characterisation tests for the receive descriptor path in mac_drv.c.
 *
 * These pin the behaviour of the CURRENT, hardware-verified driver so that the NetX Duo
 * driver written in S5b has a behavioural spec to be checked against, rather than a code
 * sample to be copied. Where a test corresponds to a finding, it says so: those are the
 * cases that were once wrong on real hardware and cost real time.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <numeric>
#include <optional>
#include <vector>

#include "eth_model.hpp"

using ethmodel::EthModel;

namespace {

std::vector<std::uint8_t> make_frame(std::uint16_t len, std::uint8_t seed = 0) {
    std::vector<std::uint8_t> f(len);
    std::iota(f.begin(), f.end(), seed);
    return f;
}

class RxRing : public ::testing::Test {
  protected:
    void SetUp() override { eth_.emplace(8, 4); }
    std::optional<EthModel> eth_;
};

}  // namespace

TEST_F(RxRing, DeliversASingleFrameOnce) {
    auto frame = make_frame(64);
    eth_->deliver_frame(frame.data(), frame.size());

    ETHHW_ProcessRx(eth_->eth());

    ASSERT_EQ(EthModel::rx_events().size(), 1u);
    EXPECT_EQ(EthModel::rx_events()[0].size, 64u);
    EXPECT_EQ(std::memcmp(EthModel::rx_events()[0].payload, frame.data(), 64), 0);
}

TEST_F(RxRing, ZeroesTheTimestampWhenTheFrameHasNone) {
    /* ts_s/ts_ns are only assigned when a context descriptor carried a timestamp, but the
     * receive thread copies them out unconditionally. Left uninitialised, an untimestamped
     * frame hands the stack's contents up as a plausible-looking time -- the same class of
     * trap as F59's NaN printing as "0.0000". */
    auto frame = make_frame(64);
    eth_->deliver_frame(frame.data(), frame.size());

    ETHHW_ProcessRx(eth_->eth());

    ASSERT_EQ(EthModel::rx_events().size(), 1u);
    EXPECT_EQ(EthModel::rx_events()[0].ts_s, 0u);
    EXPECT_EQ(EthModel::rx_events()[0].ts_ns, 0u);
    EXPECT_EQ(EthModel::rx_events()[0].bd_ctx, nullptr);
}

TEST_F(RxRing, PairsAFrameWithItsContextDescriptorTimestamp) {
    auto frame = make_frame(80);
    eth_->deliver_frame_ts(frame.data(), frame.size(), 0x1234u, 987654321u);

    ETHHW_ProcessRx(eth_->eth());

    ASSERT_EQ(EthModel::rx_events().size(), 1u);
    const auto &e = EthModel::rx_events()[0];
    EXPECT_EQ(e.size, 80u);
    EXPECT_EQ(e.ts_s, 0x1234u);
    EXPECT_EQ(e.ts_ns, 987654321u);
    EXPECT_NE(e.bd_ctx, nullptr) << "the context descriptor must be reported so the thread "
                                    "can restore it too";
}

TEST_F(RxRing, DoesNotRedeliverAFrameStillInFlight) {
    /* Finding F61, measured at 57% of all frames under ordinary PTP traffic.
     *
     * The OWN bit alone cannot distinguish "newly received" from "already queued": it stays
     * clear from the moment the MAC writes the frame until the receive thread restores the
     * descriptor, and ETHHW_ProcessRx() runs from the ISR, which can re-enter that window
     * freely. ETHHW_DescExt.inFlight is what closes it.
     *
     * Returning 0 from the read callback is what the real rx_thread does -- "I have queued
     * it, I will restore it later" -- which is precisely the window under test. */
    EthModel::read_callback_result() = 0;

    auto frame = make_frame(100);
    eth_->deliver_frame(frame.data(), frame.size());

    ETHHW_ProcessRx(eth_->eth());
    ASSERT_EQ(EthModel::rx_events().size(), 1u);

    // A second ISR arriving before the thread has restored the descriptor.
    ETHHW_ProcessRx(eth_->eth());
    EXPECT_EQ(EthModel::rx_events().size(), 1u)
        << "frame re-delivered while still in flight -- this is F61";
}

TEST_F(RxRing, AcceptsTheFrameAgainOnlyAfterTheDescriptorIsRestored) {
    EthModel::read_callback_result() = 0;

    auto frame = make_frame(100);
    eth_->deliver_frame(frame.data(), frame.size());
    ETHHW_ProcessRx(eth_->eth());
    ASSERT_EQ(EthModel::rx_events().size(), 1u);

    // The thread returns the descriptor to the DMA; inFlight clears with it.
    ETHHW_RestoreRXDesc(EthModel::rx_events()[0].bd);
    EXPECT_EQ(EthModel::rx_events()[0].bd->ext.inFlight, 0u);

    // A fresh frame in the same slot is delivered normally.
    eth_->deliver_frame(frame.data(), frame.size());
    ETHHW_ProcessRx(eth_->eth());
    EXPECT_EQ(EthModel::rx_events().size(), 2u);
}

TEST_F(RxRing, RestoringADescriptorRearmsItForTheDma) {
    auto frame = make_frame(64);
    eth_->deliver_frame(frame.data(), frame.size());
    EthModel::read_callback_result() = 0;
    ETHHW_ProcessRx(eth_->eth());

    ETHHW_DescFull *bd = EthModel::rx_events()[0].bd;
    ETHHW_RestoreRXDesc(bd);

    EXPECT_TRUE(bd->desc.DES3 & ETH_DMARXNDESCRF_OWN) << "ownership returned to the DMA";
    EXPECT_TRUE(bd->desc.DES3 & ETH_DMARXNDESCRF_BUF1V) << "buffer marked valid again";
    EXPECT_EQ(bd->desc.DES0, bd->ext.bufAddr) << "buffer address restored from the extension";
}

TEST_F(RxRing, RestoringWritesTheTailPointerToRestartTheEngine) {
    /* Finding F71. Returning ownership is not enough to restart a receive engine that has
     * already given up: on reaching a descriptor the application still owns, the DMA raises
     * RBU and suspends, and only a tail-pointer write makes it look again. DMACRDTPR was
     * written once at init and never touched, so one momentary ring exhaustion killed
     * reception permanently.
     *
     * This asserts the driver performs the write. It cannot prove the engine restarts --
     * the model has no RBU semantics, and that half stays on the bench. */
    auto frame = make_frame(64);
    eth_->deliver_frame(frame.data(), frame.size());
    EthModel::read_callback_result() = 0;
    ETHHW_ProcessRx(eth_->eth());

    eth_->eth()->DMACRDTPR = 0xDEADBEEFu;  // poison, so we can see the write happen
    ETHHW_RestoreRXDesc(EthModel::rx_events()[0].bd);
    EXPECT_EQ(eth_->eth()->DMACRDTPR, 0u) << "tail pointer not rewritten -- this is F71";
}

TEST_F(RxRing, RestoreIgnoresANullDescriptor) {
    // ETHHW_ProcessRx() passes bd_ctx unconditionally, and it is NULL for untimestamped
    // frames. The null guard is load-bearing on every non-PTP frame the board receives.
    ETHHW_RestoreRXDesc(nullptr);
    SUCCEED();
}

TEST_F(RxRing, DrainsSeveralFramesInArrivalOrder) {
    for (std::uint8_t i = 0; i < 4; i++) {
        auto frame = make_frame(64, i);
        eth_->deliver_frame(frame.data(), frame.size());
    }

    ETHHW_ProcessRx(eth_->eth());

    ASSERT_EQ(EthModel::rx_events().size(), 4u);
    for (std::uint8_t i = 0; i < 4; i++) {
        EXPECT_EQ(static_cast<const std::uint8_t *>(EthModel::rx_events()[i].payload)[0], i)
            << "frame " << int(i) << " delivered out of order";
    }
}
