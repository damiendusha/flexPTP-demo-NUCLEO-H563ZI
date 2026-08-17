/* The PHY link-state decision, separated from the thread that polls it.
 *
 * Four lines of policy were tangled with osDelay(), MSG() and register writes in phy_thread(),
 * and one of them is a real rule rather than plumbing: the FIRST poll must not announce DOWN.
 * At that point auto-negotiation has simply not finished, and reporting it makes a normal boot
 * look like a cable fault -- which is why the lwIP driver printed only the UP that followed.
 *
 * A rule that exists to make a normal boot look normal is exactly the kind that gets
 * "simplified" away by someone who has never seen the noisy version (finding CX-06).
 *
 * C++ per the rules in Src/cpp_support.cpp; depends on nothing.
 */
#ifndef ETH_LINK_STATE_H
#define ETH_LINK_STATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What the driver believes about the link. Zero-initialised means "never polled". */
typedef struct {
    bool init;      /**< has the link state ever been read? */
    bool up;
    uint16_t speed; /**< Mbps */
    bool duplex;    /**< true = full */
} EthLinkState;

/** What the PHY says right now. */
typedef struct {
    bool up;
    bool fast_ethernet; /**< 100 Mbps rather than 10 */
    bool full_duplex;
} EthPhyStatus;

/** What the caller should do about it. */
typedef struct {
    bool announce_up;      /**< print the UP line */
    bool announce_down;    /**< print the DOWN line */
    bool program_mac;      /**< push speed/duplex into MACCR */
    bool publish_to_stack; /**< write nx_interface_link_up */
} EthLinkAction;

/**
 * Fold a PHY reading into the driver's state, and say what follows from it.
 *
 * @p state is updated in place.
 */
EthLinkAction eth_link_state_update(EthLinkState *state, EthPhyStatus now);

#ifdef __cplusplus
}
#endif

#endif /* ETH_LINK_STATE_H */
