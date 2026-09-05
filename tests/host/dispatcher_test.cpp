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
