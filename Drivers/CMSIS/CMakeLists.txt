target_include_directories(
    ${CMAKE_PROJECT_NAME} PUBLIC
    Device/ST/STM32H5xx/Include/
    Include
    CMSIS_RTOS_V2)

# No sources.
#
# CMSIS_RTOS_V2/ used to hold ST's FreeRTOS-backed cmsis_os2.c together with freertos_os2.h
# and freertos_mpool.h. S3 of the ThreadX port removed all three; the CMSIS-RTOS2
# implementation is now Modules/cmsis_rtos_threadx, built by Middlewares/threadx.cmake.
#
# cmsis_os2.h stays, and is why this directory is still on the include path: it is ARM's
# standard API header and is RTOS-agnostic, so both implementations use it unchanged.
