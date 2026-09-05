// SPDX-License-Identifier: MIT

#include "windbgmcp/pipe_endpoint.h"

#include <gtest/gtest.h>

#include <string>

TEST(PipeEndpoint, EmptyInputUsesDefault) {
    const auto result = windbgmcp::NormalizePipeEndpoint("   \t");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.endpoint, R"(\\.\pipe\windbgmcp)");
}

TEST(PipeEndpoint, CanonicalizesShortName) {
    const auto result = windbgmcp::NormalizePipeEndpoint("windbgmcp-project_1.run-42");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.endpoint, R"(\\.\pipe\windbgmcp-project_1.run-42)");
}

TEST(PipeEndpoint, AcceptsFullEndpointAndWinDbgQuotes) {
    const auto result = windbgmcp::NormalizePipeEndpoint(
        R"(  "\\.\PIPE\windbgmcp-session-7"  )");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.endpoint, R"(\\.\pipe\windbgmcp-session-7)");
}

TEST(PipeEndpoint, ExplicitEmptyValueIsRejected) {
    const auto result = windbgmcp::NormalizePipeEndpoint("", /*allow_default=*/false);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("must not be empty"), std::string::npos);
}

TEST(PipeEndpoint, RejectsForeignPathAndUnsafeCharacters) {
    EXPECT_FALSE(windbgmcp::NormalizePipeEndpoint(R"(\\server\pipe\shared)").ok);
    EXPECT_FALSE(windbgmcp::NormalizePipeEndpoint("windbgmcp session").ok);
    EXPECT_FALSE(windbgmcp::NormalizePipeEndpoint("windbgmcp/session").ok);
    EXPECT_FALSE(windbgmcp::NormalizePipeEndpoint("..").ok);
}

TEST(PipeEndpoint, RejectsOverlongName) {
    EXPECT_FALSE(windbgmcp::NormalizePipeEndpoint(std::string(241, 'a')).ok);
}
