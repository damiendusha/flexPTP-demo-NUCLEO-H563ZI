/* Src/cliutils/cli.c, compiled for the host.
 *
 * Same idiom as mac_drv_uut.c: include the .c verbatim, having first supplied the handful of
 * things it reaches outside itself. Including it is also what exposes tokenize_cli_line() and
 * the command table, which are file-statics -- and which are the part with the finding history
 * (F74/N8, N12).
 *
 * Only the console is faked. The mutex, the FIFO and the thread API are the real ones, because
 * test/ already links real ThreadX and ST's real CMSIS-RTOS2 wrapper; a CLI whose mutex is a
 * stub would not be the CLI under test. The console is faked because MSG() reaches USART3 and
 * the GPDMA, and because capturing the output is how the help and history assertions work.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ST's CMSIS-RTOS2 wrapper reports this as the system timer frequency. Nothing here reads it;
   it has to exist, and defining it in a normal data section is also what keeps the link
   position-independent. */
unsigned int SystemCoreClock = 250000000U;

/* The host shim's PRIMASK, which blocking_fifo.c's critical section reads and writes. */
uint32_t host_primask = 0;

/* The shim's IPSR. Zero means thread context, which is what every test here wants
   except the ISR-context cases in test_rtos_context.cpp. */
uint32_t host_fake_ipsr = 0U;

/* --- the captured console ----------------------------------------------------------------- */

#define CLI_UUT_CAPTURE_LEN (16384)
char cli_uut_output[CLI_UUT_CAPTURE_LEN];
unsigned cli_uut_output_len;

void cli_uut_clear_output(void) {
    cli_uut_output_len = 0;
    cli_uut_output[0] = '\0';
}

static void cli_uut_append(const char *s, unsigned n) {
    if ((cli_uut_output_len + n) >= CLI_UUT_CAPTURE_LEN) {
        n = (CLI_UUT_CAPTURE_LEN - 1U) - cli_uut_output_len;
    }
    memcpy(cli_uut_output + cli_uut_output_len, s, n);
    cli_uut_output_len += n;
    cli_uut_output[cli_uut_output_len] = '\0';
}

/* vsnprintf, not embfmt.
 *
 * Deliberate, and a limitation worth stating: embfmt is the real backend and it has its own
 * finding history (F60, F66). These tests are about the TOKENIZER and the command table, so the
 * formatter is replaced with a correct one -- otherwise an embfmt defect would present as a CLI
 * test failure and vice versa. embfmt's own defects were reproduced on the host separately;
 * see patches/embfmt/README.md.
 */
void MSG(const char *format, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    if (n > 0) {
        cli_uut_append(buf, (unsigned)((size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1U));
    }
}

void MSGchar(char c) { cli_uut_append(&c, 1U); }
void MSGraw(const char *str) { cli_uut_append(str, (unsigned)strlen(str)); }
void MSG_init(void) {}

/* serial_io_rx_callback() is __weak in the firmware and overridden by cli.c; nothing here
   drives the UART, so the declaration is all that is needed. */

#include "cli/cli.c"

/* --- reaching the file-statics ------------------------------------------------------------ */

/* tokenize_cli_line() is static, so including the .c exposes it to THIS translation unit and
   no further. One forwarding wrapper is what lets the test see it, and it is deliberately the
   only one: everything else the tests use -- process_cli_line(), cli_register_command(),
   cli_remove_command() -- is already part of the public interface, which is where a test
   should be looking. */
void cli_uut_tokenize(char *pLine, CliToken_Type ppTok[], uint32_t tokMaxLen,
                      uint32_t tokMaxCnt, uint32_t *pTokCnt) {
    tokenize_cli_line(pLine, ppTok, tokMaxLen, tokMaxCnt, pTokCnt);
}

/* The command history: put_onto_cmd_hist_stk() and the stack it pushes are file-statics too. */
void cli_uut_push_history(const char *pLine) { put_onto_cmd_hist_stk(pLine); }
const char *cli_uut_history(int i) { return sppCmdHistStk[i]; }
int cli_uut_history_level(void) { return sHistStkLevel; }
int cli_uut_linebuf_length(void) { return CLI_LINEBUF_LENGTH; }
int cli_uut_command_count(void) { return sCliCmdCnt; }
void cli_uut_clear_history(void) {
    memset(sppCmdHistStk, 0, sizeof(sppCmdHistStk));
    sHistStkLevel = 0;
}
