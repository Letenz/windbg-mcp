// SPDX-License-Identifier: MIT
#include "mcp/stdio_io.h"

#include "util/log.h"

#include <Windows.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <iostream>
#include <string>

namespace wmh::mcp {

StdioServer::StdioServer() {
    // Switch stdin/stdout to binary mode so we don't get CRLF translation.
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
}

StdioServer::~StdioServer() { Stop(); }

void StdioServer::Start(OnLine on_line) {
    if (m_running.exchange(true)) return;
    m_on_line = std::move(on_line);
    m_reader = std::thread([this] { ReaderLoop(); });
}

void StdioServer::Stop() {
    if (!m_running.exchange(false)) return;
    // Closing stdin from our own process is hard; we rely on the OS to
    // close it when the AI client exits. Just join the reader.
    if (m_reader.joinable()) m_reader.join();
}

void StdioServer::Join() {
    if (m_reader.joinable()) m_reader.join();
}

void StdioServer::ReaderLoop() {
    std::string buf;
    buf.reserve(4096);
    char ch;
    while (m_running.load()) {
        // Read one byte at a time. stdin is a pipe in the MCP case, so the
        // OS will block here until the parent writes; this is fine.
        std::size_t got = std::fread(&ch, 1, 1, stdin);
        if (got == 0) {
            if (std::feof(stdin)) {
                WMCP_LOG(Info, "stdin EOF; exiting");
                break;
            }
            if (std::ferror(stdin)) {
                WMCP_LOG(Warn, "stdin error; exiting");
                break;
            }
            continue;
        }
        if (ch == '\n') {
            // Strip optional CR.
            if (!buf.empty() && buf.back() == '\r') buf.pop_back();
            if (!buf.empty()) {
                try {
                    m_on_line(buf);
                } catch (const std::exception& e) {
                    WMCP_LOG(Error, std::string("on_line threw: ") + e.what());
                }
            }
            buf.clear();
            continue;
        }
        buf.push_back(ch);
    }
    m_finished.store(true);
}

void StdioServer::Write(const nlohmann::json& msg) {
    std::string line = msg.dump();
    line.push_back('\n');
    std::lock_guard<std::mutex> lk(m_write_mu);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

} // namespace wmh::mcp
