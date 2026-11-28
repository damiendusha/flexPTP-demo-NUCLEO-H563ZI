// flexPTP's CLI hooks: finding U16.
//
// flexPTP registers its commands through CLI_REG_CMD when it starts and keeps the handles in
// sCmds[]; ptp_remove_cli_commands(), on the way down, was meant to give them back through
// CLI_REMOVE_CMD. It passed its loop index instead of the handle, so it removed whatever sat at
// positions 0..CMD_N-1 of the application's table -- the application's own commands included --
// and, against a CLI whose handles are positions, removing first to last made every handle after
// the first name the entry after its own.
//
// The table here behaves as lib/cli/cli.c does (fake_cli.h). Both firmwares leave CLI_REMOVE_CMD
// undefined today, which is what kept this path dark -- and is why a stop and a start registered
// flexPTP's commands a second time.

#include <gtest/gtest.h>

#include <set>
#include <string>

extern "C" {
#include "fake_cli.h"
#include "fake_port.h"
#include <flexptp/task_ptp.h>
}

// The application's console. Nothing here logs, but the engine links against it.
extern "C" void MSG(const char *format, ...) { (void)format; }

namespace {

CMD_FUNCTION(app_command) {
    (void)ppArgs;
    (void)argc;
    return 0;
}

std::string hint(int position) {
    const char *h = fake_cli_hint(position);
    return h != nullptr ? h : "";
}

bool starts_with(const std::string &s, const std::string &prefix) { return s.rfind(prefix, 0) == 0; }

class CliHooks : public ::testing::Test {
  protected:
    void SetUp() override {
        fake_reset();
        fake_cli_reset();
    }
};

TEST_F(CliHooks, U16_StopRemovesFlexptpsCommandsAndNoOneElses) {
    ASSERT_EQ(fake_cli_register("before \tan application command", 1, 0, app_command), 0);
    ASSERT_TRUE(reg_task_ptp());
    const int flexptp = fake_cli_count() - 1;
    ASSERT_GT(flexptp, 10) << "flexPTP registered no commands: the hooks are not wired up";
    ASSERT_EQ(fake_cli_register("after \tanother application command", 1, 0, app_command), flexptp + 1);

    unreg_task_ptp();

    ASSERT_EQ(fake_cli_count(), 2) << "flexPTP's " << flexptp << " commands were not exactly what it removed";
    EXPECT_TRUE(starts_with(hint(0), "before")) << "position 0 is now: " << hint(0);
    EXPECT_TRUE(starts_with(hint(1), "after")) << "position 1 is now: " << hint(1);
}

TEST_F(CliHooks, U16_ARestartRegistersEachCommandOnce) {
    ASSERT_TRUE(reg_task_ptp());
    const int once = fake_cli_count();
    unreg_task_ptp();
    EXPECT_EQ(fake_cli_count(), 0) << "a stop left flexPTP commands registered";

    ASSERT_TRUE(reg_task_ptp());
    EXPECT_EQ(fake_cli_count(), once) << "a stop and a start registered flexPTP's commands again";
    std::set<std::string> seen;
    for (int i = 0; i < fake_cli_count(); i++) {
        EXPECT_TRUE(seen.insert(hint(i)).second) << "registered twice: " << hint(i);
    }
    unreg_task_ptp();
}

}  // namespace
