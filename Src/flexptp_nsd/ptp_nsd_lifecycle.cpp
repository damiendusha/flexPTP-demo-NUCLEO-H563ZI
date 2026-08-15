#include "ptp_nsd_lifecycle.h"

/* C++ per the rules in Src/cpp_support.cpp: no allocation, no exceptions, extern "C" at the
   boundary. The storage is the caller's struct, so this adds nothing to any pool. */

namespace {

/* Destroy whatever exists, in the reverse of the order it was built.
 *
 * Per object rather than per configuration, which is the whole point: a configure that failed
 * halfway has created SOME of them, and "some" is exactly the state the old code could not
 * express. Each flag is cleared as its object goes, so this is safe to call twice. */
void destroy_all(PtpNsdLifecycle *lc) {
    const PtpNsdOps *ops = lc->ops;

    if (lc->l2_registered) {
        ops->l2_unregister(ops->ctx);
        lc->l2_registered = false;
    }
    if (lc->event_created) {
        ops->socket_delete(ops->ctx, PTP_NSD_SOCK_EVENT);
        lc->event_created = false;
    }
    if (lc->general_created) {
        ops->socket_delete(ops->ctx, PTP_NSD_SOCK_GENERAL);
        lc->general_created = false;
    }
}

bool build_ipv4(PtpNsdLifecycle *lc) {
    const PtpNsdOps *ops = lc->ops;

    if (!ops->socket_create(ops->ctx, PTP_NSD_SOCK_EVENT)) {
        return false;
    }
    lc->event_created = true;

    if (!ops->socket_create(ops->ctx, PTP_NSD_SOCK_GENERAL)) {
        return false;
    }
    lc->general_created = true;

    /* Bind after both creates, so a bind failure has both flags set and the cleanup deletes
       both. Marking each socket created the instant it exists -- rather than once the pair is
       complete -- is the fix for NX-02, and it is the only thing that makes the cleanup exact
       rather than approximate. */
    if (!ops->socket_bind(ops->ctx, PTP_NSD_SOCK_EVENT, ptp_endpoint_port(true))) {
        return false;
    }
    if (!ops->socket_bind(ops->ctx, PTP_NSD_SOCK_GENERAL, ptp_endpoint_port(false))) {
        return false;
    }

    ops->socket_notify(ops->ctx, PTP_NSD_SOCK_EVENT);
    ops->socket_notify(ops->ctx, PTP_NSD_SOCK_GENERAL);
    return true;
}

bool build_802_3(PtpNsdLifecycle *lc) {
    const PtpNsdOps *ops = lc->ops;

    if (!ops->l2_register(ops->ctx)) {
        return false;
    }
    lc->l2_registered = true;
    return true;
}

}  // namespace

extern "C" {

void ptp_nsd_lifecycle_init(PtpNsdLifecycle *lc, const PtpNsdOps *ops) {
    lc->ops = ops;
    lc->event_created = false;
    lc->general_created = false;
    lc->l2_registered = false;
    lc->transport = PTP_EP_TRANSPORT_NONE;
    lc->delay = PTP_EP_DELAY_E2E;
}

void ptp_nsd_lifecycle_teardown(PtpNsdLifecycle *lc) {
    /* Leave the group before the sockets go: the multicast membership is expressed through the
       objects being destroyed, so the order is not arbitrary. */
    if (lc->transport != PTP_EP_TRANSPORT_NONE) {
        lc->ops->multicast(lc->ops->ctx, false);
    }

    destroy_all(lc);
    lc->transport = PTP_EP_TRANSPORT_NONE;
}

bool ptp_nsd_lifecycle_configure(PtpNsdLifecycle *lc, PtpEndpointTransport tp,
                                 PtpEndpointDelay dm) {
    ptp_nsd_lifecycle_teardown(lc);

    if (tp == PTP_EP_TRANSPORT_NONE) {
        return false;
    }

    /* The delay mechanism is published before building, because the multicast group depends on
       it and so does the L2 destination. The TRANSPORT deliberately is not: it stays NONE until
       everything has succeeded, so a caller that consults it -- the transmit path does -- can
       never be told a transport is live while its sockets are half-built or already deleted.
       That was the second half of NX-02. */
    lc->delay = dm;

    const bool built = (tp == PTP_EP_TRANSPORT_IPV4) ? build_ipv4(lc) : build_802_3(lc);
    if (!built) {
        destroy_all(lc);
        return false;
    }

    lc->transport = tp;
    lc->ops->multicast(lc->ops->ctx, true);
    return true;
}

bool ptp_nsd_lifecycle_active(const PtpNsdLifecycle *lc) {
    return lc->event_created || lc->general_created || lc->l2_registered;
}

unsigned ptp_nsd_lifecycle_outstanding(const PtpNsdLifecycle *lc) {
    unsigned n = 0;
    if (lc->event_created) { n++; }
    if (lc->general_created) { n++; }
    if (lc->l2_registered) { n++; }
    return n;
}

}  // extern "C"
