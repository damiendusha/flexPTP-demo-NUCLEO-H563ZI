# Eclipse ThreadX and ST's CMSIS-RTOS2 wrapper.
#
# Built as a library from S2 but NOT linked into the firmware until S3, so that vendoring and
# configuring the kernel is a separate, verifiable step from swapping it in. Until then the
# firmware still runs FreeRTOS and the hardware regression must be unchanged -- if adding
# these sources alters the image, something is wired in that should not be.
#
# Source: eclipse-threadx/threadx @ v6.5.1.202602a_rel (MIT), rather than ST's
# stm32-mw-threadx fork -- see docs/threadx_netxduo_feasibility.md, "Choosing the supply
# chain". The CMSIS-RTOS2 wrapper has no MIT equivalent and comes from ST under SLA0044;
# that mixed licensing is deliberate and documented in the README.

set(THREADX_DIR ${CMAKE_CURRENT_LIST_DIR}/../Modules/threadx)
set(CMSIS_RTOS_TX_DIR ${CMAKE_CURRENT_LIST_DIR}/../Modules/cmsis_rtos_threadx)
set(THREADX_PORT_DIR ${THREADX_DIR}/ports/cortex_m33/gnu)

if (NOT EXISTS ${THREADX_DIR}/common/inc/tx_api.h)
    message(FATAL_ERROR
        "Modules/threadx is empty. Run: git submodule update --init --recursive")
endif()

# The kernel proper plus the Cortex-M33 port. GLOB is acceptable here in a way it would not be
# for first-party code: this is a vendored tree at a pinned tag, so the file list cannot drift
# underneath us without the submodule pointer moving in a commit.
file(GLOB THREADX_COMMON_SRC ${THREADX_DIR}/common/src/*.c)
file(GLOB THREADX_PORT_SRC ${THREADX_PORT_DIR}/src/*.c ${THREADX_PORT_DIR}/src/*.S)

# Drop the TrustZone secure-stack support.
#
# This part runs with TZEN=0 (option bytes; confirmed by OpenOCD at flash time) and the
# FreeRTOS port it replaces was GCC_ARM_CM33_NTZ_NONSECURE -- the non-TrustZone variant -- so
# there is no secure world for these to call into. Compiling them anyway produces five
# 'cmse_nonsecure_entry attribute ignored without -mcmse' warnings and links entry points
# that could never be reached.
#
# TX_SINGLE_MODE_NON_SECURE below is the matching switch on the header side: it removes the
# secure-stack calls from the port's context-switch paths. Excluding the files without
# defining it would fail to link; defining it without excluding them merely warns. Both.
list(FILTER THREADX_PORT_SRC EXCLUDE REGEX "secure_stack")

# The board-specific low-level init is OURS, not the port's: Eclipse keeps it in
# example_build/ precisely because the clock, tick, vector symbol and end-of-RAM marker are
# the integrator's to supply. See the header of the project's startup/tx_initialize_low_level.S
# for what was changed and, importantly, what was deliberately left out (the fault handlers).
list(APPEND THREADX_PORT_SRC ${PROJECT_DIR}/startup/tx_initialize_low_level.S)

add_library(threadx STATIC ${THREADX_COMMON_SRC} ${THREADX_PORT_SRC})

target_include_directories(threadx PUBLIC
    ${THREADX_DIR}/common/inc
    ${THREADX_PORT_DIR}/inc
    ${PROJECT_DIR}                            # tx_user.h
)

target_compile_definitions(threadx PUBLIC
    # Without this, tx_port.h never includes tx_user.h and every setting in it -- the tick
    # rate, TX_MAX_PRIORITIES, the timer-thread configuration -- silently reverts to the
    # ThreadX defaults. The failure would be a working kernel with a 100 Hz tick and a
    # priority scale the CMSIS wrapper refuses to compile against.
    TX_INCLUDE_USER_DEFINE_FILE

    # No secure world on this part: TZEN=0, and the FreeRTOS port being replaced was the
    # _NTZ_ (non-TrustZone) one. Removes the secure-stack hooks from the context switch.
    TX_SINGLE_MODE_NON_SECURE
)

target_compile_options(threadx PRIVATE
    ${cpu_PARAMS}
    $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp>
    $<$<CONFIG:Debug>:-O0 -g3 -ggdb>
    $<$<CONFIG:Release>:-Og -g0>
)

# ---------------------------------------------------------------------------
# ST's CMSIS-RTOS2 wrapper over ThreadX
# ---------------------------------------------------------------------------
#
# Ships a single .c and no header: cmsis_os2.h is ARM's, RTOS-agnostic, and this project
# already carries it in third_party/CMSIS/CMSIS_RTOS_V2/. The FreeRTOS-specific headers beside it
# (freertos_os2.h, freertos_mpool.h) are unused by this wrapper and are removed in S3.
add_library(cmsis_rtos_tx STATIC ${CMSIS_RTOS_TX_DIR}/cmsis_os2.c)

target_include_directories(cmsis_rtos_tx PUBLIC
    ${CMSIS_RTOS_TX_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/CMSIS/CMSIS_RTOS_V2        # cmsis_os2.h (ARM's, RTOS-agnostic)
    ${CMAKE_SOURCE_DIR}/third_party/CMSIS/Include              # cmsis_compiler.h
)

target_compile_definitions(cmsis_rtos_tx PUBLIC
    # The wrapper #errors unless one allocation strategy is chosen. DYNAMIC takes the region
    # above __RAM_segment_used_end__ that _tx_initialize_low_level() published, rather than
    # reserving two fixed static arrays -- which keeps the sizing question in the linker
    # script where the rest of this project's memory layout already lives.
    USE_DYNAMIC_MEMORY_ALLOCATION

    # Sizing. Under FreeRTOS, configTOTAL_HEAP_SIZE was 30 KB and the nine threads accounted
    # for roughly 15 KB of it. The split here is stacks vs everything else (control blocks,
    # queues, mutexes, event flags), so it is not a like-for-like number; both are checked
    # against tx_byte_pool_info_get() on hardware in S3 and adjusted there rather than
    # guessed at now.
    RTOS2_BYTE_POOL_STACK_SIZE=24576
    RTOS2_BYTE_POOL_HEAP_SIZE=8192
)

target_link_libraries(cmsis_rtos_tx PUBLIC threadx)

target_compile_options(cmsis_rtos_tx PRIVATE
    ${cpu_PARAMS}
    $<$<CONFIG:Debug>:-O0 -g3 -ggdb>
    $<$<CONFIG:Release>:-Og -g0>
)
