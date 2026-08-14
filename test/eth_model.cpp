#include "eth_model.hpp"

#include <cstring>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include "stm32h5xx_hal.h"
}

namespace ethmodel {

namespace {
std::vector<RxEvent> g_rx_events;
std::vector<TxTsEvent> g_tx_ts_events;
int g_read_cb_result = ETHHW_RET_RX_PROCESSED;

/* The 32-bit view of a pointer, as the DMA registers and descriptor fields hold it. Safe
   only because everything it is applied to lives in the MAP_32BIT arena; the assertion
   makes a violation loud rather than silently truncating. C++ will not narrow implicitly,
   which is why this is spelled out rather than a bare cast. */
std::uint32_t lo32(const void *p) {
    auto v = reinterpret_cast<std::uintptr_t>(p);
    if ((v >> 32) != 0) {
        std::fprintf(stderr, "eth_model: %p is outside the 32-bit arena\n", p);
        std::abort();
    }
    return static_cast<std::uint32_t>(v);
}
}  // namespace

std::vector<RxEvent> &EthModel::rx_events() { return g_rx_events; }
std::vector<TxTsEvent> &EthModel::tx_ts_events() { return g_tx_ts_events; }
int &EthModel::read_callback_result() { return g_read_cb_result; }

void EthModel::clear_events() {
    g_rx_events.clear();
    g_tx_ts_events.clear();
    g_read_cb_result = ETHHW_RET_RX_PROCESSED;
}

EthModel::EthModel(std::uint16_t rx_len, std::uint16_t tx_len)
    : rx_len_(rx_len), tx_len_(tx_len), rx_cursor_(0), tx_reap_(0) {
    host_arena_reset();
    host_mmio_reset();
    clear_events();

    /* ETHHW_GetState() recovers the state block as ((ETHHW_State *)DMACRDLAR) - 1, so it
     * must sit immediately below the RX ring with no padding. The firmware guarantees this
     * with a _Static_assert on a struct that holds both; here it is done by allocating one
     * block and carving it, which reproduces the same layout contract. */
    const std::size_t state_sz = sizeof(ETHHW_State);
    const std::size_t rx_sz = sizeof(ETHHW_DescFull) * rx_len_;
    const std::size_t tx_sz = sizeof(ETHHW_DescFull) * tx_len_;

    auto *block = static_cast<std::uint8_t *>(host_arena_alloc(state_sz + rx_sz + tx_sz, 16));
    state_ = reinterpret_cast<ETHHW_State *>(block);
    rx_ring_ = reinterpret_cast<ETHHW_DescFull *>(block + state_sz);
    tx_ring_ = reinterpret_cast<ETHHW_DescFull *>(block + state_sz + rx_sz);

    buffers_ = static_cast<std::uint8_t *>(
        host_arena_alloc(static_cast<std::size_t>(kBufSize) * (rx_len_ + tx_len_), 16));

    ETHHW_InitOpts opts{};
    opts.statePtr = state_;
    opts.rxRingLen = rx_len_;
    opts.rxRingPtr = reinterpret_cast<std::uint8_t *>(rx_ring_);
    opts.txRingLen = tx_len_;
    opts.txRingPtr = reinterpret_cast<std::uint8_t *>(tx_ring_);
    opts.bufPtr = buffers_;
    opts.blockSize = kBufSize;
    const std::uint8_t mac[6] = {0x56, 0x00, 0x1e, 0xff, 0xfe, 0x00};
    std::memcpy(opts.mac, mac, sizeof(mac));

    // The driver's own initialisation, not the model's: this is part of what is under test.
    ETHHW_Init(host_eth, &opts);

    // The DMA's "current descriptor" register. The driver reads it to find where to start.
    host_eth->DMACCARDR = lo32(rx_ring_);
}

void EthModel::advance_rx_cursor() {
    rx_cursor_ = static_cast<std::uint16_t>((rx_cursor_ + 1) % rx_len_);
    host_eth->DMACCARDR = lo32(rx_ring_ + rx_cursor_);
}

void EthModel::deliver_frame(const std::uint8_t *data, std::uint16_t len) {
    ETHHW_DescFull *bd = rx_ring_ + rx_cursor_;

    std::memcpy(reinterpret_cast<void *>(static_cast<std::uintptr_t>(bd->ext.bufAddr)), data, len);

    // Hand ownership to the application: OWN clear, first and last descriptor of the frame,
    // length in the low 14 bits of DES3. TSA clear -- no timestamp accompanies this frame.
    bd->desc.DES1 = 0;
    bd->desc.DES3 = static_cast<std::uint32_t>(len) | ETH_DMARXNDESCWBF_FD | ETH_DMARXNDESCWBF_LD;

    advance_rx_cursor();
}

void EthModel::deliver_frame_ts(const std::uint8_t *data, std::uint16_t len,
                                std::uint32_t ts_s, std::uint32_t ts_ns) {
    ETHHW_DescFull *bd = rx_ring_ + rx_cursor_;

    std::memcpy(reinterpret_cast<void *>(static_cast<std::uintptr_t>(bd->ext.bufAddr)), data, len);

    // TSA says "a context descriptor follows carrying this frame's timestamp".
    bd->desc.DES1 = ETH_DMARXNDESCWBF_TSA;
    bd->desc.DES3 = static_cast<std::uint32_t>(len) | ETH_DMARXNDESCWBF_FD | ETH_DMARXNDESCWBF_LD;

    advance_rx_cursor();

    // The context descriptor: seconds in DES1, nanoseconds in DES0, CTXT set, OWN clear.
    ETHHW_DescFull *ctx = rx_ring_ + rx_cursor_;
    ctx->desc.DES0 = ts_ns;
    ctx->desc.DES1 = ts_s;
    ctx->desc.DES3 = ETH_DMARXNDESCWBF_CTXT;

    advance_rx_cursor();
}

void EthModel::complete_tx(bool with_timestamp, std::uint32_t ts_s, std::uint32_t ts_ns) {
    ETHHW_DescFull *bd = tx_ring_ + tx_reap_;

    // The DMA clears OWN when it has transmitted the frame; TTSS says a timestamp was
    // captured, and it lands in DES0/DES1 of the same descriptor on write-back.
    bd->desc.DES3 &= ~ETH_DMATXNDESCWBF_OWN;
    if (with_timestamp) {
        bd->desc.DES0 = ts_ns;
        bd->desc.DES1 = ts_s;
        bd->desc.DES3 |= ETH_DMATXNDESCWBF_TTSS;
    }

    tx_reap_ = static_cast<std::uint16_t>((tx_reap_ + 1) % tx_len_);
}

int EthModel::rx_descs_owned_by_app() const {
    int n = 0;
    for (std::uint16_t i = 0; i < rx_len_; i++) {
        if (!(rx_ring_[i].desc.DES3 & ETH_DMARXNDESCRF_OWN)) {
            n++;
        }
    }
    return n;
}

}  // namespace ethmodel

/* --- the driver's callbacks, captured -------------------------------------- */

extern "C" int ETHHW_ReadCallback(ETHHW_EventDesc *evt) {
    if (evt->type != ETHHW_EVT_RX_READ) {
        return 0;
    }
    ethmodel::EthModel::rx_events().push_back(ethmodel::RxEvent{
        evt->data.rx.size, evt->data.rx.payload, evt->data.rx.ts_s, evt->data.rx.ts_ns,
        evt->bd, evt->bd_ctx});
    return ethmodel::EthModel::read_callback_result();
}

extern "C" int ETHHW_EventCallback(ETHHW_EventDesc *evt) {
    (void)evt;
    return 0;
}

/* The transmit-timestamp callback the driver invokes through ETHHW_DescExt.tsCbPtr. Its
   address must fit in a uint32_t, which is what -no-pie buys -- see test/host/host_arena.h. */
extern "C" void eth_model_tx_ts_cb(std::uint32_t ts_s, std::uint32_t ts_ns, void *tag) {
    ethmodel::EthModel::tx_ts_events().push_back(ethmodel::TxTsEvent{
        static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(tag)), ts_s, ts_ns});
}
