// SPDX-License-Identifier: MIT

#include "app/options.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(HostOptions, DefaultsWhenNoSelectionExists) {
    const auto result = wmh::app::ParseOptions({}, nullptr);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.options.pipe_endpoint, R"(\\.\pipe\windbgmcp)");
}

TEST(HostOptions, UsesEnvironmentFallback) {
    const auto result = wmh::app::ParseOptions({}, "windbgmcp-from-env");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.options.pipe_endpoint, R"(\\.\pipe\windbgmcp-from-env)");
}

TEST(HostOptions, CommandLineOverridesEnvironment) {
    const auto result = wmh::app::ParseOptions(
        {"--pipe", "windbgmcp-from-cli"}, "windbgmcp-from-env");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.options.pipe_endpoint, R"(\\.\pipe\windbgmcp-from-cli)");
}

TEST(HostOptions, SupportsEqualsForm) {
    const auto result = wmh::app::ParseOptions(
        {R"(--pipe=\\.\pipe\windbgmcp-equals)"}, nullptr);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.options.pipe_endpoint, R"(\\.\pipe\windbgmcp-equals)");
}

TEST(HostOptions, RejectsMissingDuplicateAndInvalidPipeValues) {
    EXPECT_FALSE(wmh::app::ParseOptions({"--pipe"}, nullptr).ok);
    EXPECT_FALSE(wmh::app::ParseOptions(
        {"--pipe", "one", "--pipe=two"}, nullptr).ok);
    EXPECT_FALSE(wmh::app::ParseOptions({"--pipe="}, nullptr).ok);
    EXPECT_FALSE(wmh::app::ParseOptions({"--pipe", "bad name"}, nullptr).ok);
}

TEST(HostOptions, RejectsUnknownArguments) {
    const auto result = wmh::app::ParseOptions({"--wat"}, nullptr);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("unknown argument"), std::string::npos);
}
