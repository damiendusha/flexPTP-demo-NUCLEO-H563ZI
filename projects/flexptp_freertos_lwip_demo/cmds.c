#include "cmds.h"

#include <FreeRTOS.h>
#include <task.h>

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
}