// SPDX-License-Identifier: MIT

#include "handlers/handlers.h"
#include "ipc/pipe_server.h"

#include <gtest/gtest.h>

TEST(LifecycleHandlers, ShutdownDeclaresBridgeOnlyTerminalSemantics) {
    windbgmcp::ipc::PipeServer pipe(
        "\\\\.\\pipe\\windbgmcp-lifecycle-handler-test");

    const auto result = windbgmcp::handlers::Shutdown(
        3, nlohmann::json::object(), pipe, 17);

    EXPECT_TRUE(result.at("ok").get<bool>());
    EXPECT_EQ(result.at("semantic"), "bridge_shutdown_only");
    EXPECT_FALSE(result.at("target_detached").get<bool>());
    EXPECT_EQ(result.at("bridge_state"), "stopping_after_response");
    EXPECT_EQ(result.at("host_state"), "running");
    EXPECT_EQ(result.at("connection_generation"), 17);
}
