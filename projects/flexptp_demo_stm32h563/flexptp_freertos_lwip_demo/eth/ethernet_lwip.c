#include "ethernet_lwip.h"

#include "eth/eth_drv_lwip.h"
#include "cmsis_os2.h"
#include "lwip/dhcp.h"
#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "standard_output/standard_output.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static ip4_addr_t ipaddr;
static ip4_addr_t netmask;
static ip4_addr_t router;
static struct netif intf;

err_t ethernetif_init(struct netif *netif);

static osTimerId_t checkDhcpTmr;

static bool prevDhcpOK = false;

/* Runs on the "Tmr Svc" thread, which is not the lwIP core thread. With
 * LWIP_TCPIP_CORE_LOCKING = 1 every lwIP API call must hold the core lock, and this made two
 * without it: dhcp_supplied_address() walks the netif's DHCP state, and ipaddr_ntoa() reads
 * netif_default->ip_addr while the stack may be rewriting it on lease renewal.
 *
 * It is the only place in the project that got this wrong -- phy_thread goes through
 * tcpip_callback(), and flexPTP's nsd_lwip.c brackets its sends with LOCK_TCPIP_CORE() -- so
 * this was an oversight rather than a convention.
 *
 * The lock is held only across the reads; the printing is done outside it, since MSG() can
 * block on the console FIFO and holding the core lock there would stall the whole stack. */
void check_dhcp_state(void *param) {
    LOCK_TCPIP_CORE();
    bool dhcpOK = dhcp_supplied_address(&intf);
    const char *addr = dhcpOK ? ipaddr_ntoa(&(netif_default->ip_addr)) : NULL;
    char addrCopy[IP4ADDR_STRLEN_MAX];
    if (addr != NULL) {
        strncpy(addrCopy, addr, sizeof(addrCopy) - 1U);
        addrCopy[sizeof(addrCopy) - 1U] = '\0';
    }
    UNLOCK_TCPIP_CORE();

    if (dhcpOK != prevDhcpOK) {
        if (dhcpOK) {
            MSG("IP-address: %s\n", addrCopy);
        } else {
            MSG("DHCP configuration lost!\n");
        }
        prevDhcpOK = dhcpOK;
    }
}

void link_chg_cb(struct netif *netif) {
    bool ls = netif_is_link_up(netif);
    MSG("ETH LINK: %s%s", (ls ? (ANSI_COLOR_BGREEN "UP ") : (ANSI_COLOR_BRED "DOWN\n")), ANSI_COLOR_RESET);

    if (ls) {
        LinkState *linkState = (LinkState *)netif->state;
        MSG("(%u Mbps, %s duplex)\n", linkState->speed, linkState->duplex ? "FULL" : "HALF");
    }

    if (ls) {
        dhcp_start(netif);
    } else {
        dhcp_stop(netif);
    }
}

void init_ethernet() {
    // initialize lwIP
    tcpip_init(NULL, NULL);

    // clear all associated addresses
    ip_addr_set_zero_ip4(&ipaddr);
    ip_addr_set_zero_ip4(&netmask);
    ip_addr_set_zero_ip4(&router);

    /* All three of these are lwIP API calls made from the "init" thread, and with
     * LWIP_TCPIP_CORE_LOCKING = 1 every one of them requires the core lock. They ran without
     * it since the port began -- netif_add() asserts it explicitly (netif.c:297) -- and
     * nothing said so, because lwIP's assertions could not print or halt (F72). The first
     * boot after that was fixed stopped here with "Function called without core lock", and
     * the backtrace named this line.
     *
     * Same class as P3.6, which fixed check_dhcp_state() by inspection. This one is the reason
     * the automated check was worth turning back on.
     *
     * The lock is held across netif_add() deliberately: it calls ethernetif_init(), which
     * calls netif_set_up() -- another API that asserts the lock -- so bracketing the whole
     * group is both simpler and more correct than three separate critical sections. */
    LOCK_TCPIP_CORE();

    // add network interface
    netif_add(&intf,
              &ipaddr,
              &netmask,
              &router,
              NULL,
              ethernetif_init,
              tcpip_input);

    // make it default
    netif_set_default(&intf);

    // register a link change callback
    netif_set_link_callback(&intf, link_chg_cb);

    UNLOCK_TCPIP_CORE();

    // initialize and start the DHCP-handling
    checkDhcpTmr = osTimerNew(check_dhcp_state, osTimerPeriodic, NULL, NULL);
    osTimerStart(checkDhcpTmr, 1000);
}

__attribute__((weak)) err_t hook_unknown_ethertype(struct pbuf *pbuf, struct netif *netif) {
    pbuf_free(pbuf);

    return ERR_OK;
}
