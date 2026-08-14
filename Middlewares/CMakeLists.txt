# RTOS: Eclipse ThreadX, reached through ST's CMSIS-RTOS2 wrapper.
#
# S3 replaced a FreeRTOS FetchContent block here. The application never called FreeRTOS in any
# quantity -- it is CMSIS-RTOS2 throughout -- so swapping the kernel is largely a matter of
# swapping which implementation of that API gets linked.
#
# Removed along with FreeRTOS, each for a stated reason:
#
#   - FetchContent of FreeRTOS-Kernel V11.3.0, and the freertos_config interface library that
#     carried Inc/FreeRTOSConfig.h to it. ThreadX takes its configuration from Inc/tx_user.h
#     instead, reached via TX_INCLUDE_USER_DEFINE_FILE.
#
#   - FREERTOS_PORT=GCC_ARM_CM33_NTZ_NONSECURE and FREERTOS_HEAP=4. The ThreadX equivalents
#     are ports/cortex_m33/gnu and the CMSIS wrapper's two byte pools; TX_SINGLE_MODE_NON_SECURE
#     is what now expresses "no TrustZone".
#
#   - the `device_header` export, which pointed the CMSIS_device_header compile definition at
#     the FreeRTOS port's portmacro.h. Nothing in this project ever included
#     CMSIS_device_header -- it was inherited from the H743 template -- so it goes rather than
#     being repointed at something equally unused.
#
# The kernel and wrapper themselves are in threadx.cmake, which was added in S2 and built but
# left unlinked so that vendoring could be verified separately from the swap.

include(${CMAKE_CURRENT_LIST_DIR}/threadx.cmake)

# Linked in from S3. cmsis_rtos_tx pulls threadx in transitively.
target_link_libraries(${CMAKE_PROJECT_NAME} cmsis_rtos_tx)

target_link_directories(${CMAKE_PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_LIST_DIR})
