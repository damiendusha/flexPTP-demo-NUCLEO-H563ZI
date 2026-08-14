/* flexPTP network stack driver for NetX Duo.
 *
 * Supplied through flexPTP's documented extension point (FLEXPTP_NSD_SRC in
 * Modules/CMakeLists.txt), so the submodule stays unpatched -- the same arrangement as the
 * H563 hardware port. Replaces nsd_stub.c, and stands in for the bundled nsd_lwip.c.
 *
 * S6 implements the UDP/IPv4 transport, which is the `default` profile and the one the board
 * boots into. PTP over raw 802.3 -- the gPTP profile -- arrives in S7 and will use
 * nx_link_ethernet_packet_send() and nx_link_packet_receive_callback_add(), which is where
 * NetX Duo's link layer earns its keep over lwIP's unknown-ethertype hook.
 */

#include "flexptp/network_stack_driver.h"

#include <string.h>

#include "nx_api.h"

#include "Drivers/EthDrv/eth_drv_netxduo.h"
#include "ethernet/ethernet_netxduo.h"
#include "ptp_tx_timestamp.h"
#include "standard_output/standard_output.h"

#include "flexptp/ptp_defs.h"
#include "flexptp/task_ptp.h"

/* --- State ------------------------------------------------------------------------------ */

static NX_UDP_SOCKET s_event_socket;   /* port 319: Sync, Delay_Req, PDelay_Req/Resp */
static NX_UDP_SOCKET s_general_socket; /* port 320: Follow_Up, Delay_Resp, Announce   */
static bool s_sockets_open;

static PtpTransportType s_tp = -1;
static PtpDelayMechanism s_dm = -1;

/* Bring-up counters. The question "is the callback firing at all?" has three different
   answers with three different causes, and none is visible from the console otherwise. */
static uint32_t s_rx_notify, s_rx_packets, s_rx_enqueued, s_tx_requested;
static uint8_t s_rx_last[16];
static uint32_t s_rx_last_len;
static uint32_t s_rx_type[16];   /* histogram by PTP messageType (low nibble of byte 0) */

void ptp_nsd_rx_types(uint32_t *out16) {
    memcpy(out16, s_rx_type, sizeof(s_rx_type));
}

void ptp_nsd_last_rx(uint8_t *out, uint32_t *len) {
    memcpy(out, s_rx_last, sizeof(s_rx_last));
    *len = s_rx_last_len;
}

void ptp_nsd_stats(uint32_t *notify, uint32_t *packets, uint32_t *enqueued, uint32_t *tx) {
    if (notify) { *notify = s_rx_notify; }
    if (packets) { *packets = s_rx_packets; }
    if (enqueued) { *enqueued = s_rx_enqueued; }
    if (tx) { *tx = s_tx_requested; }
}

/* --- Timestamp extraction --------------------------------------------------------------- */

/* NetX Duo carries a receive timestamp in the first three ULONGs of the packet buffer, written
 * over the Ethernet header it has already consumed (nx_link.c):
 *
 *     ((ULONG *)packet -> nx_packet_data_start)[0] = time -> nano_second;
 *     ((ULONG *)packet -> nx_packet_data_start)[1] = time -> second_low;
 *     ((ULONG *)packet -> nx_packet_data_start)[2] = time -> second_high;
 *
 * It is an undocumented layout convention rather than a struct field, which is the same class
 * of unversioned contract as the lwIP tx_cb signature that drifted in F48. The driver is what
 * guarantees the 12 bytes exist -- eth_drv_netxduo.c copies the frame at data_start + 2, so
 * the Ethernet header occupies +2..+15 and the IP header starts at +16.
 *
 * second_high is ignored: it is the top 32 bits of a 48-bit seconds field, and the H563's
 * timestamp unit has a 32-bit seconds register, so it is always zero here. flexPTP's
 * TimestampI is 32-bit seconds too.
 */
static void extract_rx_timestamp(NX_PACKET *packet, uint32_t *ts_s, uint32_t *ts_ns) {
    const ULONG *stamp = (const ULONG *)(const void *)packet->nx_packet_data_start;
    *ts_ns = (uint32_t)stamp[0];
    *ts_s = (uint32_t)stamp[1];
}

/* --- Receive ------------------------------------------------------------------------------ */

/* Called by NetX Duo on the IP thread when a datagram lands on either socket.
 *
 * Thread context, so flexPTP's ptp_receive_enqueue() -- which enqueues with osWaitForever in
 * the CMSIS-RTOS2 binding -- is legal here. That is only true of the RECEIVE side; the
 * transmit-timestamp callback runs in an ISR and needs ptp_tx_timestamp.c.
 */
static void ptp_udp_receive(NX_UDP_SOCKET *socket) {
    s_rx_notify++;
    for (;;) {
        NX_PACKET *packet = NX_NULL;
        if (nx_udp_socket_receive(socket, &packet, NX_NO_WAIT) != NX_SUCCESS) {
            return;
        }

        s_rx_packets++;

        uint32_t ts_s = 0, ts_ns = 0;
        extract_rx_timestamp(packet, &ts_s, &ts_ns);

        /* Contiguous by construction: a PTP message is at most MAX_PTP_MSG_SIZE and the pool
           payload is 1528, so it never chains. Checked rather than assumed, because a chained
           packet would otherwise hand flexPTP the first fragment as if it were the whole
           message. */
        ULONG length = 0;
        nx_packet_length_get(packet, &length);

        if (packet->nx_packet_next == NX_NULL) {
            s_rx_enqueued++;
            s_rx_type[packet->nx_packet_prepend_ptr[0] & 0x0FU]++;
            s_rx_last_len = (uint32_t)length;
            memcpy(s_rx_last, packet->nx_packet_prepend_ptr,
                   (length < sizeof(s_rx_last)) ? length : sizeof(s_rx_last));
            ptp_receive_enqueue(packet->nx_packet_prepend_ptr, (uint32_t)length,
                                ts_s, ts_ns, PTP_TP_IPv4);
        } else {
            MSG("PTP: chained receive packet (%u bytes) discarded\n", (unsigned)length);
        }

        nx_packet_release(packet);
    }
}

/* --- Driver interface ----------------------------------------------------------------------- */

static void close_sockets(void) {
    if (!s_sockets_open) {
        return;
    }

    nx_udp_socket_unbind(&s_event_socket);
    nx_udp_socket_delete(&s_event_socket);
    nx_udp_socket_unbind(&s_general_socket);
    nx_udp_socket_delete(&s_general_socket);
    s_sockets_open = false;
}

void ptp_nsd_init(PtpTransportType tp, PtpDelayMechanism dm) {
    NX_IP *ip = ethernet_ip();

    ptp_nsd_igmp_join_leave(false);
    close_sockets();

    /* Either parameter as -1 means "shut down" -- flexPTP calls it that way from
       unreg_task_ptp(). */
    if ((tp == -1) || (dm == -1) || (ip == NX_NULL)) {
        s_tp = -1;
        s_dm = -1;
        return;
    }

    /* The transmit-timestamp deferral must exist before the first frame goes out, or the
       ISR callback finds a NULL queue and every t3 is lost -- which is the very failure this
       port exists to avoid. Idempotent, so re-initialising the NSD on a profile change is
       harmless. */
    if (!ptp_tx_timestamp_init()) {
        MSG("PTP: transmit-timestamp deferral unavailable; t3 will be missing\n");
    }

    if (tp == PTP_TP_IPv4) {
        /* NX_IP_NORMAL type-of-service and a 32-deep receive queue, matching the depth lwIP's
           UDP PCB effectively had. NX_FRAGMENT_OKAY is irrelevant here -- PTP messages are far
           below the MTU -- but NetX Duo requires the argument. */
        if ((nx_udp_socket_create(ip, &s_event_socket, "ptp event", NX_IP_NORMAL,
                                  NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE, 32) != NX_SUCCESS) ||
            (nx_udp_socket_create(ip, &s_general_socket, "ptp general", NX_IP_NORMAL,
                                  NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE, 32) != NX_SUCCESS)) {
            MSG("PTP: could not create the UDP sockets\n");
            return;
        }

        if ((nx_udp_socket_bind(&s_event_socket, PTP_PORT_EVENT, NX_WAIT_FOREVER) != NX_SUCCESS) ||
            (nx_udp_socket_bind(&s_general_socket, PTP_PORT_GENERAL, NX_WAIT_FOREVER) != NX_SUCCESS)) {
            MSG("PTP: could not bind the UDP sockets\n");
            return;
        }

        nx_udp_socket_receive_notify(&s_event_socket, ptp_udp_receive);
        nx_udp_socket_receive_notify(&s_general_socket, ptp_udp_receive);
        s_sockets_open = true;
    }

    s_tp = tp;
    s_dm = dm;

    ptp_nsd_igmp_join_leave(true);
}

void ptp_nsd_igmp_join_leave(bool join) {
    NX_IP *ip = ethernet_ip();

    if ((s_tp != PTP_TP_IPv4) || (ip == NX_NULL)) {
        return;
    }

    /* One group, chosen by delay mechanism, exactly as nsd_lwip.c did: 224.0.1.129 for E2E,
       224.0.0.107 for peer delay. PTP_IGMP_* are the ip4_addr structs from
       Inc/flexptp_options.h; NetX Duo wants a host-order ULONG. */
    ULONG group = (s_dm == PTP_DM_E2E) ? PTP_IGMP_PRIMARY.addr : PTP_IGMP_PEER_DELAY.addr;

    /* The constants are stored little-endian (0x810100E0 == 224.0.1.129), matching what
       flexPTP's own lwIP branch uses. NetX Duo's IP_ADDRESS() form is big-endian, so swap. */
    group = __builtin_bswap32(group);

    if (join) {
        if (nx_igmp_multicast_join(ip, group) != NX_SUCCESS) {
            MSG("PTP: IGMP join failed\n");
        }
    } else {
        (void)nx_igmp_multicast_leave(ip, group);
    }
}

void ptp_nsd_get_interface_address(uint8_t *hwa) {
    NX_IP *ip = ethernet_ip();
    ULONG msw = 0, lsw = 0;

    if ((ip == NX_NULL) ||
        (nx_ip_interface_physical_address_get(ip, 0, &msw, &lsw) != NX_SUCCESS)) {
        memset(hwa, 0, 6);
        return;
    }

    hwa[0] = (uint8_t)(msw >> 8);
    hwa[1] = (uint8_t)(msw);
    hwa[2] = (uint8_t)(lsw >> 24);
    hwa[3] = (uint8_t)(lsw >> 16);
    hwa[4] = (uint8_t)(lsw >> 8);
    hwa[5] = (uint8_t)(lsw);
}

void ptp_nsd_transmit_msg(RawPtpMessage *pMsg, uint32_t uid) {
    NX_IP *ip = ethernet_ip();
    NX_PACKET_POOL *pool = ethernet_packet_pool();

    if ((pMsg == NULL) || (ip == NX_NULL) || (pool == NX_NULL) || (s_tp != PTP_TP_IPv4)) {
        return;
    }

    NX_PACKET *packet = NX_NULL;
    if (nx_packet_allocate(pool, &packet, NX_UDP_PACKET, NX_NO_WAIT) != NX_SUCCESS) {
        /* No timestamp callback will ever come for this uid. flexPTP ages the message out of
           its buffer on TTL, so the slot is not leaked -- but the Sync cycle is lost, which is
           worth saying rather than dropping silently. */
        MSG("PTP: no packet for transmit (uid %u)\n", (unsigned)uid);
        return;
    }

    if (nx_packet_data_append(packet, pMsg->data, pMsg->size, pool, NX_NO_WAIT) != NX_SUCCESS) {
        nx_packet_release(packet);
        return;
    }

    /* Ask the MAC for a transmit timestamp, and tell the driver which message it belongs to.
     *
     * The flag rides in the packet itself; nx_ip_packet_send() ORs into that field rather than
     * overwriting it, so it survives down to the driver. The uid cannot ride there -- there is
     * no field for it -- so it goes in a side channel the driver reads back, keyed on this
     * packet. See eth_drv_netxduo.c's pending-timestamp record. */
    packet->nx_packet_interface_capability_flag |= NX_INTERFACE_CAPABILITY_PTP_TIMESTAMP;
    eth_drv_request_tx_timestamp(packet, uid);
    s_tx_requested++;

    NXD_ADDRESS dest;
    dest.nxd_ip_version = NX_IP_VERSION_V4;
    dest.nxd_ip_address.v4 =
        __builtin_bswap32((s_dm == PTP_DM_E2E) ? PTP_IGMP_PRIMARY.addr
                                               : PTP_IGMP_PEER_DELAY.addr);

    /* Event messages are timestamped and go to port 319; general messages to 320. flexPTP
       classifies them for us in tx_mc. */
    NX_UDP_SOCKET *socket =
        (pMsg->tx_mc == PTP_MC_EVENT) ? &s_event_socket : &s_general_socket;
    UINT port = (pMsg->tx_mc == PTP_MC_EVENT) ? PTP_PORT_EVENT : PTP_PORT_GENERAL;

    if (nxd_udp_socket_send(socket, packet, &dest, port) != NX_SUCCESS) {
        /* The driver must be told to forget the pending record, or the next packet allocated
           at the same address would inherit this uid -- the pointer-reuse hazard that made
           this a side table rather than a single variable. */
        eth_drv_cancel_tx_timestamp(packet);
        nx_packet_release(packet);
    }
}
