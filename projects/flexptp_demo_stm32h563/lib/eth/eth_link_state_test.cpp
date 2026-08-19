/* The PHY link-state transition rules.
 *
 * Finding CX-06. Four lines of policy were tangled with osDelay(), MSG() and register writes,
 * and one of them is a real rule: the FIRST poll must not announce DOWN, because at that point
 * auto-negotiation has not finished and reporting it makes a normal boot look like a cable
 * fault. A rule whose whole purpose is to make a normal boot look normal is exactly the kind
 * that gets tidied away by someone who has never seen the noisy version.
 */

#include <gtest/gtest.h>

extern "C" {
#include "eth_link_state.h"
}

namespace {

constexpr EthPhyStatus kUp100Full{true, true, true};
constexpr EthPhyStatus kUp10Half{true, false, false};
constexpr EthPhyStatus kDown{false, false, false};

class LinkState : public ::testing::Test {
  protected:
    EthLinkState s_{};  /* zero-initialised: never polled */
};

}  // namespace

TEST_F(LinkState, TheFirstPollFindingTheLinkDownSaysNothing) {
    /* The rule. Auto-negotiation takes a moment, so the first poll of a perfectly healthy
       board finds the link down -- and announcing that makes every normal boot look faulty. */
    const EthLinkAction a = eth_link_state_update(&s_, kDown);

    EXPECT_FALSE(a.announce_down) << "a normal boot announced a cable fault";
    EXPECT_FALSE(a.announce_up);
    EXPECT_FALSE(a.program_mac);
    EXPECT_TRUE(a.publish_to_stack) << "the stack must still be told, even silently";
    EXPECT_TRUE(s_.init);
}

TEST_F(LinkState, TheUpThatFollowsIsAnnounced) {
    eth_link_state_update(&s_, kDown);
    const EthLinkAction a = eth_link_state_update(&s_, kUp100Full);

    EXPECT_TRUE(a.announce_up);
    EXPECT_TRUE(a.program_mac);
    EXPECT_TRUE(a.publish_to_stack);
    EXPECT_EQ(s_.speed, 100u);
    EXPECT_TRUE(s_.duplex);
}

TEST_F(LinkState, ADownAfterAnUpIsAnnounced) {
    eth_link_state_update(&s_, kUp100Full);
    const EthLinkAction a = eth_link_state_update(&s_, kDown);

    EXPECT_TRUE(a.announce_down) << "a real cable pull must be reported";
    EXPECT_TRUE(a.publish_to_stack);
    EXPECT_FALSE(a.program_mac);
}

TEST_F(LinkState, AnUnchangedLinkSaysNothingAtAll) {
    /* phy_thread polls twice a second for the life of the board. Anything other than silence
       here is a console full of link state. */
    eth_link_state_update(&s_, kUp100Full);

    for (int i = 0; i < 10; i++) {
        const EthLinkAction a = eth_link_state_update(&s_, kUp100Full);
        EXPECT_FALSE(a.announce_up);
        EXPECT_FALSE(a.announce_down);
        EXPECT_FALSE(a.program_mac);
        EXPECT_FALSE(a.publish_to_stack);
    }
}

TEST_F(LinkState, TheFirstPollFindingTheLinkUpIsAnnounced) {
    /* The other first-poll case: a board booted with the cable already negotiated. Silence
       would be wrong here -- the operator has no other way to know the link came up. */
    const EthLinkAction a = eth_link_state_update(&s_, kUp100Full);

    EXPECT_TRUE(a.announce_up);
    EXPECT_TRUE(a.program_mac);
    EXPECT_TRUE(a.publish_to_stack);
}

TEST_F(LinkState, SpeedAndDuplexFollowThePhy) {
    eth_link_state_update(&s_, kUp10Half);
    EXPECT_EQ(s_.speed, 10u);
    EXPECT_FALSE(s_.duplex);

    eth_link_state_update(&s_, kDown);
    const EthLinkAction a = eth_link_state_update(&s_, kUp100Full);
    EXPECT_TRUE(a.program_mac) << "the MAC must be reprogrammed after a renegotiation";
    EXPECT_EQ(s_.speed, 100u);
    EXPECT_TRUE(s_.duplex);
}

TEST_F(LinkState, AFlapIsReportedEachWay) {
    eth_link_state_update(&s_, kDown);  /* first poll, silent */
    for (int i = 0; i < 3; i++) {
        EXPECT_TRUE(eth_link_state_update(&s_, kUp100Full).announce_up);
        EXPECT_TRUE(eth_link_state_update(&s_, kDown).announce_down);
    }
}
