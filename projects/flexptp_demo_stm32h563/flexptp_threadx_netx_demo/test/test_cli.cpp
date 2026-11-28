/* The CLI tokenizer and command table.
 *
 * WHY THIS EXISTS
 *
 * `cli.c` carried finding F74 (N8, N12) and had no automated coverage. Two defects, and both
 * are memory-safety failures rather than wrong answers:
 *
 *   N12  copy_token() used strncpy(), which does NOT terminate when the source is at least as
 *        long as the limit. A token of exactly MAX_TOK_LEN characters left ppTok[i]
 *        unterminated, and every later strcmp() ran off the end into the next array element.
 *   N8   the line copy was `static char pLineCpy[CLI_LINEBUF_LENGTH]` -- one byte too small for
 *        a full-length line plus its terminator, AND shared, because this function is entered
 *        from the "cli" thread parsing input and from cli_register_command() on the "ptp"
 *        thread. strtok()'s state was shared the same way.
 *
 * Both are now correct and neither was checked. A one-byte overrun is exactly the class that
 * survives review and eyeballing.
 *
 * The command table gets the same treatment, because `ptp servo st` and `ptp servo stm` are
 * both registered on the real board: a prefix-matching mistake dispatches the wrong command
 * with no diagnostic at all.
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "cmsis_os2.h"

#include "cli/cli.h"

/* From cli_uut.c: the captured console, and the file-statics the .c include exposes. */
extern char cli_uut_output[];
extern unsigned cli_uut_output_len;
void cli_uut_clear_output(void);

/* tokenize_cli_line() is a file-static; cli_uut.c forwards to it. */
void cli_uut_tokenize(char *pLine, CliToken_Type ppTok[], uint32_t tokMaxLen,
                      uint32_t tokMaxCnt, uint32_t *pTokCnt);

/* The command history, likewise. */
void cli_uut_push_history(const char *pLine);
const char *cli_uut_history(int i);
int cli_uut_history_level(void);
int cli_uut_linebuf_length(void);
void cli_uut_clear_history(void);
}

namespace {

/* What each callback saw, so dispatch can be asserted rather than inferred from output. */
struct Dispatch {
    std::string which;
    int argc = -1;
    std::string first_arg;
};
Dispatch g_last;

template <int Id>
int record(const CliToken_Type *ppArgs, uint8_t argc) {
    g_last.which = std::to_string(Id);
    g_last.argc = argc;
    g_last.first_arg = (argc > 0) ? std::string(ppArgs[0]) : std::string();
    return 0;
}

class Cli : public ::testing::Test {
  protected:
    void SetUp() override {
        g_last = Dispatch{};
        cli_uut_clear_output();
    }

    static std::string out() { return std::string(cli_uut_output, cli_uut_output_len); }

    /** process_cli_line() writes to its argument, so it never gets a literal. */
    static void run(const std::string &line) {
        std::string buf = line;
        buf.resize(buf.size() + 1, '\0');
        process_cli_line(buf.data());
    }
};

/* Tokenising is a free function on a caller-owned buffer; this just makes it readable. */
struct Tokens {
    CliToken_Type tok[MAX_TOKEN_N];
    uint32_t count = 0;

    explicit Tokens(const std::string &line, uint32_t max_len = MAX_TOK_LEN,
                    uint32_t max_cnt = MAX_TOKEN_N) {
        std::string buf = line;
        buf.resize(buf.size() + 1, '\0');
        std::memset(tok, 0xAA, sizeof(tok));  /* poison: an unterminated token shows up */
        cli_uut_tokenize(buf.data(), tok, max_len, max_cnt, &count);
    }

    std::string operator[](unsigned i) const { return std::string(tok[i]); }
};

}  // namespace

TEST_F(Cli, SplitsOnSpaces) {
    Tokens t("ptp servo st");
    ASSERT_EQ(t.count, 3u);
    EXPECT_EQ(t[0], "ptp");
    EXPECT_EQ(t[1], "servo");
    EXPECT_EQ(t[2], "st");
}

TEST_F(Cli, CollapsesRunsOfSpaces) {
    Tokens t("  ptp    servo   ");
    ASSERT_EQ(t.count, 2u);
    EXPECT_EQ(t[0], "ptp");
    EXPECT_EQ(t[1], "servo");
}

TEST_F(Cli, EmptyAndWhitespaceOnlyLinesYieldNoTokens) {
    EXPECT_EQ(Tokens("").count, 0u);
    EXPECT_EQ(Tokens("     ").count, 0u);
}

TEST_F(Cli, ATokenOfExactlyTheMaximumLengthIsStillTerminated) {
    /* Finding F74/N12.
     *
     * strncpy() does not terminate when the source is at least as long as the limit, so a
     * MAX_TOK_LEN-character token left the buffer unterminated and every later strcmp() read
     * into the NEXT token in the array. The poison fill above is what makes that visible:
     * without termination the string runs into 0xAA bytes and the length is wrong. */
    const std::string big(MAX_TOK_LEN, 'x');
    Tokens t(big);

    ASSERT_EQ(t.count, 1u);
    EXPECT_EQ(t.tok[0][MAX_TOK_LEN - 1], '\0') << "token not terminated -- this is F74/N12";
    EXPECT_EQ(t[0].size(), static_cast<size_t>(MAX_TOK_LEN - 1))
        << "the token should be truncated to fit its terminator";
}

TEST_F(Cli, AnOverlongTokenDoesNotBleedIntoTheNextSlot) {
    /* The consequence of N12 rather than its mechanism: token 1 must be exactly what was
       typed, not the tail of token 0. */
    const std::string big(MAX_TOK_LEN + 20, 'y');
    Tokens t(big + " tail");

    ASSERT_EQ(t.count, 2u);
    EXPECT_EQ(t[1], "tail") << "the second token was corrupted by the first";
}

TEST_F(Cli, AFullLengthLineDoesNotOverrunTheCopyBuffer) {
    /* Finding F74/N8: the copy buffer was CLI_LINEBUF_LENGTH bytes for a line that can be
     * CLI_LINEBUF_LENGTH characters plus a terminator -- a one-byte overrun of a .bss object.
     *
     * A one-byte overrun does not reliably crash, so this asserts the observable consequence:
     * the line tokenises completely and the last token is intact. Under ASan or Valgrind the
     * overrun itself would be caught; here the assertion is that nothing is lost. */
    /* Exactly CLI_LINEBUF_LENGTH characters, in exactly MAX_TOKEN_N tokens: 8 x 23 + 7
       separators = 191. Both limits are hit at once on purpose -- the first version of this
       test built the line out of 47 short tokens, so tokenising stopped at the token limit
       long before the buffer was full and it asserted nothing about the buffer at all. */
    std::string line;
    for (int i = 0; i < MAX_TOKEN_N; i++) {
        if (i) {
            line += ' ';
        }
        line += std::string(23, static_cast<char>('a' + i));
    }
    ASSERT_EQ(line.size(), 191u) << "the line is not full-length; the test proves nothing";

    Tokens t(line);
    ASSERT_EQ(t.count, static_cast<uint32_t>(MAX_TOKEN_N));
    EXPECT_EQ(t[MAX_TOKEN_N - 1], std::string(23, 'h'))
        << "the tail of a full-length line was lost or corrupted";
}

TEST_F(Cli, StopsAtTheTokenLimitWithoutWritingPastTheArray) {
    Tokens t("a b c d e f g h i j k l");
    EXPECT_EQ(t.count, static_cast<uint32_t>(MAX_TOKEN_N));
    EXPECT_EQ(t[MAX_TOKEN_N - 1], "h");
}

TEST_F(Cli, DispatchesARegisteredCommand) {
    int h = cli_register_command((char *)"greet \tSay hello", 1, 0, record<1>);
    ASSERT_GE(h, 0);

    run("greet");
    EXPECT_EQ(g_last.which, "1");
    EXPECT_EQ(g_last.argc, 0);

    cli_remove_command(h);
}

TEST_F(Cli, PassesArgumentsAfterTheCommandTokens) {
    int h = cli_register_command((char *)"ptp domain \tSet domain", 2, 0, record<2>);
    ASSERT_GE(h, 0);

    run("ptp domain 42");
    EXPECT_EQ(g_last.which, "2");
    EXPECT_EQ(g_last.argc, 1);
    EXPECT_EQ(g_last.first_arg, "42");

    cli_remove_command(h);
}

TEST_F(Cli, DoesNotConfuseACommandWithALongerOneSharingItsPrefix) {
    /* `ptp servo st` and `ptp servo stm` are BOTH registered on the real board, along with
     * `ptp servo sg`. Matching compares whole tokens, so this must resolve exactly -- a
     * prefix-matching mistake dispatches the wrong handler silently, and the two set different
     * Kalman parameters.
     *
     * Registered longest-first here on purpose: the lookup returns the first table entry whose
     * tokens all match, so if it were doing prefix matching this order is the one that hides
     * it. */
    int a = cli_register_command((char *)"ptp servo stm \tsigma_theta_m^2", 3, 0, record<10>);
    int b = cli_register_command((char *)"ptp servo st \tsigma_theta^2", 3, 0, record<11>);
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);

    run("ptp servo st 5");
    EXPECT_EQ(g_last.which, "11") << "`ptp servo st` dispatched to `ptp servo stm`";
    EXPECT_EQ(g_last.first_arg, "5");

    run("ptp servo stm 6");
    EXPECT_EQ(g_last.which, "10") << "`ptp servo stm` dispatched to `ptp servo st`";
    EXPECT_EQ(g_last.first_arg, "6");

    cli_remove_command(a);
    cli_remove_command(b);
}

TEST_F(Cli, ReportsAnUnknownCommandRatherThanDispatchingAnything) {
    run("definitely not a command");
    EXPECT_EQ(g_last.argc, -1) << "something was dispatched";
    EXPECT_NE(out().find("Unknown command"), std::string::npos);
}

TEST_F(Cli, RefusesACommandWithTooFewArguments) {
    int h = cli_register_command((char *)"needsone \tNeeds an argument", 1, 1, record<3>);
    ASSERT_GE(h, 0);

    run("needsone");
    EXPECT_EQ(g_last.argc, -1) << "the callback ran without its required argument";
    EXPECT_NE(out().find("Insufficient parameters"), std::string::npos);

    cli_remove_command(h);
}

TEST_F(Cli, ARemovedCommandNoLongerDispatches) {
    int h = cli_register_command((char *)"temporary \tGoes away", 1, 0, record<4>);
    ASSERT_GE(h, 0);
    run("temporary");
    ASSERT_EQ(g_last.which, "4");

    cli_remove_command(h);
    g_last = Dispatch{};
    run("temporary");
    EXPECT_EQ(g_last.argc, -1) << "a removed command still dispatched";
}

TEST_F(Cli, RegistrationSurvivesTheTableBeingFilledAndEmptied) {
    /* `ptp reset` re-registers flexPTP's ~19 commands, so the table is filled and emptied
       repeatedly over a session. A leak of slots would eventually refuse a registration. */
    for (int round = 0; round < 20; round++) {
        int h = cli_register_command((char *)"churn \tregistered and removed", 1, 0, record<5>);
        ASSERT_GE(h, 0) << "registration failed on round " << round << " -- slots leaked";
        cli_remove_command(h);
    }

    int h = cli_register_command((char *)"churn \tstill works", 1, 0, record<5>);
    ASSERT_GE(h, 0);
    run("churn");
    EXPECT_EQ(g_last.which, "5");
    cli_remove_command(h);
}

TEST_F(Cli, HelpListsRegisteredCommands) {
    int h = cli_register_command((char *)"visible \tShows up in help", 1, 0, record<6>);
    ASSERT_GE(h, 0);

    run("?");
    EXPECT_NE(out().find("visible"), std::string::npos);
    EXPECT_NE(out().find("Shows up in help"), std::string::npos);

    cli_remove_command(h);
}

TEST_F(Cli, RemovingACommandFromTheMiddleLeavesTheRestWhole) {
    /* A handle is the command's index in the table, and removing one compacts the table: every
     * later entry moves down a slot, by a struct copy whose hint and padding pointers are then
     * rebased onto the destination. Every other test here removes only the last entry, which
     * moves nothing -- so this is the one test of the compaction itself. The slot the last
     * entry vacates is then reused, so that a hint still pointing into it would read the
     * newcomer's text. */
    int a = cli_register_command((char *)"alpha \tfirst hint", 1, 0, record<20>);
    int b = cli_register_command((char *)"bravo \tsecond hint", 1, 0, record<21>);
    int c = cli_register_command((char *)"charlie \tthird hint", 1, 0, record<22>);
    ASSERT_GE(a, 0);
    ASSERT_EQ(b, a + 1);
    ASSERT_EQ(c, a + 2);
    run("?");  /* format the help first: that is what splits each entry into its hint */

    cli_remove_command(a);
    int d = cli_register_command((char *)"delta \tfourth hint", 1, 0, record<23>);
    ASSERT_EQ(d, c) << "the slot charlie left was not the one reused";

    run("bravo");
    EXPECT_EQ(g_last.which, "21");
    run("charlie");
    EXPECT_EQ(g_last.which, "22");
    g_last = Dispatch{};
    run("alpha");
    EXPECT_EQ(g_last.argc, -1) << "a removed command still dispatched";

    cli_uut_clear_output();
    run("?");
    const auto line = [](const std::string &text, const std::string &cmd) {
        const auto at = text.find(cmd);
        if (at == std::string::npos) return std::string();
        const auto begin = text.rfind('\n', at);
        return text.substr(begin == std::string::npos ? 0 : begin, text.find('\n', at) - begin);
    };
    EXPECT_NE(line(out(), "bravo").find("second hint"), std::string::npos) << line(out(), "bravo");
    EXPECT_NE(line(out(), "charlie").find("third hint"), std::string::npos) << line(out(), "charlie");
    EXPECT_NE(line(out(), "delta").find("fourth hint"), std::string::npos) << line(out(), "delta");

    /* Handles shift as entries go, so take them from the top down. */
    cli_remove_command(d);
    cli_remove_command(c - 1);
    cli_remove_command(b - 1);
}

/* --- the command history ------------------------------------------------------------------
 *
 * Eight rows, newest first. A push moves every row down by one -- once a strcpy per row, which
 * was free of overlap only because no row fills its 192 bytes, now one memmove of whole rows --
 * so these pin what a push must leave, including for a line as long as get_line() allows. */

class History : public ::testing::Test {
  protected:
    void SetUp() override { cli_uut_clear_history(); }
    void TearDown() override { cli_uut_clear_history(); }
};

TEST_F(History, KeepsTheNewestFirstAndDropsTheOldest) {
    for (int i = 1; i <= 9; i++) {
        cli_uut_push_history(("cmd" + std::to_string(i)).c_str());
    }
    EXPECT_EQ(cli_uut_history_level(), 8) << "eight rows, and the level stops there";
    for (int row = 0; row < 8; row++) {
        EXPECT_EQ(std::string(cli_uut_history(row)), "cmd" + std::to_string(9 - row)) << "row " << row;
    }
}

TEST_F(History, DoesNotPushARepeatOfTheNewest) {
    cli_uut_push_history("ping");
    cli_uut_push_history("ping");
    EXPECT_EQ(cli_uut_history_level(), 1);
    cli_uut_push_history("pin");  /* a prefix of the newest is a different command */
    EXPECT_EQ(cli_uut_history_level(), 2);
    EXPECT_EQ(std::string(cli_uut_history(0)), "pin");
    EXPECT_EQ(std::string(cli_uut_history(1)), "ping");
}

TEST_F(History, AFullLengthLineMovesDownWhole) {
    /* CLI_LINEBUF_LENGTH characters: as long as get_line() lets a line be, so its terminator is
       the last byte of its row -- the case the per-row strcpy depended on most. */
    const std::string full(static_cast<std::size_t>(cli_uut_linebuf_length()), 'x');
    cli_uut_push_history(full.c_str());
    for (int i = 0; i < 7; i++) {
        cli_uut_push_history(("short" + std::to_string(i)).c_str());
    }
    EXPECT_EQ(std::string(cli_uut_history(7)), full) << "the full-length line, now the oldest row";
    EXPECT_EQ(std::string(cli_uut_history(6)), "short0");
}

/* --- kernel bring-up: the CLI's mutex is a real one ---------------------------------------- */

namespace {

int g_result = 1;

void test_main(void *arg) {
    (void)arg;
    g_result = RUN_ALL_TESTS();
    std::exit(g_result);
}

}  // namespace

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    if (osKernelInitialize() != osOK) {
        std::fprintf(stderr, "osKernelInitialize() failed\n");
        return 2;
    }

    osThreadAttr_t attr{};
    attr.name = "tests";
    attr.stack_size = 65536;
    attr.priority = osPriorityNormal;
    if (osThreadNew(test_main, nullptr, &attr) == nullptr) {
        std::fprintf(stderr, "could not create the test thread\n");
        return 2;
    }

    osKernelStart();
    return 2;
}
