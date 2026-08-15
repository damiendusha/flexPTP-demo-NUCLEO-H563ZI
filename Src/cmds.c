#include "cmds.h"

#include "tx_api.h"
/* The created-thread list lives in ThreadX's internal header, not tx_api.h. Included for the
   thread enumeration in osinfo below -- the same header the CMSIS-RTOS2 wrapper includes for
   the same reason. */
#include "tx_thread.h"
#include "tx_timer.h"

#include <cmsis_os2.h>

#include "cliutils/cli.h"
#include "standard_output/serial_io.h"
#include "standard_output/standard_output.h"

#include "Drivers/EthDrv/eth_drv_netxduo.h"
#include "flexptp/clock_utils.h"
#include "flexptp/event.h"
#include "flexptp/ptp_core.h"
#include "flexptp/ptp_types.h"
#include "flexptp/task_ptp.h"

/* Defined in main.c alongside flexptp_user_event_cb(). */
extern uint32_t flexptp_uev_count(unsigned uev);

/* Defined in cpp_support.cpp. Reported here because the C++ runtime's initialisation cannot
   be verified by the build alone -- only by asking the running board. */
extern const char *cpp_static_init_status(void);
extern unsigned long cpp_standard(void);

// ---------------------------------

/* The CMSIS-RTOS2 wrapper's two byte pools. It declares them extern in cmsis_os2.c and does
   not export a header for them, so they are re-declared here rather than duplicated. */
extern TX_BYTE_POOL HeapBytePool;
extern TX_BYTE_POOL StackBytePool;

static void report_byte_pool(const char *label, TX_BYTE_POOL *pool) {
    ULONG available = 0, fragments = 0;
    if (tx_byte_pool_info_get(pool, TX_NULL, &available, &fragments,
                              TX_NULL, TX_NULL, TX_NULL) == TX_SUCCESS) {
        MSG("  %s: %u bytes free in %u fragments\n", label,
            (unsigned)available, (unsigned)fragments);
    } else {
        MSG("  %s: unavailable\n", label);
    }
}

CMD_FUNCTION(os_info) {
    MSG("OS: ThreadX\nKernel version: %u.%u.%u\n",
        THREADX_MAJOR_VERSION, THREADX_MINOR_VERSION, THREADX_PATCH_VERSION);

    /* Two pools, not one heap. Thread stacks come from StackBytePool and every other object
       -- queues, mutexes, event flags, control blocks -- from HeapBytePool, so a shortage in
       one says nothing about the other. Reported separately for that reason; this is the
       figure to raise RTOS2_BYTE_POOL_*_SIZE against. */
    MSG("Free OS memory:\n");
    report_byte_pool("stacks", &StackBytePool);
    report_byte_pool("objects", &HeapBytePool);

    /* Enumerate the threads.
     *
     * Worth having, and new in S3: the port changes both the priority scale and the thread
     * set, docs/architecture.md carries a table of both, and until now the only way to check
     * that table was a debugger. ThreadX keeps its threads on a circular list from
     * _tx_thread_created_ptr, which is what tx_thread_info_get() reads.
     *
     * Priorities are printed on BOTH scales because they are inverted between them: ThreadX
     * counts 0 as most urgent, CMSIS-RTOS2 counts 56 (osPriorityISR) as most urgent, and the
     * wrapper maps one to the other as tx = osPriorityISR - cmsis. Printing only one guarantees
     * somebody misreads it against the other. */
    MSG("Threads:\n");
    TX_THREAD *thread = _tx_thread_created_ptr;
    for (ULONG i = 0; (i < _tx_thread_created_count) && (thread != TX_NULL); i++) {
        CHAR *name = TX_NULL;
        UINT state = 0;
        ULONG run_count = 0;
        UINT priority = 0;
        if (tx_thread_info_get(thread, &name, &state, &run_count, &priority,
                               TX_NULL, TX_NULL, TX_NULL, TX_NULL) == TX_SUCCESS) {
            /* Plain %s and %u only.
             *
             * embfmt's pfn_string() is `string_copy(outbuf, va_arg(char *), free_space)` --
             * no width, no left-justify, and no NULL guard. A "%-10s" is not merely ignored:
             * the flag desynchronises the argument walk and the next %s dereferences whatever
             * it picks up, which on this board was a precise BusFault at address 0. Same
             * family as F60 and F66. Column alignment is not worth a fault. */
            MSG("  %s pri=%u (cmsis %u) state=%u runs=%u\n",
                (name != TX_NULL) ? name : "<unnamed>",
                (unsigned)priority, (unsigned)(osPriorityISR - (int)priority),
                (unsigned)state, (unsigned)run_count);
        }
        thread = thread->tx_thread_created_next;
    }

    /* Enumerate the application timers, the same way as the threads above. osTimerNew() with a
       NULL attr gives ThreadX a NULL name, so they are identified by their period. */
    MSG("Timers:\n");
    TX_TIMER *timer = _tx_timer_created_ptr;
    for (ULONG i = 0; (i < _tx_timer_created_count) && (timer != TX_NULL); i++) {
        CHAR *tname = TX_NULL;
        UINT active = 0;
        ULONG remaining = 0, reschedule = 0;
        if (tx_timer_info_get(timer, &tname, &active, &remaining, &reschedule,
                              TX_NULL) == TX_SUCCESS) {
            MSG("  %s active=%u remaining=%u reschedule=%u\n",
                (tname != TX_NULL) ? tname : "<unnamed>",
                (unsigned)active, (unsigned)remaining, (unsigned)reschedule);
        }
        timer = timer->tx_timer_created_next;
    }

    /* %u, not %lu: embfmt mishandles the 'l' length modifier here and printed
       "C++867591983928594" for a value of 202002. unsigned long is 32-bit on this target so
       the cast is lossless. Same family as F60/F66. */
    MSG("C++ static init: %s (C++%u)\n", cpp_static_init_status(), (unsigned)cpp_standard());

    // Console output attempted from interrupt context is discarded rather than printed (N6).
    // Report the count so that loss is visible instead of assumed to be zero.
    uint32_t isrDrops = serial_io_isr_drop_count();
    if (isrDrops > 0U) {
        MSG("Console messages dropped in ISR context: %u\n", isrDrops);
    }

    return 0;
}

/* flexPTP core state, appended to `ethinfo`.
 *
 * The NSD counters stop at ptp_receive_enqueue(); everything past that point is inside the
 * library and was, until now, invisible from the console. These figures span the gap:
 *
 *   ticks       -- S.ticks, incremented once per PTP_CEV_HEARTBEAT processed by task_ptp().
 *                  Zero or barely moving means the heartbeat event path is broken, and the
 *                  BMCA FSM cannot advance out of INITIALIZING no matter what arrives.
 *   bmca        -- the FSM state and how many heartbeats it has sat in it.
 *   ann recved  -- PTP_UEV_ANNOUNCE_RECVED, dispatched on the last line of the Announce branch
 *                  of ptp_process_packet(). Against `nsd: enqueued` it says whether packets
 *                  are reaching the core at all.
 *   master      -- the identity BMCA has settled on, 0 while none is selected. */
static const char *const BMCA_STATE_NAME[] = {
    "INITIALIZING", "LISTENING", "PRE_MASTER", "MASTER", "SLAVE",
    "PASSIVE", "UNCALIBRATED", "FAULTY", "DISABLED"};

static void ptp_core_report(void) {
    if (!is_flexPTP_operating()) {
        MSG("flexPTP: not running\n");
        return;
    }

    const PtpCoreState *s = &gPtpCoreState;
    unsigned state = (unsigned)s->bmca.state;

    MSG("flexPTP core:\n");
    MSG("  ticks: %u (heartbeat events processed)\n", (unsigned)ptp_get_tick());
    MSG("  bmca: %s (%u), stateDuration %u, masterTOCntr %u\n",
        (state < (sizeof(BMCA_STATE_NAME) / sizeof(BMCA_STATE_NAME[0]))) ? BMCA_STATE_NAME[state] : "?",
        state, (unsigned)s->bmca.stateDuration, (unsigned)s->bmca.masterTOCntr);

    /* flexPTP's own helper, not a %-conversion: the identity is 64-bit and embfmt has no
       64-bit conversion -- printing it as two %u halves yields a number that is not the
       value. Same family of trap as F60/F66. */
    MSG("  master: ");
    ptp_print_clock_identity(s->bmca.masterProps.grandmasterClockIdentity);
    MSG(" (announce recved %u, sync recved %u, delresp recved %u)\n",
        (unsigned)flexptp_uev_count(PTP_UEV_ANNOUNCE_RECVED),
        (unsigned)flexptp_uev_count(PTP_UEV_SYNC_RECVED),
        (unsigned)flexptp_uev_count(PTP_UEV_DELAY_RESP_RECVED));
    MSG("  uev: reset-done %u, bmca-changed %u, queue-error %u\n",
        (unsigned)flexptp_uev_count(PTP_UEV_RESET_DONE),
        (unsigned)flexptp_uev_count(PTP_UEV_BMCA_STATE_CHANGED),
        (unsigned)flexptp_uev_count(PTP_UEV_QUEUE_ERROR));
}

CMD_FUNCTION(eth_info) {
    eth_drv_report();
    ptp_core_report();
    return 0;
}

CMD_FUNCTION(start_flexptp) {
    // flexPTP renamed task_ptp_is_operating() -> is_flexPTP_operating() at a60190f
    if (!is_flexPTP_operating()) {
        MSG("Starting flexPTP...\n\n");
        reg_task_ptp();
    } else {
        MSG("Nice try, but no. :)\n"
            "flexptp is already up and running!\n");
    }

    return 0;
}

// ---------------------------------

void cmd_init() {
    cli_register_command("osinfo \t\t\tPrint OS-related information", 1, 0, os_info);
    cli_register_command("flexptp \t\t\tStart flexPTP daemon", 1, 0, start_flexptp);
    cli_register_command("ethinfo \t\t\tPrint Ethernet driver state and counters", 1, 0, eth_info);
}