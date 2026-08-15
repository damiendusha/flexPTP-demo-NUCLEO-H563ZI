/* flexPTP network stack driver for NetX Duo.
 *
 * Supplied through flexPTP's documented extension point (FLEXPTP_NSD_SRC in
 * Modules/CMakeLists.txt), so the submodule stays unpatched -- the same arrangement as the
 * H563 hardware port. Replaces nsd_stub.c, and stands in for the bundled nsd_lwip.c.
 *
 * Both transports are implemented: UDP/IPv4 (the `default` and `defp2p` profiles) and raw
 * 802.3 (`gPTP`). The 802.3 half is where NetX Duo's link layer earns its keep over lwIP --
 * nx_link_packet_receive_callback_add() delivers PTP frames WITH their hardware timestamp as a
 * callback argument, where lwIP needed LWIP_HOOK_UNKNOWN_ETH_PROTOCOL plus a hand-rolled
 * EtherType and destination-MAC parse in nsd_lwip.c, and had to smuggle the timestamp through
 * two injected pbuf fields.
 */

#include "flexptp/network_stack_driver.h"

#include <string.h>

#include "nx_api.h"
#include "nx_link.h"

#include "Drivers/EthDrv/eth_drv_netxduo.h"
#include "ethernet/ethernet_netxduo.h"
#include "ptp_endpoint.h"
#include "ptp_ts_codec.h"
#include "ptp_tx_timestamp.h"
#include "standard_output/standard_output.h"

#include "flexptp/ptp_defs.h"
#include "flexptp/task_ptp.h"

/* --- State ------------------------------------------------------------------------------ */

static NX_UDP_SOCKET s_event_socket;   /* port 319: Sync, Delay_Req, PDelay_Req/Resp */
static NX_UDP_SOCKET s_general_socket; /* port 320: Follow_Up, Delay_Resp, Announce   */
static bool s_sockets_open;

/* The 802.3 receive registration. NetX Duo keeps this queue structure in a list on the
   interface, so it must outlive the call -- a stack local would corrupt that list. */
static NX_LINK_RECEIVE_QUEUE s_l2_queue;
static bool s_l2_registered;

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
    /* The headroom the driver guarantees: it copies the frame at data_start + 2, so
       prepend_ptr - data_start is 2 + 14 = 16 by the time UDP has stripped its header and
       more besides. Passing the real distance rather than a constant means the codec's
       precondition is checked against what actually happened, not what was intended. */
    const uint32_t headroom =
        (uint32_t)(packet->nx_packet_prepend_ptr - packet->nx_packet_data_start);

    PtpTsCodecResult ts;
    if (ptp_ts_codec_extract(packet->nx_packet_data_start, headroom, &ts)) {
        *ts_s = ts.sec;
        *ts_ns = ts.nanosec;
    } else {
        *ts_s = 0;
        *ts_ns = 0;
    }
}

/* flexPTP's enums to the endpoint unit's. One call site, so the mapping stays visible. */
static void resolve_endpoint(PtpEndpoint *out) {
    PtpEndpointTransport tp = PTP_EP_TRANSPORT_NONE;
    if (s_tp == PTP_TP_IPv4) {
        tp = PTP_EP_TRANSPORT_IPV4;
    } else if (s_tp == PTP_TP_802_3) {
        tp = PTP_EP_TRANSPORT_802_3;
    }
    ptp_endpoint_resolve(tp, (s_dm == PTP_DM_E2E) ? PTP_EP_DELAY_E2E : PTP_EP_DELAY_P2P, out);
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

/* --- Receive, 802.3 -------------------------------------------------------------------------- */

/* Registered for EtherType 0x88F7. NetX Duo hands us the frame AND its hardware timestamp;
 * contrast nsd_lwip.c, which had to hook unknown ethertypes, re-read the EtherType out of the
 * frame by hand, compare the destination MAC itself, and pull the timestamp from a pbuf field
 * this project had injected.
 *
 * Returning NX_SUCCESS means "consumed" -- NetX Duo then leaves the packet to us, so it must be
 * released here. Returning anything else lets it fall through to the next registered queue.
 *
 * Runs on the IP thread, so ptp_receive_enqueue()'s blocking enqueue is legal.
 */
static UINT ptp_l2_receive(NX_IP *ip_ptr, UINT interface_index, NX_PACKET *packet_ptr,
                           ULONG physical_address_msw, ULONG physical_address_lsw,
                           UINT packet_type, UINT header_size, VOID *context,
                           NX_LINK_TIME *time_ptr) {
    (void)ip_ptr;
    (void)interface_index;
    (void)physical_address_msw;
    (void)physical_address_lsw;
    (void)packet_type;
    (void)context;

    s_rx_notify++;
    s_rx_packets++;

    /* The timestamp comes as an argument here, rather than being dug out of the packet buffer
       as it must be on the UDP path. NULL if the MAC did not stamp this frame. */
    uint32_t ts_s = 0, ts_ns = 0;
    if (time_ptr != NX_NULL) {
        ts_ns = (uint32_t)time_ptr->nano_second;
        ts_s = (uint32_t)time_ptr->second_low;
    }

    /* header_size is the Ethernet header NetX Duo has already parsed but NOT stripped -- it
       leaves prepend_ptr at the start of the frame for a raw receiver. flexPTP wants the PTP
       message, so skip it. */
    UCHAR *ptp = packet_ptr->nx_packet_prepend_ptr + header_size;
    ULONG length = packet_ptr->nx_packet_length - header_size;

    if (packet_ptr->nx_packet_next == NX_NULL) {
        s_rx_type[ptp[0] & 0x0FU]++;
        s_rx_last_len = (uint32_t)length;
        memcpy(s_rx_last, ptp, (length < sizeof(s_rx_last)) ? length : sizeof(s_rx_last));
        s_rx_enqueued++;
        ptp_receive_enqueue(ptp, (uint32_t)length, ts_s, ts_ns, PTP_TP_802_3);
    }

    nx_packet_release(packet_ptr);
    return NX_SUCCESS;
}

/* The two L2 groups, as NetX Duo's msw/lsw pair. 01:1B:19:00:00:00 for E2E, 01:80:C2:00:00:0E
   for peer delay -- the latter being the address ordinary bridges must not forward, which is
   why gPTP needs a direct cable (see docs/ptp_profiles.md). */
static void l2_group_address(ULONG *msw, ULONG *lsw) {
    PtpEndpoint ep;
    resolve_endpoint(&ep);
    *msw = ((ULONG)ep.mac[0] << 8) | ep.mac[1];
    *lsw = ((ULONG)ep.mac[2] << 24) | ((ULONG)ep.mac[3] << 16) |
           ((ULONG)ep.mac[4] << 8) | ep.mac[5];
}

/* --- Driver interface ----------------------------------------------------------------------- */

static void close_sockets(void) {
    NX_IP *ip = ethernet_ip();
    if (s_l2_registered && (ip != NX_NULL)) {
        (void)nx_link_packet_receive_callback_remove(ip, 0, &s_l2_queue);
        s_l2_registered = false;
    }

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
    } else if (tp == PTP_TP_802_3) {
        if (nx_link_packet_receive_callback_add(ip, 0, &s_l2_queue, NX_LINK_ETHERNET_PTP,
                                                ptp_l2_receive, NX_NULL) != NX_SUCCESS) {
            MSG("PTP: could not register the 802.3 receive callback\n");
            return;
        }
        s_l2_registered = true;
    }

    s_tp = tp;
    s_dm = dm;

    ptp_nsd_igmp_join_leave(true);
}

void ptp_nsd_igmp_join_leave(bool join) {
    NX_IP *ip = ethernet_ip();

    if (ip == NX_NULL) {
        return;
    }

    /* 802.3 has no IGMP -- the grouping is a link-layer multicast MAC. nx_link_multicast_join()
       is the equivalent, and the lwIP driver had no counterpart at all: it relied on the MAC
       being in pass-all-multicast, which works but filters nothing. */
    if (s_tp == PTP_TP_802_3) {
        ULONG msw = 0, lsw = 0;
        l2_group_address(&msw, &lsw);
        if (join) {
            if (nx_link_multicast_join(ip, 0, msw, lsw) != NX_SUCCESS) {
                MSG("PTP: L2 multicast join failed\n");
            }
        } else {
            (void)nx_link_multicast_leave(ip, 0, msw, lsw);
        }
        return;
    }

    if (s_tp != PTP_TP_IPv4) {
        return;
    }

    /* One group, chosen by delay mechanism, exactly as nsd_lwip.c did: 224.0.1.129 for E2E,
       224.0.0.107 for peer delay. PTP_IGMP_* are the ip4_addr structs from
       Inc/flexptp_options.h; NetX Duo wants a host-order ULONG. */
    /* The endpoint unit returns host byte order, which is what NetX Duo's IP_ADDRESS() form
       is -- so no swap here, unlike the flexPTP constants it replaced (those are stored
       little-endian, 0x810100E0 for 224.0.1.129, matching flexPTP's own lwIP branch). */
    PtpEndpoint ep;
    resolve_endpoint(&ep);
    ULONG group = ep.ipv4_group;

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

    if ((pMsg == NULL) || (ip == NX_NULL) || (pool == NX_NULL)) {
        return;
    }
    if ((s_tp != PTP_TP_IPv4) && (s_tp != PTP_TP_802_3)) {
        return;
    }

    /* NX_UDP_PACKET reserves room for the UDP, IP and Ethernet headers; NX_PHYSICAL_HEADER
       reserves only the Ethernet one, which is what a raw 802.3 frame needs. Using the UDP
       offset for an L2 frame would still work but would waste 28 bytes of every packet and
       obscure where the header actually starts. */
    NX_PACKET *packet = NX_NULL;
    UINT alloc_type = (s_tp == PTP_TP_IPv4) ? NX_UDP_PACKET : NX_PHYSICAL_HEADER;
    if (nx_packet_allocate(pool, &packet, alloc_type, NX_NO_WAIT) != NX_SUCCESS) {
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

    if (s_tp == PTP_TP_802_3) {
        /* Raw 802.3. nx_link_ethernet_packet_send() builds the Ethernet header itself and
           issues NX_LINK_RAW_PACKET_SEND, which the driver passes through untouched -- so the
           capability flag set above still reaches it and the timestamp is still requested.
           This is the whole gPTP path, and it is four lines. */
        ULONG msw = 0, lsw = 0;
        l2_group_address(&msw, &lsw);
        if (nx_link_ethernet_packet_send(ip, 0, packet, msw, lsw,
                                         NX_LINK_ETHERNET_PTP) != NX_SUCCESS) {
            eth_drv_cancel_tx_timestamp(packet);
            nx_packet_release(packet);
        }
        return;
    }

    PtpEndpoint ep;
    resolve_endpoint(&ep);

    NXD_ADDRESS dest;
    dest.nxd_ip_version = NX_IP_VERSION_V4;
    dest.nxd_ip_address.v4 = ep.ipv4_group;

    /* Event messages are timestamped and go to port 319; general messages to 320. flexPTP
       classifies them for us in tx_mc. */
    NX_UDP_SOCKET *socket =
        (pMsg->tx_mc == PTP_MC_EVENT) ? &s_event_socket : &s_general_socket;
    UINT port = ptp_endpoint_port(pMsg->tx_mc == PTP_MC_EVENT);

    if (nxd_udp_socket_send(socket, packet, &dest, port) != NX_SUCCESS) {
        /* The driver must be told to forget the pending record, or the next packet allocated
           at the same address would inherit this uid -- the pointer-reuse hazard that made
           this a side table rather than a single variable. */
        eth_drv_cancel_tx_timestamp(packet);
        nx_packet_release(packet);
    }
}
