set(H5HAL_ROOT ${CMAKE_CURRENT_LIST_DIR}/stm32h5xx-hal-driver)

# Every non-template source shipped by the HAL submodule. Sources whose module is
# not enabled in Inc/stm32h5xx_hal_conf.h compile to empty translation units, so
# listing them all is safe and keeps this list mechanically regenerable.
set(H5HAL_SRC
    ${H5HAL_ROOT}/Src/stm32h5xx_hal.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_adc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_adc_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_ccb.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_cec.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_comp.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_cordic.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_cortex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_crc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_crc_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_cryp.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_cryp_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_dac.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_dac_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_dcache.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_dcmi.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_dma.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_dma2d.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_dma_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_dts.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_eth.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_eth_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_exti.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_fdcan.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_flash.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_flash_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_fmac.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_gfxtim.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_gpio.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_gtzc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_hash.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_hcd.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_i2c.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_i2c_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_i2s.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_i2s_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_i3c.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_icache.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_irda.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_iwdg.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_jpeg.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_lptim.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_ltdc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_ltdc_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_mdf.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_mmc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_mmc_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_nand.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_nor.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_opamp.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_opamp_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_otfdec.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_pcd.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_pcd_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_pka.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_play.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_pssi.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_pwr.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_pwr_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_ramcfg.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_rcc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_rcc_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_rng.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_rng_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_rtc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_rtc_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_sai.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_sai_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_sd.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_sd_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_sdio.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_sdram.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_smartcard.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_smartcard_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_smbus.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_smbus_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_spi.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_spi_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_sram.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_tim.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_tim_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_uart.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_uart_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_usart.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_usart_ex.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_wwdg.c
    ${H5HAL_ROOT}/Src/stm32h5xx_hal_xspi.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_adc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_comp.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_cordic.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_crc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_crs.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_dac.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_dlyb.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_dma.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_dma2d.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_exti.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_fmac.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_fmc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_gpio.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_i2c.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_i3c.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_icache.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_lptim.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_lpuart.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_opamp.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_pka.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_play.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_pwr.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_rcc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_rng.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_rtc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_sdmmc.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_spi.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_tim.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_ucpd.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_usart.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_usb.c
    ${H5HAL_ROOT}/Src/stm32h5xx_ll_utils.c
    ${H5HAL_ROOT}/Src/stm32h5xx_util_i3c.c
)

add_library(H5HAL STATIC ${H5HAL_SRC})

target_compile_options(H5HAL PUBLIC ${cpu_PARAMS})
target_compile_definitions(H5HAL PUBLIC ${compile_DEFS})
target_include_directories(H5HAL PUBLIC
    ${H5HAL_ROOT}/Inc
    ${PROJECT_DIR}                                   # stm32h5xx_hal_conf.h
    ${CMAKE_SOURCE_DIR}/third_party/CMSIS/Device/ST/STM32H5xx/Include
    ${CMAKE_SOURCE_DIR}/third_party/CMSIS/Include
)

target_include_directories(
    ${CMAKE_PROJECT_NAME} PUBLIC
    ${H5HAL_ROOT}/Inc
)

target_link_libraries(${CMAKE_PROJECT_NAME} H5HAL)

# CMSIS moved to third_party/ in phase 0 of the Bazel migration; the Ethernet driver moved
# into the project, and is added by the project's own CMakeLists.txt.
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/CMSIS cmsis)
