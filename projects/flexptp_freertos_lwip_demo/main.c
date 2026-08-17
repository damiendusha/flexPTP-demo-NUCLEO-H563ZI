#include <memory.h>
#include <stdint.h>

#include <stdlib.h>
#include <stm32h5xx_hal.h>

#include <stm32h5xx_ll_rng.h>

#include "FreeRTOSConfig.h"

#include "FreeRTOS.h" // TaskHandle_t and taskDISABLE_INTERRUPTS() for the failure hooks
#include "task.h"


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


    // A PLL that fails to lock must not be shrugged off: execution would continue on
    // whatever SYSCLK survived, giving a board that runs at an unexpected speed with a
    // detuned PTP clock and no indication -- the F45/F6 failure mode, undetected.
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    // Enable the PLL1Q output. HAL_RCC_OscConfig() programmes the divider but leaves the
    // output gated; without this pll1_q_ck is never generated and the PTP timestamp
    // counter simply never advances (finding F53).
    __HAL_RCC_PLL1_CLKOUT_ENABLE(RCC_PLL1_DIVQ);

    // initialize clock tree
    clk.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                     RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 |
                     RCC_CLOCKTYPE_PCLK3);
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;

    // HCLK is the AHB bus clock and the MDIO CSR reference -- it is NOT the PTP timestamp
    // clock. clk_ptp_ref_i is hardwired to pll1_q_ck on this device (see PLLQ above), so
    // the AHB divider does not affect PTP at all. DIV1 is chosen for bus performance and
    // because the MDIO CR divider is picked for a 250 MHz HCLK. See docs/ptp_clock.md.
    //
    // An earlier revision of this comment claimed HCLK was the PTP clock. That belief is
    // what left PLL1Q disabled and the timestamp counter dead -- finding F53.
    // NOTE: the H5 divider namespaces are the inverse of the H7's. AHBCLKDivider takes
    // RCC_SYSCLK_DIVn (it divides SYSCLK); the APB dividers take RCC_HCLK_DIVn (they
    // divide HCLK). RCC_HCLK_DIVn exists on both families with different encodings, so
    // carrying the H7 spelling over here would compile and silently set the wrong AHB
    // divider.
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;

    clk.APB1CLKDivider = RCC_HCLK_DIV2;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    clk.APB3CLKDivider = RCC_HCLK_DIV2;

    // 250 MHz at VOS0 requires 5 wait states
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) {
        Error_Handler();
    }

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
                          "The application is built on FreeRTOS, flexPTP is currently compiled against %s and uses the supplied example %s Network Stack Driver. "
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

/* Was declared and never defined, so nothing could call it and every HAL initialisation
 * failure was discarded instead.
 *
 * The awkward case is that its most important caller, init_pll(), runs BEFORE
 * serial_io_init(): USART3 is not up, so the polled writer returns without printing. It is
 * still attempted, because Error_Handler() is equally reachable from later initialisation
 * where the console does exist. For the early case the red LED is the fallback -- a bench
 * failure should be visible without a debugger attached. */
void Error_Handler(void) {
    __disable_irq();

    serial_io_panic_write("\n\n*** FATAL: Error_Handler() - hardware initialisation failed\n"
                          "Halted.\n");

    // LD3 (red) on MB1404. PG4 is otherwise unused by this project.
    __HAL_RCC_GPIOG_CLK_ENABLE();

    GPIO_InitTypeDef led;
    memset(&led, 0, sizeof(led));
    led.Pin = GPIO_PIN_4;
    led.Mode = GPIO_MODE_OUTPUT_PP;
    led.Speed = GPIO_SPEED_FREQ_LOW;
    led.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOG, &led);

    for (;;) {
        HAL_GPIO_TogglePin(GPIOG, GPIO_PIN_4);
        for (volatile uint32_t i = 0U; i < 2000000U; i++) {
        }
    }
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

    /* Note on ordering: serial_io_init() above calls osThreadNew() to create "serialio",
     * BEFORE osKernelInitialize(). That is outside the CMSIS-RTOS2 contract, which says
     * osKernelInitialize() comes first.
     *
     * It works, and is left alone deliberately. The shim's osThreadNew() checks only for IRQ
     * context, then calls xTaskCreate(), which is explicitly documented as callable before
     * vTaskStartScheduler() -- the task is created and simply does not run until
     * osKernelStart(). Reordering would mean either splitting serial_io_init() into hardware
     * setup and thread creation, or moving all of it after osKernelInitialize() and giving up
     * having USART3 configured before anything can want to print through it.
     *
     * Recorded rather than corrected, so the next reader does not have to re-derive that this
     * is safe. */

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

/* configCHECK_FOR_STACK_OVERFLOW and configUSE_MALLOC_FAILED_HOOK are both enabled, but
 * neither hook was implemented, so both resolved to the CMSIS-RTOS2 shim's __WEAK stubs --
 * and those call configASSERT(0), which halts with interrupts disabled and prints nothing.
 * The two most likely runtime failures in this firmware therefore presented exactly as F53
 * did: a board that stops, with no clue why.
 *
 * Both report through the polled writer rather than MSG(), which cannot work here: it pushes
 * into a FIFO drained by a task that will never run again. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;

    taskDISABLE_INTERRUPTS();
    serial_io_panic_write("\n\n*** FATAL: stack overflow in task '");
    serial_io_panic_write((pcTaskName != NULL) ? pcTaskName : "<unnamed>");
    serial_io_panic_write("'\n"
                          "Halted. Raise that task's stack_size; see docs/architecture.md#threads.\n");

    for (;;) {
    }
}

/* Decimal output for the panic paths. MSG() cannot be used here: it queues into a FIFO
 * drained by a task that will never run again. */
static void panic_write_ulong(unsigned long v) {
    char buf[12];
    unsigned i = sizeof(buf) - 1U;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10UL));
        v /= 10UL;
    } while ((v != 0UL) && (i > 0U));

    serial_io_panic_write(&buf[i]);
}

static void panic_write_hex32(uint32_t v) {
    static const char digits[] = "0123456789abcdef";
    char buf[11];

    buf[0] = '0';
    buf[1] = 'x';
    for (unsigned i = 0U; i < 8U; i++) {
        buf[2U + i] = digits[(v >> (28U - (4U * i))) & 0xFU];
    }
    buf[10] = '\0';

    serial_io_panic_write(buf);
}

/* Fault reporter.
 *
 * Every fault vector resolved to Default_Handler -- an infinite loop that prints nothing --
 * so a bad pointer, an unaligned access or a stack overflow all produced the same result: a
 * board that went quiet. That is not hypothetical here. Both P1.3's null callback and P2.1's
 * stack-overflow test ended up in exactly this loop, and each had to be diagnosed by halting
 * over SWD and reading CFSR by hand.
 *
 * Stack overflow deserves special mention: the CM33 port programs PSPLIM on every context
 * switch, so the hardware raises UsageFault.STKOF on the offending push, long before
 * FreeRTOS's software pattern check would run at the next switch. UsageFault is not enabled
 * in SHCSR, so it escalates here. This handler -- not vApplicationStackOverflowHook() -- is
 * what actually catches a stack overflow on this target.
 *
 * `frame` is the exception stack frame, so the reported PC is the faulting instruction
 * rather than an address inside this handler. */
void fault_report(const uint32_t *frame, const char *name) {
    const uint32_t cfsr = SCB->CFSR;
    const uint32_t hfsr = SCB->HFSR;

    serial_io_panic_write("\n\n*** FATAL: ");
    serial_io_panic_write(name);
    serial_io_panic_write("\n  CFSR = ");
    panic_write_hex32(cfsr);
    serial_io_panic_write("   HFSR = ");
    panic_write_hex32(hfsr);
    serial_io_panic_write("\n  PC   = ");
    panic_write_hex32(frame[6]);
    serial_io_panic_write("   LR   = ");
    panic_write_hex32(frame[5]);

    // decode the causes this project has actually hit, so the reader need not consult the ARM ARM
    if ((cfsr & (1UL << 20)) != 0U) {
        serial_io_panic_write("\n  UFSR.STKOF  - stack overflow (PSPLIM). Raise that task's stack_size.");
    }
    if ((cfsr & (1UL << 8)) != 0U) {
        serial_io_panic_write("\n  BFSR.IBUSERR - instruction bus error, typically a call through a NULL pointer.");
    }
    if ((cfsr & (1UL << 15)) != 0U) {
        serial_io_panic_write("\n  BFSR.BFARVALID - faulting address = ");
        panic_write_hex32(SCB->BFAR);
    }
    if ((cfsr & (1UL << 7)) != 0U) {
        serial_io_panic_write("\n  MMFSR.MMARVALID - faulting address = ");
        panic_write_hex32(SCB->MMFAR);
    }

    serial_io_panic_write("\nHalted.\n");

    for (;;) {
    }
}

/* Naked so the exception frame pointer is recovered before the compiler touches the stack:
 * EXC_RETURN bit 2 selects which stack the frame was pushed onto. */
#define FAULT_HANDLER(fn, label)                        \
    void fn(void) __attribute__((naked));               \
    void fn(void) {                                     \
        __asm volatile(                                 \
            "  cpsid i                          \n"     \
            "  tst   lr, #4                     \n"     \
            "  ite   eq                         \n"     \
            "  mrseq r0, msp                    \n"     \
            "  mrsne r0, psp                    \n"     \
            "  ldr   r1, =" label "             \n"     \
            "  b     fault_report               \n");   \
    }

FAULT_HANDLER(HardFault_Handler, "fault_name_hard")
FAULT_HANDLER(MemManage_Handler, "fault_name_mem")
FAULT_HANDLER(BusFault_Handler, "fault_name_bus")
FAULT_HANDLER(UsageFault_Handler, "fault_name_usage")

const char fault_name_hard[] = "HardFault";
const char fault_name_mem[] = "MemManage fault";
const char fault_name_bus[] = "BusFault";
const char fault_name_usage[] = "UsageFault";

/* Reached from configASSERT(). A failed FreeRTOS assertion is a programming error, and the
 * previous definition halted with interrupts disabled and printed nothing -- indistinguishable
 * from a hang, and the same signature that made F53 costly. Safe from an ISR: the writer
 * polls rather than using the FIFO. */
void vAssertCalled(const char *pcFile, unsigned long ulLine) {
    taskDISABLE_INTERRUPTS();
    serial_io_panic_write("\n\n*** FATAL: FreeRTOS assertion failed\n  ");
    serial_io_panic_write((pcFile != NULL) ? pcFile : "<unknown>");
    serial_io_panic_write(":");
    panic_write_ulong(ulLine);
    serial_io_panic_write("\nHalted.\n");

    for (;;) {
    }
}

void vApplicationMallocFailedHook(void) {
    taskDISABLE_INTERRUPTS();
    serial_io_panic_write("\n\n*** FATAL: FreeRTOS heap exhausted (pvPortMalloc returned NULL)\n"
                          "Halted. Raise configTOTAL_HEAP_SIZE; 'osinfo' reports the free space.\n");

    for (;;) {
    }
}

// --------
