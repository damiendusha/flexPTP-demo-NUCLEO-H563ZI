#include "eth_drv_lwip.h"

#include <memory.h>
#include <stddef.h> // offsetof, for the layout assertion below

#include <stdbool.h>
#include <stdint.h>
#include <stm32h5xx_hal.h>

#include "cmsis_os2.h"
#include "lwip/arch.h"
#include "lwip/err.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/snmp.h"

#include "lwip/tcpip.h"
#include "mac_drv.h"
#include "phy_drv/phy_common.h"

#include "lwip/opt.h"

#include "lwip/def.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/snmp.h"
#include "lwip/stats.h"
#include "netif/ppp/pppoe.h"
#include "standard_output/standard_output.h"

#include <string.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

// -------------------------------------
// --------- Ethernet buffers ----------
// -------------------------------------

#define ETH_BUFFER_SIZE (1528UL)
#define ETH_RX_BUF_SIZE (ETH_BUFFER_SIZE)
#define ETH_TX_BUF_SIZE (ETH_BUFFER_SIZE)

uint8_t ETHBuffer[(ETH_RX_DESC_CNT + ETH_TX_DESC_CNT) * ETH_BUFFER_SIZE] __attribute__((section(".ETHBufferSection"))); /* Ethernet Receive Buffers */

typedef struct {
    ETHHW_State ETHState;
    ETHHW_DescFull DMARxDscrTab[ETH_RX_DESC_CNT];
    ETHHW_DescFull DMATxDscrTab[ETH_TX_DESC_CNT];
} ETHStateAndDescLayout;

ETHStateAndDescLayout ETHStateAndDesc __attribute__((section(".ETHStateAndDecripSection")));

/* ETHHW_GetState() recovers the driver state as ((ETHHW_State *)eth->DMACRDLAR) - 1, i.e. it
 * assumes the state block sits immediately below the RX descriptor ring the DMA was pointed
 * at. Until now that was guaranteed only by the field order above and a comment in
 * mac_drv.h; reordering these two members, or any padding appearing between them, would have
 * produced a driver reading its state from the middle of a descriptor -- silently, and at
 * runtime. Make the layout contract a build error instead. */
_Static_assert(offsetof(ETHStateAndDescLayout, DMARxDscrTab) == sizeof(ETHHW_State),
               "ETHHW_State must immediately precede the RX descriptor ring, with no padding: "
               "ETHHW_GetState() derives the state pointer by subtracting one from DMACRDLAR.");

// -------------------------------------
// ---------- Global objects -----------
// -------------------------------------

static struct netif *if0;

static LinkState linkState = {false, false, 0, false};

static osMessageQueueId_t rx_msg_q;

/* Takes the tcpip_callback argument it is registered with, and ignores it.
 *
 * This was  -- an empty parameter list, which in C means "unspecified"
 * rather than "none", so passing it to tcpip_callback() was a type mismatch that GCC reported
 * as -Wincompatible-pointer-types. The CMake build compiled it with -Wall -Wextra but not
 * -Werror, so it warned on every build and nobody had to act on it. D13 makes first-party
 * warnings errors, which is what surfaced it. */
static void fetch_link_properties(void *arg) {
    (void)arg;
    const PHY_LinkStatus *status = phy_get_link_status();

    // set link up/down state
    if ((linkState.up != status->up) || (!linkState.init)) {
        linkState.up = status->up;

        if (linkState.up) {
            bool fe = status->speed == PHY_LS_100Mbps;
            bool duplex = status->type == PHY_LT_FULL_DUPLEX;
            ETHHW_SetLinkProperties(ETH, fe, duplex);

            // convert "Fast Ethernet" to numerical speed value
            linkState.speed = (status->speed == PHY_LS_100Mbps) ? 100 : 10;

            // save duplex field
            linkState.duplex = duplex;
        }

        // invoke link change notification callback
        if (linkState.up) {
            // MSG("UP!\n");
            netif_set_link_up(if0);
        } else {
            netif_set_link_down(if0);
            // MSG("DOWN!\n");
        }
    }

    linkState.init = true;
}

static void phy_thread(void *arg) {
    while (true) {
        tcpip_callback(fetch_link_properties, NULL);

        // Drain whatever the ETH ISR latched. The abnormal DMA flags -- RBU, FBE, RPS, TPS --
        // cannot be printed where they are detected (F68), so they are reported from here:
        // this thread already exists, already runs at a sensible period, and is in thread
        // context. Without this the flags were acknowledged and thrown away, which is why a
        // dead receive path had never once explained itself.
        ETHHW_ReportDmaError();

        osDelay(500);
    }
    return;
}

// ------------------------------

/* Define those to better describe your network interface. */
#define IFNAME0 'i'
#define IFNAME1 '0'

/* Forward declarations. */
static void ethernetif_input(struct netif *netif);

/* One event per RX descriptor is the hard ceiling: ETHHW_ProcessRx() marks a descriptor
 * inFlight before queueing it and does not revisit it until rx_thread restores it, so no
 * descriptor can contribute two entries. The old value of 32 was above that ceiling and
 * therefore unreachable in its top third; tying it to the ring keeps the two in step if
 * ETH_RX_DESC_CNT is ever changed.
 *
 * Note this is not the throughput limit. That is PBUF_POOL_SIZE (8 in Inc/lwipopts.h): a
 * frame is dropped if rx_thread cannot allocate a pbuf for it, which happens before the
 * queue could ever fill. Measured adequate -- 300 pings of 1400 bytes at 100, 250 and 500
 * packets/s all returned 0% loss -- so the pool is deliberately left alone. If that ever
 * becomes the constraint, raise PBUF_POOL_SIZE, not this. */
#define RX_MSG_Q_LEN (ETH_RX_DESC_CNT)
static void rx_thread(void *arg);

/**
 * In this function, the hardware should be initialized.
 * Called from ethernetif_init().
 *
 * @param netif the already initialized lwip network interface structure
 *        for this ethernetif
 */
static void low_level_init(struct netif *netif) {
    // ------- Ethernet MAC initialization -----

    ETHHW_InitOpts opts = {
        .statePtr = &(ETHStateAndDesc.ETHState),

        .rxRingLen = ETH_RX_DESC_CNT,
        .bufPtr = ETHBuffer,
        .rxRingPtr = (uint8_t *)ETHStateAndDesc.DMARxDscrTab,

        .txRingLen = ETH_TX_DESC_CNT,
        .txRingPtr = (uint8_t *)ETHStateAndDesc.DMATxDscrTab,
        .blockSize = ETH_BUFFER_SIZE,
        .mac = {ETH_MAC_ADDR0, ETH_MAC_ADDR1, ETH_MAC_ADDR2, ETH_MAC_ADDR3, ETH_MAC_ADDR4, ETH_MAC_ADDR5}};

    ETHHW_Init(ETH, &opts);

    // -------------------------------------------------------------------

    // create receive event queue
    rx_msg_q = osMessageQueueNew(RX_MSG_Q_LEN, sizeof(ETHHW_EventDesc), NULL);

    /* Above the console, below tcpip_thread.
     *
     * Six of the nine threads used to share osPriorityNormal (24), including this one and
     * both console producers ("cli" and "serialio"). rx_thread drains the Ethernet ring into
     * lwIP, and every descriptor it has not yet restored is one the DMA cannot reuse -- so
     * letting a burst of CLI output round-robin against it lengthens exactly the window that
     * F61's inFlight marker exists to make safe. The marker fixes the correctness bug; this
     * keeps the window short.
     *
     * AboveNormal (32) rather than High (40): tcpip_thread must stay the highest, since
     * rx_thread hands frames to it and getting ahead of it only queues work. */
    osThreadAttr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.stack_size = 2048;
    attr.name = "rx";
    attr.priority = osPriorityAboveNormal;
    osThreadNew(rx_thread, NULL, &attr);

    // -------------------------------------------------------------------

    ETHHW_Start(ETH);

    // -------- Process PHY events occured during the initialization phase

    // start PHY event processing thread
    memset(&attr, 0, sizeof(attr));
    attr.stack_size = 1024;
    attr.name = "phy";
    osThreadNew(phy_thread, NULL, &attr);

    // -------------------------------------------------------------------

    /* set MAC hardware address length */
    netif->hwaddr_len = ETHARP_HWADDR_LEN;

    /* set MAC hardware address */
    memcpy(netif->hwaddr, opts.mac, ETHARP_HWADDR_LEN);

    /* maximum transfer unit */
    netif->mtu = 1500;

    /* device capabilities */
    /* don't set NETIF_FLAG_ETHARP if this device is not an ethernet one */
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_IGMP /*| NETIF_FLAG_LINK_UP*/;

#if LWIP_IPV6 && LWIP_IPV6_MLD
    /*
     * For hardware/netifs that implement MAC filtering.
     * All-nodes link-local is handled by default, so we must let the hardware know
     * to allow multicast packets in.
     * Should set mld_mac_filter previously. */
    if (netif->mld_mac_filter != NULL) {
        ip6_addr_t ip6_allnodes_ll;
        ip6_addr_set_allnodes_linklocal(&ip6_allnodes_ll);
        netif->mld_mac_filter(netif, &ip6_allnodes_ll, NETIF_ADD_MAC_FILTER);
    }
#endif /* LWIP_IPV6 && LWIP_IPV6_MLD */

    /* Do whatever else is needed to initialize interface. */
}

/**
 * This function should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @param p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 * @return ERR_OK if the packet could be sent
 *         an err_t value if the packet couldn't be sent
 *
 * @note Returning ERR_MEM here if a DMA queue of your MAC is full can lead to
 *       strange results. You might consider waiting for space in the DMA queue
 *       to become available since the stack doesn't retry to send a packet
 *       dropped because of memory failure (except for the TCP timers).
 */
/* Where raw-802.3 PTP frames actually went, as a small histogram over distinct destination
   MACs. A histogram rather than "the last one": in P2P a node interleaves PDelay_Req to the
   peer-delay group with Sync/Announce to the primary group, so the most recent frame is a
   coin toss between two correct answers and cannot be asserted on. Diagnostics only; see the
   note in low_level_output(). */
#define PTP_L2_DST_SLOTS (4U)
static uint8_t s_l2_dst[PTP_L2_DST_SLOTS][6];
static uint32_t s_l2_dst_count[PTP_L2_DST_SLOTS];
static uint32_t s_ptp_l2_frames;

static void note_l2_dst(const uint8_t *mac) {
    for (unsigned i = 0; i < PTP_L2_DST_SLOTS; i++) {
        if ((s_l2_dst_count[i] != 0U) && (memcmp(s_l2_dst[i], mac, 6) == 0)) {
            s_l2_dst_count[i]++;
            return;
        }
    }
    for (unsigned i = 0; i < PTP_L2_DST_SLOTS; i++) {
        if (s_l2_dst_count[i] == 0U) {
            memcpy(s_l2_dst[i], mac, 6);
            s_l2_dst_count[i] = 1U;
            return;
        }
    }
    /* More than PTP_L2_DST_SLOTS distinct destinations: the frame is still counted in
       s_ptp_l2_frames, so the total and the histogram disagreeing is the signal. */
}

void eth_drv_clear_ptp_l2_dst(void) {
    memset(s_l2_dst, 0, sizeof(s_l2_dst));
    memset(s_l2_dst_count, 0, sizeof(s_l2_dst_count));
    s_ptp_l2_frames = 0;
}

bool eth_drv_ptp_l2_dst(unsigned slot, uint8_t *mac6, uint32_t *pCount) {
    if ((slot >= PTP_L2_DST_SLOTS) || (s_l2_dst_count[slot] == 0U)) {
        return false;
    }
    if (mac6 != NULL) {
        memcpy(mac6, s_l2_dst[slot], 6);
    }
    if (pCount != NULL) {
        *pCount = s_l2_dst_count[slot];
    }
    return true;
}

uint32_t eth_drv_ptp_l2_frames(void) {
    return s_ptp_l2_frames;
}

static err_t low_level_output(struct netif *netif, struct pbuf *p) {
    struct pbuf *q;
    static u8_t concat_buf[ETH_TX_BUF_SIZE];
    u32_t concat_buf_level = 0;

#if ETH_PAD_SIZE
    pbuf_remove_header(p, ETH_PAD_SIZE); /* drop the padding word */
#endif

    /* Not reachable at netif->mtu = 1500 (1514 bytes on the wire against a 1528-byte
     * buffer), but the copy below had no bound of any kind -- so an MTU change, a jumbo
     * frame or a malformed chain would overrun a static buffer with no diagnostic.
     *
     * Deliberately NOT ERR_MEM: this function's own doc comment carries lwIP's warning that
     * returning ERR_MEM from linkoutput "can lead to strange results", because the stack does
     * not retry a frame dropped for memory reasons. An over-length frame will never fit, so
     * report it and drop it. */
    if (p->tot_len > (u16_t)ETH_TX_BUF_SIZE) {
        MSG("ETH: TX frame of %u bytes exceeds the %u-byte buffer; dropped\n",
            (unsigned)p->tot_len, (unsigned)ETH_TX_BUF_SIZE);
        LINK_STATS_INC(link.lenerr);
        LINK_STATS_INC(link.drop);
        return ERR_IF;
    }

    /* Concat pbufs into a single buffer */
    for (q = p; q != NULL; q = q->next) {
        /* Send the data from the pbuf to the interface, one pbuf at a
           time. The size of the data in each pbuf is kept in the ->len
           variable. */
        memcpy(concat_buf + concat_buf_level, q->payload, q->len);
        concat_buf_level += q->len;
    }

    /* Record where PTP frames are actually being sent.
     *
     * The 802.3 destination is chosen inside flexPTP's nsd_lwip.c, from the profile's
     * primary/pdelay_p2p_8023_destination fields, and nothing downstream reports it. Without
     * this the only way to check that a profile change took effect on the wire is a packet
     * capture, which needs root -- so a defect in that selection is invisible to the HIL
     * suite. See docs/flexptp_upstream_review.md finding U4.
     *
     * concat_buf is the finished frame: destination MAC at 0..5, EtherType at 12..13. */
    if ((concat_buf_level >= 14U) &&
        (concat_buf[12] == 0x88U) && (concat_buf[13] == 0xF7U)) {
        note_l2_dst(concat_buf);
        s_ptp_l2_frames++;
    }

    /* check if timestamping is demanded */
    uint8_t opts = ETHHW_TXOPT_NONE;
    bool tsEn = (p->tx_cb != NULL);
    ETHHW_OptArg_TxTsCap optArg;
    memset(&optArg, 0, sizeof(ETHHW_OptArg_TxTsCap));

    if (tsEn) {
        opts = ETHHW_TXOPT_CAPTURE_TS;
        optArg.txTsCbPtr = (uint32_t)(p->tx_cb);
        optArg.tag = (uint32_t)p->tag;
    }

    /* Consume the request: this driver is the only reader of these two fields, and once they
     * are in optArg the pbuf has no further use for them.
     *
     * A second line of defence for finding U5, whose source upstream fixed in RC1.0-26
     * (flexPTP #22). `tx_cb` and `tag` live in LWIP_PBUF_CUSTOM_DATA (lwipopts.h), which lwIP
     * does NOT initialise in pbuf_alloc() -- it manages only its own fields. nsd_lwip.c used to
     * set both and pbuf_free() the buffer without clearing them, so the next PBUF_RAM
     * allocation on that recycled heap block inherited a live `tx_cb`, and the gate above --
     * the only signal nsd_lwip.c offers -- requested a hardware TX timestamp for an ICMP or ARP
     * frame that never asked for one and fired ptp_transmit_timestamp_cb() with a stale uid.
     * Measured on this board under a 120 s flood: 284 392 such events before #22, 0 after.
     *
     * Why clear here as well, now that the producer does: this is the choke point every
     * tainted buffer passes through, so it breaks the propagation loop for ANY producer rather
     * than relying on each one to tidy up. An uncleared block does not merely mis-timestamp
     * one frame -- it stays tainted through every later recycle and spreads at transmit rate.
     * Two stores per frame is the whole cost.
     *
     * That was not the whole of it. This comment used to say lwIP offers no hook for zeroing
     * custom data on allocation; it does -- LWIP_PBUF_CUSTOM_DATA_INIT, called from
     * pbuf_init_alloced_pbuf() -- and lwipopts.h never defined it, so a pbuf lwIP allocated
     * for itself could carry stale bytes in tx_cb that no PTP code had ever written. Clearing
     * here only after the read cannot catch that: the gate above trusts the field. A stale
     * 0xfffffe00 was called from the ETH interrupt after a flood and took a HardFault.
     * lwipopts.h now defines the hook; this clear stays as the second line.
     *
     * check_ptp_flood in tools/hil/ asserts ptp.drop_txts stays small, so losing both this and
     * the upstream clear would fail the hardware suite rather than pass it quietly. */
    p->tx_cb = NULL;
    p->tag = NULL;

    /* Pass the data to the MAC.
     *
     * A drop here used to be reported to lwIP as a successful transmit -- link.xmit bumped,
     * ERR_OK returned -- so the stack could not see it and link.drop, which the over-length
     * branch above is careful to bump, stayed at zero (finding N14). ERR_IF for the same
     * reason spelled out there: this function's own doc comment carries lwIP's warning that
     * ERR_MEM from linkoutput "can lead to strange results", because the stack does not retry
     * a frame dropped for memory reasons. */
    if (!ETHHW_Transmit(ETH, concat_buf, concat_buf_level, opts, &optArg)) {
        LINK_STATS_INC(link.drop);
        MIB2_STATS_NETIF_INC(netif, ifoutdiscards);
        return ERR_IF;
    }

    MIB2_STATS_NETIF_ADD(netif, ifoutoctets, p->tot_len);
    if (((u8_t *)p->payload)[0] & 1) {
        /* broadcast or multicast packet*/
        MIB2_STATS_NETIF_INC(netif, ifoutnucastpkts);
    } else {
        /* unicast packet */
        MIB2_STATS_NETIF_INC(netif, ifoutucastpkts);
    }
    /* increase ifoutdiscards or ifouterrors on error */

#if ETH_PAD_SIZE
    pbuf_add_header(p, ETH_PAD_SIZE); /* reclaim the padding word */
#endif

    LINK_STATS_INC(link.xmit);

    return ERR_OK;
}

static void rx_thread(void *arg) {
    ETHHW_EventDesc evt;

    while (true) {

        /* get event */
        if (osMessageQueueGet(rx_msg_q, &evt, NULL, osWaitForever) != osOK) {
            continue;
        }


        /* move received packet into a new pbuf */
        struct pbuf *p, *q;
        /* We allocate a pbuf chain of pbufs from the pool. */
        p = pbuf_alloc(PBUF_RAW, evt.data.rx.size, PBUF_POOL);

        if (p != NULL) {
            /* save size waiting for being stored */
            u16_t size_left = evt.data.rx.size;
            const u8_t *src = (const u8_t *)evt.data.rx.payload;

            /* We iterate over the pbuf chain until we have read the entire
             * packet into the pbuf. */
            for (q = p; (q != NULL) && (size_left > 0); q = q->next) {
                /* Read enough bytes to fill this pbuf in the chain. The
                 * available data in the pbuf is given by the q->len
                 * variable.
                 * This does not necessarily have to be a memcpy, you can also preallocate
                 * pbufs for a DMA-enabled MAC and after receiving truncate it to the
                 * actually received size. In this case, ensure the tot_len member of the
                 * pbuf is the sum of the chained pbuf len members.
                 */

                /* compute copy size and copy */
                u16_t copy_size = MIN(size_left, q->len);
                memcpy(q->payload, src, copy_size);
                src += copy_size; // must advance, or every segment repeats the frame start (F58)
                size_left -= copy_size;
            }

            /* Copy the timestamp into the first pbuf */
            p->time_s = evt.data.rx.ts_s;
            p->time_ns = evt.data.rx.ts_ns;

            /* if0, not netif: there is no "netif" in this scope. These compile today only
             * because MIB2 statistics are disabled, so the macros expand to nothing --
             * enabling them broke the build. */
            MIB2_STATS_NETIF_ADD(if0, ifinoctets, p->tot_len);
            if (((u8_t *)p->payload)[0] & 1) {
                /* broadcast or multicast packet*/
                MIB2_STATS_NETIF_INC(if0, ifinnucastpkts);
            } else {
                /* unicast packet*/
                MIB2_STATS_NETIF_INC(if0, ifinucastpkts);
            }
        } else {
            LINK_STATS_INC(link.memerr);
            LINK_STATS_INC(link.drop);
            MIB2_STATS_NETIF_INC(if0, ifindiscards);
        }

        /* if no packet could be read, silently ignore this */
        if (p != NULL) {
            /* pass all packets to ethernet_input, which decides what packets it supports */
            if (if0->input(p, if0) != ERR_OK) {
                LWIP_DEBUGF(NETIF_DEBUG, ("ethernetif_input: IP input error\n"));
                pbuf_free(p);
                p = NULL;
            }
        }

        /* release the RX descriptors */
        ETHHW_RestoreRXDesc(evt.bd);
        ETHHW_RestoreRXDesc(evt.bd_ctx);
    }
}

int ETHHW_ReadCallback(ETHHW_EventDesc *evt) {
    // packet reception
    if (evt->type != ETHHW_EVT_RX_READ) {
        return 0;
    }

    if (osMessageQueuePut(rx_msg_q, evt, 0, 0) != osOK) {
        return ETHHW_RET_RX_PROCESSED; // this frame has been dropped
    } else {
        return 0; // the buffer will be passed to the processing thread
    }
}

int ETHHW_EventCallback(ETHHW_EventDesc *evt) {
    if (evt->type == ETHHW_EVT_RX_NOTFY) {
        ethernetif_input(if0);
    }

    return 0; // unhandled event
}

/**
 * This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface. Then the type of the received packet is determined and
 * the appropriate input function is called.
 *
 * @param netif the lwip network interface structure for this ethernetif
 */
static void ethernetif_input(struct netif *netif) {
    /* read received packets */
    ETHHW_ProcessRx(ETH);
}

/**
 * Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 * This function should be passed as a parameter to netif_add().
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if the loopif is initialized
 *         ERR_MEM if private data couldn't be allocated
 *         any other err_t on error
 */
err_t ethernetif_init(struct netif *netif) {
    LWIP_ASSERT("netif != NULL", (netif != NULL));

    /* store netif for later usage */
    if0 = netif;

#if LWIP_NETIF_HOSTNAME
    /* Initialize interface hostname */
    netif->hostname = "if0";
#endif /* LWIP_NETIF_HOSTNAME */

    /*
     * Initialize the snmp variables and counters inside the struct netif.
     * The last argument should be replaced with your link speed, in units
     * of bits per second.
     */
    MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, LINK_SPEED_OF_YOUR_NETIF_IN_BPS);

    netif->state = (void *)&linkState;
    netif->name[0] = IFNAME0;
    netif->name[1] = IFNAME1;
    /* We directly use etharp_output() here to save a function call.
     * You can instead declare your own function an call etharp_output()
     * from it if you have to do some checks before sending (e.g. if link
     * is available...) */
#if LWIP_IPV4
    netif->output = etharp_output;
#endif /* LWIP_IPV4 */
#if LWIP_IPV6
    netif->output_ip6 = ethip6_output;
#endif /* LWIP_IPV6 */
    netif->linkoutput = low_level_output;

    /* initialize the hardware */
    low_level_init(netif);

    /* start up the interface */
    netif_set_up(netif);

    /* refresh and fetch PHY and link status */
    // phy_refresh_link_status();
    // fetch_link_properties();

    return ERR_OK;
}

// -----

/* The driver's view of itself, for `ethinfo`.
 *
 * Backported from the ThreadX firmware's eth_drv_report(), and deliberately smaller than it,
 * because two of that function's three sections have nothing to report against here:
 *
 *   - The NetX Duo driver counts its own drops -- no packet, bad size, queue full, link down,
 *     MAC busy -- because the ThreadX port added those counters when chasing where frames were
 *     going. This driver has none of them. That is a real gap and it is stated rather than
 *     papered over: adding them means touching the RX and TX paths of a driver that currently
 *     works, which is its own change with its own hardware verification, not a reporting one.
 *   - lwIP is built with LWIP_STATS 0, so the stack keeps no counters at all. The ThreadX
 *     firmware's "what the stack made of it" half -- nx_ip_info_get and friends -- has no
 *     counterpart until that is turned on, which costs RAM and changes the image.
 *
 * What IS here is the part that transfers unchanged: the link state, and the RX timestamp
 * context counters, which come from //projects/flexptp_demo_stm32h563/lib's shared MAC driver and therefore mean exactly the
 * same thing on both firmwares.
 */
void eth_drv_report(void) {
    MSG("ETH driver:\n");
    MSG("  init=%u link=%s %u Mbps %s\n", (unsigned)linkState.init,
        linkState.up ? "UP" : "DOWN", (unsigned)linkState.speed,
        linkState.duplex ? "full" : "half");
    MSG("  netif=%s\n", (if0 != NULL) ? "attached" : "absent");

    /* Both expected to stay at zero. `deferred` counts frames whose context descriptor the DMA
       had not written yet -- retried rather than stamped with whatever the slot held (NX-01);
       `stray` counts context descriptors that turned up without a frame in front of them. A
       rising `deferred` under load is not a fault, but a rising `stray` is. */
    {
        uint32_t ctx_deferred = 0, ctx_stray_ctx = 0, ctx_stray_tsa = 0;
        ETHHW_GetRxCtxCounters(&ctx_deferred, &ctx_stray_ctx, &ctx_stray_tsa);
        const uint32_t ctx_stray = ctx_stray_ctx + ctx_stray_tsa;
        MSG("  rx ts: %u deferred (context descriptor late), %u stray context descriptors\n",
            (unsigned)ctx_deferred, (unsigned)ctx_stray);
    }

    MSG("  (no driver drop counters, and LWIP_STATS is 0 -- see eth_drv_report())\n");
}

void ETH_IRQHandler() {
    ETHHW_ISR(ETH);
}