/* A model of the STM32H5 Ethernet DMA, good enough to drive mac_drv.c's descriptor logic.
 *
 * WHAT THIS IS
 *
 * The driver's job on receive is to walk the descriptor ring the DMA has written into, pair
 * each data descriptor with its context descriptor, hand the frame up, and return ownership.
 * To test that, something has to play the DMA's part: clear OWN bits, fill in lengths and
 * status flags, and place timestamps in context descriptors. That is this class.
 *
 * WHAT THIS IS NOT
 *
 * It is a model, and every behaviour it gets wrong is a test that passes for the wrong
 * reason. It is deliberately small and readable so a reader can check it against RM0481
 * rather than trust it. Known omissions, stated rather than hidden:
 *
 *   - No RBU/suspend semantics. The real engine, on reaching a descriptor the application
 *     still owns, raises RBU and STOPS until the tail pointer is written again -- which is
 *     finding F71, and the exact case where a naive model would have been wrong in the same
 *     way the code was. Tests here assert that the driver writes DMACRDTPR; they cannot
 *     prove the engine restarts, and that stays on the bench.
 *   - No multi-descriptor frames. The firmware's 1528-byte buffers hold a full MTU frame,
 *     so FD and LD are always both set in practice.
 *   - Write-back ordering between a data descriptor and its context descriptor is under the
 *     TEST's control, not the model's: deliver_frame_ts() writes both at once (the ordinary
 *     case), deliver_frame_ts_pending() writes only the first. The model does not decide
 *     which the hardware does -- it lets a test ask for either. An earlier version offered
 *     only the first, which is why NX-01 was invisible to a green suite.
 *   - No checksum offload, no MAC filtering, no PHY.
 *   - No memory ordering. __DSB/__DMB are no-ops on the host; whether the target's barriers
 *     are correctly placed is not observable here.
 */
#ifndef ETH_MODEL_HPP
#define ETH_MODEL_HPP

#include <cstdint>
#include <cstddef>
#include <vector>

extern "C" {
#include "host_arena.h"
#include "host_mmio.h"
#include "mac_drv.h"
}

namespace ethmodel {

/** One RX delivery as the driver reported it to ETHHW_ReadCallback(). */
struct RxEvent {
    std::uint16_t size;
    const void *payload;
    std::uint32_t ts_s;
    std::uint32_t ts_ns;
    ETHHW_DescFull *bd;
    ETHHW_DescFull *bd_ctx;
};

/** One transmit-timestamp callback as the driver dispatched it. */
struct TxTsEvent {
    std::uint32_t tag;
    std::uint32_t ts_s;
    std::uint32_t ts_ns;
};

/**
 * A configured Ethernet peripheral with rings in 32-bit-addressable memory.
 *
 * Construction runs the real ETHHW_Init(), so the register programming under test is the
 * driver's own -- the model does not set up the rings itself.
 */
class EthModel {
  public:
    static constexpr std::uint16_t kBufSize = 1528;

    EthModel(std::uint16_t rx_len = 8, std::uint16_t tx_len = 4);

    /** Deliver a frame to the ring as the DMA would, without a timestamp. */
    void deliver_frame(const std::uint8_t *data, std::uint16_t len);

    /** Deliver a frame plus the context descriptor carrying its timestamp. */
    void deliver_frame_ts(const std::uint8_t *data, std::uint16_t len,
                          std::uint32_t ts_s, std::uint32_t ts_ns);

    /**
     * Deliver a frame whose timestamp has NOT yet been written back.
     *
     * TSA is set on the data descriptor, but the following context descriptor is still owned
     * by the DMA -- the state ST's HAL handles by breaking out and retrying later
     * (stm32h5xx_hal_eth.c: "timestamp context descriptor is not ready, exit and retry
     * later"). Releasing the data descriptor before the context descriptor is a real
     * write-back ordering the databook permits, and reading the context slot in that state
     * yields whatever the driver last left there -- on a freshly armed descriptor, the buffer
     * address, which is a large and entirely plausible nanosecond value.
     */
    void deliver_frame_ts_pending(const std::uint8_t *data, std::uint16_t len);

    /** Write the context descriptor a previous deliver_frame_ts_pending() left outstanding. */
    void complete_pending_ts(std::uint32_t ts_s, std::uint32_t ts_ns);

    /**
     * Place a context descriptor with no data descriptor in front of it.
     *
     * Defence in depth rather than a state the MAC produces on its own: it is what the ring
     * is left holding if a context descriptor is ever released while the DMA still owns it.
     * Its DES3 carries CTXT and a zero length, so a driver that does not test for CTXT
     * delivers it upward as a zero-length frame.
     */
    void deliver_stray_context_descriptor(std::uint32_t ts_s, std::uint32_t ts_ns);

    /** Complete the oldest outstanding transmit, optionally capturing a timestamp. */
    void complete_tx(bool with_timestamp, std::uint32_t ts_s = 0, std::uint32_t ts_ns = 0);

    /** How many RX descriptors the application currently owns (OWN clear). */
    int rx_descs_owned_by_app() const;

    ETH_TypeDef *eth() const { return host_eth; }
    ETHHW_DescFull *rx_ring() const { return rx_ring_; }
    ETHHW_DescFull *tx_ring() const { return tx_ring_; }
    std::uint16_t rx_len() const { return rx_len_; }
    std::uint16_t tx_len() const { return tx_len_; }

    /* --- observation ---------------------------------------------------- */

    /** Frames the driver handed to ETHHW_ReadCallback() since the last clear. */
    static std::vector<RxEvent> &rx_events();
    /** Transmit timestamps the driver dispatched since the last clear. */
    static std::vector<TxTsEvent> &tx_ts_events();
    static void clear_events();

    /** What ETHHW_ReadCallback() should return. ETHHW_RET_RX_PROCESSED means "restore it for
     *  me now"; 0 means "I have queued it, I will restore it later" -- which is what the real
     *  rx_thread does, and the case F61 is about. */
    static int &read_callback_result();

  private:
    std::uint16_t rx_len_;
    std::uint16_t tx_len_;
    ETHHW_State *state_;
    ETHHW_DescFull *rx_ring_;
    ETHHW_DescFull *tx_ring_;
    std::uint8_t *buffers_;
    std::uint16_t rx_cursor_;  // where the model writes next, mirroring DMACCARDR
    std::uint16_t tx_reap_;    // oldest transmit not yet completed

    void advance_rx_cursor();
};

}  // namespace ethmodel

#endif  // ETH_MODEL_HPP
