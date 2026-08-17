#include "cmds.h"

#include <FreeRTOS.h>
#include <task.h>

/* For ethstat's ptp.* fields: flexPTP's core state and tick. */
#include "flexptp/ptp_core.h"
#include "flexptp/ptp_types.h"

#include "cli/cli.h"
#include "standard_output/serial_io.h"
#include "standard_output/standard_output.h"

#include "flexptp/task_ptp.h"

// ---------------------------------

/* The linker's build ID, as an ELF note.
 *
 * A SHA-1 over the linked output, emitted by -Wl,--build-id=sha1 and placed in FLASH by the
 * linker script. Printing it is what lets tools/hil/ prove it is testing the image it was
 * handed rather than whatever happened to be on the board (finding GI-01).
 *
 * The note layout is the ELF standard: three 32-bit words (namesz, descsz, type), then the
 * name padded to 4, then the descriptor. Parsed rather than assumed, because the name is
 * "GNU\0" today and the padding rules are what would bite if it were not.
 */
static void report_build_id(void) {
    extern const uint8_t __build_id_note_start[];
    extern const uint8_t __build_id_note_end[];

    const uint8_t *p = __build_id_note_start;
    const uint32_t bytes = (uint32_t)(__build_id_note_end - __build_id_note_start);

    if (bytes < 16U) {
        MSG("Build ID: <absent>\n");
        return;
    }

    uint32_t namesz, descsz;
    memcpy(&namesz, p + 0, sizeof(namesz));
    memcpy(&descsz, p + 4, sizeof(descsz));

    const uint32_t name_padded = (namesz + 3U) & ~3U;
    const uint32_t offset = 12U + name_padded;

    if ((descsz == 0U) || ((offset + descsz) > bytes)) {
        MSG("Build ID: <malformed>\n");
        return;
    }

    MSG("Build ID: ");
    for (uint32_t i = 0; i < descsz; i++) {
        /* %02x would be the obvious thing; embfmt has no width or zero-pad, so the nibbles
           are emitted by hand. A build ID printed with the leading zeros missing is not the
           build ID -- see the note on embfmt at the thread enumeration below. */
        static const char hex[] = "0123456789abcdef";
        MSGchar(hex[(p[offset + i] >> 4) & 0xFU]);
        MSGchar(hex[p[offset + i] & 0xFU]);
    }
    MSG("\n");
}

CMD_FUNCTION(os_info) {
    MSG("OS: FreeRTOS\nKernel version: %s\n", tskKERNEL_VERSION_NUMBER);

    HeapStats_t stats;
    vPortGetHeapStats(&stats);
    MSG("Free OS memory: %u bytes\n", stats.xAvailableHeapSpaceInBytes);

    // Console output attempted from interrupt context is discarded rather than printed (N6).
    // Report the count so that loss is visible instead of assumed to be zero.
    uint32_t isrDrops = serial_io_isr_drop_count();
    if (isrDrops > 0U) {
        MSG("Console messages dropped in ISR context: %u\n", isrDrops);
    }

    report_build_id();

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
CMD_FUNCTION(eth_stats) {
    MSG("BEGIN ethstat\n");

    {
        uint32_t deferred = 0, stray = 0;
        ETHHW_GetRxCtxCounters(&deferred, &stray);
        MSG("rx.ts_deferred=%u\n", (unsigned)deferred);
        MSG("rx.ts_stray=%u\n", (unsigned)stray);
    }

    {
        uint32_t peak = 0, size = 0;
        extern void sysmem_heap_usage(uint32_t *, uint32_t *);
        sysmem_heap_usage(&peak, &size);
        MSG("heap.peak=%u\n", (unsigned)peak);
        MSG("heap.size=%u\n", (unsigned)size);
    }

    MSG("ptp.running=%u\n", (unsigned)(is_flexPTP_operating() ? 1U : 0U));
    if (is_flexPTP_operating()) {
        MSG("ptp.bmca=%u\n", (unsigned)gPtpCoreState.bmca.state);
        MSG("ptp.ticks=%u\n", (unsigned)ptp_get_tick());
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
}