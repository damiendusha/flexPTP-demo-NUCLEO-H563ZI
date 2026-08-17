/* The flexPTP network-stack driver's lifecycle, separated from the stack it drives.
 *
 * WHY THIS EXISTS
 *
 * ptp_nsd_init() is a state machine -- tear down what is configured, build what is asked for,
 * commit -- and it got the failure paths wrong (finding NX-02). A socket created but not bound
 * was left behind, because the "are we open?" flag was only set on the success path, so nothing
 * ever deleted it. The next configure call then created it again, which NetX Duo answers with
 * NX_PTR_ERROR in Debug and, in Release -- where NX_DISABLE_ERROR_CHECKING removes the guard --
 * by re-linking the socket into the IP instance's created list and corrupting it.
 *
 * The reason that survived review is that it was untestable: the logic was interleaved with
 * NX_UDP_SOCKET, NX_IP and nx_link, so exercising it needed a running network stack.
 *
 * So the decisions live here and the stack calls live behind PtpNsdOps. On the target that is
 * NetX Duo (nsd_netxduo.c); in test/test_ptp_nsd_lifecycle.cpp it is a fake that counts. The
 * properties worth asserting are then plain: every socket created is eventually deleted, a
 * failure leaves nothing behind, and a failed configure leaves the transport unset so the
 * transmit path cannot act on sockets that are gone.
 *
 * C++ per the rules in Src/cpp_support.cpp: no allocation, no exceptions, extern "C" at the
 * boundary. The storage is the caller's struct.
 */
#ifndef PTP_NSD_LIFECYCLE_H
#define PTP_NSD_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#include "ptp_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Which of the two PTP sockets an operation refers to. */
typedef enum {
    PTP_NSD_SOCK_EVENT = 0,   /**< port 319: Sync, Delay_Req, PDelay_Req/Resp */
    PTP_NSD_SOCK_GENERAL = 1  /**< port 320: Follow_Up, Delay_Resp, Announce  */
} PtpNsdSocket;

/**
 * The network stack, as this state machine needs to see it.
 *
 * Deliberately small and deliberately boolean: everything that can fail returns whether it
 * did, and everything that cannot fail returns nothing. NetX Duo's status codes are the
 * caller's business, not this unit's.
 *
 * `socket_delete` must tolerate being called on a socket that was created but never bound --
 * that is precisely the state NX-02 was about, and the one the cleanup has to be able to undo.
 */
typedef struct {
    bool (*socket_create)(void *ctx, PtpNsdSocket which);
    bool (*socket_bind)(void *ctx, PtpNsdSocket which, uint16_t port);
    void (*socket_notify)(void *ctx, PtpNsdSocket which);
    void (*socket_delete)(void *ctx, PtpNsdSocket which);
    bool (*l2_register)(void *ctx);
    void (*l2_unregister)(void *ctx);
    void (*multicast)(void *ctx, bool join);
    void *ctx;
} PtpNsdOps;

/** Caller-owned state. Zero-initialise, then ptp_nsd_lifecycle_init(). */
typedef struct {
    const PtpNsdOps *ops;
    bool event_created;
    bool general_created;
    bool l2_registered;
    PtpEndpointTransport transport;  /**< PTP_EP_TRANSPORT_NONE when not configured */
    PtpEndpointDelay delay;
} PtpNsdLifecycle;

void ptp_nsd_lifecycle_init(PtpNsdLifecycle *lc, const PtpNsdOps *ops);

/**
 * Move to the requested configuration, or to no configuration at all.
 *
 * Transactional. On success the requested transport is live and `transport` reports it; on any
 * failure everything this call created has been destroyed and `transport` is
 * PTP_EP_TRANSPORT_NONE -- never the previous value, because the previous transport's sockets
 * are already gone by then and a transmit path that believed in them would send on a deleted
 * socket.
 *
 * PTP_EP_TRANSPORT_NONE is the shutdown request; flexPTP issues it as ptp_nsd_init(-1, -1).
 *
 * @return true if the requested transport is now live. Shutting down returns false, because
 *         nothing is live afterwards -- callers that care should test the request, not this.
 */
bool ptp_nsd_lifecycle_configure(PtpNsdLifecycle *lc, PtpEndpointTransport tp,
                                 PtpEndpointDelay dm);

/** Destroy everything and leave the transport unset. Idempotent. */
void ptp_nsd_lifecycle_teardown(PtpNsdLifecycle *lc);

/** Sockets or an L2 registration currently exist. */
bool ptp_nsd_lifecycle_active(const PtpNsdLifecycle *lc);

/** How many stack objects are outstanding: 0, 1 or 2 sockets, or 1 L2 registration. */
unsigned ptp_nsd_lifecycle_outstanding(const PtpNsdLifecycle *lc);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PTP_NSD_LIFECYCLE_H
