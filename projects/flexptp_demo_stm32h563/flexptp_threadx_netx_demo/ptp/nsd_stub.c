/* A network stack driver that does nothing.
 *
 * S3 removes lwIP and does not yet add NetX Duo, so flexPTP has no way to send or receive.
 * Rather than take flexPTP out of the build for three stages, it is kept with this stub.
 *
 * WHY THAT IS WORTH DOING
 *
 * flexPTP registers 19 of the CLI's 23 commands. Dropping it would take the `cli` check from
 * a real assertion down to four commands for the whole middle of the port, and would make S6
 * a re-integration of flexPTP rather than just filling in a driver. With the stub, everything
 * above the network stack -- the CLI surface, the servo, the profile machinery, the hardware
 * port and the PTP clock registers -- stays compiled, linked and exercised throughout.
 *
 * WHAT IT DOES NOT DO
 *
 * Nothing is transmitted or received, so flexPTP will start, run its BMCA with no peers, and
 * settle into MASTER. `ptp_start` still passes (the addend is programmed by the hardware
 * port, not the stack); `ptp_ts` and `ptp_sync` cannot and are not run at these stages.
 *
 * This file is replaced by nsd_netxduo.c in S6, not extended.
 */

#include "flexptp/network_stack_driver.h"
#include "flexptp/ptp_types.h"

#include <stm32h5xx_hal_conf.h>   /* ETH_MAC_ADDR0..5, derived from the device UID */

/* The four functions network_stack_driver.h requires. All of them are reachable from the CLI
   -- `ptp transport`, `ptp delmech` and `ptp profile preset` all re-init the driver -- so
   they must be safe to call, not merely present. */

void ptp_nsd_init(const NsdInitSettings *init) {
    (void)init;
}

void ptp_nsd_get_interface_address(uint8_t *hwa) {
    /* The MAC address the Ethernet driver would use. Reported rather than zeroed because
       flexPTP derives its clockIdentity from it, and a zero clockIdentity would make the
       BMCA logs meaningless while looking superficially fine.
     *
     * Assigned, not statically initialised: ETH_MAC_ADDR0..5 read the device UID at
     * 0x08FFF800 through a volatile pointer, so they are runtime expressions rather than
     * constants and cannot appear in an initialiser list. */
    hwa[0] = ETH_MAC_ADDR0;
    hwa[1] = ETH_MAC_ADDR1;
    hwa[2] = ETH_MAC_ADDR2;
    hwa[3] = ETH_MAC_ADDR3;
    hwa[4] = ETH_MAC_ADDR4;
    hwa[5] = ETH_MAC_ADDR5;
}

void ptp_nsd_transmit_msg(RawPtpMessage *pMsg, uint32_t uid) {
    (void)pMsg;
    (void)uid;

    /* Deliberately NOT calling ptp_transmit_timestamp_cb() here.
     *
     * Faking a transmit timestamp would let the servo run on invented data and would defeat
     * the `ptp_ts` check, whose entire purpose is to detect a missing t3 (see finding 1 in
     * docs/threadx_netxduo_feasibility.md). A stack that cannot transmit should look like
     * one that cannot transmit. */
}

void ptp_nsd_igmp_join_leave(bool join) {
    (void)join;
}
