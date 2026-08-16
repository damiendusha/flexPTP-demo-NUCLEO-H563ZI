add_subdirectory(phy_drv)

# The stack-facing glue. mac_drv.c below is stack-agnostic and always built; only the shim
# that adapts it to a particular stack varies.
if(ETH_STACK STREQUAL "NETXDUO")
    set(ETH_DRV_SRC
        eth_drv_netxduo.c
        eth_drv_netxduo.h
        # The framing decisions lifted out of drv_send() so they could be tested (TS-04).
        # C++ per the rules in Src/cpp_support.cpp; depends on nothing but stdint.
        eth_framer.cpp
        eth_framer.h
        eth_link_state.cpp
        eth_link_state.h
    )
elseif(ETH_STACK STREQUAL "NONE")
    # S3-S5 of the ThreadX port: eth_drv_lwip.c was removed with lwIP and eth_drv_netxduo.c
    # arrives in S5a. mac_drv.c is still compiled -- the PTP clock registers it owns are
    # reached by the flexPTP hardware port, which stays live -- but nothing drives the rings.
    set(ETH_DRV_SRC)
endif()

target_sources(
    ${CMAKE_PROJECT_NAME}
    PUBLIC

    mac_drv.c
    mac_drv.h

    ${ETH_DRV_SRC}
)
