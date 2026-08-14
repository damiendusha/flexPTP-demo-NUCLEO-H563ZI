/* NetX Duo configuration for this project.
 *
 * Reached from ports/cortex_m33/gnu/inc/nx_port.h, which includes this file only when
 * NX_INCLUDE_USER_DEFINE_FILE is defined -- see Middlewares/netxduo.cmake.
 *
 * The counterpart of the old Inc/lwipopts.h. Every setting below that differs from the NetX
 * Duo default is a decision with a reason, and the reasons are here rather than in the
 * porting document, because this is the file someone will be looking at when it matters.
 */

#ifndef NX_USER_H
#define NX_USER_H

/* --- Required for PTP hardware timestamping ---------------------------------------------- */

/* The whole reason this port is worth doing.
 *
 * Enables NX_PACKET.nx_packet_interface_capability_flag, which is how a transmit request
 * carries NX_INTERFACE_CAPABILITY_PTP_TIMESTAMP down to the driver. Without it there is no
 * per-packet field at all and the driver cannot tell which frames need a timestamp.
 *
 * The IP send path ORs into that flag rather than overwriting it (verified in
 * _nx_ip_packet_send), so a flag the NSD sets before sending survives to the driver. */
#define NX_ENABLE_INTERFACE_CAPABILITY

/* --- Disabled: not used, and each costs code or RAM --------------------------------------- */

/* IPv6 and its dependents. This demo is IPv4-only -- flexPTP's UDP transport is
   224.0.1.129/224.0.0.107 and its L2 transport is EtherType 0x88F7 -- and the two IPv6 CVEs
   in the 6.4.x line (CVE-2025-55094, CVE-2025-55102) are simply not reachable with it off. */
#define NX_DISABLE_IPV6

/* No TCP anywhere in this firmware: PTP is UDP and raw L2, DHCP is UDP, ICMP is its own
   protocol. Disabling it is the single largest code saving available here. */
#define NX_DISABLE_TCP

/* Fragmentation. The largest frame this board sends is a 1400-byte ping reply and the
   largest PTP message is well under 200 bytes, so nothing fragments. Note that NetX Duo
   requires packet chaining to stay enabled if fragmentation is disabled -- so
   NX_DISABLE_PACKET_CHAIN is deliberately NOT defined below. */
#define NX_DISABLE_FRAGMENTATION

/* --- Checksums ----------------------------------------------------------------------------- */

/* Left in software for now, deliberately.
 *
 * The H563 MAC can offload IPv4/UDP/TCP checksums and the driver programs MACCR.IPC to do it
 * (mac_drv.c), but enabling NX_DISABLE_*_CHECKSUM here is a claim about the DRIVER's
 * behaviour, not the MAC's: NetX Duo would stop computing them and rely on the interface
 * capability flags being correct end to end. That is a thing to turn on in S5c with ping
 * proving it, not something to assume while the driver is still a stub. */

/* --- Sizing ---------------------------------------------------------------------------------- */

/* Retained from the lwIP configuration this replaces, and for the same reasons.
 *
 * PBUF_POOL_BUFSIZE was 1528 -- a 1514-byte maximum Ethernet frame rounded up, matching the
 * DMA buffer size in the MAC driver. NetX Duo wants its payload 4-byte aligned and adds its
 * own header, so the pool payload is the same 1528.
 *
 * PBUF_POOL_SIZE was 8, and architecture.md records that as the measured throughput limit --
 * 300 pings of 1400 bytes at 100, 250 and 500 packets/s all at 0% loss. Kept at 8 so the
 * comparison against the S0 baseline is like for like; if receive drops appear in S5c, this
 * is the number to raise. */
#define NX_PACKET_POOL_PAYLOAD_SIZE 1528
#define NX_PACKET_POOL_PACKET_COUNT 8

/* --- Diagnostics ------------------------------------------------------------------------------ */

#ifdef DEBUG
/* Per-interface and per-protocol counters, reachable from the CLI. Cheap, and the lwIP
   equivalents (LINK_STATS/MIB2) were the only way F75 and F76 were ever quantified. */
#define NX_ENABLE_INTERFACE_CAPABILITY_INFO
#else
/* Error checking costs a call and a few comparisons per API. Kept in Debug for the same
   reason ThreadX's is, dropped in Release. */
#define NX_DISABLE_ERROR_CHECKING
#endif

#endif /* NX_USER_H */
