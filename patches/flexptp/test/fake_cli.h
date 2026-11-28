/* A command table with the firmware CLI's semantics, for flexPTP's CLI hooks: finding U16.
 *
 * lib/cli/cli.c hands out a command's position in its table as the handle, and removing a command
 * moves every later one down a slot. This reproduces exactly that, and nothing else -- no
 * tokenizer, no help, no mutex -- so a test can watch what flexPTP registers and removes. The
 * types are cli.h's own, the ones flexPTP's callbacks are compiled against in the firmware. */
#ifndef FLEXPTP_TEST_FAKE_CLI_H
#define FLEXPTP_TEST_FAKE_CLI_H

#include "cli.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CLI_REG_CMD: the command's position, or -1 when the table is full, as cli.c does. */
int fake_cli_register(const char *pHintLine, uint8_t cmdTokCnt, uint8_t minArgCnt, fnCliCallback pCB);

/* CLI_REMOVE_CMD: drop the command at that position; the rest move down. Out of range: nothing. */
void fake_cli_remove(int handle);

int fake_cli_count(void);
const char *fake_cli_hint(int position); /* the hint line registered there, or NULL */
void fake_cli_reset(void);

#ifdef __cplusplus
}
#endif

#endif
