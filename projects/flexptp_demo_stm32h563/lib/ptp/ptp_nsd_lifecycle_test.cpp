/* The NSD lifecycle, against a stack that counts.
 *
 * Finding NX-02: ptp_nsd_init() returned early on a create or bind failure without deleting
 * what it had already created, because its "open" flag was only set once the whole sequence had
 * succeeded. The socket stayed on NetX Duo's created list, and the next configure created it
 * again -- NX_PTR_ERROR in Debug, and in Release, where NX_DISABLE_ERROR_CHECKING removes the
 * guard, a corrupted doubly-linked list.
 *
 * The fake below is not a NetX Duo simulator. It is a ledger: it records every create, bind,
 * notify and delete, and it can be told to fail any one of them. That is enough to state the
 * properties that matter as assertions rather than as code-reading.
 */

#include <gtest/gtest.h>

#include <string>
#include <vector>

extern "C" {
#include "ptp_nsd_lifecycle.h"
}

namespace {

/** A stack that records what was asked of it, and fails on demand. */
struct FakeStack {
    std::vector<std::string> log;

    /* Which call should fail, by name; empty means none. */
    std::string fail_on;

    /* Live objects, as the stack itself would count them. This is the number NX-02 was about:
       a socket that exists on the IP instance and that nobody will ever delete. */
    int sockets_live = 0;
    int l2_live = 0;

    /* Every create must be matched by exactly one delete. A second create of a live socket is
       the corruption NX-02 produces in Release, so it is a hard failure here. */
    bool event_live = false;
    bool general_live = false;
    bool double_create = false;
    bool delete_of_absent = false;

    bool should_fail(const char *what) {
        log.emplace_back(what);
        return fail_on == what;
    }

    static const char *name(PtpNsdSocket s) {
        return (s == PTP_NSD_SOCK_EVENT) ? "event" : "general";
    }

    bool &live_flag(PtpNsdSocket s) {
        return (s == PTP_NSD_SOCK_EVENT) ? event_live : general_live;
    }
};

bool fake_socket_create(void *ctx, PtpNsdSocket which) {
    auto *f = static_cast<FakeStack *>(ctx);
    if (f->should_fail((std::string("create:") + FakeStack::name(which)).c_str())) {
        return false;
    }
    if (f->live_flag(which)) {
        f->double_create = true;  /* this is the NX-02 corruption, made loud */
    }
    f->live_flag(which) = true;
    f->sockets_live++;
    return true;
}

bool fake_socket_bind(void *ctx, PtpNsdSocket which, uint16_t port) {
    auto *f = static_cast<FakeStack *>(ctx);
    return !f->should_fail(
        (std::string("bind:") + FakeStack::name(which) + ":" + std::to_string(port)).c_str());
}

void fake_socket_notify(void *ctx, PtpNsdSocket which) {
    auto *f = static_cast<FakeStack *>(ctx);
    (void)f->should_fail((std::string("notify:") + FakeStack::name(which)).c_str());
}

void fake_socket_delete(void *ctx, PtpNsdSocket which) {
    auto *f = static_cast<FakeStack *>(ctx);
    (void)f->should_fail((std::string("delete:") + FakeStack::name(which)).c_str());
    if (!f->live_flag(which)) {
        f->delete_of_absent = true;
    }
    f->live_flag(which) = false;
    f->sockets_live--;
}

bool fake_l2_register(void *ctx) {
    auto *f = static_cast<FakeStack *>(ctx);
    if (f->should_fail("l2:register")) {
        return false;
    }
    f->l2_live++;
    return true;
}

void fake_l2_unregister(void *ctx) {
    auto *f = static_cast<FakeStack *>(ctx);
    (void)f->should_fail("l2:unregister");
    f->l2_live--;
}

void fake_multicast(void *ctx, bool join) {
    auto *f = static_cast<FakeStack *>(ctx);
    (void)f->should_fail(join ? "mcast:join" : "mcast:leave");
}

class Lifecycle : public ::testing::Test {
  protected:
    void SetUp() override {
        ops_ = PtpNsdOps{fake_socket_create, fake_socket_bind, fake_socket_notify,
                         fake_socket_delete, fake_l2_register, fake_l2_unregister,
                         fake_multicast,     &fake_};
        ptp_nsd_lifecycle_init(&lc_, &ops_);
    }

    /** Nothing the stack handed out is still outstanding, and nothing was misused. */
    void ExpectStackClean() {
        EXPECT_EQ(fake_.sockets_live, 0) << "a socket was created and never deleted -- NX-02";
        EXPECT_EQ(fake_.l2_live, 0) << "an L2 registration was left behind";
        EXPECT_FALSE(fake_.double_create) << "a live socket was created a second time -- this is "
                                             "the list corruption NX-02 produces in Release";
        EXPECT_FALSE(fake_.delete_of_absent) << "a socket was deleted twice";
    }

    FakeStack fake_;
    PtpNsdOps ops_{};
    PtpNsdLifecycle lc_{};
};

}  // namespace

TEST_F(Lifecycle, StartsWithNothingConfigured) {
    EXPECT_FALSE(ptp_nsd_lifecycle_active(&lc_));
    EXPECT_EQ(lc_.transport, PTP_EP_TRANSPORT_NONE);
    EXPECT_EQ(ptp_nsd_lifecycle_outstanding(&lc_), 0u);
}

TEST_F(Lifecycle, Ipv4CreatesBindsAndNotifiesBothSockets) {
    ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));

    EXPECT_EQ(lc_.transport, PTP_EP_TRANSPORT_IPV4);
    EXPECT_EQ(ptp_nsd_lifecycle_outstanding(&lc_), 2u);
    EXPECT_EQ(fake_.log, (std::vector<std::string>{
                             "create:event", "create:general",
                             "bind:event:319", "bind:general:320",
                             "notify:event", "notify:general", "mcast:join"}));
}

TEST_F(Lifecycle, TeardownDeletesEverythingAndLeavesTheGroup) {
    ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));
    fake_.log.clear();

    ptp_nsd_lifecycle_teardown(&lc_);

    EXPECT_EQ(fake_.log, (std::vector<std::string>{"mcast:leave", "delete:event",
                                                   "delete:general"}));
    EXPECT_EQ(lc_.transport, PTP_EP_TRANSPORT_NONE);
    EXPECT_FALSE(ptp_nsd_lifecycle_active(&lc_));
    ExpectStackClean();
}

TEST_F(Lifecycle, ASecondSocketCreateFailureLeavesTheFirstDeleted) {
    /* NX-02, first half. The event socket exists by the time the general one fails; the old
       code returned here with the "open" flag still false, so nothing ever deleted it. */
    fake_.fail_on = "create:general";

    EXPECT_FALSE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));

    EXPECT_EQ(ptp_nsd_lifecycle_outstanding(&lc_), 0u);
    ExpectStackClean();
}

TEST_F(Lifecycle, ABindFailureLeavesBothSocketsDeleted) {
    /* NX-02, second half, and the worse one: BOTH sockets are created before either is bound. */
    fake_.fail_on = "bind:general:320";

    EXPECT_FALSE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));

    EXPECT_EQ(ptp_nsd_lifecycle_outstanding(&lc_), 0u);
    ExpectStackClean();
}

TEST_F(Lifecycle, AFailedConfigureLeavesTheTransportUnset) {
    /* The half of NX-02 that is not a leak. The old code left s_tp at its previous value on
       every failure return, so ptp_nsd_transmit_msg() would take the IPv4 branch and send on
       sockets that had just been deleted. */
    ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));
    ASSERT_EQ(lc_.transport, PTP_EP_TRANSPORT_IPV4);

    fake_.fail_on = "create:event";
    EXPECT_FALSE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));

    EXPECT_EQ(lc_.transport, PTP_EP_TRANSPORT_NONE)
        << "a failed configure left a transport the transmit path would act on";
    ExpectStackClean();
}

TEST_F(Lifecycle, RecoversAfterAFailedConfigure) {
    /* The consequence of getting the cleanup right: a transient failure is transient. With the
       socket left behind, the retry created it a second time -- NX_PTR_ERROR in Debug, so PTP
       stayed dead for the rest of the session long after the cause had gone. */
    fake_.fail_on = "bind:event:319";
    ASSERT_FALSE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));

    fake_.fail_on.clear();
    EXPECT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));
    EXPECT_EQ(ptp_nsd_lifecycle_outstanding(&lc_), 2u);
    EXPECT_FALSE(fake_.double_create);
}

TEST_F(Lifecycle, Ethernet802_3RegistersTheLinkCallbackAndNoSockets) {
    ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_802_3, PTP_EP_DELAY_P2P));

    EXPECT_EQ(lc_.transport, PTP_EP_TRANSPORT_802_3);
    EXPECT_EQ(fake_.sockets_live, 0);
    EXPECT_EQ(fake_.l2_live, 1);
    EXPECT_EQ(fake_.log, (std::vector<std::string>{"l2:register", "mcast:join"}));
}

TEST_F(Lifecycle, AnL2RegistrationFailureLeavesNothingBehind) {
    fake_.fail_on = "l2:register";

    EXPECT_FALSE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_802_3, PTP_EP_DELAY_P2P));

    EXPECT_EQ(lc_.transport, PTP_EP_TRANSPORT_NONE);
    ExpectStackClean();
}

TEST_F(Lifecycle, SwitchingTransportDestroysTheOldOneFirst) {
    /* `ptp transport 802.3` at the CLI, and back. Every object of the outgoing transport must
       be gone before the incoming one is built -- they share an interface index. */
    ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));
    fake_.log.clear();

    ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_802_3, PTP_EP_DELAY_P2P));

    EXPECT_EQ(fake_.log, (std::vector<std::string>{"mcast:leave", "delete:event",
                                                   "delete:general", "l2:register",
                                                   "mcast:join"}));
    EXPECT_EQ(fake_.sockets_live, 0);
    EXPECT_EQ(fake_.l2_live, 1);
}

TEST_F(Lifecycle, SwitchingBackAndForthLeaksNothing) {
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));
        ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_802_3, PTP_EP_DELAY_P2P));
    }
    ptp_nsd_lifecycle_teardown(&lc_);
    ExpectStackClean();
}

TEST_F(Lifecycle, ShutdownRequestTearsDownAndReportsNothingLive) {
    /* flexPTP calls ptp_nsd_init(-1, -1) from unreg_task_ptp(). */
    ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));

    EXPECT_FALSE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_NONE, PTP_EP_DELAY_E2E));

    EXPECT_EQ(lc_.transport, PTP_EP_TRANSPORT_NONE);
    ExpectStackClean();
}

TEST_F(Lifecycle, TeardownIsIdempotent) {
    ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));
    ptp_nsd_lifecycle_teardown(&lc_);
    fake_.log.clear();

    ptp_nsd_lifecycle_teardown(&lc_);

    EXPECT_TRUE(fake_.log.empty()) << "teardown touched the stack with nothing configured";
    ExpectStackClean();
}

TEST_F(Lifecycle, TheDelayMechanismIsPublishedBeforeTheGroupIsJoined) {
    /* The multicast group depends on the delay mechanism -- 224.0.1.129 for E2E and
       224.0.0.107 for peer delay -- so the ops callback must be able to read the new value,
       not the previous one, by the time it is asked to join. */
    ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_E2E));
    ASSERT_EQ(lc_.delay, PTP_EP_DELAY_E2E);

    ASSERT_TRUE(ptp_nsd_lifecycle_configure(&lc_, PTP_EP_TRANSPORT_IPV4, PTP_EP_DELAY_P2P));
    EXPECT_EQ(lc_.delay, PTP_EP_DELAY_P2P);
}
