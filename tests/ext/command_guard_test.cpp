// SPDX-License-Identifier: MIT

#include "handlers/command_guard.h"

#include <gtest/gtest.h>

TEST(CommandGuard, AllowsOrdinaryDebuggerCommands) {
    EXPECT_FALSE(windbgmcp::handlers::UnsafeLifecycleCommand("k; lm m driver"));
    EXPECT_FALSE(windbgmcp::handlers::UnsafeLifecycleCommand(".reload /f driver.sys"));
}

TEST(CommandGuard, RejectsExtensionLifecycleAndUnload) {
    EXPECT_TRUE(windbgmcp::handlers::UnsafeLifecycleCommand("!mcpext.stop"));
    EXPECT_TRUE(windbgmcp::handlers::UnsafeLifecycleCommand("!MCPEXT.status"));
    EXPECT_TRUE(windbgmcp::handlers::UnsafeLifecycleCommand(".unload mcpext.dll"));
    EXPECT_TRUE(windbgmcp::handlers::UnsafeLifecycleCommand("r; .reboot; g"));
}

TEST(CommandGuard, RejectsDebuggerQuitSegments) {
    EXPECT_TRUE(windbgmcp::handlers::UnsafeLifecycleCommand("q"));
    EXPECT_TRUE(windbgmcp::handlers::UnsafeLifecycleCommand("r; q\n"));
    EXPECT_TRUE(windbgmcp::handlers::UnsafeLifecycleCommand("  QD  "));
    EXPECT_TRUE(windbgmcp::handlers::UnsafeLifecycleCommand("q /d"));
}

TEST(CommandBatch, SplitsOnlyTopLevelSeparators) {
    auto batch = windbgmcp::handlers::ParseCommandBatch(".echo A\r\n.echo B;\n.echo C");
    EXPECT_TRUE(batch.error.empty());
    EXPECT_EQ(batch.commands, (std::vector<std::string>{".echo A", ".echo B", ".echo C"}));
    batch = windbgmcp::handlers::ParseCommandBatch(".printf \"A;B\\n\"; .if (1) {\n.echo C; .echo D;\n}; r");
    EXPECT_TRUE(batch.error.empty());
    EXPECT_EQ(batch.commands.size(), 3u);
}

TEST(CommandBatch, PreservesCommentAndAliasLineOwnership) {
    auto batch = windbgmcp::handlers::ParseCommandBatch("* comment; r\nas Alias value; r\n.echo next");
    EXPECT_TRUE(batch.error.empty());
    EXPECT_EQ(batch.commands, (std::vector<std::string>{"* comment; r", "as Alias value; r", ".echo next"}));
    batch = windbgmcp::handlers::ParseCommandBatch("$<script.txt; r\n.echo next");
    EXPECT_EQ(batch.commands, (std::vector<std::string>{"$<script.txt; r", ".echo next"}));
}

TEST(CommandBatch, RejectsMalformedOrOversizedInput) {
    EXPECT_FALSE(windbgmcp::handlers::ParseCommandBatch(".printf \"broken").error.empty());
    EXPECT_FALSE(windbgmcp::handlers::ParseCommandBatch(".if (1) { r").error.empty());
    EXPECT_FALSE(windbgmcp::handlers::ParseCommandBatch(std::string("r\0k", 3)).error.empty());
    std::string large;
    for (int i = 0; i < 65; ++i) large += "r;";
    EXPECT_FALSE(windbgmcp::handlers::ParseCommandBatch(large).error.empty());
    EXPECT_FALSE(windbgmcp::handlers::ParseCommandBatch("j (1) 'g'; 'r'").error.empty());
}

TEST(CommandBatch, OnlyTerminalStandaloneRunControlIsAllowed) {
    using namespace windbgmcp::handlers;
    EXPECT_FALSE(UnsafeRunControl(ParseCommandBatch("r; g;\n")));
    EXPECT_FALSE(UnsafeRunControl(ParseCommandBatch(".printf \"literal g; p; t\\n\"; r")));
    EXPECT_TRUE(UnsafeRunControl(ParseCommandBatch("r; g; k")));
    EXPECT_TRUE(UnsafeRunControl(ParseCommandBatch("r; p; k")));
    EXPECT_TRUE(UnsafeRunControl(ParseCommandBatch(".if (1) { g }")));
    EXPECT_TRUE(UnsafeRunControl(ParseCommandBatch(".for (;;) { g; r }")));
    EXPECT_TRUE(UnsafeRunControl(ParseCommandBatch(".if (1) { j (1) 'g'; 'r' }")));
    EXPECT_FALSE(UnsafeRunControl(ParseCommandBatch("as Alias value; g; q\n.echo next")));
}

TEST(CommandGuard, IgnoresQuotedTextButChecksNestedStatements) {
    using namespace windbgmcp::handlers;
    EXPECT_FALSE(UnsafeLifecycleCommand(".printf \"literal .unload; q; !mcpext.stop\\n\"; r"));
    EXPECT_FALSE(UnsafeLifecycleCommand("* q; .unload; !mcpext.stop"));
    EXPECT_FALSE(UnsafeLifecycleCommand("as Alias value; q; .unload\n.echo next"));
    EXPECT_TRUE(UnsafeLifecycleCommand(".if (1) { .unload mcpext.dll }"));
    EXPECT_TRUE(UnsafeLifecycleCommand(".if (1) { q }"));
    EXPECT_TRUE(UnsafeLifecycleCommand(".printf \"safe\"; .reboot"));
}
