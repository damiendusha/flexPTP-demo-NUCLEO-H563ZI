/* The packet→uid association behind PTP transmit timestamps.
 *
 * This table is the only thing standing between a captured transmit timestamp and the message
 * it belongs to. If it hands back the wrong uid, flexPTP pairs a Delay_Req with someone else's
 * t3 and the servo is fed a fabricated path delay -- silently, and only on the cycles unlucky
 * enough to collide. That is not a failure a bench can be relied on to show, which is why the
 * logic was extracted from eth_drv_netxduo.c to be tested here.
 */

#include <gtest/gtest.h>

extern "C" {
#include "ptp_tx_registry.h"
}

namespace {

class TxRegistry : public ::testing::Test {
  protected:
    void SetUp() override { ptp_tx_registry_reset(&reg_); }

    PtpTxRegistry reg_{};

    /* Distinct addresses standing in for NX_PACKET pointers. Their values are irrelevant --
       the table only ever compares them -- but they must be distinct and non-null. */
    int slots_[8]{};
    const void *pkt(int i) const { return &slots_[i]; }
};

}  // namespace

TEST_F(TxRegistry, StartsEmpty) {
    EXPECT_EQ(ptp_tx_registry_count(&reg_), 0u);
}

TEST_F(TxRegistry, RoundTripsAUid) {
    ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(0), 4242));

    uint32_t uid = 0;
    ASSERT_TRUE(ptp_tx_registry_take(&reg_, pkt(0), &uid));
    EXPECT_EQ(uid, 4242u);
}

TEST_F(TxRegistry, TakeFreesTheSlot) {
    ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(0), 1));
    EXPECT_EQ(ptp_tx_registry_count(&reg_), 1u);

    uint32_t uid = 0;
    ASSERT_TRUE(ptp_tx_registry_take(&reg_, pkt(0), &uid));
    EXPECT_EQ(ptp_tx_registry_count(&reg_), 0u);
}

TEST_F(TxRegistry, DoesNotYieldTheSameEntryTwice) {
    /* The MAC reports one timestamp per descriptor, but ETHHW_ProcessTx() re-scans the ring
       and F57 was precisely a second dispatch of an already-processed descriptor. If that ever
       recurs, the table must not corroborate it. */
    ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(0), 7));

    uint32_t uid = 0;
    ASSERT_TRUE(ptp_tx_registry_take(&reg_, pkt(0), &uid));
    EXPECT_FALSE(ptp_tx_registry_take(&reg_, pkt(0), &uid));
}

TEST_F(TxRegistry, RecycledPacketDoesNotInheritAStaleUid) {
    /* THE ABA HAZARD, and the reason this is a table rather than a variable.
     *
     * NetX Duo recycles packets. A packet registered, sent, consumed, released and then
     * reallocated at the same address must NOT still be associated with the first message --
     * if it were, the servo would receive a real timestamp under a wrong uid, which is worse
     * than receiving none. `take` clearing the slot is what prevents it. */
    ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(0), 100));

    uint32_t uid = 0;
    ASSERT_TRUE(ptp_tx_registry_take(&reg_, pkt(0), &uid));
    ASSERT_EQ(uid, 100u);

    // Same address comes back from the pool, this time with no timestamp requested.
    uid = 0xDEADBEEF;
    EXPECT_FALSE(ptp_tx_registry_take(&reg_, pkt(0), &uid))
        << "a recycled packet inherited the previous message's uid";
}

TEST_F(TxRegistry, CancelPreventsInheritance) {
    /* The other half. When a send fails the NSD releases the packet without it ever reaching
     * the driver, so nothing will ever `take` the entry. Without an explicit cancel it would
     * sit there until a later packet landed on the same address and claimed it. */
    ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(0), 55));
    ptp_tx_registry_cancel(&reg_, pkt(0));

    uint32_t uid = 0;
    EXPECT_FALSE(ptp_tx_registry_take(&reg_, pkt(0), &uid))
        << "a cancelled entry was still claimable -- this is the send-failure leak";
}

TEST_F(TxRegistry, CancellingSomethingAbsentIsHarmless) {
    ptp_tx_registry_cancel(&reg_, pkt(3));
    EXPECT_EQ(ptp_tx_registry_count(&reg_), 0u);
}

TEST_F(TxRegistry, KeepsSeveralMessagesApart) {
    ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(0), 10));
    ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(1), 20));
    ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(2), 30));

    // Taken out of order, as they would be if the MAC completed them out of order.
    uint32_t uid = 0;
    ASSERT_TRUE(ptp_tx_registry_take(&reg_, pkt(1), &uid));
    EXPECT_EQ(uid, 20u);
    ASSERT_TRUE(ptp_tx_registry_take(&reg_, pkt(2), &uid));
    EXPECT_EQ(uid, 30u);
    ASSERT_TRUE(ptp_tx_registry_take(&reg_, pkt(0), &uid));
    EXPECT_EQ(uid, 10u);
}

TEST_F(TxRegistry, ReRegisteringAKeyOverwritesRatherThanDuplicating) {
    /* Two live entries for one address could be taken in either order, which is exactly the
       mispairing this table exists to prevent. */
    ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(0), 1));
    ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(0), 2));
    EXPECT_EQ(ptp_tx_registry_count(&reg_), 1u);

    uint32_t uid = 0;
    ASSERT_TRUE(ptp_tx_registry_take(&reg_, pkt(0), &uid));
    EXPECT_EQ(uid, 2u);
    EXPECT_FALSE(ptp_tx_registry_take(&reg_, pkt(0), &uid));
}

TEST_F(TxRegistry, RefusesWhenFullAndCountsIt) {
    for (int i = 0; i < PTP_TX_REGISTRY_CAPACITY; i++) {
        ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(i), static_cast<uint32_t>(i)));
    }
    EXPECT_FALSE(ptp_tx_registry_add(&reg_, pkt(PTP_TX_REGISTRY_CAPACITY), 99));
    EXPECT_EQ(reg_.overflows, 1u);

    // Refusing must not corrupt what is already held.
    uint32_t uid = 0;
    ASSERT_TRUE(ptp_tx_registry_take(&reg_, pkt(0), &uid));
    EXPECT_EQ(uid, 0u);
}

TEST_F(TxRegistry, RecoversAfterOverflow) {
    for (int i = 0; i < PTP_TX_REGISTRY_CAPACITY; i++) {
        ASSERT_TRUE(ptp_tx_registry_add(&reg_, pkt(i), static_cast<uint32_t>(i)));
    }
    EXPECT_FALSE(ptp_tx_registry_add(&reg_, pkt(PTP_TX_REGISTRY_CAPACITY), 99));

    uint32_t uid = 0;
    ASSERT_TRUE(ptp_tx_registry_take(&reg_, pkt(1), &uid));
    EXPECT_TRUE(ptp_tx_registry_add(&reg_, pkt(PTP_TX_REGISTRY_CAPACITY), 99))
        << "a full table stayed full after a slot was freed";
}

TEST_F(TxRegistry, IgnoresANullKey) {
    EXPECT_FALSE(ptp_tx_registry_add(&reg_, nullptr, 1));
    uint32_t uid = 0;
    EXPECT_FALSE(ptp_tx_registry_take(&reg_, nullptr, &uid));
    ptp_tx_registry_cancel(&reg_, nullptr);
    EXPECT_EQ(ptp_tx_registry_count(&reg_), 0u);
}
