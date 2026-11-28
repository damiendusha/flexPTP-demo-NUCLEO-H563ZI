/* The OS-less options, plus flexPTP's two CLI hooks over a command table like the firmware's
 * (fake_cli.h). Both firmwares define CLI_REG_CMD; this defines CLI_REMOVE_CMD as well, which is
 * the path finding U16 is in. */
#ifndef FLEXPTP_TEST_OPTIONS_OSLESS_CLI
#define FLEXPTP_TEST_OPTIONS_OSLESS_CLI

#include "osless/flexptp_options.h"

#include "fake_cli.h"

#define CLI_REG_CMD(cmd_hintline, n_cmd, n_min_arg, cb) fake_cli_register(cmd_hintline, n_cmd, n_min_arg, cb)
#define CLI_REMOVE_CMD(handle) fake_cli_remove(handle)

#endif
