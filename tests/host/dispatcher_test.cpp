// SPDX-License-Identifier: MIT

#include "tools/dispatcher.h"

#include <gtest/gtest.h>

#include <map>
#include <string>

TEST(DispatcherTools, LifecycleToolsHaveDistinctAndClosedSchemas) {
    wmh::transport::PipeClient pipe("\\\\.\\pipe\\windbgmcp-dispatcher-schema-test");
    wmh::tools::Dispatcher dispatcher(pipe);

    const auto descriptors = dispatcher.Descriptors();
    ASSERT_TRUE(descriptors.is_array());
    EXPECT_EQ(descriptors.size(), 8u);

    std::map<std::string, nlohmann::json> by_name;
    for (const auto& descriptor : descriptors) {
        by_name.emplace(descriptor.at("name").get<std::string>(), descriptor);
    }

    for (const char* name : {"wm_detach", "wm_shutdown", "wm_exit"}) {
        ASSERT_TRUE(by_name.count(name));
        const auto& schema = by_name.at(name).at("inputSchema");
        EXPECT_EQ(schema.at("type"), "object");
        EXPECT_TRUE(schema.at("properties").empty());
        EXPECT_FALSE(schema.at("additionalProperties").get<bool>());
    }

    EXPECT_NE(by_name.at("wm_detach").at("description").get<std::string>().find(
                  "target only"),
              std::string::npos);
    EXPECT_NE(by_name.at("wm_shutdown").at("description").get<std::string>().find(
                  "after delivering the response"),
              std::string::npos);
    EXPECT_NE(by_name.at("wm_exit").at("description").get<std::string>().find(
                  "Deprecated"),
              std::string::npos);
}

TEST(DispatcherTools, SemanticCommandErrorsPreserveEvidence) {
    wmh::transport::Response response;
    response.ok = true;
    response.data = {{"ok", false}, {"output", "prefix and failure"}, {"commands_executed", 2},
                     {"err", {{"code", "engine_error"}}}};
    const auto result = wmh::tools::FromResponse(response);
    EXPECT_TRUE(result.is_error);
    EXPECT_EQ(nlohmann::json::parse(result.text)["output"], "prefix and failure");
}

TEST(DispatcherTools, RejectsInvalidRunCommandArgumentsWithoutConnecting) {
    wmh::transport::PipeClient pipe("windbgmcp-no-connection-needed");
    wmh::tools::Dispatcher dispatcher(pipe);
    for (const auto& args : std::vector<nlohmann::json>{
        {{"cmd", "r"}, {"timeout_ms", -1}}, {{"cmd", "r"}, {"timeout_ms", 0}},
        {{"cmd", "r"}, {"timeout_ms", "100"}}, {{"cmd", "r"}, {"timeout_ms", 120001}},
        {{"cmd", "r"}, {"preview_bytes", -1}}, {{"cmd", "r"}, {"output_file", 3}},
        {{"cmd", "r"}, {"unknown", true}}, {{"cmd", nlohmann::json::array({"r", "k"})}},
    }) {
        const auto result = dispatcher.Call("wm_run_cmd", args);
        EXPECT_TRUE(result.is_error);
        EXPECT_EQ(nlohmann::json::parse(result.text)["err"]["code"], "invalid_arg");
    }
}
