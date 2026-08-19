#include "eth_link_state.h"

extern "C" EthLinkAction eth_link_state_update(EthLinkState *state, EthPhyStatus now) {
    EthLinkAction action{false, false, false, false};

    const bool first_poll = !state->init;
    if ((state->up == now.up) && state->init) {
        return action; /* nothing changed; say nothing */
    }

    state->init = true;
    state->up = now.up;

    /* The stack is told on every transition, including the first.
     *
     * Not just ourselves: nx_interface_link_up is what the IP thread consults before handing
     * anything to the driver. Setting it once at NX_LINK_INITIALIZE and leaving it produced a
     * board that reported "ETH LINK: UP", received frames happily, and transmitted absolutely
     * nothing. */
    action.publish_to_stack = true;

    if (now.up) {
        state->speed = now.fast_ethernet ? 100U : 10U;
        state->duplex = now.full_duplex;
        action.program_mac = true;
        action.announce_up = true;
    } else if (!first_poll) {
        /* Genuine transitions after the first poll are always reported. The first poll is not
           a transition -- auto-negotiation has not finished -- and announcing it makes a normal
           boot look like a cable fault. */
        action.announce_down = true;
    }

    return action;
}
