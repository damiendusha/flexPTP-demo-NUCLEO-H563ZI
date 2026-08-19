/* The guard name is a leftover: this header was EtherLib-specific in the H743 project it came
   from, and EtherLib itself was removed in the Bazel migration (D9). Renaming the guard is a
   no-op with a small chance of a duplicate-guard collision, so it is left alone and explained
   instead. */
#ifndef SRC_FLEXPTP_OPTIONS_ETHERLIB
#define SRC_FLEXPTP_OPTIONS_ETHERLIB

#ifdef ETH_LWIP
#define LWIP
#endif

/* No network stack linked (S3-S5 of the ThreadX port), or NetX Duo (from S6).
 *
 * flexPTP does not have a "generic stack" mode: ptp_defs.h typedefs ip_addr_t from a type
 * named ip4_addr unless LWIP is defined, and ptp_defs.c #errors unless it recognises the
 * stack. Both are satisfied here rather than by pretending to be lwIP -- providing the type
 * and the two IGMP constants is all flexPTP actually needs from a stack at this level.
 *
 * PTP_IGMP_*_INIT is the extension point added by
 * patches/flexptp/0003-generic-netstack-igmp-addresses.patch; the values are the same
 * little-endian words flexPTP's own lwIP branch uses. */
#if !defined(ETH_LWIP)
typedef struct ip4_addr {
    uint32_t addr;
} ip4_addr;

#define PTP_IGMP_PRIMARY_INIT    {0x810100E0} /* 224.0.1.129 */
#define PTP_IGMP_PEER_DELAY_INIT {0x6B0000E0} /* 224.0.0.107 */
#endif

/* OS binding. Was FLEXPTP_FREERTOS until S3 of the ThreadX port.
 *
 * flexPTP has no ThreadX binding, but it has a CMSIS-RTOS2 one, and this application is
 * CMSIS-RTOS2 throughout -- so the same wrapper that carries the application onto ThreadX
 * carries flexPTP with it, with no change to the submodule.
 *
 * WARNING, and it is the sharpest hazard in this port: flexPTP's CMSIS-RTOS2 binding calls
 * osMessageQueuePut(..., osWaitForever) from ptp_transmit_timestamp_cb() and
 * ptp_transmit_enqueue(), both of which run in interrupt context on this hardware
 * (ETHHW_ProcessTx is called from ETH_IRQHandler). CMSIS-RTOS2 forbids a non-zero timeout
 * from an ISR and ST's wrapper returns osErrorParameter, which flexPTP does not check -- so
 * every transmit timestamp would be silently discarded and the servo would never see t3.
 *
 * That is handled, and the warning above is kept because the hazard is still there in the
 * binding: Src/flexptp_nsd/ptp_tx_timestamp.c breaks the call chain, posting from the ISR with
 * timeout 0 and making flexPTP's blocking call from a thread. Remove that file and every
 * transmit timestamp goes silently missing again. See finding 1 in
 * docs/threadx_netxduo_feasibility.md, and the ptp_ts check in tools/hil/. (F40) */
#define FLEXPTP_CMSIS_OS2 (1)

#define FLEXPTP_TASK_PRIORITY 24

// -------------------------------------------
// --- DEFINES FOR PORTING IMPLEMENTATION ----
// -------------------------------------------

// Include the network stack's headers here.
/* No lwip/ip_addr.h: ip_addr_t comes from the ip4_addr typedef above when no stack is
   linked. */

// Give a printf-like printing implementation MSG(...)
// Give a maskable printing implementation CLILOG(en,...)
// Provide an SPRINTF-implementation SPRINTF(str,n,fmt,...)

#include "cli/cli.h"
#include "embfmt/embformat.h"
#include "standard_output/standard_output.h"

#define FLEXPTP_SNPRINTF(...) embfmt(__VA_ARGS__)

// Include hardware port files and fill the defines below to port the PTP stack to a physical hardware:
// - PTP_HW_INIT(increment, addend): function initializing timestamping hardware
// - PTP_MAIN_OSCILLATOR_FREQ_HZ: clock frequency fed into the timestamp unit [Hz]
// - PTP_INCREMENT_NSEC: hardware clock increment [ns]
// - PTP_UPDATE_CLOCK(s,ns): function jumping clock by defined value (negative time value means jumping backward)
// - PTP_SET_ADDEND(addend): function writing hardware clock addend register

#include "eth/mac_drv.h"

// Project-local hardware port: flexPTP ships no STM32H5 port.
#include "ptp_port_stm32h563.h"

// Select the ADDEND clock interface explicitly. It is auto-selected when no interface
// macro is defined, but the porting guide asks for it to be stated.
#define PTP_ADDEND_INTERFACE

// The STM32H563 has no ETH PTP clock mux (RCC_CCIPR5.ETHPTPCLKSEL exists only on
// STM32H543/553xx), so clk_ptp_ref_i is hardwired to pll1_q_ck -- NOT to HCLK.
// init_pll() configures PLL1Q for 250 MHz and enables the output. The increment must
// exceed the 4 ns clock period, making 5 ns the smallest usable value, and yielding
// addend = 2^32 / 1.25 = 3435973836 (0xCCCCCCCC) with 25% tuning headroom.
// Full derivation in docs/ptp_clock.md.
#define PTP_MAIN_OSCILLATOR_FREQ_HZ (250000000)
#define PTP_INCREMENT_NSEC (5)

#include <stdlib.h>

#define PTP_HW_INIT(increment, addend) ptphw_init(increment, addend)
// PTP_UPDATE_CLOCK is deliberately not defined: the flexPTP core never references it. Coarse
// correction goes through PTP_SET_CLOCK. The relative-update driver function it would have
// called was removed as dead code with a wrong subtract path (F41).
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
