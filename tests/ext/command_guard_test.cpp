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
