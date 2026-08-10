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
    set(ETH_STACK_LIB lwipcore)
    set(LWIP_DIR ${CMAKE_CURRENT_LIST_DIR}/lwip)
    set(LWIP_PORT_DIR ${CMAKE_CURRENT_LIST_DIR}/lwip_port)
    #set(LWIP_CONTRIB_DIR ${LWIP_DIR}/contrib)
    set(LWIP_INCLUDE_DIRS
        ${LWIP_DIR}/src/include
        ${LWIP_DIR}/contrib
        ${LWIP_PORT_DIR}/arch
        ${LWIP_PORT_DIR}
        ${include_dirs}
    )
    message(${LWIP_PORT_DIR})
    set(LWIP_COMPILER_FLAGS ${cpu_PARAMS})
    include(${LWIP_DIR}/src/Filelists.cmake)
    #include(${LWIP_DIR}/contrib/Filelists.cmake)
    
    target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC ${LWIP_DIR}/src/include ${LWIP_PORT_DIR})
    target_include_directories(lwipcore PUBLIC ${LWIP_DIR}/src/include ${LWIP_PORT_DIR})
    target_sources(lwipcore PUBLIC ${LWIP_PORT_DIR}/sys_arch.c)
    #target_sources(${CMAKE_PROJECT_NAME} PUBLIC ${LWIP_PORT_DIR}/OS/sys_arch.c)

    target_link_libraries(${CMAKE_PROJECT_NAME} lwipcore)
endif()

# flexPTP ships no STM32H5 hardware port, so this project supplies one. Populating
# FLEXPTP_HWPORT_SRC and leaving FLEXPTP_HWPORT unmatched makes flexPTP take its
# "custom hardware port defined" path and use these files verbatim.
set(FLEXPTP_HWPORT "H563_CUSTOM")
set(FLEXPTP_HWPORT_SRC ${CMAKE_SOURCE_DIR}/Src/flexptp_port/ptp_port_stm32h563.c)

set(FLEXPTP_NSD ${ETH_STACK})
set(FLEXPTP_SERVO "KALMAN")
set(FLEXPTP_INCLUDES ${include_dirs} ${LWIP_DIR}/src/include ${LWIP_PORT_DIR}
    ${CMAKE_SOURCE_DIR}/Src/flexptp_port)
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
