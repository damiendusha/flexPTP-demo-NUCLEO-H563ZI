target_include_directories(
    ${CMAKE_PROJECT_NAME}
    PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

get_property(include_dirs TARGET ${CMAKE_PROJECT_NAME} PROPERTY INCLUDE_DIRECTORIES)
# foreach(dir ${include_dirs})
#   message(STATUS "dir='${dir}'")
# endforeach()

set(EMBFMT_CPU_PARAMS ${cpu_PARAMS})
add_subdirectory(embfmt)
target_link_libraries(${CMAKE_PROJECT_NAME} embfmt)

add_subdirectory(blocking_io)

if (ETH_STACK STREQUAL "ETHERLIB")
    set(ETH_STACK_LIB etherlib)
    set(ETHERLIB_INCLUDES ${include_dirs})
    set(ETHERLIB_CPU_PARAMS ${cpu_PARAMS})
    add_subdirectory(etherlib)
    target_link_libraries(${CMAKE_PROJECT_NAME} etherlib)
elseif(ETH_STACK STREQUAL "LWIP")
    # Historical. lwIP was removed from the build in S3; the submodule is still pinned so
    # that this path -- and the pre-port firmware -- remain reconstructable from history.
    message(FATAL_ERROR
        "ETH_STACK=LWIP is no longer wired up. lwIP was removed in S3 of the ThreadX port; "
        "see docs/threadx_porting_plan.md. Check out a pre-S3 commit to build it.")
elseif(ETH_STACK STREQUAL "NETXDUO")
    # NetX Duo is built by Middlewares/netxduo.cmake and linked into the firmware there;
    # flexPTP links against it too, for the NSD.
    set(ETH_STACK_LIB netxduo)
elseif(ETH_STACK STREQUAL "NONE")
    # S3-S5: no network stack. flexPTP is kept in the build with a no-op driver so that its
    # 19 CLI commands, the servo, the profile machinery and the H563 hardware port all stay
    # compiled and exercised while the stack underneath is replaced. See Src/flexptp_nsd/.
    set(ETH_STACK_LIB)
endif()

# flexPTP ships no STM32H5 hardware port, so this project supplies one. Populating
# FLEXPTP_HWPORT_SRC and leaving FLEXPTP_HWPORT unmatched makes flexPTP take its
# "custom hardware port defined" path and use these files verbatim.
set(FLEXPTP_HWPORT "H563_CUSTOM")
set(FLEXPTP_HWPORT_SRC ${CMAKE_SOURCE_DIR}/Src/flexptp_port/ptp_port_stm32h563.c)

# A project-supplied network stack driver, using flexPTP's documented extension point
# (FLEXPTP_NSD_SRC + FLEXPTP_NETWORK_STACK) rather than one of its bundled examples. Leaving
# FLEXPTP_NSD unmatched is what makes flexPTP take its "custom network stack driver" path.
set(FLEXPTP_NSD "CUSTOM")
# S6 replaces this with nsd_netxduo.c. Until then the stub keeps flexPTP linked and its 19
# CLI commands registered while the stack underneath is rebuilt.
set(FLEXPTP_NSD_SRC ${CMAKE_SOURCE_DIR}/Src/flexptp_nsd/nsd_stub.c)
set(FLEXPTP_NETWORK_STACK ${ETH_STACK_LIB})
set(FLEXPTP_SERVO "KALMAN")
set(FLEXPTP_INCLUDES ${include_dirs} ${CMAKE_SOURCE_DIR}/Src/flexptp_port)
set(FLEXPTP_CPU_PARAMS ${cpu_PARAMS})
# flexPTP >= a60190f uses __PRI64_PREFIX, a glibc-internal macro, in cli_cmds.c,
# master.c and slave.c. Newlib does not define it, so the library does not build for
# arm-none-eabi as-is. Map it onto newlib's equivalent (__INT64, from <sys/_intsup.h>
# via <inttypes.h>, which all three files include) rather than hardcoding "ll", so it
# follows whatever type the target uses for int64_t. See finding F47.
set(FLEXPTP_COMPILE_DEFS ${symbols_c_SYMB} __PRI64_PREFIX=__INT64)
add_subdirectory(flexptp)

# ptp_port_stm32h563.c is ours, but it is compiled into flexPTP's static library, so it
# inherited that library's flags and was the only piece of first-party code building without
# -Wall -Wextra. Warn on it explicitly. It is clean today; this keeps it that way.
#
# TARGET_DIRECTORY is required: source-file properties are scoped to the directory that
# defines the target, and flexPTP's library is defined in Modules/flexptp, not here. Setting
# it from this scope silently does nothing -- which is exactly how the gap went unnoticed.
set_source_files_properties(${FLEXPTP_HWPORT_SRC}
    TARGET_DIRECTORY flexptp
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")
target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${FLEXPTP_INCLUDE_EXPORT})
target_link_libraries(${CMAKE_PROJECT_NAME} flexptp)
