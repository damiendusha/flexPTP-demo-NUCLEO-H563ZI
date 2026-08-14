/* NetX Duo bring-up: packet pool, IP instance, and the protocols this firmware needs.
 *
 * Replaces ethernet_lwip.c. The shape is different enough to be worth stating: lwIP was
 * initialised by tcpip_init() and then had a netif attached to it, whereas NetX Duo creates
 * an IP instance that OWNS its interface and its driver from the moment nx_ip_create()
 * returns. There is no separate "add the interface" step for interface 0.
 *
 * Stage S4 wires everything except the driver, which is a stub -- see nx_driver_stub() at the
 * bottom. The IP thread runs, the pool exists, and nothing reaches the wire.
 */

#include "ethernet_netxduo.h"

#include "nx_api.h"

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
static bool eth_up = false;

NX_IP *ethernet_ip(void) { return eth_up ? &eth_ip : NX_NULL; }
NX_PACKET_POOL *ethernet_packet_pool(void) { return eth_up ? &eth_pool : NX_NULL; }

/* --- The driver ---------------------------------------------------------------------------- */

/* S4 stub. Answers the two commands NetX Duo issues during nx_ip_create() and
 * nx_ip_interface_status_check(), and reports success for everything else without doing it.
 *
 * The point of the stage is to prove the stack itself comes up -- IP thread scheduled, pool
 * created, protocols enabled -- with the driver as the only unknown left. S5a replaces this
 * with eth_drv_netxduo.c built on the real MAC driver.
 *
 * NX_LINK_GET_STATUS reports the link DOWN rather than UP. Claiming UP would make DHCP start
 * trying to transmit through a driver that discards, which looks like a network fault instead
 * of an unimplemented driver.
 */
static VOID nx_driver_stub(NX_IP_DRIVER *request) {
    switch (request->nx_ip_driver_command) {
    case NX_LINK_INTERFACE_ATTACH:
    case NX_LINK_INITIALIZE:
    case NX_LINK_ENABLE:
    case NX_LINK_DISABLE:
    case NX_LINK_INTERFACE_DETACH:
        request->nx_ip_driver_status = NX_SUCCESS;
        break;

    case NX_LINK_GET_STATUS:
        *(request->nx_ip_driver_return_ptr) = NX_FALSE; /* link down: there is no driver yet */
        request->nx_ip_driver_status = NX_SUCCESS;
        break;

    /* The send commands, and ONLY these, carry a packet to dispose of.
     *
     * Releasing the packet matters even in a stub -- ARP and DHCP both try to transmit, and a
     * pool of 8 would be exhausted in seconds otherwise, presenting as a hang rather than as
     * silence. But it must be done for exactly the commands that have one:
     * nx_ip_driver_packet is only populated for these, and is uninitialised stack for the
     * rest. Releasing it unconditionally from a `default:` branch dereferenced garbage and
     * took the board down with an unaligned UsageFault (CFSR = 0x01000000) inside
     * _nxe_packet_transmit_release, before the console FIFO had even been drained. */
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
        /* Multicast join/leave, deferred processing, and anything else NetX Duo asks for.
           Reported as handled so the stack does not treat the interface as broken. */
        request->nx_ip_driver_status = NX_SUCCESS;
        break;
    }
}

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
                          &eth_pool, nx_driver_stub,
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

    eth_up = true;
    MSG("ETH: NetX Duo %u.%u.%u up (pool %u x %u B, stub driver -- no link yet)\n",
        (unsigned)NETXDUO_MAJOR_VERSION, (unsigned)NETXDUO_MINOR_VERSION,
        (unsigned)NETXDUO_PATCH_VERSION,
        (unsigned)NX_PACKET_POOL_PACKET_COUNT, (unsigned)NX_PACKET_POOL_PAYLOAD_SIZE);
    return true;
}
