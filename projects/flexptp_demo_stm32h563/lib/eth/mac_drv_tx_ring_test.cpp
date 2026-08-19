/* Characterisation tests for the transmit descriptor path in mac_drv.c.
 *
 * The transmit side is where the PTP t3 timestamp comes from, so a defect here is silent by
 * construction: frames still go out, the link still works, and only the servo notices. Both
 * findings pinned below behaved exactly that way on hardware.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <numeric>
#include <optional>
#include <vector>

#include "eth_model.hpp"

using ethmodel::EthModel;

extern "C" void eth_model_tx_ts_cb(std::uint32_t ts_s, std::uint32_t ts_ns, void *tag);

/* ETHHW_ProcessTx() has external linkage in mac_drv.c but is not declared in mac_drv.h --
   on target its only caller is ETHHW_ISR() in the same file. Declared here rather than added
   to the header, so the test does not widen the driver's public surface. */
extern "C" void ETHHW_ProcessTx(ETH_TypeDef *eth);

namespace {

std::vector<std::uint8_t> make_frame(std::uint16_t len, std::uint8_t seed = 0) {
    std::vector<std::uint8_t> f(len);
    std::iota(f.begin(), f.end(), seed);
    return f;
}

class TxRing : public ::testing::Test {
  protected:
    void SetUp() override { eth_.emplace(8, 8); }

    /* The driver's state block, recovered the same way ETHHW_GetState() does. It is file-
       static inside mac_drv.c, so the tests reach it through the register rather than the
       symbol -- which also re-checks the layout contract on every use. */
    ETHHW_State *state() {
        return reinterpret_cast<ETHHW_State *>(
                   static_cast<std::uintptr_t>(eth_->eth()->DMACRDLAR)) - 1;
    }

    /** Transmit a frame requesting a timestamp, tagged so the dispatch can be identified. */
    bool transmit_ts(std::uint32_t tag, std::uint16_t len = 64) {
        auto frame = make_frame(len, static_cast<std::uint8_t>(tag));
        ETHHW_OptArg_TxTsCap arg{};
        arg.txTsCbPtr = static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(&eth_model_tx_ts_cb));
        arg.tag = tag;
        return ETHHW_Transmit(eth_->eth(), frame.data(), len, ETHHW_TXOPT_CAPTURE_TS, &arg);
    }

    std::optional<EthModel> eth_;
};

}  // namespace

TEST_F(TxRing, FillsTheDescriptorAndHandsItToTheDma) {
    auto frame = make_frame(100);
    ASSERT_TRUE(ETHHW_Transmit(eth_->eth(), frame.data(), frame.size(), ETHHW_TXOPT_NONE, nullptr));

    ETHHW_DescFull *bd = eth_->tx_ring();
    EXPECT_TRUE(bd->desc.DES3 & ETH_DMATXNDESCRF_OWN) << "ownership passed to the DMA";
    EXPECT_TRUE(bd->desc.DES3 & ETH_DMATXNDESCRF_FD);
    EXPECT_TRUE(bd->desc.DES3 & ETH_DMATXNDESCRF_LD);
    EXPECT_EQ(bd->desc.DES2 & 0x3FFF, 100u) << "length in the low 14 bits of DES2";
    EXPECT_EQ(bd->desc.DES0, bd->ext.bufAddr);
    EXPECT_EQ(std::memcmp(reinterpret_cast<void *>(static_cast<std::uintptr_t>(bd->ext.bufAddr)),
                          frame.data(), 100), 0);
}

TEST_F(TxRing, WritesTheTailPointerSoTheEngineLooksAgain) {
    eth_->eth()->DMACTDTPR = 0xDEADBEEFu;
    auto frame = make_frame(64);
    ASSERT_TRUE(ETHHW_Transmit(eth_->eth(), frame.data(), frame.size(), ETHHW_TXOPT_NONE, nullptr));
    EXPECT_EQ(eth_->eth()->DMACTDTPR, 0u);
}

TEST_F(TxRing, RequestsATimestampOnlyWhenAsked) {
    auto frame = make_frame(64);
    ASSERT_TRUE(ETHHW_Transmit(eth_->eth(), frame.data(), frame.size(), ETHHW_TXOPT_NONE, nullptr));
    EXPECT_FALSE(eth_->tx_ring()[0].desc.DES2 & ETH_DMATXNDESCRF_TTSE);
    EXPECT_EQ(eth_->tx_ring()[0].ext.tsCbPtr, 0u);

    ASSERT_TRUE(transmit_ts(42));
    EXPECT_TRUE(eth_->tx_ring()[1].desc.DES2 & ETH_DMATXNDESCRF_TTSE)
        << "TTSE must be set or the MAC captures no transmit timestamp";
    EXPECT_NE(eth_->tx_ring()[1].ext.tsCbPtr, 0u);
    EXPECT_EQ(eth_->tx_ring()[1].ext.tsCbArg, 42u);
}

TEST_F(TxRing, DispatchesTheTimestampToTheCallbackWithItsTag) {
    ASSERT_TRUE(transmit_ts(7));
    eth_->complete_tx(true, 0x2222u, 123456789u);

    ETHHW_ProcessTx(eth_->eth());

    ASSERT_EQ(EthModel::tx_ts_events().size(), 1u);
    EXPECT_EQ(EthModel::tx_ts_events()[0].tag, 7u);
    EXPECT_EQ(EthModel::tx_ts_events()[0].ts_s, 0x2222u);
    EXPECT_EQ(EthModel::tx_ts_events()[0].ts_ns, 123456789u);
}

TEST_F(TxRing, DoesNotDispatchAFrameThatCarriedNoTimestamp) {
    auto frame = make_frame(64);
    ASSERT_TRUE(ETHHW_Transmit(eth_->eth(), frame.data(), frame.size(), ETHHW_TXOPT_NONE, nullptr));
    eth_->complete_tx(false);

    ETHHW_ProcessTx(eth_->eth());

    EXPECT_TRUE(EthModel::tx_ts_events().empty());
}

TEST_F(TxRing, DoesNotRedispatchAfterTheRingIsDrained) {
    /* Finding F57. The count of timestamp-carrying descriptors was declared outside the
     * retry loop and never reset per pass, so each re-scan ADDED to the previous count
     * instead of replacing it. The count stayed positive after the ring had been drained and
     * the loop ran again with stale state, re-dispatching a descriptor already processed.
     *
     * On hardware that produced duplicate transmit-timestamp callbacks for one uid, which
     * flexPTP's slave.c then mismatched against its Delay_Req and discarded -- surfacing as
     * "no response received" rather than as anything resembling a driver bug. */
    ASSERT_TRUE(transmit_ts(1));
    ASSERT_TRUE(transmit_ts(2));
    eth_->complete_tx(true, 10u, 11u);
    eth_->complete_tx(true, 20u, 21u);

    ETHHW_ProcessTx(eth_->eth());
    ASSERT_EQ(EthModel::tx_ts_events().size(), 2u);

    // Nothing further has completed; a second pass must find an empty ring.
    ETHHW_ProcessTx(eth_->eth());
    EXPECT_EQ(EthModel::tx_ts_events().size(), 2u)
        << "descriptor dispatched twice after the ring drained -- this is F57";
}

TEST_F(TxRing, ClearsTheDescriptorBeforeInvokingTheCallback) {
    // The descriptor is released first specifically so it cannot be observed a second time
    // even if ETHHW_ProcessTx() re-enters from another interrupt.
    ASSERT_TRUE(transmit_ts(9));
    eth_->complete_tx(true, 1u, 2u);
    ETHHW_ProcessTx(eth_->eth());

    EXPECT_EQ(eth_->tx_ring()[0].ext.tsCbPtr, 0u);
    EXPECT_FALSE(eth_->tx_ring()[0].desc.DES3 & ETH_DMATXNDESCWBF_TTSS);
}

TEST_F(TxRing, DispatchesOldestFirst) {
    ASSERT_TRUE(transmit_ts(101));
    ASSERT_TRUE(transmit_ts(102));
    ASSERT_TRUE(transmit_ts(103));
    eth_->complete_tx(true, 1u, 1u);
    eth_->complete_tx(true, 2u, 2u);
    eth_->complete_tx(true, 3u, 3u);

    ETHHW_ProcessTx(eth_->eth());

    ASSERT_EQ(EthModel::tx_ts_events().size(), 3u);
    EXPECT_EQ(EthModel::tx_ts_events()[0].tag, 101u);
    EXPECT_EQ(EthModel::tx_ts_events()[1].tag, 102u);
    EXPECT_EQ(EthModel::tx_ts_events()[2].tag, 103u);
}

TEST_F(TxRing, DispatchesInOrderAcrossTheSixteenBitCounterWrap) {
    /* The ordering key is a uint16 transmit counter, compared with a MODULAR int16 delta
     * rather than a widened subtraction. The driver's comment gives the failing case:
     * "with txCntAcked = 65530 and descriptors 65531, 65532 and 2 pending, the deltas are
     * +1, +2 and -65528, so the 'delta < minDelta' rule picks the newest descriptor as the
     * oldest and dispatches its timestamp out of order."
     *
     * The wrap is not rare: txCntr increments on EVERY transmit, not only timestamped ones,
     * so on a busy link it comes round in minutes.
     *
     * CONSTRUCTING A SCENARIO THAT ACTUALLY DISCRIMINATES took some care, and a weaker one
     * is worse than none. Two candidates must be pending *simultaneously* on opposite sides
     * of the wrap, and neither may sit at delta == 1, because that hits the "consecutive
     * packet" fast path which is correct under either arithmetic. Four timestamped frames in
     * a row do NOT discriminate: the fast path takes the first, and once the others are all
     * post-wrap both arithmetics order them identically.
     *
     * So: txCntAcked = 65530, with timestamped descriptors at txCntr 65532 and 2, and plain
     * untimestamped frames filling the gap -- which is exactly how it arises in the field.
     *
     *     modular int16 : 65532 -> +2,  2 -> +8    => 65532 is older. Correct.
     *     widened int32 : 65532 -> +2,  2 -> -65528 => 2 looks older. WRONG.
     */
    state()->txCntSent = 65531;
    state()->txCntAcked = 65530;

    auto plain = make_frame(64);
    ASSERT_TRUE(transmit_ts(201));  // txCntr 65532  <- candidate, pre-wrap
    for (int i = 0; i < 5; i++) {   // txCntr 65533, 65534, 65535, 0, 1
        ASSERT_TRUE(ETHHW_Transmit(eth_->eth(), plain.data(), plain.size(),
                                   ETHHW_TXOPT_NONE, nullptr));
    }
    ASSERT_TRUE(transmit_ts(202));  // txCntr 2      <- candidate, post-wrap

    EXPECT_EQ(eth_->tx_ring()[0].ext.txCntr, 65532u);
    EXPECT_EQ(eth_->tx_ring()[6].ext.txCntr, 2u) << "counter must wrap, not saturate";

    eth_->complete_tx(true, 1u, 1u);            // the pre-wrap timestamped frame
    for (int i = 0; i < 5; i++) {
        eth_->complete_tx(false);               // the untimestamped filler
    }
    eth_->complete_tx(true, 2u, 2u);            // the post-wrap timestamped frame

    ETHHW_ProcessTx(eth_->eth());

    ASSERT_EQ(EthModel::tx_ts_events().size(), 2u);
    EXPECT_EQ(EthModel::tx_ts_events()[0].tag, 201u)
        << "the pre-wrap frame is older and must be dispatched first";
    EXPECT_EQ(EthModel::tx_ts_events()[1].tag, 202u);
}

TEST_F(TxRing, DropsTheFrameWhenTheDescriptorIsStillOwnedByTheDma) {
    /* Finding N14/F75, and the bounded-wait half of F54. A stalled TX DMA must not spin
     * forever: ETHHW_Transmit() runs on tcpip_thread WHILE HOLDING THE LWIP CORE LOCK, so
     * blocking here deadlocks every stack user with no watchdog to recover. Dropping is the
     * right failure, and it must be reported rather than returned as success. */
    auto frame = make_frame(64);
    ASSERT_TRUE(ETHHW_Transmit(eth_->eth(), frame.data(), frame.size(), ETHHW_TXOPT_NONE, nullptr));

    // Rewind so the next transmit targets the descriptor the DMA still owns.
    state()->nextTxDescIdx = 0;
    host_msg_clear();

    EXPECT_FALSE(ETHHW_Transmit(eth_->eth(), frame.data(), frame.size(), ETHHW_TXOPT_NONE, nullptr))
        << "a frame that could not be queued must not be reported as sent";
    EXPECT_NE(std::strstr(host_msg_text(), "frame dropped"), nullptr)
        << "the drop must be reported; silence here is what made N14 invisible";
}
