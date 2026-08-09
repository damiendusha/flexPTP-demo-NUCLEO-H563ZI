#ifndef SRC_FLEXPTP_OPTIONS_ETHERLIB
#define SRC_FLEXPTP_OPTIONS_ETHERLIB

#ifdef ETH_ETHERLIB
#define ETHLIB
#elif defined(ETH_LWIP)
#define LWIP
#endif

// Select the OS binding explicitly. Without this, ptp_defs.h silently falls back to
// FLEXPTP_FREERTOS. That fallback happens to be what we want -- flexPTP calls the FreeRTOS
// API directly while the application uses CMSIS-OS2 on top of the same kernel -- but the
// choice is load-bearing, so state it. (F40)
#define FLEXPTP_FREERTOS (1)

#define FLEXPTP_TASK_PRIORITY 24

// -------------------------------------------
// --- DEFINES FOR PORTING IMPLEMENTATION ----
// -------------------------------------------

// Include LwIP/EtherLib headers here

#ifdef ETH_ETHERLIB
#include <etherlib/etherlib.h>
#elif defined(ETH_LWIP)
#include <lwip/ip_addr.h>
#endif

// Give a printf-like printing implementation MSG(...)
// Give a maskable printing implementation CLILOG(en,...)
// Provide an SPRINTF-implementation SPRINTF(str,n,fmt,...)

#include "cliutils/cli.h"
#include "embfmt/embformat.h"
#include "standard_output/standard_output.h"

#define FLEXPTP_SNPRINTF(...) embfmt(__VA_ARGS__)

// Include hardware port files and fill the defines below to port the PTP stack to a physical hardware:
// - PTP_HW_INIT(increment, addend): function initializing timestamping hardware
// - PTP_MAIN_OSCILLATOR_FREQ_HZ: clock frequency fed into the timestamp unit [Hz]
// - PTP_INCREMENT_NSEC: hardware clock increment [ns]
// - PTP_UPDATE_CLOCK(s,ns): function jumping clock by defined value (negative time value means jumping backward)
// - PTP_SET_ADDEND(addend): function writing hardware clock addend register

#include "Drivers/EthDrv/mac_drv.h"

// Project-local hardware port: flexPTP ships no STM32H5 port.
#include "ptp_port_stm32h563.h"

// Select the ADDEND clock interface explicitly. It is auto-selected when no interface
// macro is defined, but the porting guide asks for it to be stated.
#define PTP_ADDEND_INTERFACE

// The STM32H563 has no ETH PTP clock mux (no RCC_CCIPR5.ETHPTPCLKSEL), so the timestamp
// unit is fed from HCLK. The increment must exceed the 4 ns HCLK period, which makes 5 ns
// the smallest usable value. Yields addend = 2^32 / 1.25 = 3435973836 (0xCCCCCCCC) with
// 25% frequency-tuning headroom. Full derivation in docs/ptp_clock.md.
#define PTP_MAIN_OSCILLATOR_FREQ_HZ (250000000)
#define PTP_INCREMENT_NSEC (5)

#include <stdlib.h>

#define PTP_HW_INIT(increment, addend) ptphw_init(increment, addend)
// PTP_UPDATE_CLOCK is deliberately not defined: the flexPTP core never references it, and
// the ETHHW_UpdatePTPTime() it used to call has a suspect subtract path. Coarse correction
// goes through PTP_SET_CLOCK. (F41)
#define PTP_SET_CLOCK(s, ns) ETHHW_InitPTPTime(ETH, labs(s), abs(ns))
#define PTP_SET_ADDEND(addend) ETHHW_SetPTPAddend(ETH, addend)
#define PTP_HW_GET_TIME(pt) ptphw_gettime(pt)

// Include the clock servo (controller) and define the following:
// - PTP_SERVO_INIT(): function initializing clock servo
// - PTP_SERVO_DEINIT(): function deinitializing clock servo
// - PTP_SERVO_RESET(): function reseting clock servo
// - PTP_SERVO_RUN(d): function running the servo, input: master-slave time difference (error), return: clock tuning value in PPB
//

#include <flexptp/servo/kalman_filter.h>
 
#define PTP_SERVO_INIT() kalman_filter_init()
#define PTP_SERVO_DEINIT() kalman_filter_deinit()
#define PTP_SERVO_RESET() kalman_filter_reset()
#define PTP_SERVO_RUN(d, pscd) kalman_filter_run(d, pscd)

// Optionally add interactive, tokenizing CLI-support
// - CLI_REG_CMD(cmd_hintline,n_cmd,n_min_arg,cb): function for registering CLI-commands
//      cmd_hintline: text line printed in the help beginning with the actual command, separated from help text by \t charaters
//      n_cmd: number of tokens (words) the command consists of
//      n_arg: minimal number of arguments must be passed with the command
//      cb: callback function cb(const CliToken_Type *ppArgs, uint8_t argc)
//  return: cmd id (can be null, if discarded)

#define CLI_REG_CMD(cmd_hintline, n_cmd, n_min_arg, cb) cli_register_command(cmd_hintline, n_cmd, n_min_arg, cb)

// -------------------------------------------

#define CLILOG(en, ...)       \
    {                         \
        if (en)               \
            MSG(__VA_ARGS__); \
    }

// -------------------------------------------

#define PTP_ENABLE_MASTER_OPERATION (1) // Enable or disable master operations

// Static fields of the Announce message
#define PTP_CLOCK_PRIORITY1 (128)                      // priority1 (0-255)
#define PTP_CLOCK_PRIORITY2 (128)                      // priority2 (0-255)
#define PTP_BEST_CLOCK_CLASS (PTP_CC_DEFAULT)          // best clock class of this device
#define PTP_WORST_ACCURACY (PTP_CA_UNKNOWN)            // worst accucary of the clock
#define PTP_TIME_SOURCE (PTP_TSRC_INTERNAL_OSCILLATOR) // source of the distributed time

// -------------------------------------------

#include "flexptp/event.h"
extern void flexptp_user_event_cb(PtpUserEventCode uev);
#define PTP_USER_EVENT_CALLBACK flexptp_user_event_cb

#endif /* SRC_FLEXPTP_OPTIONS_ETHERLIB */
