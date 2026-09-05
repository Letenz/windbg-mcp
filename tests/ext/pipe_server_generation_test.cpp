// SPDX-License-Identifier: MIT

#include "ipc/pipe_server.h"

#include <gtest/gtest.h>

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string UniqueEndpoint() {
    static std::atomic<unsigned long> sequence{0};
    return "\\\\.\\pipe\\windbgmcp-ext-test-" +
           std::to_string(::GetCurrentProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1));
}

std::wstring Widen(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

bool WaitUntil(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

HANDLE ConnectClient(const std::wstring& endpoint) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        HANDLE client = ::CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE,
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

TEST(PipeServerGeneration, ReconnectFencesOldClientAndRetainsEndpoint) {
    const std::string endpoint = UniqueEndpoint();
    const std::wstring endpoint_wide = Widen(endpoint);
    windbgmcp::ipc::PipeServer server(endpoint);
    ASSERT_TRUE(server.Start([](std::string, windbgmcp::ipc::ConnectionGeneration) {}));

    HANDLE first = ConnectClient(endpoint_wide);
    ASSERT_NE(first, INVALID_HANDLE_VALUE);
    ASSERT_TRUE(WaitUntil([&] { return server.IsConnected(); }));
    const auto first_generation = server.CurrentGeneration();

    ::CloseHandle(first);
    ASSERT_TRUE(WaitUntil([&] { return !server.IsConnected(); }));

    // The original server handle remains open while no client is attached,
    // so another extension cannot steal this endpoint in the reconnect gap.
    HANDLE thief = ::CreateNamedPipeW(
        endpoint_wide.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    EXPECT_EQ(thief, INVALID_HANDLE_VALUE);
    if (thief != INVALID_HANDLE_VALUE) ::CloseHandle(thief);

    HANDLE second = ConnectClient(endpoint_wide);
    ASSERT_NE(second, INVALID_HANDLE_VALUE);
    ASSERT_TRUE(WaitUntil([&] { return server.IsConnected(); }));
    const auto second_generation = server.CurrentGeneration();
    ASSERT_NE(first_generation, second_generation);

    EXPECT_FALSE(server.Send("stale", first_generation));
    EXPECT_FALSE(server.SendFinal("stale-final", first_generation));
    EXPECT_TRUE(server.IsRunning());
    EXPECT_TRUE(server.Send("fresh", second_generation));

    ::CloseHandle(second);
    server.Stop();
}

TEST(PipeServerGeneration, FinalResponseIsReadableBeforeCurrentGenerationStops) {
    const std::string endpoint = UniqueEndpoint();
    const std::wstring endpoint_wide = Widen(endpoint);
    windbgmcp::ipc::PipeServer server(endpoint);
    ASSERT_TRUE(server.Start([](std::string, windbgmcp::ipc::ConnectionGeneration) {}));

    HANDLE client = ConnectClient(endpoint_wide);
    ASSERT_NE(client, INVALID_HANDLE_VALUE);
    ASSERT_TRUE(WaitUntil([&] { return server.IsConnected(); }));
    const auto generation = server.CurrentGeneration();

    const std::string payload = R"({"frame":"resp","id":41,"ok":true})";
    auto final_send = std::async(std::launch::async, [&] {
        return server.SendFinal(payload, generation, 41);
    });

    std::uint32_t body_size = 0;
    ASSERT_TRUE(ReadExact(client, &body_size, sizeof(body_size)));
    ASSERT_EQ(body_size, payload.size());
    std::string body(body_size, '\0');
    ASSERT_TRUE(ReadExact(client, body.data(), body_size));
    EXPECT_EQ(body, payload);
    ASSERT_TRUE(WriteFrame(client, nlohmann::json{
        {"frame", "ack"}, {"terminal", true}, {"id", 41}}));

    ASSERT_EQ(final_send.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);
    ASSERT_TRUE(final_send.get());
    EXPECT_FALSE(server.IsRunning());
    EXPECT_FALSE(server.IsConnected());
    EXPECT_FALSE(server.Send("late", generation));

    ::CloseHandle(client);
    server.Stop();
}
