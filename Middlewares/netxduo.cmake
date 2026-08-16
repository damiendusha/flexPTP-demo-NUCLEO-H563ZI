# Eclipse NetX Duo.
#
# Source: eclipse-threadx/netxduo @ v6.5.1.202602_rel (MIT), rather than ST's stm32-mw-netxduo
# fork. That is the decision with the sharpest evidence behind it in this port: ST's fork is
# pinned at 6.4.0 (January 2024) and is missing the bounds checks tagged GHSA-cf2g-j6vv-m8c5
# and GHSA-c9pq-93jp-w649 in _nx_ipv4_packet_receive() -- both on the IPv4/DHCP receive path
# this firmware runs on every frame. See docs/threadx_netxduo_feasibility.md.
#
# Nothing from ST's fork is used, including its nx_stm32_eth_driver.c: that driver is
# HAL_ETH-based, which is exactly the abstraction this project bypassed to reach the DMA
# descriptors that hardware timestamping needs. The driver here is written against
# the project's eth/mac_drv.c instead, from S5a.

set(NETXDUO_DIR ${CMAKE_CURRENT_LIST_DIR}/../Modules/netxduo)
set(NETXDUO_PORT_DIR ${NETXDUO_DIR}/ports/cortex_m33/gnu)

if (NOT EXISTS ${NETXDUO_DIR}/common/inc/nx_api.h)
    message(FATAL_ERROR
        "Modules/netxduo is empty. Run: git submodule update --init --recursive")
endif()

# Vendored tree at a pinned tag, so GLOB cannot drift without the submodule pointer moving in
# a commit -- the same reasoning as threadx.cmake.
file(GLOB NETXDUO_COMMON_SRC ${NETXDUO_DIR}/common/src/*.c)

# The DHCP client, needed from S5d. Its sibling nxd_dhcp_server.c is a server and is not
# built; addons are opt-in file by file rather than by directory.
set(NETXDUO_ADDON_SRC
    ${NETXDUO_DIR}/addons/dhcp/nxd_dhcp_client.c
)

add_library(netxduo STATIC ${NETXDUO_COMMON_SRC} ${NETXDUO_ADDON_SRC})

target_include_directories(netxduo PUBLIC
    ${NETXDUO_DIR}/common/inc
    ${NETXDUO_DIR}/addons/dhcp
    ${NETXDUO_PORT_DIR}/inc
    ${PROJECT_DIR}                            # nx_user.h
)

target_compile_definitions(netxduo PUBLIC
    # Without this, nx_port.h never includes nx_user.h and every setting in it silently
    # reverts to the NetX Duo defaults -- including NX_ENABLE_INTERFACE_CAPABILITY, whose
    # absence removes the per-packet field the PTP transmit timestamp request travels in.
    # The failure would be a working network stack that cannot timestamp, discovered in S6.
    NX_INCLUDE_USER_DEFINE_FILE
)

# NetX Duo needs ThreadX: it runs its own IP thread and uses ThreadX mutexes and event flags
# throughout. PUBLIC so anything linking netxduo gets tx_api.h too.
target_link_libraries(netxduo PUBLIC threadx)

target_compile_options(netxduo PRIVATE
    ${cpu_PARAMS}
    $<$<CONFIG:Debug>:-O0 -g3 -ggdb>
    $<$<CONFIG:Release>:-Og -g0>
)
