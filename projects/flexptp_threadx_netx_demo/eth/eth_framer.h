/* Ethernet framing decisions, separated from NetX Duo so they can be tested.
 *
 * WHY THIS EXISTS
 *
 * drv_send() in eth_drv_netxduo.c has the highest defect density of anything in this port --
 * three of the five bring-up defects were in or immediately around it: the address-mapping
 * flag, the checksum-offload declaration, and the 2-byte alignment shim. It also had no
 * automated coverage at all, because every line of it is entangled with NX_PACKET,
 * NX_IP_DRIVER and ETHHW_Transmit (finding TS-04).
 *
 * Not all of it can be lifted out, and pretending otherwise would produce an abstraction that
 * is harder to read than the thing it replaced. What CAN be lifted is the part that is pure
 * decision and pure byte-shuffling, and that is also the part where a mistake is invisible:
 *
 *   - which EtherType goes with which driver command;
 *   - the destination/source MAC byte order, written out of NetX Duo's msw/lsw split -- twelve
 *     hand-written shifts whose failure mode is "a MAC that looks plausible in a debugger and
 *     is wrong on the wire";
 *   - the saturating append that flattens a packet chain into one buffer.
 *
 * The rest -- ownership, release, cancelling the pending timestamp on every drop path -- stays
 * in the driver, where it belongs, and is exercised on hardware.
 *
 * C++ per the rules in Src/cpp_support.cpp: no allocation, no exceptions, extern "C" at the
 * boundary. All storage is the caller's.
 */
#ifndef ETH_FRAMER_H
#define ETH_FRAMER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ETH_FRAMER_HEADER_SIZE (14U)

/** What the frame carries, in the driver's terms rather than NetX Duo's. */
typedef enum {
    ETH_FRAMER_PAYLOAD_IPV4 = 0,
    ETH_FRAMER_PAYLOAD_IPV6,
    ETH_FRAMER_PAYLOAD_ARP,
    ETH_FRAMER_PAYLOAD_RARP
} EthFramerPayload;

/** The EtherType for a payload kind, big-endian on the wire but returned in host order. */
uint16_t eth_framer_ethertype(EthFramerPayload payload);

/**
 * Write the 14-byte Ethernet header at @p out.
 *
 * Addresses arrive as NetX Duo's msw/lsw split: the top 16 bits of the MAC in @p msw, the
 * bottom 32 in @p lsw. Getting that split wrong is the failure this function exists to make
 * testable -- it produces a frame that is perfectly formed except for its addresses, which
 * nothing accepts and no counter anywhere reports.
 *
 * @p out must have room for ETH_FRAMER_HEADER_SIZE bytes.
 */
void eth_framer_write_header(uint8_t *out, uint32_t dst_msw, uint32_t dst_lsw,
                             uint32_t src_msw, uint32_t src_lsw, uint16_t ethertype);

/**
 * Append @p len bytes to @p dst at @p offset, saturating at @p cap.
 *
 * Returns the new offset. Copies nothing and returns @p cap once full, so a caller can loop a
 * whole packet chain through it without checking each time and cannot be made to write past
 * the buffer by a chain whose declared length disagrees with its contents.
 */
uint32_t eth_framer_append(uint8_t *dst, uint32_t cap, uint32_t offset,
                           const uint8_t *src, uint32_t len);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ETH_FRAMER_H
