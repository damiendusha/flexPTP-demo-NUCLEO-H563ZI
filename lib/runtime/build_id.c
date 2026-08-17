/**
 * @file      build_id.c
 * @brief     Reading the linker's build-ID note out of FLASH and printing it.
 *
 * This was a static function copied byte-for-byte into both projects' cmds.c. It depends on the
 * ELF note format and on the linker script -- both of which //lib now owns -- and on nothing
 * else, so the copies were duplication with no reason behind it.
 *
 * Why it exists at all: tools/hil/ needs to know that the board is running the image it was
 * handed, not whatever was flashed last. Finding GI-01 is what happens when it cannot -- a test
 * run that passes against a stale image and says nothing.
 */

#include "runtime/runtime_info.h"

#include <string.h>

#include "standard_output/standard_output.h"

void report_build_id(void) {
    extern const uint8_t __build_id_note_start[];
    extern const uint8_t __build_id_note_end[];

    const uint8_t *p = __build_id_note_start;
    const uint32_t bytes = (uint32_t)(__build_id_note_end - __build_id_note_start);

    if (bytes < 16U) {
        MSG("Build ID: <absent>\n");
        return;
    }

    /* The note layout is the ELF standard: three 32-bit words (namesz, descsz, type), then the
       name padded to 4, then the descriptor. Parsed rather than assumed, because the name is
       "GNU\0" today and the padding rules are what would bite if it were not. */
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
        /* %02x would be the obvious thing; embfmt has no width or zero-pad, so the nibbles are
           emitted by hand. A build ID printed with the leading zeros missing is not the build
           ID. Same family as F60/F66. */
        static const char hex[] = "0123456789abcdef";
        MSGchar(hex[(p[offset + i] >> 4) & 0xFU]);
        MSGchar(hex[p[offset + i] & 0xFU]);
    }
    MSG("\n");
}
