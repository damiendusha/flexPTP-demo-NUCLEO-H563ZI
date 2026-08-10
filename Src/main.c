#include <memory.h>
#include <stdint.h>

#include <stdlib.h>
#include <stm32h5xx_hal.h>

#include <stm32h5xx_ll_rng.h>

#include "FreeRTOSConfig.h"


#include "cliutils/cli.h"
#include "cmds.h"
#include "ethernet/ethernet_lwip.h"

#include "flexptp/event.h"
#include "flexptp/logging.h"
#include "flexptp/profiles.h"
#include "flexptp/ptp_profile_presets.h"
#include "flexptp/settings_interface.h"
#include "standard_output/serial_io.h"
#include "standard_output/standard_output.h"

#include <cmsis_os2.h>


#define FLEXPTP_INITIAL_PROFILE ("gPTP")

// ------------------------

void Error_Handler(void);

// ------------------------

#define TARGET_SYSCLK_MHZ (configCPU_CLOCK_HZ / 1000000)

void init_pll() {
    RCC_OscInitTypeDef osc;
    RCC_ClkInitTypeDef clk;

    // clear the structures
    memset(&osc, 0, sizeof(RCC_OscInitTypeDef));
    memset(&clk, 0, sizeof(RCC_ClkInitTypeDef));

    // ---------------------

    // Configure the Main PLL.

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	osc.HSEState = RCC_HSE_BYPASS; // ST-Link MCO drives HSE; use RCC_HSE_ON if an X3 crystal is fitted
	osc.HSIState = RCC_HSI_OFF;
	osc.CSIState = RCC_CSI_OFF;
	osc.PLL.PLLState = RCC_PLL_ON;
	osc.PLL.PLLSource = RCC_PLL1_SOURCE_HSE;

	osc.PLL.PLLM = HSE_VALUE / 2000000;
	osc.PLL.PLLN = TARGET_SYSCLK_MHZ;
	osc.PLL.PLLFRACN = 0;
	osc.PLL.PLLP = 2;
	osc.PLL.PLLR = 2;
	// PLL1Q feeds the Ethernet PTP timestamp unit on this device -- clk_ptp_ref_i is
	// hardwired to pll1_q_ck (RM0481 Table 121; the ETHPTPCLKSEL mux exists only on
	// STM32H543/553xx). VCO is 500 MHz, so Q = 2 gives 250 MHz. See docs/ptp_clock.md.
	osc.PLL.PLLQ = 2;

	osc.PLL.PLLVCOSEL = RCC_PLL1_VCORANGE_WIDE; // VCO = 500 MHz, inside 192..836 MHz
	osc.PLL.PLLRGE = RCC_PLL1_VCIRANGE_1;       // VCI = 2 MHz, inside 2..4 MHz


    HAL_RCC_OscConfig(&osc);

    // Enable the PLL1Q output. HAL_RCC_OscConfig() programmes the divider but leaves the
    // output gated; without this pll1_q_ck is never generated and the PTP timestamp
    // counter simply never advances (finding F53).
    __HAL_RCC_PLL1_CLKOUT_ENABLE(RCC_PLL1_DIVQ);

    // initialize clock tree
    clk.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                     RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                     RCC_CLOCKTYPE_PCLK3);
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;

    // HCLK is also the Ethernet PTP timestamp clock on the STM32H563 -- there is no
    // ETHPTPCLKSEL mux on this device. It must run at the full 250 MHz; dividing here
    // would silently detune the PTP clock. See docs/ptp_clock.md.
    // NOTE: the H5 divider namespaces are the inverse of the H7's. AHBCLKDivider takes
    // RCC_SYSCLK_DIVn (it divides SYSCLK); the APB dividers take RCC_HCLK_DIVn (they
    // divide HCLK). RCC_HCLK_DIVn exists on both families with different encodings, so
    // carrying the H7 spelling over here would compile and silently set the wrong AHB
    // divider.
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;

    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    clk.APB3CLKDivider = RCC_HCLK_DIV2;

    HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5); // 250 MHz at VOS0 requires 5 wait states

    // ---------------------
}

void init_osc_and_clk() {
    // The STM32H5 has no supply-source selection equivalent to the H7's
    // HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY); the only accepted argument is
    // PWR_EXTERNAL_SOURCE_SUPPLY, which does not apply here.

    // configure internal voltage regulator to VOS0, required for 250 MHz
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);
    while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {
    }

    // initialize the main PLL
    init_pll();

    // turn on additional clock sources and prepare GPIOs for high-speed operation.
    // SYSCFG was renamed SBS on the STM32H5.
    __HAL_RCC_HSI48_ENABLE();
    __HAL_RCC_SBS_CLK_ENABLE();
    __HAL_RCC_CSI_ENABLE();
    HAL_SBS_EnableVddIO1CompensationCell();
    HAL_SBS_EnableVddIO2CompensationCell();

    // compute SYSCLK values (HSE_VALUE must be correct when invoking this)
    SystemCoreClockUpdate();

    // set tick frequency
    HAL_SetTickFreq(HAL_TICK_FREQ_1KHZ);
}

void print_welcome_message() {
    MSGraw("\033[2J\033[H");
    MSG(ANSI_COLOR_BGREEN "Hi!" ANSI_COLOR_BYELLOW " This is a flexPTP demo for the STMicroelectronics NUCLEO-H563ZI (STM32H563) board.\n\n"
                          "The application is built on FreeRTOS, flexPTP is currenty compiled against %s and uses the supplied example %s Network Stack Driver. "
                          "In this demo, the underlying Ethernet stack can be either lwip or EtherLib, the 'ETH_STACK' CMake variable (in the main CMakeLists.txt file) determines which one will be used. "
                          "The STM32H5xx PTP hardware module driver is supplied by this project. This flexPTP instance features a full CLI control interface, the help can be listed by typing '?' once the flexPTP has loaded. "
                          "The initial PTP preset that loads upon flexPTP initialization is the 'gPTP' (802.1AS) profile. It's a nowadays common profile, but we encourage "
                          "you to also try out the 'default' (plain IEEE 1588) profile and fiddle around with other options as well. The application will try to acquire an IP-address with DHCP. "
                          "Once the IP-address is secured, you might start the flexPTP module by typing 'flexptp'. 'Have a great time! :)'\n\n" ANSI_COLOR_RESET,
        ETH_STACK, ETH_STACK);

    MSG(ANSI_COLOR_BRED "By default the MCU is clocked from the on-board ST-Link's 8 MHz MCO. That signal is noisy enough to limit how precisely synchronization settles, "
                        "so a crystal is the better reference -- and on this board (MB1404) one is already fitted: X3, 25 MHz, 20 ppm. Nothing needs soldering onto empty pads.\n\n"
                        "Selecting it takes both hardware and firmware changes (see docs/port_review.md, F55): SB3 and SB4 ON, SB49 OFF, C70/C71 populated with 5.6 pF; then "
                        "HSE_VALUE = 25000000, HSEState = RCC_HSE_ON, and a re-derived PLL -- PLLM 5, PLLN 100, PLLRGE = RCC_PLL1_VCIRANGE_2. Changing HSE_VALUE alone is not "
                        "enough and fails silently: init_pll() computes PLLM as HSE_VALUE/2000000, and 25/2 is not an integer. This path is untested on this port.\n\n" ANSI_COLOR_RESET);

    // MSG("Freq: %u\n", SystemCoreClock);
}


void task_startup(void *arg) {
    // initialize the CLI
    cli_init();

    // print greetings
    print_welcome_message();

    // initialize Ethernet
    init_ethernet();

    // initialize commands
    cmd_init();

    // -------------

    for (;;) {
        osDelay(1000);
    }
}

void btn_cb() {
}

void init_randomizer() {
    __HAL_RCC_RNG_CLK_ENABLE();

    LL_RNG_Enable(RNG);

    while (!LL_RNG_IsActiveFlag_DRDY(RNG)) {
    }

    srand(LL_RNG_ReadRandData32(RNG));
}

void init_mpu() {
    HAL_MPU_Disable();
}

int main(void) {
    // initialize FPU and several system blocks
    SystemInit();

    // initialize HAL library
    HAL_Init();

    // initialize oscillator and clocking
    init_osc_and_clk();

    // make random a bit more less deterministic
    init_randomizer();

    // initialize MPU
    init_mpu();

    // initialize standard output. MSG_init() must precede any MSG() call and must happen
    // before the scheduler starts: cli_init() creates the "cli" task, whose first act is to
    // print, while task_startup() goes on to print the banner -- both at priority 24, so
    // they can interleave from the very first tick.
    MSG_init();
    serial_io_init();

    // -------------

    // initialize the FreeRTOS kernel
    osKernelInitialize();

    // create startup thread
    osThreadAttr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.stack_size = 2048;
    attr.name = "init";
    osThreadNew(task_startup, NULL, &attr);

    // start the FreeRTOS!
    osKernelStart();

    for (;;) {
    }
}

void flexptp_user_event_cb(PtpUserEventCode uev) {
    switch (uev) {
    case PTP_UEV_INIT_DONE:
        ptp_load_profile(ptp_profile_preset_get(FLEXPTP_INITIAL_PROFILE));
        ptp_print_profile();

        ptp_log_enable(PTP_LOG_DEF, true);
        ptp_log_enable(PTP_LOG_BMCA, true);
        break;
    default:
        break;
    }
}


// ------------------------

uint8_t ucHeap[configTOTAL_HEAP_SIZE] __attribute__((section(".FreeRTOSHeapSection")));

// ------------------------

void vApplicationTickHook(void) {
    HAL_IncTick();
}

void vApplicationIdleHook(void) {
    return;
}

// --------
