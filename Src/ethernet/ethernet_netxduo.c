/* NetX Duo bring-up: packet pool, IP instance, and the protocols this firmware needs.
 *
 * Replaces ethernet_lwip.c. The shape is different enough to be worth stating: lwIP was
 * initialised by tcpip_init() and then had a netif attached to it, whereas NetX Duo creates
 * an IP instance that OWNS its interface and its driver from the moment nx_ip_create()
 * returns. There is no separate "add the interface" step for interface 0.
 *
 * The Ethernet driver under it is Drivers/EthDrv/eth_drv_netxduo.c, wired in at nx_ip_create()
 * below. S4 had a stub here instead, which is why parts of this file are written as though the
 * wire were unreachable; it was replaced in S5a and everything reaches the wire now.
 */

#include "ethernet_netxduo.h"

#include "nx_api.h"
#include "nxd_dhcp_client.h"

#include "Drivers/EthDrv/eth_drv_netxduo.h"

#include "standard_output/standard_output.h"

#include <stdbool.h>

/* --- Sizing ---------------------------------------------------------------------------- */

/* NX_PACKET_POOL_PAYLOAD_SIZE/PACKET_COUNT come from Inc/nx_user.h, where the reasoning about
   matching the old PBUF_POOL sizing lives. The pool needs room for the NX_PACKET headers on
   top of the payloads, which is what nx_packet_pool_create()'s size argument covers. */
#define ETH_POOL_BYTES \
    ((NX_PACKET_POOL_PAYLOAD_SIZE + sizeof(NX_PACKET)) * NX_PACKET_POOL_PACKET_COUNT)

/* The IP helper thread's stack. NetX Duo runs all protocol processing on it, the way lwIP ran
   everything on tcpip_thread. 2048 matched that thread's 3072 less the lwIP-specific slack;
   revisited in S5c under load rather than guessed at finally here. */
#define ETH_IP_THREAD_STACK_BYTES 2048

/* Above the console, below nothing else.
 *
 * ThreadX counts 0 as most urgent, so this is NOT the same number as lwIP's tcpip_thread
 * priority of 40 on the CMSIS scale -- it is the same POSITION. tcpip_thread was the highest
 * application thread; osPriorityHigh (40) maps to ThreadX 56-40 = 16, and everything else in
 * this firmware sits at 32. See docs/architecture.md#threads. */
#define ETH_IP_THREAD_PRIORITY 16

/* --- State ------------------------------------------------------------------------------- */

/* SRAM3, alongside the Ethernet DMA descriptors and buffers. Same region the lwIP heap and
   the pbuf pool used, and for the same reason: it keeps the network memory out of SRAM1,
   where the thread stacks and the ThreadX byte pools live. */
static UCHAR eth_pool_memory[ETH_POOL_BYTES] __attribute__((section(".ETHBufferSection")));
static UCHAR eth_ip_thread_stack[ETH_IP_THREAD_STACK_BYTES]
    __attribute__((section(".ETHBufferSection")));

static NX_PACKET_POOL eth_pool;
static NX_IP eth_ip;
static NX_DHCP eth_dhcp;
static bool eth_up = false;
static bool eth_dhcp_up = false;

NX_IP *ethernet_ip(void) { return eth_up ? &eth_ip : NX_NULL; }
NX_PACKET_POOL *ethernet_packet_pool(void) { return eth_up ? &eth_pool : NX_NULL; }

/* The driver lives in Drivers/EthDrv/eth_drv_netxduo.c, built on this project's own MAC
   driver. S4 had a stub here instead; it was replaced in S5a. */

/* --- Bring-up -------------------------------------------------------------------------------- */

/**
 * Create the packet pool, the IP instance and the protocols.
 *
 * Called from task_startup(), i.e. from thread context after the kernel is running -- NetX
 * Duo's create calls need a running ThreadX, unlike ThreadX's own object creation which is
 * legal from tx_application_define().
 */
bool init_ethernet(void) {
    UINT status;

    status = nx_packet_pool_create(&eth_pool, "eth pool", NX_PACKET_POOL_PAYLOAD_SIZE,
                                   eth_pool_memory, sizeof(eth_pool_memory));
    if (status != NX_SUCCESS) {
        MSG("ETH: packet pool creation failed (0x%x)\n", status);
        return false;
    }

    /* Address 0.0.0.0/0: DHCP supplies the real one from S5d. NetX Duo is explicit about
       this being the "no address yet" state, where lwIP used a zeroed ip_addr_t. */
    status = nx_ip_create(&eth_ip, "eth ip", IP_ADDRESS(0, 0, 0, 0), 0xFFFFFFFFUL,
                          &eth_pool, nx_h563_eth_driver,
                          eth_ip_thread_stack, sizeof(eth_ip_thread_stack),
                          ETH_IP_THREAD_PRIORITY);
    if (status != NX_SUCCESS) {
        MSG("ETH: IP instance creation failed (0x%x)\n", status);
        return false;
    }

    /* ARP, with a cache. lwIP sized this through ARP_TABLE_SIZE; here the cache is memory the
       application supplies. 1 KB is 52 entries, far more than a point-to-point bench link
       needs, and it is the smallest allocation in this file. */
    static ULONG arp_cache[256];
    status = nx_arp_enable(&eth_ip, arp_cache, sizeof(arp_cache));
    if (status != NX_SUCCESS) {
        MSG("ETH: ARP enable failed (0x%x)\n", status);
        return false;
    }

    /* ICMP: the `ping` check in tools/hil/ is the throughput and loss test for the whole
       receive and transmit path, so this is not optional diagnostics. */
    status = nx_icmp_enable(&eth_ip);
    if (status != NX_SUCCESS) {
        MSG("ETH: ICMP enable failed (0x%x)\n", status);
        return false;
    }

    /* UDP: DHCP and PTP's default transport both need it. */
    status = nx_udp_enable(&eth_ip);
    if (status != NX_SUCCESS) {
        MSG("ETH: UDP enable failed (0x%x)\n", status);
        return false;
    }

    /* IGMP: PTP over UDP is multicast (224.0.1.129 for E2E, 224.0.0.107 for P2P), and the
       NSD joins those groups from S6. */
    status = nx_igmp_enable(&eth_ip);
    if (status != NX_SUCCESS) {
        MSG("ETH: IGMP enable failed (0x%x)\n", status);
        return false;
    }

    /* DHCP client. Creates its own thread and its own socket; nx_dhcp_start() is safe before
     * the link is up -- it retries -- so there is no need to gate it on phy_thread.
     *
     * The address is reported by ethernet_poll_dhcp() rather than from a callback, because
     * the report has to reach the console through MSG(), and MSG() blocks on the console FIFO.
     * Doing that from inside a DHCP state-change callback would block the DHCP thread on the
     * console -- the same class of mistake as printing from an ISR (F68). */

    status = nx_dhcp_create(&eth_dhcp, &eth_ip, "h563");
    if (status != NX_SUCCESS) {
        MSG("ETH: DHCP client creation failed (0x%x)\n", status);
    } else if ((status = nx_dhcp_start(&eth_dhcp)) != NX_SUCCESS) {
        MSG("ETH: DHCP start failed (0x%x)\n", status);
    } else {
        eth_dhcp_up = true;
    }

    eth_up = true;
    MSG("ETH: NetX Duo %u.%u.%u up (pool %u x %u B)\n",
        (unsigned)NETXDUO_MAJOR_VERSION, (unsigned)NETXDUO_MINOR_VERSION,
        (unsigned)NETXDUO_PATCH_VERSION,
        (unsigned)NX_PACKET_POOL_PACKET_COUNT, (unsigned)NX_PACKET_POOL_PAYLOAD_SIZE);
    return true;
}


/**
 * Report the DHCP-assigned address once, when it arrives, and again if it is ever lost.
 *
 * Polled from the "init" thread at 1 Hz, which is exactly what the lwIP build did with a
 * check_dhcp_state() timer -- and for a better reason here: see the note at nx_dhcp_start().
 */
void ethernet_poll_dhcp(void) {
    static bool reported = false;

    if (!eth_up || !eth_dhcp_up) {
        return;
    }

    ULONG address = 0, mask = 0;
    if (nx_ip_address_get(&eth_ip, &address, &mask) != NX_SUCCESS) {
        return;
    }

    bool have = (address != 0);
    if (have == reported) {
        return;
    }
    reported = have;

    if (have) {
        MSG("IP-address: %u.%u.%u.%u\n",
            (unsigned)((address >> 24) & 0xFF), (unsigned)((address >> 16) & 0xFF),
            (unsigned)((address >> 8) & 0xFF), (unsigned)(address & 0xFF));
    } else {
        MSG("DHCP configuration lost!\n");
    }
}
