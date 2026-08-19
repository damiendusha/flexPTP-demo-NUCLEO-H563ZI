/* A 32-bit-addressable allocation arena for the DMA structures under test.
 *
 * WHY THIS EXISTS
 *
 * mac_drv.c is written for a 32-bit target and stores addresses in 32-bit fields, because
 * that is what the hardware registers and descriptors are:
 *
 *     WRITE_REG(eth->DMACRDLAR, (uint32_t)init->rxRingPtr);   // ring base -> DMA register
 *     return ((ETHHW_State *)eth->DMACRDLAR) - 1;             // ...and recovered from it
 *     bd->ext.bufAddr = ((uint32_t)(rxBuf)) + alignedBufSize * i;
 *
 * On x86-64 those casts truncate and the recovered pointer is garbage. That is not a driver
 * defect -- on a Cortex-M33 a pointer *is* 32 bits -- it is an unavoidable host/target
 * mismatch, and it has to be solved without editing the driver.
 *
 * Two accommodations, together, make the round-trip lossless:
 *
 *   1. This arena, mapped with MAP_32BIT so every descriptor, buffer and state block sits
 *      below 2 GB.
 *   2. -no-pie on the test executable (see CMakeLists.txt), which puts .text at ~0x400000
 *      so that function pointers survive ETHHW_DescExt.tsCbPtr, which is also a uint32_t.
 *
 * The alternative was -m32, which is more faithful still -- it would match the target's
 * whole ABI, not just pointer width -- but it needs gcc-multilib installed. If you have it,
 * building with -m32 is strictly better and this arena becomes unnecessary.
 *
 * MAP_32BIT is Linux/x86-64 specific. On another host, allocate low memory some other way.
 */
#ifndef HOST_ARENA_H
#define HOST_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Allocate `size` bytes, zeroed, aligned to `align`, at an address that fits in uint32_t.
 *  Aborts on failure -- a test that cannot get low memory must not silently run high. */
void *host_arena_alloc(size_t size, size_t align);

/** Release everything handed out so far. Call between tests. */
void host_arena_reset(void);

/** True if `p` survives a round-trip through uint32_t. Used by the arena's own guard test. */
int host_arena_fits32(const void *p);

#ifdef __cplusplus
}
#endif

#endif /* HOST_ARENA_H */
