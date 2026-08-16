#include <memory.h>
#include <stdint.h>

#include <stdlib.h>
#include <stm32h5xx_hal.h>

#include <stm32h5xx_ll_rng.h>

#include "tx_api.h" // TX_THREAD and the stack-error notification, for the failure hooks


#include "cliutils/cli.h"
#include "cmds.h"
/* The stack-specific bring-up header. main.c itself is stack-agnostic -- it calls
   init_ethernet() and ethernet_poll_dhcp() and nothing else -- so the switch is a header
   choice, not a code path. Keeping it that way is what lets ETH_STACK still mean something;
   before this it was an unconditional include and neither alternative configuration would even
   compile (AR-05). */
#if !defined(ETH_NONE)
#include "ethernet/ethernet_netxduo.h"
#endif

#include "flexptp/event.h"
#include "flexptp/logging.h"
#include "flexptp/profiles.h"
#include "flexptp/ptp_profile_presets.h"
#include "flexptp/settings_interface.h"
#include "standard_output/serial_io.h"
#include "standard_output/standard_output.h"

#include <cmsis_os2.h>


// Plain IEEE 1588 over UDP/IPv4 with E2E delay measurement. Chosen over 'gPTP' because it is
// the only preset that survives ordinary, non-802.1AS switches: gPTP's destination MAC
// 01:80:C2:00:00:0E is in the IEEE 802.1D reserved range that conformant bridges must not
// forward. See docs/ptp_profiles.md.
#define FLEXPTP_INITIAL_PROFILE ("default")

// ------------------------

void Error_Handler(void);

/* Registered with tx_thread_stack_error_notify() in main(); defined below with the other
   failure paths. */
void app_stack_error_handler(TX_THREAD *thread_ptr);

// ------------------------

/* Target SYSCLK. Was configCPU_CLOCK_HZ, which went with FreeRTOSConfig.h in S3.
 *
 * Stated here because this is where the PLL that produces it is configured. Two other places
 * must agree and neither can include this header: SYSTEM_CLOCK in
 * Startup/tx_initialize_low_level.S (the SysTick divisor) and PTP_MAIN_OSCILLATOR_FREQ_HZ in
 * Inc/flexptp_options.h (pll1_q_ck feeds the timestamp unit at the same 250 MHz). Change one,
 * change all three. */
#define TARGET_SYSCLK_HZ  (250000000UL)
#define TARGET_SYSCLK_MHZ (TARGET_SYSCLK_HZ / 1000000UL)

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
                          "The application is built on Eclipse ThreadX, flexPTP is currently compiled against %s and uses the supplied example %s Network Stack Driver. "
                          "In this demo, the underlying Ethernet stack can be either NetX Duo or EtherLib, the 'ETH_STACK' CMake variable (in the main CMakeLists.txt file) determines which one will be used. "
                          "The STM32H5xx PTP hardware module driver is supplied by this project. This flexPTP instance features a full CLI control interface, the help can be listed by typing '?' once the flexPTP has loaded. "
                          "The initial PTP preset that loads upon flexPTP initialization is the 'default' (plain IEEE 1588) profile: UDP/IPv4 transport with E2E delay measurement. "
                          "It is the preset that works across ordinary switches, and a stock 'ptp4l' with no config file matches it. The 'gPTP' (802.1AS) preset is also available "
                          "('ptp profile preset gPTP'), but it needs a direct cable or 802.1AS-capable bridges end to end -- see docs/ptp_profiles.md. The application will try to acquire an IP-address with DHCP. "
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

    /* NetX Duo: packet pool, IP instance, ARP/ICMP/UDP/IGMP, DHCP client and the Ethernet
     * driver's threads. A failure here is reported and the CLI continues, because a board that
     * boots without a network is far more debuggable than one that does not boot. */
#ifndef ETH_NONE
    (void)init_ethernet();
#endif

    // initialize commands
    cmd_init();

    // -------------

    /* Resident at 1 Hz. The DHCP poll is here rather than on a timer because it prints, and
       printing blocks on the console FIFO -- see ethernet_poll_dhcp(). */
    for (;;) {
#ifdef ETH_NETXDUO
        /* NetX Duo only: EtherLib's bring-up has no equivalent, and there is no stack at all
           under ETH_STACK=NONE. */
        ethernet_poll_dhcp();
#endif
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
    /* Hardware half only. The RTOS half -- FIFO, event flags, thread -- is serial_io_start(),
       after osKernelInitialize(); see the note there. */
    serial_io_init();

    // -------------

    /* Ordering, corrected in S3 of the ThreadX port.
     *
     * Under FreeRTOS, serial_io_init() created its thread here -- before osKernelInitialize()
     * -- which is outside the CMSIS-RTOS2 contract but worked, because the shim forwarded to
     * xTaskCreate() and that is documented as callable before vTaskStartScheduler(). The
     * previous comment here recorded that as safe and deliberately left alone.
     *
     * ThreadX turns it into a hard failure. ST's CMSIS-RTOS2 wrapper creates the two byte
     * pools that back every RTOS object inside osKernelInitialize() (and calls
     * _tx_initialize_kernel_setup() there, which is what makes creating objects before
     * osKernelStart() legal at all). Anything allocated before that has nowhere to come from.
     *
     * So the split the old comment described as the alternative is now the arrangement:
     * hardware first, so the polled panic writer works, then the kernel, then everything that
     * needs to allocate. */

    // initialize the ThreadX kernel, via the CMSIS-RTOS2 wrapper
    if (osKernelInitialize() != osOK) {
        serial_io_panic_write("\n\n*** FATAL: osKernelInitialize() failed\n"
                              "Halted. The byte pools could not be created; check\n"
                              "__RAM_segment_used_end__ and RTOS2_BYTE_POOL_* sizing.\n");
        for (;;) {
        }
    }

    // Objects, now that there is somewhere to allocate them from.
    MSG_init();
    serial_io_start();

    // Report a stack overflow the kernel notices before the hardware trap does.
    tx_thread_stack_error_notify(app_stack_error_handler);

    // create startup thread
    osThreadAttr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.stack_size = 2048;
    attr.name = "init";
    /* Priority stated explicitly. Under FreeRTOS this was left at zero and the shim
       substituted osPriorityNormal; ST's ThreadX wrapper instead REJECTS osPriorityNone (0)
       because it is below osPriorityIdle (1), and osThreadNew() returns NULL. See
       docs/threadx_porting_plan.md, S3. */
    attr.priority = osPriorityNormal;
    if (osThreadNew(task_startup, NULL, &attr) == NULL) {
        serial_io_panic_write("\n\n*** FATAL: could not create the 'init' thread\n"
                              "Halted. Raise RTOS2_BYTE_POOL_STACK_SIZE.\n");
        for (;;) {
        }
    }

    // start ThreadX
    osKernelStart();

    for (;;) {
    }
}

/* Histogram of flexPTP user events, reported by `ethinfo`.
 *
 * A counter rather than a print statement, for the reason the rest of this port uses
 * counters: the events fire faster than the console can carry them, and PTP_UEV_ANNOUNCE_RECVED
 * in particular is the only non-invasive probe for "ptp_process_packet() was reached AND the
 * domain/transportSpecific gate let the message through" -- it is dispatched on the last line
 * of that branch. Reading it against the NSD's `enqueued` counter separates queue plumbing
 * from content checks without touching the submodule. */
#define UEV_COUNT_MAX (32)
static uint32_t sUevCount[UEV_COUNT_MAX];

uint32_t flexptp_uev_count(unsigned uev) {
    return (uev < UEV_COUNT_MAX) ? sUevCount[uev] : 0U;
}

void flexptp_user_event_cb(PtpUserEventCode uev) {
    if ((unsigned)uev < UEV_COUNT_MAX) {
        sUevCount[(unsigned)uev]++;
    }

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

/* --- Kernel integration points, S3 of the ThreadX port -----------------------------------
 *
 * What FreeRTOS needed here, and what replaced it:
 *
 *   ucHeap[configTOTAL_HEAP_SIZE]  -> gone. ThreadX has no heap. The CMSIS-RTOS2 wrapper
 *                                    carves two byte pools out of the RAM above
 *                                    __RAM_segment_used_end__, so the sizing lives in the
 *                                    linker script and Middlewares/threadx.cmake rather than
 *                                    in a C array.
 *   vApplicationTickHook           -> gone. HAL_IncTick() is now called from SysTick_Handler
 *                                    in Startup/tx_initialize_low_level.S. It still has to
 *                                    happen -- the HAL's own timeouts read uwTick -- but
 *                                    ThreadX offers no tick hook, so the call moved into the
 *                                    handler this project already owns.
 *   vApplicationIdleHook           -> gone. It was empty.
 *   vAssertCalled                  -> gone with configASSERT(). ThreadX's equivalent is its
 *                                    API error checking, which returns codes rather than
 *                                    halting, and the CMSIS wrapper forbids disabling it.
 *   vApplicationMallocFailedHook   -> gone. Byte-pool exhaustion is a return value, not a
 *                                    hook, so each creation site checks its own result --
 *                                    see osKernelInitialize() and serial_io_start().
 *   vApplicationStackOverflowHook  -> app_stack_error_handler(), registered in main() with
 *                                    tx_thread_stack_error_notify().
 */

/**
 * Stack-overflow notification from ThreadX.
 *
 * On this part it is usually NOT the first thing to notice an overflow. The Cortex-M33 port
 * programs PSPLIM on every context switch, so the offending push faults and lands in
 * UsageFault_Handler below with UFSR.STKOF set -- exactly as under FreeRTOS, and for the same
 * reason. This covers the case hardware cannot see: a stack overrun and then unwound before
 * the next context switch, caught by TX_ENABLE_STACK_CHECKING's pattern check.
 *
 * Reports through the polled writer rather than MSG(), which queues into a FIFO drained by a
 * thread that may be the one that just died.
 */
void app_stack_error_handler(TX_THREAD *thread_ptr) {
    __disable_irq();
    serial_io_panic_write("\n\n*** FATAL: stack overflow in thread '");
    serial_io_panic_write((thread_ptr != NULL && thread_ptr->tx_thread_name != NULL)
                              ? thread_ptr->tx_thread_name
                              : "<unnamed>");
    serial_io_panic_write("'\n"
                          "Halted. Raise that thread's stack_size; see docs/architecture.md#threads.\n");

    for (;;) {
    }
}

/* Decimal output for the panic paths. MSG() cannot be used here: it queues into a FIFO
 * drained by a task that will never run again.
 *
 * Currently uncalled: its user was vAssertCalled(), which went with configASSERT() in S3.
 * Kept as the decimal counterpart of panic_write_hex32() below -- a line number or a byte
 * count wants decimal -- and marked unused rather than deleted, because deleting it would
 * mean rewriting it the first time a panic path needs to print a quantity. */
__attribute__((unused)) static void panic_write_ulong(unsigned long v) {
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
 * in SHCSR, so it escalates here. This handler -- not app_stack_error_handler() -- is
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

// --------
