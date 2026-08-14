if (ETH_STACK STREQUAL "ETHERLIB")
    set(ETH_STACK_SRC
        ethernet_etherlib.c
        ethernet_etherlib.h
    )
elseif(ETH_STACK STREQUAL "NETXDUO")
    set(ETH_STACK_SRC
        ethernet_netxduo.c
        ethernet_netxduo.h
    )
elseif(ETH_STACK STREQUAL "NONE")
    # S3 only: no stack at all.
    set(ETH_STACK_SRC)
endif()

message(STATUS "Ethernet stack: ${ETH_STACK}")

target_sources(
    ${CMAKE_PROJECT_NAME}
    PUBLIC

    ${ETH_STACK_SRC}
)
