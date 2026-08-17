#include "standard_output/standard_output.h"
#include "cmsis_os2.h"
#include "standard_output/serial_io.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <stm32h5xx.h> // __get_IPSR()

#include <embfmt/embformat.h>

#define STDIO_OUTPUT_LINEBUF_LEN (2048)
static char lineBuf[STDIO_OUTPUT_LINEBUF_LEN + 1];

/* lineBuf is a single shared buffer and MSG() is called from at least six threads: "cli",
 * "ptp", "init", "tcpip_thread" (link_chg_cb), "Tmr Svc" (check_dhcp_state) and the PHY
 * prints that reach it through tcpip_callback(). The FIFO underneath is serialised, but only
 * inside bfifo_push() -- long after the formatting has already been done into this buffer, so
 * two concurrent callers interleaved into it and corrupted each other's output.
 *
 * That matters more here than it normally would: this is the only diagnostic channel the
 * firmware has, and it is what every failure in this project has had to be read through. */
static osMutexId_t sMsgMtx = NULL;

void MSG_init(void) {
    if (sMsgMtx == NULL) {
        sMsgMtx = osMutexNew(NULL);
    }
}

/* Take the lock, reporting whether it was actually taken.
 *
 * Deliberately skips locking rather than blocking or failing when it cannot be held:
 *   - before the scheduler runs there is only one context, so there is nothing to serialise.
 *   - if creation failed, printing unserialised beats not printing at all.
 *
 * Interrupt context is no longer one of these cases: MSG() refuses it outright, above. */
static bool msg_lock(void) {
    if ((sMsgMtx == NULL) || (__get_IPSR() != 0U) || (osKernelGetState() != osKernelRunning)) {
        return false;
    }
    return (osMutexAcquire(sMsgMtx, osWaitForever) == osOK);
}

static void msg_unlock(bool locked) {
    if (locked) {
        osMutexRelease(sMsgMtx);
    }
}

/* `maxLen` is the largest string length the buffer may hold, so growing to exactly maxLen is
 * fine but growing past it is not. The guard was "len <= maxLen" and was evaluated at loop
 * *entry*, while the body increments len and then writes str[len] -- so a message that
 * reached maxLen and still contained a bare '\n' wrote one byte beyond the buffer, and the
 * trailing str[len] = '\0' wrote there too. "len < maxLen" leaves room for the character the
 * body is about to add. */
static uint32_t insert_carriage_return(char *str, uint32_t maxLen) {
    uint32_t len = strlen(str);
    for (uint32_t i = 0; (i < len) && (len < maxLen); i++) {
        // if a \n is found and no \r precedes it or it is the first character os the string
        if ((str[i] == '\n') && ((i == 0) || (str[i - 1] != '\r'))) {
            len++;                               // increase length
            for (uint32_t k = len; k > i; k--) { // copy each character one position right
                str[k] = str[k - 1];
            }
            str[i] = '\r'; // insert '\r'
            i++;           // skip the current \r\n block
        }
    }
    str[len] = '\0';
    return len;
}

void MSG(const char *format, ...) {
    /* Refused from interrupt context, and refused HERE rather than one layer down, because
     * the write path is not the only thing that cannot work: lineBuf below is a single shared
     * buffer whose lock an ISR cannot take. Formatting into it from an interrupt corrupts
     * whatever a preempted thread was building -- and a mangled buffer can lose its
     * terminator, at which point the strlen() in insert_carriage_return() runs off the end.
     * That is a memory-safety bug reached from a diagnostic, which is a poor trade.
     *
     * Interrupt-context diagnostics latch state and let a thread print it; the count of
     * anything that tried anyway is reported by `osinfo`. See finding N6. */
    if (__get_IPSR() != 0U) {
        serial_io_note_isr_drop();
        return;
    }

    // held across format, carriage-return insertion and the push, so a whole line reaches
    // the FIFO before another thread may reuse lineBuf
    bool locked = msg_lock();

    va_list vaArgP;
    va_start(vaArgP, format);
    uint32_t lineLen = vembfmt(lineBuf, STDIO_OUTPUT_LINEBUF_LEN, format, vaArgP);
    va_end(vaArgP);
    lineLen = insert_carriage_return(lineBuf, STDIO_OUTPUT_LINEBUF_LEN); // insert carriage returns
    if (lineLen > 0) {
        serial_io_write(lineBuf, lineLen); // send line to the CDC
    }

    msg_unlock(locked);
}

/* Takes a char, not an int. The old signature took the address of the int parameter and sent
 * one byte from it, which is only the intended character on a little-endian target -- an
 * accident this port happens to survive rather than a decision. */
void MSGchar(char c) {
    serial_io_write(&c, 1);
}

void MSGraw(const char *str) {
    serial_io_write(str, strlen(str));
}
