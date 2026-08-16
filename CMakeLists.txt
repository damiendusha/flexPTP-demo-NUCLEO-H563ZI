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

# The project this build produces. Phase 0 of the Bazel migration moved the first-party sources
# under projects/<name>/ so that a second firmware can sit beside this one; see
# docs/bazel_migration.md. Everything below refers to the project through this variable rather
# than to the old top-level Src/, Inc/ and Startup/.
set(PROJECT_DIR ${CMAKE_SOURCE_DIR}/projects/flexptp_threadx_netx_demo)

# Sources
# Only the startup file: main.c, syscall.c and sysmem.c are added by the project's
# CMakeLists.txt,
# and listing them in both places was a duplicate CMake happened to deduplicate.
set(sources_SRCS
    ${PROJECT_DIR}/startup/startup_stm32h563zitx.s
)

# Include directories for all compilers
set(include_DIRS
    ${PROJECT_DIR}
    ${PROJECT_DIR}/ptp
    ${CMAKE_SOURCE_DIR})

# Include directories for each compiler
set(include_c_DIRS
    ${PROJECT_DIR}
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
    ${PROJECT_DIR}/startup/stm32h563zitx_FLASH.ld
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
set(ETH_STACK "NETXDUO") # "NETXDUO", "NONE" (S3); "LWIP" historical, "ETHERLIB" removed

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

# One definition per stack, so the application can select a header without a chain of
# negations. ETH_NONE is the no-stack configuration S3-S5 used and the one to fall back to if
# NetX Duo ever has to be bisected out.
if(ETH_STACK STREQUAL "NETXDUO")
    add_compile_definitions("ETH_NETXDUO")
elseif(ETH_STACK STREQUAL "NONE")
    add_compile_definitions("ETH_NONE")
elseif(ETH_STACK STREQUAL "LWIP")
    add_compile_definitions("ETH_LWIP")
endif()

# Add Drivers (HAL and CMSIS)
add_subdirectory(Drivers)

# Add Modules
add_subdirectory(Modules)

# Add the project's own sources
add_subdirectory(${PROJECT_DIR} project)

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
    # measurements behind it (re-measured after the ThreadX/NetX Duo port; one build
    # directory, one variable):
    #
    #     -Og   FLASH 149 400 B   (current)
    #     -O2   FLASH 149 456 B   LARGER by 56 B
    #     -Os   FLASH 147 400 B   smaller by 2 000 B, i.e. 0.1% of the 2 MB part
    #
    # The absolute figures are ~18 KB below the ones quoted before the port -- lwIP and the
    # FreeRTOS kernel left, NetX Duo and ThreadX arrived, and the two do not weigh the same.
    # The RELATIONSHIP is unchanged, which is the part the decision rests on. Compare deltas
    # rather than absolutes if you re-run this; the figure also depends on the build
    # directory's path length where any assertion macro embeds __FILE__ (finding N17).
    #
    # All three were built AND run on hardware, this time as well as originally: the full
    # 12-check S8 suite, 12/12 on each, reaching SLAVE with |offset| <= 14 895 ns (-O2),
    # 9 142 ns (-Os) and 11 036 ns (-Og). So changing it is safe -- it simply buys nothing.
    # Flash sits at 7% of 2 MB and the PTP servo runs once per Sync (1-8 Hz), so neither size
    # nor speed constrains this firmware, while -Og keeps release images meaningfully
    # debuggable, which on this project has repeatedly mattered.
    #
    # The F62 regression was re-checked at -Og, the level that ships: the UID is read with
    # `ldr.w rN, [rM, #0x800]` and the bytes extracted with `ubfx` -- 32-bit loads, which is
    # exactly what the `volatile const uint32_t *` in stm32h5xx_hal_conf.h exists to force.
    # NOT re-checked at -O2 and -Os: drv_initialize() is inlined away at those levels and the
    # UID base register was not tracked through the inlined code. The hardware runs above are
    # the evidence there, and they are weaker evidence -- F62 was a HardFault, so a pass does
    # mean the byte access is absent, but only for the paths S8 exercises.
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

    # A SHA-1 over the linked output, placed in .note.gnu.build-id by the linker script.
    # `osinfo` prints it and tools/hil/ compares it against the ELF, so the hardware suite
    # cannot report a pass for an image it did not test (GI-01).
    -Wl,--build-id=sha1
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