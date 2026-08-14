#include "cmds.h"

#include <FreeRTOS.h>
#include <task.h>

#include "cliutils/cli.h"
#include "standard_output/serial_io.h"
#include "standard_output/standard_output.h"

#include "flexptp/task_ptp.h"

/* Defined in cpp_support.cpp. Reported here because the C++ runtime's initialisation cannot
   be verified by the build alone -- only by asking the running board. */
extern const char *cpp_static_init_status(void);
extern unsigned long cpp_standard(void);

// ---------------------------------

CMD_FUNCTION(os_info) {
    MSG("OS: FreeRTOS\nKernel version: %s\n", tskKERNEL_VERSION_NUMBER);

    HeapStats_t stats;
    vPortGetHeapStats(&stats);
    MSG("Free OS memory: %u bytes\n", stats.xAvailableHeapSpaceInBytes);

    MSG("C++ static init: %s (C++%lu)\n", cpp_static_init_status(), cpp_standard());

    // Console output attempted from interrupt context is discarded rather than printed (N6).
    // Report the count so that loss is visible instead of assumed to be zero.
    uint32_t isrDrops = serial_io_isr_drop_count();
    if (isrDrops > 0U) {
        MSG("Console messages dropped in ISR context: %u\n", isrDrops);
    }

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