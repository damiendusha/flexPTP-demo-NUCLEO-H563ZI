target_sources(
    ${CMAKE_PROJECT_NAME}
    PUBLIC
    main.c
    syscall.c
    sysmem.c
    system_stm32h5xx.c

    cmds.c
    cmds.h

    # The project's first C++ translation unit. Its only job is to make the C++ runtime
    # observable on hardware -- see the file header and the cpp_ctor check in tools/hil/.
    cpp_support.cpp
)

# EtherLib's option/glue translation unit, only meaningful when that stack is selected.
# It was compiled unconditionally, which meant an lwIP build carried a file whose whole
# purpose is to configure a stack that is not present.
if (ETH_STACK STREQUAL "ETHERLIB")
    target_sources(${CMAKE_PROJECT_NAME} PUBLIC etherlib_options.c)
endif()

target_include_directories(
    ${CMAKE_PROJECT_NAME}
    PUBLIC
    .
)

add_subdirectory(standard_output)
add_subdirectory(cliutils)
add_subdirectory(ethernet)