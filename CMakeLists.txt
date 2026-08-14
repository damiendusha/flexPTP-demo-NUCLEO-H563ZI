cmake_minimum_required(VERSION 3.22)

#
# User is free to modify the file as much as necessary
#

include("cmake/gcc-arm-none-eabi.cmake")
message("Build CMAKE_MODULE_PATH: " ${CMAKE_MODULE_PATH})

# Core project settings
project(flexptp-demo)
enable_language(C CXX ASM)
message("Build type: " ${CMAKE_BUILD_TYPE})

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# Core MCU flags, CPU type, instruction set and FPU setup
set(cpu_PARAMS
    -mthumb
    -mcpu=cortex-m33
    -mfpu=fpv5-sp-d16
    -mfloat-abi=hard
)

# Set MCU type
set(MCU_TYPE STM32H563xx)

# Sources
# Only the startup file: main.c, syscall.c and sysmem.c are added by Src/CMakeLists.txt,
# and listing them in both places was a duplicate CMake happened to deduplicate.
set(sources_SRCS
    ${CMAKE_SOURCE_DIR}/Startup/startup_stm32h563zitx.s
)

# Include directories for all compilers
set(include_DIRS 
    ${CMAKE_SOURCE_DIR}/Inc
    ${CMAKE_SOURCE_DIR}/Src
    ${CMAKE_SOURCE_DIR}/Src/flexptp_port
    ${CMAKE_SOURCE_DIR})

# Include directories for each compiler
set(include_c_DIRS
    ${CMAKE_SOURCE_DIR}/Inc
)
set(include_cxx_DIRS)
set(include_asm_DIRS)

# Symbols definition for all compilers
set(symbols_SYMB)

# Symbols definition for each compiler
set(symbols_c_SYMB)
set(symbols_cxx_SYMB)
set(symbols_asm_SYMB)

# Link directories and names of libraries
set(link_DIRS)
# The RTOS comes in via Middlewares/CMakeLists.txt (cmsis_rtos_tx -> threadx).
set(link_LIBS)

# Linker script
set(linker_script_SRC
    ${CMAKE_SOURCE_DIR}/stm32h563zitx_FLASH.ld
)

# Compiler options
set(compiler_OPTS)

# Linker options
set(linker_OPTS) 

# Link directories setup
# Must be before executable is added
link_directories(${CMAKE_PROJECT_NAME} ${link_DIRS})

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PUBLIC ${sources_SRCS})

# Seéect Ethernet stack
# S3 removed lwIP and S4-S6 bring NetX Duo in. "NONE" is the interim state: the MAC driver
# and flexPTP are still compiled, but nothing above the link layer is.
set(ETH_STACK "NETXDUO") # "NETXDUO", "NONE" (S3), "LWIP"/"ETHERLIB" (historical)

# Add definitions
# DEBUG is NOT listed here: it belongs to the Debug configuration only, and is applied
# below through $<$<CONFIG:Debug>:DEBUG>. Listing it unconditionally defined it in
# Release builds too.
set(compile_DEFS ${MCU_TYPE} USE_HAL_DRIVER CMSIS_OS2 PPS_SIMPLE ETH_STACK="${ETH_STACK}")
add_compile_definitions(${compile_DEFS})
add_compile_definitions(PROJECT_NAME="${CMAKE_PROJECT_NAME}")

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC
    ${include_DIRS}
    $<$<COMPILE_LANGUAGE:C>: ${include_c_DIRS}>
    $<$<COMPILE_LANGUAGE:CXX>: ${include_cxx_DIRS}>
    $<$<COMPILE_LANGUAGE:ASM>: ${include_asm_DIRS}>
)

# Add Middlewares
add_subdirectory(Middlewares)

if (ETH_STACK STREQUAL "ETHERLIB")
    add_compile_definitions("ETH_ETHERLIB")
elseif(ETH_STACK STREQUAL "LWIP")
    add_compile_definitions("ETH_LWIP")
endif()

# Add Drivers (HAL and CMSIS)
add_subdirectory(Drivers)

# Add Modules
add_subdirectory(Modules)

# Add generic sources
add_subdirectory(Src)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    ${symbols_SYMB}
    $<$<COMPILE_LANGUAGE:C>: ${symbols_c_SYMB}>
    $<$<COMPILE_LANGUAGE:CXX>: ${symbols_cxx_SYMB}>
    $<$<COMPILE_LANGUAGE:ASM>: ${symbols_asm_SYMB}>

    # Configuration specific
    $<$<CONFIG:Debug>:DEBUG>
    $<$<CONFIG:Release>: >
)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME} ${link_LIBS})

# Compiler options
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    ${cpu_PARAMS}
    ${compiler_OPTS}
    -Wall
    -Wextra
    # -Wpedantic
    -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:C>: >
    $<$<COMPILE_LANGUAGE:CXX>:

    # -Wno-volatile
    # -Wold-style-cast
    # -Wuseless-cast
    # -Wsuggest-override
    >
    $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp -MMD -MP>
    $<$<CONFIG:Debug>:-O0 -g3 -ggdb>

    # Release stays at -Og. That is unusual -- "optimize for debugging" under a name that
    # normally means -O2 -- so it is a decision rather than an oversight, and these are the
    # measurements behind it (re-measured in P6.12; one build directory, one variable):
    #
    #     -Og   FLASH 167 404 B   (current)
    #     -O2   FLASH 167 708 B   LARGER by 304 B
    #     -Os   FLASH 165 140 B   smaller by 2 264 B, i.e. 0.1% of the 2 MB part
    #
    # The absolute numbers moved down ~13 KB from the original set when P6.6 removed newlib's
    # printf from the link; the relationships are unchanged. Compare deltas rather than
    # absolutes if you re-run this: the figure also depends on the build directory's path
    # length, because configASSERT() embeds __FILE__ and the FreeRTOS kernel is fetched into
    # the build tree (finding N17). Measured spread: 336 bytes between two paths.
    #
    # All three were built AND run on hardware: boot, PHY, link, DHCP, flexPTP start,
    # "ptp addend" = 3435973836, ping 0% loss at 1400 bytes, no faults. So changing it is
    # safe -- it simply buys nothing. Flash sits at 8.6% of 2 MB and the PTP servo runs once
    # per Sync (1-8 Hz), so neither size nor speed constrains this firmware, while -Og keeps
    # release images meaningfully debuggable, which on this project has repeatedly mattered.
    #
    # The F62 regression was checked at each level: no byte-wide loads of the UID region
    # appear in any of the three disassemblies, so the `volatile` fix holds above the
    # optimisation level it was originally tested at.
    #
    # To change it, edit the line below; all three levels are known-good.
    $<$<CONFIG:Release>:-Og -g0>

    # These two presets previously received no project optimisation flags at all, falling
    # back to CMake's defaults. Neither is hardware-tested; Debug and Release are the pair
    # this project verifies.
    $<$<CONFIG:RelWithDebInfo>:-O2 -g3>
    $<$<CONFIG:MinSizeRel>:-Os -g0>
)

# Linker options
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
    -T${linker_script_SRC}
    ${cpu_PARAMS}
    ${linker_OPTS}
    -Wl,-Map=${CMAKE_PROJECT_NAME}.map
    --specs=nosys.specs

    # Section garbage collection. This used to ride along in CMAKE_C_FLAGS in
    # cmake/gcc-arm-none-eabi.cmake, which CMake passes to the link as well as to every
    # compile -- so it was doing real work in an odd place. Moving it here and dropping it
    # there is not cosmetic: removing it from CMAKE_C_FLAGS without adding it here grew the
    # Release image from 167 424 B to 233 924 B, which is what caught the mistake (H-3).
    -Wl,--gc-sections
    -Wl,--start-group
    -lc
    -lm
    -lstdc++
    -lsupc++
    -Wl,--end-group
    -Wl,-z,max-page-size=8 # Allow good software remapping across address space (with proper GCC section making)
    -Wl,--print-memory-usage
)

# Relink when the linker script changes.
#
# -T names the script on the link line but tells CMake nothing about it, so an incremental
# build after editing stm32h563zitx_FLASH.ld silently keeps the previous memory layout. That
# is not hypothetical: P3.8's section reorder appeared to have had no effect at all until the
# ELF was deleted by hand, because the link step never re-ran.
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
    LINK_DEPENDS ${linker_script_SRC}
)

# Execute post-build to print size, generate hex and bin
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
)