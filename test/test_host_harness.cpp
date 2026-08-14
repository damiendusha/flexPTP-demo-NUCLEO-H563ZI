/* Tests of the test harness itself.
 *
 * Two of these guard the host/target accommodations described in test/host/host_arena.h. If
 * either regresses -- someone drops -no-pie, or MAP_32BIT stops returning low memory -- the
 * driver tests would not fail cleanly, they would corrupt memory or jump into nowhere. These
 * fail first, and say why.
 */

#include <gtest/gtest.h>

#include "eth_model.hpp"

using ethmodel::EthModel;

extern "C" void eth_model_tx_ts_cb(std::uint32_t, std::uint32_t, void *);

TEST(HostHarness, ArenaReturnsThirtyTwoBitAddresses) {
    void *p = host_arena_alloc(4096, 16);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(host_arena_fits32(p))
        << "arena handed out " << p << ", which will truncate when the driver stores it in a "
        << "uint32_t descriptor field";
}

TEST(HostHarness, FunctionPointersFitInThirtyTwoBits) {
    // ETHHW_DescExt.tsCbPtr is a uint32_t. Without -no-pie the callback address is above
    // 4 GB, truncates on store, and the first transmit-timestamp dispatch jumps into nowhere.
    auto addr = reinterpret_cast<std::uintptr_t>(&eth_model_tx_ts_cb);
    EXPECT_EQ(addr >> 32, 0u)
        << "callback at 0x" << std::hex << addr
        << " does not fit in ETHHW_DescExt.tsCbPtr; is -no-pie still set?";
}

TEST(HostHarness, InitProgramsRingGeometry) {
    EthModel eth(8, 4);

    EXPECT_EQ(eth.eth()->DMACRDRLR, 8u - 1u) << "RX ring length register";
    EXPECT_EQ(eth.eth()->DMACTDRLR, 4u - 1u) << "TX ring length register";
    EXPECT_EQ(eth.eth()->DMACRDLAR,
              static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(eth.rx_ring())));
    EXPECT_EQ(eth.eth()->DMACTDLAR,
              static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(eth.tx_ring())));
}

TEST(HostHarness, InitArmsEveryReceiveDescriptor) {
    EthModel eth(8, 4);
    // Every RX descriptor should be owned by the DMA after init: nothing to process yet.
    EXPECT_EQ(eth.rx_descs_owned_by_app(), 0);
}

TEST(HostHarness, StateBlockIsRecoverableFromTheRingRegister) {
    // ETHHW_GetState() derives the state pointer as ((ETHHW_State *)DMACRDLAR) - 1. The
    // firmware protects this with a _Static_assert; reproduce the contract here so a layout
    // change breaks the host tests too.
    EthModel eth(8, 4);
    auto *from_reg = reinterpret_cast<ETHHW_State *>(
                         static_cast<std::uintptr_t>(eth.eth()->DMACRDLAR)) - 1;
    EXPECT_EQ(reinterpret_cast<std::uint8_t *>(from_reg) + sizeof(ETHHW_State),
              reinterpret_cast<std::uint8_t *>(eth.rx_ring()));
}

TEST(HostHarness, CrcStrippingIsEnabled) {
    // Finding N13/F76: CST and ACS were both clear, so the MAC handed the 4-byte FCS up with
    // every frame and the length in DES3 counted it. Nothing misbehaved visibly, which is
    // exactly why it is worth pinning.
    EthModel eth(8, 4);
    EXPECT_TRUE(eth.eth()->MACCR & ETH_MACCR_CST) << "CRC stripping for Type frames (CST)";
    EXPECT_TRUE(eth.eth()->MACCR & ETH_MACCR_ACS) << "automatic pad/CRC stripping (ACS)";
}
