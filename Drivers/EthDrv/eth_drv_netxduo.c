/* NetX Duo Ethernet driver for the STM32H563, built on this project's own MAC driver.
 *
 * Replaces eth_drv_lwip.c. It is written against Drivers/EthDrv/mac_drv.c -- which is
 * UNCHANGED by this port -- rather than adapted from ST's nx_stm32_eth_driver.c, because that
 * one is HAL_ETH-based and the HAL is precisely the abstraction this project bypassed to reach
 * the DMA descriptors that hardware timestamping needs.
 *
 * Built up across three stages so that each is provable on hardware:
 *
 *   S5a  INITIALIZE / ENABLE / DISABLE / GET_STATUS      link up/down on the console
 *   S5b  receive  -> nx_link_ethernet_packet_received()  host `arping` gets replies
 *   S5c  transmit, multicast join/leave, deferred        `ping` 0% loss at 1400 B
 *
 * S5b and S5c landed together: the natural first proof of receive is an ARP reply, and a
 * reply needs the transmit path, so the two are not separately observable on hardware.
 */

#include "eth_drv_netxduo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmsis_os2.h>
#include <stm32h5xx_hal.h>

#include "nx_api.h"
/* NX_LINK_TIME, nx_link_ethernet_packet_received() and the EtherType constants. Not pulled in
   by nx_api.h -- the link layer is a separate header, and it is the one that carries this
   port's hardware timestamps. */
#include "nx_link.h"

#include "eth_framer.h"
#include "mac_drv.h"
#include "phy_drv/phy_common.h"
#include "flexptp_nsd/ptp_tx_registry.h"
#include "flexptp_nsd/ptp_tx_timestamp.h"
#include "standard_output/standard_output.h"

/* --- Descriptors and buffers ----------------------------------------------------------- */

/* Unchanged from the lwIP driver, deliberately: same ring depths, same buffer size, same
 * SRAM3 placement. The receive ring depth is a hardware property of how much the DMA can
 * absorb before it stalls, not a property of the stack above it, and keeping the numbers
 * identical is what makes the S5c ping comparison against the S0 baseline meaningful. */
#define ETH_BUFFER_SIZE (1528UL)

uint8_t ETHBuffer[(ETH_RX_DESC_CNT + ETH_TX_DESC_CNT) * ETH_BUFFER_SIZE]
    __attribute__((section(".ETHBufferSection")));

typedef struct {
    ETHHW_State ETHState;
    ETHHW_DescFull DMARxDscrTab[ETH_RX_DESC_CNT];
    ETHHW_DescFull DMATxDscrTab[ETH_TX_DESC_CNT];
} ETHStateAndDescLayout;

static ETHStateAndDescLayout ETHStateAndDesc __attribute__((section(".ETHStateAndDecripSection")));

/* ETHHW_GetState() recovers the driver state as ((ETHHW_State *)eth->DMACRDLAR) - 1, i.e. it
 * assumes the state block sits immediately below the RX descriptor ring the DMA was pointed
 * at. Carried over from eth_drv_lwip.c, where it was added because reordering these two
 * members -- or any padding appearing between them -- produced a driver reading its state
 * from the middle of a descriptor, silently and at runtime. The same contract now also has a
 * host test (test/test_host_harness.cpp). */
_Static_assert(offsetof(ETHStateAndDescLayout, DMARxDscrTab) == sizeof(ETHHW_State),
               "ETHHW_State must immediately precede the RX descriptor ring, with no padding: "
               "ETHHW_GetState() derives the state pointer by subtracting one from DMACRDLAR.");

/* --- Driver state ------------------------------------------------------------------------ */

/* Carried over from the deleted eth_drv_lwip.h, where lwIP kept it behind netif->state. NetX
   Duo has no equivalent per-interface user pointer that the console code needs, so it is
   simply a file-static here -- there is exactly one Ethernet interface on this part. */
typedef struct {
    bool init;   /* has the link state ever been read? */
    bool up;
    uint16_t speed;  /* Mbps */
    bool duplex;     /* true = full */
} LinkState;

static NX_IP *drv_ip;
static NX_INTERFACE *drv_interface;
static bool drv_enabled;
static LinkState drv_link = {false, false, 0, false};

/* Defined with eth_drv_report() at the end of the file. */
static ULONG drv_rx_frames, drv_rx_dropped_nopacket, drv_rx_dropped_size;
/* Written from the ETH ISR, read from a thread. See ETHHW_ReadCallback(). */
static volatile ULONG drv_rx_dropped_queue;
static ULONG drv_tx_frames, drv_tx_dropped_linkdown, drv_tx_dropped_mac;
static ULONG drv_tx_last_len, drv_tx_last_cmd;
static ULONG drv_tx_timestamped;
static uint8_t drv_tx_last_head[34];

/* --- Receive ------------------------------------------------------------------------------- */

/* One event per RX descriptor is the hard ceiling, and the depth is tied to the ring rather
 * than chosen: ETHHW_ProcessRx() marks a descriptor inFlight before queueing it and does not
 * revisit it until rx_thread restores it, so one descriptor can never contribute two entries.
 * Carried over from the lwIP driver, where it had been 32 -- above that ceiling, so its top
 * third was unreachable. */
#define RX_MSG_Q_LEN (ETH_RX_DESC_CNT)

static osMessageQueueId_t rx_msg_q;

/* NetX Duo's link layer stashes an RX timestamp in the first THREE ULONGs of the packet
 * buffer, overwriting the Ethernet header it has already consumed:
 *
 *     ((ULONG *)packet -> nx_packet_data_start)[0] = time -> nano_second;
 *     ((ULONG *)packet -> nx_packet_data_start)[1] = time -> second_low;
 *     ((ULONG *)packet -> nx_packet_data_start)[2] = time -> second_high;
 *
 * That is how the timestamp survives up to a UDP socket, and it is an undocumented layout
 * dependency of exactly the kind that produced F48. It is only safe while at least 12 bytes
 * sit between nx_packet_data_start and the IP header -- true here because the full 14-byte
 * Ethernet header is copied in at data_start -- so the precondition is asserted rather than
 * assumed. */
#define NX_LINK_TIMESTAMP_HEADROOM (3U * sizeof(ULONG))

/* The 2-byte shim's whole contract, as a link-time assertion rather than a comment.
 *
 * rx_thread copies the frame at prepend_ptr + 2 so that the 14-byte Ethernet header leaves the
 * IP header 4-byte aligned, AND so that the 12 bytes NetX Duo later writes over data_start
 * land on header bytes it has already consumed. Both need 2 + 14 = 16 bytes of headroom, which
 * is what NX_PHYSICAL_HEADER provides -- today.
 *
 * If a NetX Duo bump or an nx_user.h edit ever reduced it, nothing would fail to compile and
 * nothing would say anything: every IPv4 packet would silently fail its header checksum while
 * ARP, which has none, worked perfectly. That asymmetry cost a bench session to diagnose the
 * first time (TS-10). */
_Static_assert(NX_PHYSICAL_HEADER >= 16,
               "the 2-byte alignment shim needs 16 bytes of headroom: 2 + 14, so the IP header "
               "lands 4-byte aligned and NetX Duo's 12-byte receive timestamp fits below it. "
               "See rx_thread(); reducing NX_PHYSICAL_HEADER silently breaks every IPv4 "
               "checksum while leaving ARP working.");

/**
 * Drains the RX ring into NetX Duo.
 *
 * Above the console, below the IP thread. Every descriptor this thread has not yet restored
 * is one the DMA cannot reuse, so letting a burst of CLI output round-robin against it
 * lengthens exactly the window that F61's inFlight marker exists to make safe. The marker
 * fixes the correctness bug; the priority keeps the window short.
 */
static void rx_thread(void *arg) {
    (void)arg;
    ETHHW_EventDesc evt;

    for (;;) {
        if (osMessageQueueGet(rx_msg_q, &evt, NULL, osWaitForever) != osOK) {
            continue;
        }

        NX_PACKET *packet = NX_NULL;
        NX_PACKET_POOL *pool = (drv_ip != NX_NULL) ? drv_ip->nx_ip_default_packet_pool : NX_NULL;

        /* NX_NO_WAIT, not a timeout: this runs with two descriptors held out of the ring, and
           blocking here to wait for a pool that is empty because the stack is busy would
           starve the DMA to fix a shortage upstream of it. Dropping is the right failure. */
        if ((pool != NX_NULL) &&
            (nx_packet_allocate(pool, &packet, NX_RECEIVE_PACKET, NX_NO_WAIT) == NX_SUCCESS)) {

            const ULONG size = evt.data.rx.size;
            const ULONG capacity =
                (ULONG)(packet->nx_packet_data_end - packet->nx_packet_prepend_ptr);

            /* Two bytes of shim before the Ethernet header, so that the IP header behind it
             * lands 4-byte aligned.
             *
             * This is why NX_PHYSICAL_HEADER is 16 and not 14: 2 + 14 = 16, and NetX Duo's
             * checksum and header routines read the IP header as aligned words. Copying the
             * frame to prepend_ptr directly puts the IP header at data_start + 14, i.e.
             * 2 mod 4, and EVERY IPv4 packet then fails its header checksum -- measured
             * exactly that way during bring-up: "ip: rx 11 (invalid 11, dropped 11, cksum
             * 11)" while ARP, which has no checksum, worked perfectly and answered every
             * request. That asymmetry is the signature of this bug.
             *
             * The timestamp NetX Duo later writes over data_start[0..11] still fits: the
             * Ethernet header now occupies data_start+2 .. +15, so the 12 bytes it clobbers
             * are header bytes it has already consumed, and the IP header at +16 is
             * untouched. */
            if ((size + 2U <= capacity) && (size >= NX_LINK_TIMESTAMP_HEADROOM)) {
                packet->nx_packet_prepend_ptr += 2;
                memcpy(packet->nx_packet_prepend_ptr, (const void *)evt.data.rx.payload, size);
                packet->nx_packet_length = size;
                packet->nx_packet_append_ptr = packet->nx_packet_prepend_ptr + size;

                /* The hardware timestamp, straight from the context descriptor the MAC wrote.
                   Both halves are zero for a frame the MAC did not stamp -- ETHHW_ProcessRx()
                   zeroes the event for exactly that reason -- and NetX Duo is happy to receive
                   a zero timestamp; what it must not get is stack garbage. */
                NX_LINK_TIME ts;
                ts.nano_second = evt.data.rx.ts_ns;
                ts.second_low = evt.data.rx.ts_s;
                ts.second_high = 0;

                drv_rx_frames++;
                nx_link_ethernet_packet_received(drv_ip,
                                                 drv_interface->nx_interface_index,
                                                 packet, &ts);
            } else {
                /* Too big for the pool payload, or too short to carry the timestamp NetX Duo
                   will write over its header. Neither can be delivered; say so rather than
                   corrupting a buffer. */
                drv_rx_dropped_size++;
                MSG("ETH: RX frame of %u bytes not deliverable (capacity %u)\n",
                    (unsigned)size, (unsigned)capacity);
                nx_packet_release(packet);
            }
        } else {
            /* No packet to put the frame in. Either the pool is empty, or a receive interrupt
               arrived before NX_LINK_INITIALIZE set drv_ip.

               Counted, not silent: this is precisely the number Inc/nx_user.h points at when it
               says to raise NX_PACKET_POOL_PACKET_COUNT if receive drops appear. Without the
               increment `ethinfo` reports "0 dropped (no packet)" however hard the pool is being
               starved, which sends you looking at the cable instead of the pool. */
            drv_rx_dropped_nopacket++;
        }

        /* Return both descriptors to the DMA. Unconditional: a frame dropped for want of a
           packet must not also cost a descriptor, which is how a momentary shortage turns
           into a permanently dead receive path (F71). */
        ETHHW_RestoreRXDesc(evt.bd);
        ETHHW_RestoreRXDesc(evt.bd_ctx);
    }
}

/* --- PHY link monitoring -------------------------------------------------------------------- */

/* Polls the PHY and pushes speed/duplex into the MAC when the link comes up.
 *
 * Same job as the lwIP driver's phy_thread, including draining the DMA error latch: the
 * abnormal flags (RBU, FBE, RPS, TPS) cannot be printed where they are detected because that
 * is interrupt context (F68), so they are reported from here -- a thread that already exists
 * and already runs at a sensible period. Without this a dead receive path never explains
 * itself, which is finding F71's other half.
 */
static void phy_thread(void *arg) {
    (void)arg;

    for (;;) {
        phy_refresh_link_status();
        const PHY_LinkStatus *status = phy_get_link_status();

        if ((drv_link.up != status->up) || (!drv_link.init)) {
            bool first_poll = !drv_link.init;
            drv_link.up = status->up;
            drv_link.init = true;

            /* Tell NetX Duo, not just ourselves.
             *
             * nx_interface_link_up is what the IP thread consults before handing anything to
             * the driver -- an interface it believes is down is simply never sent on. Setting
             * it once at NX_LINK_INITIALIZE and leaving it produced a board that reported
             * "ETH LINK: UP" on the console, received frames happily, and transmitted
             * absolutely nothing: DHCP never completed and the host counted zero frames
             * arriving in twelve seconds. NX_LINK_GET_STATUS is not enough on its own --
             * NetX Duo only asks when the application asks it to. */
            if (drv_interface != NX_NULL) {
                drv_interface->nx_interface_link_up = drv_link.up ? NX_TRUE : NX_FALSE;
            }

            /* Do not announce "DOWN" on the very first poll. At that point auto-negotiation
               has simply not finished yet, and reporting it makes a normal boot look like a
               cable fault -- the lwIP driver printed only the UP that followed. Genuine
               transitions after the first poll are always reported. */
            if (first_poll && !drv_link.up) {
                /* nothing to say yet */
            } else if (drv_link.up) {
                bool fe = (status->speed == PHY_LS_100Mbps);
                bool duplex = (status->type == PHY_LT_FULL_DUPLEX);
                ETHHW_SetLinkProperties(ETH, fe, duplex);
                drv_link.speed = fe ? 100U : 10U;
                drv_link.duplex = duplex;

                MSG("ETH LINK: " ANSI_COLOR_BGREEN "UP " ANSI_COLOR_RESET "(%u Mbps, %s duplex)\n",
                    drv_link.speed, duplex ? "FULL" : "HALF");
            } else {
                MSG("ETH LINK: " ANSI_COLOR_BRED "DOWN" ANSI_COLOR_RESET "\n");
            }
        }

        ETHHW_ReportDmaError();

        osDelay(500);
    }
}

/* --- Command handlers ---------------------------------------------------------------------- */

static void drv_initialize(NX_IP_DRIVER *request) {
    drv_ip = request->nx_ip_driver_ptr;
    drv_interface = request->nx_ip_driver_interface;

    ETHHW_InitOpts opts = {
        .statePtr = &(ETHStateAndDesc.ETHState),

        .rxRingLen = ETH_RX_DESC_CNT,
        .bufPtr = ETHBuffer,
        .rxRingPtr = (uint8_t *)ETHStateAndDesc.DMARxDscrTab,

        .txRingLen = ETH_TX_DESC_CNT,
        .txRingPtr = (uint8_t *)ETHStateAndDesc.DMATxDscrTab,
        .blockSize = ETH_BUFFER_SIZE,
        .mac = {ETH_MAC_ADDR0, ETH_MAC_ADDR1, ETH_MAC_ADDR2,
                ETH_MAC_ADDR3, ETH_MAC_ADDR4, ETH_MAC_ADDR5}};

    ETHHW_Init(ETH, &opts);

    /* Tell NetX Duo the hardware address, as two halves.
     *
     * lwIP took a 6-byte array in netif->hwaddr; NetX Duo splits it across a 16-bit MSW and a
     * 32-bit LSW, which is also the form nx_link_ethernet_packet_send() takes its destination
     * in. Getting the split wrong produces a MAC that looks plausible in a debugger and is
     * wrong on the wire, so it is written out explicitly rather than memcpy'd. */
    drv_interface->nx_interface_physical_address_msw =
        (ULONG)((opts.mac[0] << 8) | opts.mac[1]);
    drv_interface->nx_interface_physical_address_lsw =
        (ULONG)((opts.mac[2] << 24) | (opts.mac[3] << 16) | (opts.mac[4] << 8) | opts.mac[5]);

    /* Declare the MAC's checksum offload, and the PTP timestamp capability.
     *
     * This is NOT an optimisation, it is a correctness requirement. ETHHW_Transmit() sets
     * ETH_DMATXNDESCRF_CIC_IPHDR_PAYLOAD_INSERT_PHDR_CALC unconditionally, so the MAC ALWAYS
     * computes and inserts the IPv4 header checksum and the TCP/UDP/ICMP payload checksum,
     * and the payload one is computed assuming the field holds a pseudo-header checksum.
     * A stack that also computes a complete checksum leaves a full value there, the MAC
     * folds it in again, and every IP frame goes out with a wrong checksum -- accepted by
     * nothing. Measured during bring-up: ARP round-tripped perfectly (no checksum) while
     * "ip: tx 5" produced not one reply the host would accept.
     *
     * lwIP was told the same thing by CHECKSUM_BY_HARDWARE in lwipopts.h. NetX Duo's
     * equivalent is per-interface, which is why NX_ENABLE_INTERFACE_CAPABILITY is on.
     *
     * IGMP is deliberately absent: the MAC's payload engine covers TCP, UDP and ICMP, not
     * IGMP (IP protocol 2), so NetX Duo must keep computing that one itself. Claiming it
     * would break exactly the multicast joins PTP depends on. */
    drv_interface->nx_interface_capability_flag =
        NX_INTERFACE_CAPABILITY_IPV4_TX_CHECKSUM | NX_INTERFACE_CAPABILITY_IPV4_RX_CHECKSUM |
        NX_INTERFACE_CAPABILITY_UDP_TX_CHECKSUM | NX_INTERFACE_CAPABILITY_UDP_RX_CHECKSUM |
        NX_INTERFACE_CAPABILITY_TCP_TX_CHECKSUM | NX_INTERFACE_CAPABILITY_TCP_RX_CHECKSUM |
        NX_INTERFACE_CAPABILITY_ICMPV4_TX_CHECKSUM | NX_INTERFACE_CAPABILITY_ICMPV4_RX_CHECKSUM |
        NX_INTERFACE_CAPABILITY_PTP_TIMESTAMP;

    /* Ethernet needs IP-to-MAC mapping. Not a detail: nx_ip_driver_packet_send() gates ALL
     * destination resolution on this flag --
     *
     *     if (interface -> nx_interface_address_mapping_needed) { ...broadcast? multicast?
     *                                                             ARP lookup... }
     *
     * -- and with it false, as a zeroed NX_INTERFACE leaves it, NetX Duo skips the lot and
     * hands the driver a request whose physical address fields are still zero. The frames go
     * out perfectly formed except for a destination MAC of 00:00:00:00:00:00, which nothing
     * accepts and no counter anywhere reports. Measured exactly that way during bring-up:
     * "tx: 16 sent, 0 dropped" against a host that received nothing, and a frame dump reading
     * "00 00 00 00 00 00 | 56 00 1e 00 04 51 | 08 00 | 45 00 ...".
     *
     * It is false by default because the flag exists for point-to-point links (PPP, SLIP)
     * where there are no addresses to resolve. Ethernet is the other case. */
    drv_interface->nx_interface_address_mapping_needed = NX_TRUE;

    /* MTU is the IP payload NetX Duo may hand down, i.e. the 1514-byte maximum Ethernet
       frame less the 14-byte header. There is no address-length field to set: NetX Duo takes
       the 6-byte Ethernet address as given by the msw/lsw split above. */
    drv_interface->nx_interface_ip_mtu_size = 1500;

    /* The link starts down and phy_thread reports the transition. Saying UP here would let
       DHCP start before the PHY has negotiated. */
    drv_interface->nx_interface_link_up = NX_FALSE;

    /* The receive queue and thread, created once. NX_LINK_INITIALIZE can be issued again if
       the interface is taken down and back up, and creating a second thread each time would
       leak both it and its stack out of the byte pool. */
    static bool rx_started = false;
    if (!rx_started) {
        /* Guarded separately from rx_started: if the queue was created and only the thread
           failed, a second NX_LINK_INITIALIZE would allocate another queue from the byte pool
           and orphan the first -- the leak this rx_started guard exists to prevent (NX-08). */
        if (rx_msg_q == NULL) {
            rx_msg_q = osMessageQueueNew(RX_MSG_Q_LEN, sizeof(ETHHW_EventDesc), NULL);
        }

        osThreadAttr_t rx_attr;
        memset(&rx_attr, 0, sizeof(rx_attr));
        rx_attr.name = "rx";
        rx_attr.stack_size = 2048;
        /* AboveNormal (32) rather than High (40): the IP thread must stay highest, since this
           thread hands frames to it and getting ahead of it only queues work. */
        rx_attr.priority = osPriorityAboveNormal;

        if ((rx_msg_q == NULL) || (osThreadNew(rx_thread, NULL, &rx_attr) == NULL)) {
            MSG("ETH: could not create the receive thread or queue\n");
        } else {
            rx_started = true;
        }
    }

    static bool phy_thread_started = false;
    if (!phy_thread_started) {
        osThreadAttr_t attr;
        memset(&attr, 0, sizeof(attr));
        attr.name = "phy";
        attr.stack_size = 1024;
        attr.priority = osPriorityNormal;
        if (osThreadNew(phy_thread, NULL, &attr) == NULL) {
            MSG("ETH: could not create the PHY thread\n");
        } else {
            phy_thread_started = true;
        }
    }

    request->nx_ip_driver_status = NX_SUCCESS;
}

static void drv_enable(NX_IP_DRIVER *request) {
    if (drv_enabled) {
        request->nx_ip_driver_status = NX_ALREADY_ENABLED;
        return;
    }

    ETHHW_Start(ETH);
    drv_enabled = true;
    request->nx_ip_driver_status = NX_SUCCESS;
}

static void drv_disable(NX_IP_DRIVER *request) {
    /* Transmit-only, deliberately, and worth saying because the asymmetry is not obvious.
     *
     * drv_send() refuses once this clears, so nothing further goes out. The MAC's receiver and
     * both DMA engines are left running: stopping them means draining the rings and handling a
     * restart, and nothing in this firmware ever disables the interface -- NetX Duo issues
     * NX_LINK_DISABLE only from nx_ip_interface_detach() and nx_ip_delete(), neither of which
     * this application calls.
     *
     * If a link-flap recovery path is ever added, this is the function that has to grow: frames
     * currently keep arriving and being handed to a stack that believes the interface is
     * down (NX-10). */
    drv_enabled = false;
    request->nx_ip_driver_status = NX_SUCCESS;
}

static void drv_get_status(NX_IP_DRIVER *request) {
    *(request->nx_ip_driver_return_ptr) = drv_link.up ? NX_TRUE : NX_FALSE;
    request->nx_ip_driver_status = NX_SUCCESS;
}

/* --- Pending transmit timestamps ------------------------------------------------------------ */

/* Which uid belongs to which outgoing packet.
 *
 * lwIP let this project bolt `tag` and `tx_cb` straight onto struct pbuf through
 * LWIP_PBUF_CUSTOM_DATA. NX_PACKET has no such hook and no spare field, so the association
 * lives beside the packet instead, keyed on its address.
 *
 * Keying on a pointer has one hazard worth naming: NetX Duo recycles packets, so a stale entry
 * whose packet has been freed and reallocated would hand the servo a completely wrong uid --
 * silently, and only for the Delay_Req cycles unlucky enough to collide. Three things prevent
 * that, and none of them is optional:
 *
 *   - the entry is cleared the moment the driver consumes it;
 *   - the NSD cancels the record if its send call reports failure;
 *   - drv_send() cancels it on every path that drops the packet itself.
 *
 * The third is not redundant with the second. drv_send()'s link-down path returns NX_SUCCESS on
 * purpose, so the NSD sees no failure to react to; and NetX Duo does not reliably propagate
 * nx_ip_driver_status back through nxd_udp_socket_send() anyway, so the other drop paths cannot
 * count on being noticed upstream either. Whoever frees the packet withdraws the record.
 *
 * Four entries is ample -- flexPTP has at most a Sync and a Delay_Req outstanding -- and a
 * linear scan of four is cheaper than anything cleverer. */
/* The table itself is Src/flexptp_nsd/ptp_tx_registry.{h,cpp}, extracted so the pointer-reuse
 * hazard described above can be tested. Its rules -- take rather than read, overwrite a
 * duplicate key, refuse when full -- are pinned by test/test_ptp_tx_registry.cpp. */
static PtpTxRegistry tx_ts_pending;

void eth_drv_request_tx_timestamp(NX_PACKET *packet, uint32_t uid) {
    if (!ptp_tx_registry_add(&tx_ts_pending, packet, uid)) {
        /* Full. The message still goes out, just unstamped -- flexPTP times that Delay_Req out
           rather than mispairing it. Reported because a full table means timestamps are not
           being consumed, which is a fault upstream of here. */
        MSG("ETH: transmit-timestamp table full; uid %u will not be stamped\n", (unsigned)uid);
    }
}

void eth_drv_cancel_tx_timestamp(NX_PACKET *packet) {
    ptp_tx_registry_cancel(&tx_ts_pending, packet);
}

/* --- Transmit -------------------------------------------------------------------------------- */

/* Assembled here because ETHHW_Transmit() takes one flat buffer and NX_PACKET may be chained.
   Static rather than on the stack: 1528 bytes would be most of rx/IP thread stacks, and the
   send path is already serialised by NetX Duo's driver mutex. */
static uint8_t tx_concat_buf[ETH_BUFFER_SIZE];

/**
 * Transmit one packet, prepending the Ethernet header where NetX Duo expects the driver to.
 *
 * NX_LINK_RAW_PACKET_SEND is the exception: the caller has already built the header --
 * nx_link_ethernet_packet_send() does, which is the path PTP-over-802.3 uses from S7 -- so
 * adding another would produce a frame with two.
 */
static void drv_send(NX_IP_DRIVER *request) {
    NX_PACKET *packet = request->nx_ip_driver_packet;

    if (packet == NX_NULL) {
        request->nx_ip_driver_status = NX_INVALID_PACKET;
        return;
    }

    if (!drv_link.up || !drv_enabled) {
        /* Dropping while the link is down is correct and must still release the packet: a
           pool of 8 disappears in seconds otherwise, and the symptom would be a hang rather
           than an unplugged cable.

           The status stays NX_SUCCESS deliberately (see above), which is exactly why the
           pending record has to be withdrawn HERE: the NSD only cancels when its send call
           reports failure, so a link flap during PTP transmission would otherwise strand four
           entries and fill the table. */
        drv_tx_dropped_linkdown++;
        eth_drv_cancel_tx_timestamp(packet);
        nx_packet_transmit_release(packet);
        request->nx_ip_driver_packet = NX_NULL;
        request->nx_ip_driver_status = NX_SUCCESS;
        return;
    }

    if (request->nx_ip_driver_command != NX_LINK_RAW_PACKET_SEND) {
        /* NetX Duo's command -> what the frame carries. One line per case, where a reader can
           see it; the EtherType values themselves are eth_framer.cpp's, which depends on
           nothing so that a test of the framing is not a test of NetX Duo's headers. */
        EthFramerPayload payload;
        switch (request->nx_ip_driver_command) {
        case NX_LINK_ARP_SEND:
        case NX_LINK_ARP_RESPONSE_SEND:
            payload = ETH_FRAMER_PAYLOAD_ARP;
            break;
        case NX_LINK_RARP_SEND:
            payload = ETH_FRAMER_PAYLOAD_RARP;
            break;
        default:
            payload = (packet->nx_packet_ip_version == NX_IP_VERSION_V6)
                          ? ETH_FRAMER_PAYLOAD_IPV6
                          : ETH_FRAMER_PAYLOAD_IPV4;
            break;
        }

        if ((ULONG)(packet->nx_packet_prepend_ptr - packet->nx_packet_data_start) <
            NX_LINK_ETHERNET_HEADER_SIZE) {
            MSG("ETH: no room for the Ethernet header; frame dropped\n");
            eth_drv_cancel_tx_timestamp(packet);
            nx_packet_transmit_release(packet);
            request->nx_ip_driver_packet = NX_NULL;
            request->nx_ip_driver_status = NX_UNDERFLOW;
            return;
        }

        packet->nx_packet_prepend_ptr -= NX_LINK_ETHERNET_HEADER_SIZE;
        packet->nx_packet_length += NX_LINK_ETHERNET_HEADER_SIZE;

        /* The destination comes from the request's msw/lsw pair, the source from the
           interface's. Both splits, and the big-endian layout, are eth_framer's -- and
           host-tested, because a wrong split produces a MAC that looks plausible in a debugger
           and is wrong on the wire (TS-04). */
        eth_framer_write_header(packet->nx_packet_prepend_ptr,
                                request->nx_ip_driver_physical_address_msw,
                                request->nx_ip_driver_physical_address_lsw,
                                drv_interface->nx_interface_physical_address_msw,
                                drv_interface->nx_interface_physical_address_lsw,
                                eth_framer_ethertype(payload));
    }

    /* Bounded, and reported. The lwIP driver learned this as N14: a frame that cannot be sent
       must not be counted as sent, and an over-length one will never fit however long you
       wait. */
    if (packet->nx_packet_length > ETH_BUFFER_SIZE) {
        MSG("ETH: TX frame of %u bytes exceeds the %u-byte buffer; dropped\n",
            (unsigned)packet->nx_packet_length, (unsigned)ETH_BUFFER_SIZE);
        eth_drv_cancel_tx_timestamp(packet);
        nx_packet_transmit_release(packet);
        request->nx_ip_driver_packet = NX_NULL;
        request->nx_ip_driver_status = NX_INVALID_PACKET;
        return;
    }

    /* Flatten the chain. eth_framer_append() saturates at the buffer and returns the new
       offset, so a chain whose declared length disagrees with its contents cannot walk off the
       end -- and the saturation is host-tested rather than argued about. */
    ULONG offset = 0;
    for (NX_PACKET *p = packet; p != NX_NULL; p = p->nx_packet_next) {
        const ULONG chunk = (ULONG)(p->nx_packet_append_ptr - p->nx_packet_prepend_ptr);
        offset = eth_framer_append(tx_concat_buf, ETH_BUFFER_SIZE, (uint32_t)offset,
                                   p->nx_packet_prepend_ptr, (uint32_t)chunk);
        if (offset >= ETH_BUFFER_SIZE) {
            break;
        }
    }

    /* Bring-up diagnostic: keep the head of the last frame so `ethinfo` can show what
       actually went to the MAC. */
    drv_tx_last_cmd = request->nx_ip_driver_command;
    drv_tx_last_len = offset;
    memcpy(drv_tx_last_head, tx_concat_buf, (offset < sizeof(drv_tx_last_head))
                                                ? offset : sizeof(drv_tx_last_head));

    /* Does this frame want a hardware transmit timestamp?
     *
     * Two things must both be true: the NSD asked for one (the capability flag, which survives
     * nx_ip_packet_send because that ORs rather than assigns), and it left a uid for us. The
     * record is taken -- not merely read -- so it cannot be matched twice, and so a packet
     * recycled at the same address later starts clean. */
    uint8_t tx_opts = ETHHW_TXOPT_NONE;
    ETHHW_OptArg_TxTsCap ts_arg;
    void *tx_opt_args = NULL;
    uint32_t ts_uid = 0;

    if ((packet->nx_packet_interface_capability_flag & NX_INTERFACE_CAPABILITY_PTP_TIMESTAMP) &&
        ptp_tx_registry_take(&tx_ts_pending, packet, &ts_uid)) {
        memset(&ts_arg, 0, sizeof(ts_arg));
        /* The MAC driver stores both as uint32_t in the descriptor extension; on this 32-bit
           target that is lossless for a function pointer and for the uid alike. */
        ts_arg.txTsCbPtr = (uint32_t)(uintptr_t)&ptp_tx_timestamp_from_isr;
        ts_arg.tag = ts_uid;
        tx_opts = ETHHW_TXOPT_CAPTURE_TS;
        tx_opt_args = &ts_arg;
        drv_tx_timestamped++;
    }

    bool sent = ETHHW_Transmit(ETH, tx_concat_buf, (uint16_t)offset,
                               tx_opts, tx_opt_args);

    if (sent) {
        drv_tx_frames++;
    } else {
        drv_tx_dropped_mac++;
    }

    nx_packet_transmit_release(packet);
    request->nx_ip_driver_packet = NX_NULL;

    /* NX_TX_QUEUE_DEPTH rather than NX_SUCCESS on failure: reporting a dropped frame as sent
       is exactly finding N14, where the stack could not see the loss and the drop counter
       stayed at zero. */
    request->nx_ip_driver_status = sent ? NX_SUCCESS : NX_TX_QUEUE_DEPTH;
}

/* --- Entry point ------------------------------------------------------------------------------ */

VOID nx_h563_eth_driver(NX_IP_DRIVER *request) {
    switch (request->nx_ip_driver_command) {
    case NX_LINK_INTERFACE_ATTACH:
        drv_interface = request->nx_ip_driver_interface;
        request->nx_ip_driver_status = NX_SUCCESS;
        break;

    case NX_LINK_INITIALIZE:
        drv_initialize(request);
        break;

    case NX_LINK_ENABLE:
        drv_enable(request);
        break;

    case NX_LINK_DISABLE:
        drv_disable(request);
        break;

    case NX_LINK_GET_STATUS:
        drv_get_status(request);
        break;

    case NX_LINK_PACKET_SEND:
    case NX_LINK_PACKET_BROADCAST:
    case NX_LINK_RARP_SEND:
    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
    case NX_LINK_RAW_PACKET_SEND:
        drv_send(request);
        break;

    case NX_LINK_MULTICAST_JOIN:
    case NX_LINK_MULTICAST_LEAVE:
        /* Nothing to program. mac_drv.c sets MACPFR.PM -- pass all multicast -- so every
           group already reaches the stack and the filtering is NetX Duo's, in software. The
           lwIP driver relied on the same thing. If per-group hardware filtering is ever
           wanted, this is where the hash table would be maintained. */
        request->nx_ip_driver_status = NX_SUCCESS;
        break;

    case NX_LINK_DEFERRED_PROCESSING:
        /* Not used: this driver hands received frames straight to NetX Duo from rx_thread
           rather than deferring them onto the IP thread. */
        request->nx_ip_driver_status = NX_SUCCESS;
        break;

    default:
        /* Every command this driver implements has its own case above. NX_SUCCESS rather than
           an error for the rest -- NX_LINK_GET_INTERFACE_TYPE, the IPv6 and PPP commands, and
           anything a future NetX Duo adds -- because the stack marks an interface broken on a
           driver error, and refusing a command this firmware does not need is not a reason to
           take the link down. */
        request->nx_ip_driver_status = NX_SUCCESS;
        break;
    }
}

/* --- MAC driver callbacks ---------------------------------------------------------------------- */

/* Called from ETHHW_ProcessRx(), in interrupt context.
 *
 * Returning 0 means "queued, I will restore the descriptors later" -- rx_thread does it.
 * Returning ETHHW_RET_RX_PROCESSED means "restore them now", which is the right answer when
 * the queue is full and the frame is being dropped: holding descriptors for a frame nobody
 * will collect is how the ring runs dry.
 *
 * Timeout 0 is mandatory, not conservative: this is an ISR, and CMSIS-RTOS2 rejects any
 * non-zero timeout from interrupt context -- the same rule whose violation inside flexPTP is
 * finding 1 of the feasibility study. */
int ETHHW_ReadCallback(ETHHW_EventDesc *evt) {
    if (evt->type != ETHHW_EVT_RX_READ) {
        return 0;
    }

    if (osMessageQueuePut(rx_msg_q, evt, 0, 0) != osOK) {
        /* Counted, and this is the drop path that actually bites.
         *
         * docs/architecture.md has the measurements: the packet pool never starves -- 53 000
         * frames at line rate with the pool cut from 8 to 2 produced zero drops, because
         * rx_thread allocates, copies and releases within one queue entry. The queue is the
         * limit, and until now it dropped silently, which left `ethinfo` reporting a receive
         * path with no losses at all while the one place losses can occur said nothing.
         *
         * The frame is dropped here, not deferred: returning ETHHW_RET_RX_PROCESSED tells
         * ETHHW_ProcessRx() to restore the descriptors immediately, because holding them for a
         * frame nobody will collect is how the ring runs dry (F71). */
        drv_rx_dropped_queue++;
        return ETHHW_RET_RX_PROCESSED;
    }
    return 0;
}

int ETHHW_EventCallback(ETHHW_EventDesc *evt) {
    if (evt->type == ETHHW_EVT_RX_NOTFY) {
        ETHHW_ProcessRx(ETH);
    }
    return 0;
}

void ETH_IRQHandler(void) {
    ETHHW_ISR(ETH);
}


/* --- Diagnostics ------------------------------------------------------------------------------ */

/* Counters, because "it does not work" is not a diagnosis.
 *
 * Every one of these exists because it was needed during bring-up: the difference between
 * "the driver never got the frame", "the driver got it and dropped it" and "the stack refused
 * it" is invisible from the console otherwise, and each has a completely different cause. */
void eth_drv_report(void) {
    MSG("ETH driver:\n");
    MSG("  enabled=%u link=%s %u Mbps %s\n", (unsigned)drv_enabled,
        drv_link.up ? "UP" : "DOWN", drv_link.speed, drv_link.duplex ? "full" : "half");
    MSG("  nx_interface_link_up=%u\n",
        (drv_interface != NX_NULL) ? (unsigned)drv_interface->nx_interface_link_up : 0U);
    MSG("  rx: %u delivered, %u dropped (no packet), %u dropped (size), %u dropped (queue full)\n",
        (unsigned)drv_rx_frames, (unsigned)drv_rx_dropped_nopacket, (unsigned)drv_rx_dropped_size,
        (unsigned)drv_rx_dropped_queue);

    /* Both expected to stay at zero. `deferred` counts frames whose context descriptor the
       DMA had not written yet -- retried rather than stamped with whatever the slot held
       (NX-01); `stray` counts context descriptors that turned up without a frame in front of
       them. A rising `deferred` under load is not a fault, but a rising `stray` is. */
    {
        uint32_t ctx_deferred = 0, ctx_stray = 0;
        ETHHW_GetRxCtxCounters(&ctx_deferred, &ctx_stray);
        MSG("  rx ts: %u deferred (context descriptor late), %u stray context descriptors\n",
            (unsigned)ctx_deferred, (unsigned)ctx_stray);
    }

    MSG("  tx: %u sent, %u dropped (link down), %u dropped (MAC busy)\n",
        (unsigned)drv_tx_frames, (unsigned)drv_tx_dropped_linkdown, (unsigned)drv_tx_dropped_mac);

    MSG("  tx timestamped: %u requested\n", (unsigned)drv_tx_timestamped);
    MSG("  tx last: cmd=%u, %u bytes\n    ", (unsigned)drv_tx_last_cmd, (unsigned)drv_tx_last_len);
    for (unsigned i = 0; i < sizeof(drv_tx_last_head); i++) {
        MSG("%x ", (unsigned)drv_tx_last_head[i]);
    }
    MSG("\n");

    /* NetX Duo's own view, which is the other half of the story: the driver counters above
       say what reached the stack, these say what the stack made of it. A frame counted
       delivered here and invalid there is a framing problem; delivered here and simply absent
       there is a dispatch problem. */
    if (drv_ip != NX_NULL) {
        ULONG ip_total_tx = 0, ip_total_rx = 0, ip_invalid_rx = 0, ip_dropped_rx = 0;
        ULONG ip_cksum_err = 0, ip_send_fail = 0;
        /* Parameter order matters and is easy to get wrong -- it is
           (sent, bytes_sent, received, bytes_received, invalid, dropped, cksum_err,
            send_dropped, frags_sent, frags_received). */
        if (nx_ip_info_get(drv_ip, &ip_total_tx, NX_NULL,
                           &ip_total_rx, NX_NULL,
                           &ip_invalid_rx, &ip_dropped_rx, &ip_cksum_err,
                           &ip_send_fail, NX_NULL, NX_NULL) == NX_SUCCESS) {
            MSG("  ip: rx %u (invalid %u, dropped %u, cksum %u), tx %u (failed %u)\n",
                (unsigned)ip_total_rx, (unsigned)ip_invalid_rx, (unsigned)ip_dropped_rx,
                (unsigned)ip_cksum_err, (unsigned)ip_total_tx, (unsigned)ip_send_fail);
        }

        ULONG udp_rx = 0, udp_tx = 0, udp_invalid = 0, udp_dropped = 0, udp_cksum = 0;
        if (nx_udp_info_get(drv_ip, &udp_tx, NX_NULL, &udp_rx, NX_NULL,
                            &udp_invalid, &udp_dropped, &udp_cksum) == NX_SUCCESS) {
            MSG("  udp: rx %u (invalid %u, dropped %u, cksum %u), tx %u\n",
                (unsigned)udp_rx, (unsigned)udp_invalid, (unsigned)udp_dropped,
                (unsigned)udp_cksum, (unsigned)udp_tx);
        }

        ULONG arp_req_sent = 0, arp_req_rcvd = 0, arp_rsp_sent = 0, arp_rsp_rcvd = 0;
        if (nx_arp_info_get(drv_ip, &arp_req_sent, &arp_req_rcvd, &arp_rsp_sent, &arp_rsp_rcvd,
                            NX_NULL, NX_NULL, NX_NULL, NX_NULL) == NX_SUCCESS) {
            MSG("  arp: req sent %u rcvd %u, rsp sent %u rcvd %u\n",
                (unsigned)arp_req_sent, (unsigned)arp_req_rcvd,
                (unsigned)arp_rsp_sent, (unsigned)arp_rsp_rcvd);
        }
    }
}

/* The driver's counters as `key=value`, for tools/hil/.
 *
 * Deliberately duplicated rather than shared with eth_drv_report(): the prose form is laid out
 * for a human reading a console and its layout must stay free to change, while these key names
 * are a contract with the harness. Keeping them in one function would make every cosmetic edit
 * a potential silent breakage of a hardware check -- which is the finding this answers
 * (GI-02). */
void eth_drv_stats_kv(void) {
    MSG("drv.enabled=%u\n", (unsigned)drv_enabled);
    MSG("drv.link_up=%u\n", (unsigned)drv_link.up);
    MSG("drv.link_speed=%u\n", (unsigned)drv_link.speed);
    MSG("drv.rx_frames=%u\n", (unsigned)drv_rx_frames);
    MSG("drv.rx_drop_nopacket=%u\n", (unsigned)drv_rx_dropped_nopacket);
    MSG("drv.rx_drop_size=%u\n", (unsigned)drv_rx_dropped_size);
    MSG("drv.rx_drop_queue=%u\n", (unsigned)drv_rx_dropped_queue);
    MSG("drv.tx_frames=%u\n", (unsigned)drv_tx_frames);
    MSG("drv.tx_drop_linkdown=%u\n", (unsigned)drv_tx_dropped_linkdown);
    MSG("drv.tx_drop_mac=%u\n", (unsigned)drv_tx_dropped_mac);
    MSG("drv.tx_timestamped=%u\n", (unsigned)drv_tx_timestamped);
}
