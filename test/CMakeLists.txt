# Host-side unit tests.
#
# This is a SEPARATE CMake root, not a target in the firmware build, because the top-level
# CMakeLists.txt includes cmake/gcc-arm-none-eabi.cmake unconditionally -- there is no flag
# that would make it produce a host binary.
#
#   cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test
#
# What is tested here, and what deliberately is not, is set out in
# docs/threadx_porting_plan.md ("How far mocks actually reach"). In short: the descriptor
# state machine and pure decision logic are reachable off-bench; codegen defects (F70, F62)
# and anything downstream of the timestamp capture are not, and a green suite here is no
# evidence about them.

cmake_minimum_required(VERSION 3.22)
project(flexptp-demo-host-tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REPO_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# ---------------------------------------------------------------------------
# GoogleTest, pinned.
#
# Pinned for the same reason the FreeRTOS kernel is pinned in Middlewares/CMakeLists.txt: a
# floating dependency turns an unrelated upstream change into a mystery failure in this
# project's CI.
# ---------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# ---------------------------------------------------------------------------
# The driver under test, compiled for the host.
# ---------------------------------------------------------------------------
#
# Include order matters: test/host must come FIRST so its core_cm33.h, system_stm32h5xx.h and
# cmsis_compiler.h shadow the ARM ones. Everything below that is the real thing -- ST's CMSIS
# device header and ST's HAL headers -- so the register layouts and bit positions under test
# are the same definitions the firmware compiles against, and they follow the pinned HAL
# submodule automatically. Nothing is hand-copied.
set(HOST_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/host
    ${REPO_ROOT}/Drivers/CMSIS/Device/ST/STM32H5xx/Include
    ${REPO_ROOT}/Drivers/stm32h5xx-hal-driver/Inc
    ${REPO_ROOT}/Inc
    ${REPO_ROOT}/Src
    ${REPO_ROOT}/Drivers/EthDrv
    ${CMAKE_CURRENT_LIST_DIR}
)

add_library(host_mac_drv STATIC
    mac_drv_uut.c
    host/host_mmio.c
    host/host_arena.c
)
target_include_directories(host_mac_drv PUBLIC ${HOST_INCLUDES})
target_compile_definitions(host_mac_drv PUBLIC STM32H563xx USE_HAL_DRIVER)

# The driver casts pointers to uint32_t because on the target they are 32 bits. On x86-64
# that warns everywhere and says nothing useful -- the truncation is handled by keeping every
# such object in the MAP_32BIT arena (see test/host/host_arena.h). Silence just this class,
# on this target only; the firmware build keeps -Wall -Wextra and is unaffected.
target_compile_options(host_mac_drv PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
)

# -no-pie puts .text at ~0x400000 so that function pointers still fit in the uint32_t
# ETHHW_DescExt.tsCbPtr field. Without it the transmit-timestamp callback pointer truncates
# and the first dispatch jumps into nowhere. Must apply to the executables too, so PUBLIC.
target_link_options(host_mac_drv PUBLIC -no-pie)
target_compile_options(host_mac_drv PUBLIC -fno-pie)

# ---------------------------------------------------------------------------
# Test executables
# ---------------------------------------------------------------------------
add_library(eth_model STATIC eth_model.cpp)
target_link_libraries(eth_model PUBLIC host_mac_drv)

function(add_host_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE eth_model GTest::gtest_main)
    target_compile_options(${name} PRIVATE -Wall -Wextra)

    # A timeout is not boilerplate here. Some of the defects these tests pin present as a
    # HANG rather than a wrong value: reintroducing F57 -- hoisting ETHHW_ProcessTx()'s
    # per-pass descriptor count out of the retry loop -- makes `found` never reach zero, so
    # the drain loop spins forever. Verified by mutation: without this, the suite wedges and
    # CI waits for its job limit instead of reporting a failure.
    #
    # Whole suite runs in 0.04 s, so 10 s is ~250x headroom and still fails fast.
    gtest_discover_tests(${name} PROPERTIES TIMEOUT 10)
endfunction()

# The NSD/driver logic units. They depend on nothing but stdint, which is the whole point of
# extracting them -- no ThreadX, no NetX Duo, no fakes.
add_library(ptp_units STATIC
    ${REPO_ROOT}/Src/flexptp_nsd/ptp_tx_registry.cpp
    ${REPO_ROOT}/Src/flexptp_nsd/ptp_ts_codec.cpp
    ${REPO_ROOT}/Src/flexptp_nsd/ptp_endpoint.cpp
)
target_include_directories(ptp_units PUBLIC ${REPO_ROOT}/Src/flexptp_nsd)
target_compile_options(ptp_units PRIVATE -Wall -Wextra)

function(add_unit_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ptp_units GTest::gtest_main)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    gtest_discover_tests(${name} PROPERTIES TIMEOUT 10)
endfunction()

add_unit_test(test_ptp_tx_registry)
add_unit_test(test_ptp_ts_codec)
add_unit_test(test_ptp_endpoint)

add_host_test(test_host_harness)
add_host_test(test_rx_ring)
add_host_test(test_tx_ring)
