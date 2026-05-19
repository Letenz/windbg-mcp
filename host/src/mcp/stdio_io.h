// SPDX-License-Identifier: MIT
//
// Newline-delimited JSON over stdin/stdout. Each message is one line.
// stdin is read on a worker thread; outbound writes are serialised via
// mutex. We set stdin/stdout to binary mode so newlines aren't mangled.

#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>

#include <nlohmann/json.hpp>

namespace wmh::mcp {

using OnLine = std::function<void(const std::string& line)>;

class StdioServer {
public:
    StdioServer();
    ~StdioServer();

    StdioServer(const StdioServer&) = delete;
    StdioServer& operator=(const StdioServer&) = delete;

    void Start(OnLine on_line);
    void Stop();

    // Serialised write. Appends a trailing newline if not present.
    void Write(const nlohmann::json& msg);

    void Join();   // block until stdin closes

private:
    void ReaderLoop();

    OnLine                     m_on_line;
    std::thread                m_reader;
    std::mutex                 m_write_mu;
    std::atomic<bool>          m_running{false};
    std::atomic<bool>          m_finished{false};
};

} // namespace wmh::mcp
