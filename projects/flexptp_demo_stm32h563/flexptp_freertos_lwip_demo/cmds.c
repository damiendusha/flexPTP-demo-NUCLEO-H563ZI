#include "cmds.h"

#include <FreeRTOS.h>
#include <task.h>

/* For ethstat's ptp.* fields: flexPTP's core state and tick. */
#include "flexptp/clock_utils.h"
#include "flexptp/stats.h"
#include "flexptp/event.h"
#include "flexptp/ptp_core.h"
#include "flexptp/ptp_types.h"

#include "cli/cli.h"
#include "eth/eth_drv_lwip.h"
#include "runtime/runtime_info.h"
#include "standard_output/serial_io.h"
#include "standard_output/standard_output.h"

#include "flexptp/task_ptp.h"

// ---------------------------------

CMD_FUNCTION(os_info) {
    MSG("OS: FreeRTOS\nKernel version: %s\n", tskKERNEL_VERSION_NUMBER);

    /* One heap, not ThreadX's two pools: heap_4.c serves task stacks and every other object
       from the same arena. The low-water mark matters more than the current figure -- it is
       what says whether configTOTAL_HEAP_SIZE has headroom or merely happens to today. */
    HeapStats_t stats;
    vPortGetHeapStats(&stats);
    MSG("Free OS memory: %u bytes (low water %u, %u allocs, %u frees)\n",
        (unsigned)stats.xAvailableHeapSpaceInBytes,
        (unsigned)stats.xMinimumEverFreeBytesRemaining,
        (unsigned)stats.xNumberOfSuccessfulAllocations,
        (unsigned)stats.xNumberOfSuccessfulFrees);

    /* The newlib heap is a second allocator and it was invisible here until now. Backported
       from the ThreadX firmware, where NX-03 put a hard bound in _sbrk against the linker
       script's symbols: the bound is shared code in //projects/flexptp_demo_stm32h563/lib:runtime, so both firmwares have it,
       but only one of them was reporting the number that says whether it is marginal. */
    {
        uint32_t peak = 0, size = 0;
        sysmem_heap_usage(&peak, &size);
        MSG("  C heap: %u of %u bytes used (peak)\n", (unsigned)peak, (unsigned)size);
    }

    /* Enumerate the tasks.
     *
     * Backported from the ThreadX firmware's thread enumeration, and for the same reason: this
     * project's docs carry a table of tasks and priorities, and until now the only way to check
     * that table was a debugger. The APIs differ -- ThreadX walks _tx_thread_created_ptr,
     * FreeRTOS has uxTaskGetSystemState(), enabled here by configUSE_TRACE_FACILITY=1 -- but
     * what is being asserted is identical.
     *
     * Priorities are printed on BOTH scales, exactly as the ThreadX side does, because the two
     * are inverted relative to each other and printing one guarantees somebody misreads it
     * against the other. Here the inversion is the other way round from ThreadX: FreeRTOS
     * counts UP for more urgent (configMAX_PRIORITIES-1 is the most urgent), and the CMSIS
     * wrapper maps cmsis = freertos + 1 over most of the range.
     *
     * The array is a stack allocation sized by configMAX_PRIORITIES rather than a
     * pvPortMalloc(), because osinfo is a diagnostic and a diagnostic that can fail on a full
     * heap is not much of a diagnostic. */
    MSG("Tasks:\n");
    {
        TaskStatus_t task_status[16];
        const UBaseType_t count = uxTaskGetNumberOfTasks();
        const UBaseType_t capacity = (UBaseType_t)(sizeof(task_status) / sizeof(task_status[0]));

        if (count > capacity) {
            MSG("  (%u tasks, showing %u)\n", (unsigned)count, (unsigned)capacity);
        }

        const UBaseType_t got = uxTaskGetSystemState(task_status, capacity, NULL);
        for (UBaseType_t i = 0; i < got; i++) {
            /* Plain %s and %u only.
             *
             * embfmt's pfn_string() is `string_copy(outbuf, va_arg(char *), free_space)` -- no
             * width, no left-justify, and no NULL guard. A "%-10s" is not merely ignored: the
             * flag desynchronises the argument walk and the next %s dereferences whatever it
             * picks up, which on this board was a precise BusFault at address 0. Same family
             * as F60 and F66. Column alignment is not worth a fault. */
            MSG("  %s pri=%u (cmsis %u) state=%u stack_free=%u\n",
                (task_status[i].pcTaskName != NULL) ? task_status[i].pcTaskName : "<unnamed>",
                (unsigned)task_status[i].uxCurrentPriority,
                (unsigned)(task_status[i].uxCurrentPriority + 1U),
                (unsigned)task_status[i].eCurrentState,
                (unsigned)task_status[i].usStackHighWaterMark);
        }
    }

    /* Interrupt priorities, read back out of the registers.
     *
     * Backported from the ThreadX firmware, where finding NX-04 was that the architecture
     * doc's interrupt priority table had two wrong rows out of six and nothing checked it.
     * That table is per-firmware -- FreeRTOS's port sets SysTick and PendSV to the lowest
     * priority from its own configuration, ThreadX's tx_initialize_low_level.S sets them
     * itself -- but the failure mode is identical and it is the READBACK that catches it. The
     * header that looks like the setting is not necessarily the setting.
     *
     * Printed as the priority NUMBER, i.e. the register byte shifted down by
     * (8 - __NVIC_PRIO_BITS), which is what NVIC_SetPriority takes and what the table quotes.
     * Asserted by the irq_prio check in tools/hil/. */
    MSG("Interrupt priorities:\n");
    {
        static const struct {
            IRQn_Type irq;
            const char *name;
        } kIrqs[] = {
            {MemoryManagement_IRQn, "MemManage"}, {BusFault_IRQn, "BusFault"},
            {UsageFault_IRQn, "UsageFault"},      {SVCall_IRQn, "SVCall"},
            {PendSV_IRQn, "PendSV"},              {SysTick_IRQn, "SysTick"},
            {ETH_IRQn, "ETH"},                    {USART3_IRQn, "USART3"},
            {GPDMA1_Channel0_IRQn, "GPDMA1_Ch0"},
        };

        for (unsigned i = 0; i < (sizeof(kIrqs) / sizeof(kIrqs[0])); i++) {
            MSG("  %s pri=%u\n", kIrqs[i].name, (unsigned)NVIC_GetPriority(kIrqs[i].irq));
        }
    }

    /* %u, not %lu: embfmt mishandles the 'l' length modifier here and printed
       "C++867591983928594" for a value of 202002. unsigned long is 32-bit on this target so
       the cast is lossless. Same family as F60/F66. */
    MSG("C++ static init: %s (C++%u)\n", cpp_static_init_status(), (unsigned)cpp_standard());

    report_build_id();

    // Console output attempted from interrupt context is discarded rather than printed (N6).
    // Report the count so that loss is visible instead of assumed to be zero.
    uint32_t isrDrops = serial_io_isr_drop_count();
    if (isrDrops > 0U) {
        MSG("Console messages dropped in ISR context: %u\n", isrDrops);
    }

    return 0;
}


/* Machine-readable diagnostics, backported from the ThreadX firmware (GI-02).
 *
 * That firmware grew `ethstat` because a human-readable dump is a thing tooling has to parse by
 * eye, and tools/hil/ needed a contract. This project had NO counter visibility at all until
 * now: a dropped frame or an exhausted heap was invisible from the console.
 *
 * A SUBSET, deliberately, and the boundary is which network-stack driver each firmware uses.
 * The ThreadX one supplies its own NSD from ptp/, instrumented throughout -- nsd.*, ptpts.* and
 * drv.* all come from code this project does not have, because it uses flexPTP's bundled
 * nsd_lwip.c instead. What IS here is everything that comes from code the two share:
 *
 *   rx.ts_*    the MAC driver's receive-timestamp context counters (mac_drv.c, common)
 *   heap.*     the newlib heap high-water mark (sysmem.c, common)
 *   ptp.*      flexPTP's own state
 *
 * The `counters` check in tools/hil/ therefore does not apply to this firmware as written --
 * it asserts nsd.* and drv.* keys that only exist on the other one. Extending the lwIP NSD to
 * match is a real piece of work and is not pretended to be done here.
 */
/* Defined in main.c alongside flexptp_user_event_cb(). */
extern uint32_t flexptp_uev_count(unsigned uev);

/* Indexed by PtpBmcaState. Backported verbatim from the ThreadX firmware -- these are flexPTP's
   own states, so nothing here is stack- or RTOS-specific. */
static const char *const BMCA_STATE_NAME[] = {
    "INITIALIZING", "LISTENING", "PRE_MASTER", "MASTER", "SLAVE",
    "PASSIVE", "UNCALIBRATED", "FAULTY", "DISABLED"};

/* flexPTP's core state, for `ethinfo`. Backported unchanged: every field below comes from
   flexPTP, which is the same submodule at the same pin on both firmwares. */
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
    /* Two layers rather than the ThreadX firmware's three: the driver's counters and flexPTP's
       core state. The middle layer there is ptp_nsd_report(), which reports the project-supplied
       NetX Duo network stack driver -- this firmware uses flexPTP's own bundled nsd_lwip.c
       instead, which keeps no such counters. */
    eth_drv_report();
    ptp_core_report();
    return 0;
}

CMD_FUNCTION(eth_stats) {
    MSG("BEGIN ethstat\n");

    {
        uint32_t deferred = 0, strayCtx = 0, strayTsa = 0;
        ETHHW_GetRxCtxCounters(&deferred, &strayCtx, &strayTsa);
        MSG("rx.ts_deferred=%u\n", (unsigned)deferred);
        /* The sum stays, because the harness and the saved baselines name it. The two
           components are what say which anomaly fired -- only strayTsa costs a timestamp. */
        MSG("rx.ts_stray=%u\n", (unsigned)(strayCtx + strayTsa));
        MSG("rx.ts_stray_ctx=%u\n", (unsigned)strayCtx);
        MSG("rx.ts_stray_tsa=%u\n", (unsigned)strayTsa);
    }

    {
        uint32_t peak = 0, size = 0;
        sysmem_heap_usage(&peak, &size);
        MSG("heap.peak=%u\n", (unsigned)peak);
        MSG("heap.size=%u\n", (unsigned)size);
    }

    MSG("ptp.running=%u\n", (unsigned)(is_flexPTP_operating() ? 1U : 0U));
    if (is_flexPTP_operating()) {
        MSG("ptp.bmca=%u\n", (unsigned)gPtpCoreState.bmca.state);
        MSG("ptp.ticks=%u\n", (unsigned)ptp_get_tick());
    }

    {
        /* flexPTP's own refusal counters, new in RC1.0-17 (upstream `e873105`). They are the
           point of that change: every enqueue there became non-blocking, and a drop nobody
           counts is just a quieter bug. Under a saturated link these are the numbers that say
           what the link cost -- see tools/hil/flood.py. */
        const PtpStats *st = ptp_get_stats();
        MSG("ptp.drop_rx=%u\n", (unsigned)st->drop_cntrs.rx);
        MSG("ptp.drop_tx=%u\n", (unsigned)st->drop_cntrs.tx);
        MSG("ptp.drop_txts=%u\n", (unsigned)st->drop_cntrs.txts);
    }

    MSG("END ethstat\n");
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
    cli_register_command("ethstat \t\t\tPrint driver, heap and PTP counters as key=value, for tooling", 1, 0, eth_stats);
    cli_register_command("osinfo \t\t\tPrint OS-related information", 1, 0, os_info);
    cli_register_command("flexptp \t\t\tStart flexPTP daemon", 1, 0, start_flexptp);
    cli_register_command("ethinfo \t\t\tPrint Ethernet driver state and counters", 1, 0, eth_info);
}