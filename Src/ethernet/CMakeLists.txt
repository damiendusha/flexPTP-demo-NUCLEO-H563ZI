if (ETH_STACK STREQUAL "ETHERLIB")
    set(ETH_STACK_SRC
        ethernet_etherlib.c
        ethernet_etherlib.h
    )
elseif(ETH_STACK STREQUAL "NONE")
    # S3-S5 of the ThreadX port: no stack to bring up. ethernet_lwip.c went with lwIP and
    # ethernet_netxduo.c arrives in S4.
    set(ETH_STACK_SRC)
endif()

message(STATUS "Ethernet stack: ${ETH_STACK}")

target_sources(
    ${CMAKE_PROJECT_NAME}
    PUBLIC

    ${ETH_STACK_SRC}
)
