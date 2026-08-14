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
 * This file is at S5a. The send and receive paths are stubbed at the bottom, and say so.
 */

#include "eth_drv_netxduo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmsis_os2.h>
#include <stm32h5xx_hal.h>

#include "nx_api.h"

#include "mac_drv.h"
#include "phy_drv/phy_common.h"
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

    /* MTU is the IP payload NetX Duo may hand down, i.e. the 1514-byte maximum Ethernet
       frame less the 14-byte header. There is no address-length field to set: NetX Duo takes
       the 6-byte Ethernet address as given by the msw/lsw split above. */
    drv_interface->nx_interface_ip_mtu_size = 1500;

    /* The link starts down and phy_thread reports the transition. Saying UP here would let
       DHCP start before the PHY has negotiated. */
    drv_interface->nx_interface_link_up = NX_FALSE;

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
    drv_enabled = false;
    request->nx_ip_driver_status = NX_SUCCESS;
}

static void drv_get_status(NX_IP_DRIVER *request) {
    *(request->nx_ip_driver_return_ptr) = drv_link.up ? NX_TRUE : NX_FALSE;
    request->nx_ip_driver_status = NX_SUCCESS;
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

    /* Send commands. Stubbed until S5c -- but the packet must still be released, or ARP and
     * DHCP will drain the 8-packet pool within seconds and the failure will present as a hang
     * rather than as an unimplemented transmit path.
     *
     * Attached to these six commands specifically, NOT to a default: branch:
     * nx_ip_driver_packet is populated only for the send commands and is uninitialised stack
     * for everything else. Releasing it unconditionally is what took the board down with an
     * unaligned UsageFault in S4. */
    case NX_LINK_PACKET_SEND:
    case NX_LINK_PACKET_BROADCAST:
    case NX_LINK_RARP_SEND:
    case NX_LINK_ARP_SEND:
    case NX_LINK_ARP_RESPONSE_SEND:
    case NX_LINK_RAW_PACKET_SEND:
        if (request->nx_ip_driver_packet != NX_NULL) {
            nx_packet_transmit_release(request->nx_ip_driver_packet);
            request->nx_ip_driver_packet = NX_NULL;
        }
        request->nx_ip_driver_status = NX_SUCCESS;
        break;

    default:
        /* Multicast join/leave and deferred processing arrive in S5c. Reported as handled so
           the stack does not mark the interface broken in the meantime. */
        request->nx_ip_driver_status = NX_SUCCESS;
        break;
    }
}

/* --- MAC driver callbacks ---------------------------------------------------------------------- */

/* Receive arrives in S5b. Returning ETHHW_RET_RX_PROCESSED tells the MAC driver to restore
   the descriptor immediately, so the ring keeps cycling and the DMA never stalls. */
int ETHHW_ReadCallback(ETHHW_EventDesc *evt) {
    (void)evt;
    return ETHHW_RET_RX_PROCESSED;
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
