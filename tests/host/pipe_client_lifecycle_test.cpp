// SPDX-License-Identifier: MIT

#include "transport/pipe_client.h"
#include "transport/frame_codec.h"
#include "windbgmcp/protocol.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

namespace {

std::string UniqueEndpoint() {
    static std::atomic<unsigned long> sequence{0};
    return "\\\\.\\pipe\\windbgmcp-test-" +
           std::to_string(::GetCurrentProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1));
}

std::wstring Widen(const std::string& value) {
    return std::wstring(value.begin(), value.end());
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
    if (!wmh::transport::Encode(message.dump(), wire)) return false;
    DWORD written = 0;
    return ::WriteFile(handle, wire.data(), static_cast<DWORD>(wire.size()),
                       &written, nullptr) && written == wire.size();
}

} // namespace

TEST(PipeClientLifecycle, ConnectCloseIsPermanentAndIdempotent) {
    const std::string endpoint = UniqueEndpoint();
    const std::wstring endpoint_wide = Widen(endpoint);
    HANDLE server = ::CreateNamedPipeW(
        endpoint_wide.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    ASSERT_NE(server, INVALID_HANDLE_VALUE);

    wmh::transport::PipeClient client(endpoint);
    ASSERT_TRUE(client.EnsureConnected(2'000));
    const BOOL accepted = ::ConnectNamedPipe(server, nullptr);
    ASSERT_TRUE(accepted || ::GetLastError() == ERROR_PIPE_CONNECTED);
    EXPECT_TRUE(client.IsConnected());

    client.Close();
    EXPECT_TRUE(client.IsClosed());
    EXPECT_FALSE(client.IsConnected());
    EXPECT_FALSE(client.EnsureConnected(10));
    client.Close();

    ::DisconnectNamedPipe(server);
    ::CloseHandle(server);
}

TEST(PipeClientLifecycle, CloseSerializesWithFailedConnectAttempt) {
    const std::string endpoint = UniqueEndpoint();
    wmh::transport::PipeClient client(endpoint);

    auto connect = std::async(std::launch::async, [&] {
        return client.EnsureConnected(250);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto close = std::async(std::launch::async, [&] { client.Close(); });

    ASSERT_EQ(connect.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(connect.get());
    ASSERT_EQ(close.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    close.get();
    EXPECT_TRUE(client.IsClosed());
    EXPECT_FALSE(client.EnsureConnected(10));
}

TEST(PipeClientLifecycle, ChunkSinkCanCloseClientWithoutPendingMutexDeadlock) {
    const std::string endpoint = UniqueEndpoint();
    const std::wstring endpoint_wide = Widen(endpoint);
    HANDLE server = ::CreateNamedPipeW(
        endpoint_wide.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    ASSERT_NE(server, INVALID_HANDLE_VALUE);

    wmh::transport::PipeClient client(endpoint);
    ASSERT_TRUE(client.EnsureConnected(2'000));
    const BOOL accepted = ::ConnectNamedPipe(server, nullptr);
    ASSERT_TRUE(accepted || ::GetLastError() == ERROR_PIPE_CONNECTED);
    ASSERT_TRUE(WriteFrame(server, nlohmann::json{
        {"frame", "hello"},
        {"protocol_version", windbgmcp::kProtocolVersion},
        {"bridge_instance_id", "pipe-client-lifecycle-test"}}));

    std::promise<void> sink_called;
    auto request = std::async(std::launch::async, [&] {
        return client.Request(
            "stream_test", nlohmann::json::object(), 2'000,
            [&](std::string_view, bool) {
                client.Close(); // re-enters pending cleanup from reader thread
                sink_called.set_value();
            });
    });

    std::uint32_t body_size = 0;
    ASSERT_TRUE(ReadExact(server, &body_size, sizeof(body_size)));
    std::string body(body_size, '\0');
    ASSERT_TRUE(ReadExact(server, body.data(), body_size));
    const auto request_frame = nlohmann::json::parse(body);
    const auto request_id = request_frame.at("id").get<std::int64_t>();

    const std::string chunk = nlohmann::json{
        {"frame", "chunk"}, {"id", request_id}, {"seq", 0},
        {"eof", false}, {"chunk", "partial"}}.dump();
    std::vector<std::uint8_t> wire;
    ASSERT_TRUE(wmh::transport::Encode(chunk, wire));
    DWORD written = 0;
    ASSERT_TRUE(::WriteFile(server, wire.data(), static_cast<DWORD>(wire.size()),
                            &written, nullptr));
    ASSERT_EQ(written, wire.size());

    auto sink = sink_called.get_future();
    ASSERT_EQ(sink.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    ASSERT_EQ(request.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    const auto response = request.get();
    EXPECT_FALSE(response.ok);
    EXPECT_EQ(response.err.code, "disconnected");

    // The reader-thread Close intentionally defers its self-join. Complete
    // cleanup from this owner thread and verify idempotence.
    client.Close();
    ::DisconnectNamedPipe(server);
    ::CloseHandle(server);
}
