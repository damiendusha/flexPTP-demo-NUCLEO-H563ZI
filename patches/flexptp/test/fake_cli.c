#include "fake_cli.h"

#define FAKE_CLI_SLOTS (64) /* lib/cli/cli.c's CLI_MAX_CMD_CNT */

static const char *s_hints[FAKE_CLI_SLOTS];
static int s_count;

int fake_cli_register(const char *pHintLine, uint8_t cmdTokCnt, uint8_t minArgCnt, fnCliCallback pCB) {
    (void)cmdTokCnt;
    (void)minArgCnt;
    (void)pCB;
    if (s_count >= FAKE_CLI_SLOTS) {
        return -1;
    }
    s_hints[s_count] = pHintLine;
    return s_count++;
}

void fake_cli_remove(int handle) {
    if ((handle < 0) || (handle >= s_count)) {
        return;
    }
    for (int i = handle; i < s_count - 1; i++) {
        s_hints[i] = s_hints[i + 1];
    }
    s_count--;
}

int fake_cli_count(void) { return s_count; }

const char *fake_cli_hint(int position) {
    return ((position >= 0) && (position < s_count)) ? s_hints[position] : NULL;
}

void fake_cli_reset(void) { s_count = 0; }
