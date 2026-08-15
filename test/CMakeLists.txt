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
# Static analysis, on the first-party code that compiles for the host.
#
# GCC's -fanalyzer is a real interprocedural checker -- double-free, use-after-free, NULL
# dereference, leaks, uninitialised reads -- and it costs nothing to enable here, where the
# first-party sources already build with the system compiler (finding GI-03).
#
# Scoped to OUR targets. The vendored trees (ThreadX, NetX Duo, the CMSIS wrapper, GoogleTest)
# are compiled with -w already and a finding in them is not this project's to fix; the same
# reasoning as CI's first-party-warnings gate.
#
# Deliberately NOT -Werror: -fanalyzer has false positives, and a checker that blocks the suite
# on one is a checker somebody disables. It is loud in the build log and CI's warning gate is
# what turns first-party warnings into failures.
set(FIRST_PARTY_ANALYSIS -fanalyzer)


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
    ${FIRST_PARTY_ANALYSIS}
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
    ${REPO_ROOT}/Src/flexptp_nsd/ptp_nsd_lifecycle.cpp
)
target_include_directories(ptp_units PUBLIC ${REPO_ROOT}/Src/flexptp_nsd)
target_compile_options(ptp_units PRIVATE -Wall -Wextra ${FIRST_PARTY_ANALYSIS})

function(add_unit_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ptp_units GTest::gtest_main)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    gtest_discover_tests(${name} PROPERTIES TIMEOUT 10)
endfunction()

add_unit_test(test_ptp_tx_registry)
add_unit_test(test_ptp_ts_codec)
add_unit_test(test_ptp_endpoint)
add_unit_test(test_ptp_nsd_lifecycle)

# The framing decisions lifted out of drv_send() (TS-04/CX-05). Depends on nothing, like the
# NSD units, so it gets its own tiny library rather than joining ptp_units.
add_library(eth_framer STATIC ${REPO_ROOT}/Drivers/EthDrv/eth_framer.cpp)
target_include_directories(eth_framer PUBLIC ${REPO_ROOT}/Drivers/EthDrv)
target_compile_options(eth_framer PRIVATE -Wall -Wextra ${FIRST_PARTY_ANALYSIS})

add_library(eth_link_state STATIC ${REPO_ROOT}/Drivers/EthDrv/eth_link_state.cpp)
target_include_directories(eth_link_state PUBLIC ${REPO_ROOT}/Drivers/EthDrv)
target_compile_options(eth_link_state PRIVATE -Wall -Wextra ${FIRST_PARTY_ANALYSIS})

add_executable(test_eth_link_state test_eth_link_state.cpp)
target_link_libraries(test_eth_link_state PRIVATE eth_link_state GTest::gtest_main)
target_compile_options(test_eth_link_state PRIVATE -Wall -Wextra)
gtest_discover_tests(test_eth_link_state PROPERTIES TIMEOUT 10)

add_executable(test_eth_framer test_eth_framer.cpp)
target_link_libraries(test_eth_framer PRIVATE eth_framer GTest::gtest_main)
target_compile_options(test_eth_framer PRIVATE -Wall -Wextra)
gtest_discover_tests(test_eth_framer PROPERTIES TIMEOUT 10)

# Header-only, so it needs no library -- just Src/ on the include path, which HOST_INCLUDES
# already provides via ptp_units' PUBLIC directories? It does not: ptp_units exports only
# Src/flexptp_nsd. Point at Src/ explicitly.
add_executable(test_sysmem_bounds test_sysmem_bounds.cpp)
target_include_directories(test_sysmem_bounds PRIVATE ${REPO_ROOT}/Src)
target_link_libraries(test_sysmem_bounds PRIVATE GTest::gtest_main)
target_compile_options(test_sysmem_bounds PRIVATE -Wall -Wextra)
gtest_discover_tests(test_sysmem_bounds PROPERTIES TIMEOUT 10)

add_host_test(test_host_harness)
add_host_test(test_rx_ring)
add_host_test(test_tx_ring)

# ---------------------------------------------------------------------------
# Real ThreadX on the host, for context-rule tests (S5e Option B).
#
# The one class of defect this port kept getting wrong is "a blocking call from a context that
# may not block" -- three times, all in flexPTP's CMSIS-RTOS2 binding:
#
#   * the transmit-timestamp callback, from the Ethernet ISR
#   * the heartbeat, from ThreadX's timer thread (F80 -- cost a bench session)
#   * and a comment of mine in Inc/tx_user.h reasoning the second was safe because it was not
#     the first
#
# None of that needs NetX Duo, a RAM driver or a PTP exchange. It needs REAL ThreadX, because
# the restriction lives in txe_queue_send.c -- and that file is in common/src, so the Linux
# port enforces it identically to the Cortex-M33 one. That is what makes this testable at all.
#
# Kept as its own target: it links a real kernel and spawns real pthreads, so it does not
# belong in the same binary as the sub-millisecond logic tests.
# ---------------------------------------------------------------------------
set(THREADX_DIR ${REPO_ROOT}/Modules/threadx)

if (EXISTS ${THREADX_DIR}/common/inc/tx_api.h)
    file(GLOB TX_HOST_COMMON ${THREADX_DIR}/common/src/*.c)
    file(GLOB TX_HOST_PORT ${THREADX_DIR}/ports/linux/gnu/src/*.c)

    add_library(host_threadx STATIC ${TX_HOST_COMMON} ${TX_HOST_PORT})
    target_include_directories(host_threadx PUBLIC
        ${THREADX_DIR}/common/inc
        ${THREADX_DIR}/ports/linux/gnu/inc
        ${REPO_ROOT}/Inc                       # the FIRMWARE's tx_user.h, deliberately
    )
    # Inc/tx_user.h, not a host copy. The plan flagged a second configuration of the world as
    # a way for host and target to drift apart; sharing the header means a test that passes
    # here is a test of the kernel the board actually runs.
    target_compile_definitions(host_threadx PUBLIC
        TX_INCLUDE_USER_DEFINE_FILE
        # The Linux port hands ThreadX a single malloc() of this size as its unused memory,
        # and the CMSIS-RTOS2 wrapper carves BOTH byte pools out of it. The default 64000 is
        # smaller than the pools below, and overrunning it is not an allocation failure --
        # _tx_byte_pool_create() writes its first block header past the end and segfaults.
        # Must stay comfortably larger than RTOS2_BYTE_POOL_STACK_SIZE + _HEAP_SIZE.
        TX_LINUX_MEMORY_SIZE=4194304
    )
    target_compile_options(host_threadx PRIVATE -w)  # vendored tree, not ours to lint
    find_package(Threads REQUIRED)
    target_link_libraries(host_threadx PUBLIC Threads::Threads)

    add_library(host_cmsis_rtos STATIC ${REPO_ROOT}/Modules/cmsis_rtos_threadx/cmsis_os2.c)
    # test/host FIRST, so its cmsis_compiler.h shadows ARM's. ARM's pulls in cmsis_gcc.h,
    # whose __get_IPSR() emits `mrs r0, ipsr` -- an instruction x86 cannot assemble. The shim
    # supplies a host __get_IPSR() that always returns 0, and says what that costs.
    target_include_directories(host_cmsis_rtos PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/host
        ${REPO_ROOT}/Modules/cmsis_rtos_threadx
        ${REPO_ROOT}/Drivers/CMSIS/CMSIS_RTOS_V2
    )
    # Pool sizes deliberately DIFFER from the firmware's.
    #
    # tx_user.h is shared so the kernel behaves identically, but these two are not kernel
    # behaviour -- they are how much RAM the wrapper is given, and the host is running
    # GoogleTest rather than a 2 KB CLI thread. Sizing them like the board's 24 KB just makes
    # osThreadNew() fail at startup, which is exactly what happened first time.
    target_compile_definitions(host_cmsis_rtos PUBLIC
        USE_DYNAMIC_MEMORY_ALLOCATION
        RTOS2_BYTE_POOL_STACK_SIZE=1048576
        RTOS2_BYTE_POOL_HEAP_SIZE=262144   # both fit inside TX_LINUX_MEMORY_SIZE above
    )
    target_compile_options(host_cmsis_rtos PRIVATE -w)
    target_link_libraries(host_cmsis_rtos PUBLIC host_threadx)

    # The console FIFO. Real semaphores and a real critical section, because a fake of either
    # would be a fake of the thing under test (TS-01). Its own binary for the same reason
    # test_rtos_context is: it enters a kernel that never returns.
    add_executable(test_blocking_fifo test_blocking_fifo.cpp
                   ${REPO_ROOT}/Modules/blocking_io/blocking_fifo.c)
    target_include_directories(test_blocking_fifo PRIVATE
        ${REPO_ROOT}/Modules/blocking_io
        ${CMAKE_CURRENT_LIST_DIR}/host)          # cmsis_compiler.h: PRIMASK, __get_IPSR
    target_link_libraries(test_blocking_fifo PRIVATE host_cmsis_rtos GTest::gtest)
    target_compile_options(test_blocking_fifo PRIVATE -Wall -Wextra
                           $<$<COMPILE_LANGUAGE:C>:${FIRST_PARTY_ANALYSIS}>)
    add_test(NAME BlockingFifo COMMAND test_blocking_fifo)
    set_tests_properties(BlockingFifo PROPERTIES TIMEOUT 30)

    # The CLI tokenizer and command table (TS-02). The mutex, the FIFO and the thread API are
    # the real ones -- only the console is faked, in cli_uut.c, which also exposes the
    # file-statics by including cli.c verbatim.
    add_executable(test_cli test_cli.cpp cli_uut.c
                   ${REPO_ROOT}/Modules/blocking_io/blocking_fifo.c)
    target_include_directories(test_cli PRIVATE
        ${REPO_ROOT}/Src
        ${REPO_ROOT}/Modules
        ${REPO_ROOT}/Modules/blocking_io
        ${CMAKE_CURRENT_LIST_DIR}/host)
    target_link_libraries(test_cli PRIVATE host_cmsis_rtos GTest::gtest)
    target_compile_options(test_cli PRIVATE -Wall -Wextra -Wno-unused-parameter)
    add_test(NAME Cli COMMAND test_cli)
    set_tests_properties(Cli PROPERTIES TIMEOUT 30)

    add_executable(test_rtos_context test_rtos_context.cpp)
    target_link_libraries(test_rtos_context PRIVATE host_cmsis_rtos GTest::gtest)
    target_compile_options(test_rtos_context PRIVATE -Wall -Wextra)
    # Not gtest_discover_tests: discovery runs the binary to enumerate, and this one enters a
    # kernel that never returns. Registered as a single ctest entry instead.
    add_test(NAME RtosContextRules COMMAND test_rtos_context)
    set_tests_properties(RtosContextRules PROPERTIES TIMEOUT 30)
else()
    message(WARNING "Modules/threadx absent -- skipping the RTOS context tests")
endif()
