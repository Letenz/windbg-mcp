// SPDX-License-Identifier: MIT

#include "ipc/frame_codec.h"
#include "ipc/pipe_server.h"
#include "ipc/router.h"

#include <gtest/gtest.h>

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string RouterTestEndpoint() {
    static std::atomic<unsigned long> sequence{0};
    return "\\\\.\\pipe\\windbgmcp-router-test-" +
           std::to_string(::GetCurrentProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1));
}

HANDLE Connect(const std::string& endpoint) {
    const std::wstring wide(endpoint.begin(), endpoint.end());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        HANDLE client = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (client != INVALID_HANDLE_VALUE) return client;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return INVALID_HANDLE_VALUE;
}

bool ReadExact(HANDLE handle, void* buffer, DWORD bytes) {
    auto* cursor = static_cast<std::uint8_t*>(buffer);
    while (bytes > 0) {
        DWORD got = 0;
        if (!::ReadFile(handle, cursor, bytes, &got, nullptr) || got == 0) return false;
        cursor += got;
        bytes -= got;
    }
    return true;
}

bool WriteFrame(HANDLE handle, const nlohmann::json& message) {
    std::vector<std::uint8_t> wire;
    if (!windbgmcp::ipc::Encode(message.dump(), wire)) return false;
    DWORD written = 0;
    return ::WriteFile(handle, wire.data(), static_cast<DWORD>(wire.size()),
                       &written, nullptr) && written == wire.size();
}

} // namespace

TEST(RouterLifecycle, WorkerStopRefusesSelfJoinAndOwnerCanJoinLater) {
    const std::string endpoint = RouterTestEndpoint();
    windbgmcp::ipc::PipeServer pipe(endpoint);
    windbgmcp::ipc::Router router;
    router.SetPipe(&pipe);

    std::promise<bool> worker_stop_result;
    router.Register("self_stop", [&](std::int64_t, const nlohmann::json&,
                                     windbgmcp::ipc::PipeServer&,
                                     windbgmcp::ipc::ConnectionGeneration) {
        worker_stop_result.set_value(router.Stop());
        return nlohmann::json{{"ok", true}};
    });
    router.Start();
    ASSERT_TRUE(pipe.Start([&](std::string payload,
                               windbgmcp::ipc::ConnectionGeneration generation) {
        router.OnPayload(std::move(payload), generation);
    }));
    router.CommitStart();

    HANDLE client = Connect(endpoint);
    ASSERT_NE(client, INVALID_HANDLE_VALUE);

    const std::string request = nlohmann::json{
        {"frame", "req"}, {"id", 1}, {"op", "self_stop"},
        {"args", nlohmann::json::object()}}.dump();
    std::vector<std::uint8_t> wire;
    ASSERT_TRUE(windbgmcp::ipc::Encode(request, wire));
    DWORD written = 0;
    ASSERT_TRUE(::WriteFile(client, wire.data(), static_cast<DWORD>(wire.size()),
                            &written, nullptr));
    ASSERT_EQ(written, wire.size());

    auto result = worker_stop_result.get_future();
    ASSERT_EQ(result.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(result.get());

    // The worker was not detached or destroyed. A non-worker owner can join
    // it deterministically and only then allow Router destruction.
    EXPECT_TRUE(router.Stop());
    ::CloseHandle(client);
    pipe.Stop();
}

TEST(RouterLifecycle, FinalActionRunsOnlyAfterReadableResponseFencesGeneration) {
    const std::string endpoint = RouterTestEndpoint();
    windbgmcp::ipc::PipeServer pipe(endpoint);
    windbgmcp::ipc::Router router;
    router.SetPipe(&pipe);

    struct ActionObservation {
        windbgmcp::ipc::ConnectionGeneration generation = 0;
        bool pipe_running = true;
        bool generation_current = true;
    };
    std::promise<ActionObservation> action_called;
    auto action_result = action_called.get_future();
    router.Register("shutdown", [](std::int64_t, const nlohmann::json&,
                                    windbgmcp::ipc::PipeServer&,
                                    windbgmcp::ipc::ConnectionGeneration generation) {
        return nlohmann::json{{"connection_generation", generation}};
    });
    router.RegisterFinalResponseAction(
        "shutdown",
        [&](windbgmcp::ipc::ConnectionGeneration generation) {
            // SendFinal retires the generation before this callback runs.
            action_called.set_value(ActionObservation{
                generation, pipe.IsRunning(), pipe.IsCurrentGeneration(generation)});
        });
    router.Start();
    ASSERT_TRUE(pipe.Start([&](std::string payload,
                               windbgmcp::ipc::ConnectionGeneration generation) {
        router.OnPayload(std::move(payload), generation);
    }));
    router.CommitStart();

    HANDLE client = Connect(endpoint);
    ASSERT_NE(client, INVALID_HANDLE_VALUE);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!pipe.IsConnected() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(pipe.IsConnected());
    const auto generation = pipe.CurrentGeneration();

    const std::string request = nlohmann::json{
        {"frame", "req"}, {"id", 7}, {"op", "shutdown"},
        {"args", nlohmann::json::object()}}.dump();
    std::vector<std::uint8_t> wire;
    ASSERT_TRUE(windbgmcp::ipc::Encode(request, wire));
    DWORD written = 0;
    ASSERT_TRUE(::WriteFile(client, wire.data(), static_cast<DWORD>(wire.size()),
                            &written, nullptr));
    ASSERT_EQ(written, wire.size());

    std::uint32_t response_size = 0;
    ASSERT_TRUE(ReadExact(client, &response_size, sizeof(response_size)));
    std::string response_body(response_size, '\0');
    ASSERT_TRUE(ReadExact(client, response_body.data(), response_size));
    const auto response = nlohmann::json::parse(response_body);
    EXPECT_TRUE(response.value("ok", false));
    EXPECT_EQ(response.value("id", 0), 7);
    EXPECT_EQ(response["data"].value("connection_generation", 0ull), generation);
    ASSERT_TRUE(WriteFrame(client, nlohmann::json{
        {"frame", "ack"}, {"terminal", true}, {"id", 7}}));

    ASSERT_EQ(action_result.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    const auto action = action_result.get();
    EXPECT_EQ(action.generation, generation);
    EXPECT_FALSE(action.pipe_running);
    EXPECT_FALSE(action.generation_current);

    EXPECT_TRUE(router.Stop());
    ::CloseHandle(client);
    pipe.Stop();
}
