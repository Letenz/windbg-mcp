// SPDX-License-Identifier: MIT
//
// Length-prefixed JSON frame codec.
//
//   wire format:  [ uint32 LE length ][ payload bytes ]
//
// Pure functions; no I/O. The pipe layer feeds bytes in and pops complete
// frames out. The router layer parses payload as JSON and dispatches.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace windbgmcp::ipc {

// Maximum length of a single payload (4-byte header excluded). Mirrors
// kMaxFrameBytes in the public header. We re-declare here to avoid pulling
// the public header into ipc internals.
inline constexpr std::uint32_t kMaxPayloadBytes = 16u * 1024u * 1024u;

// Encode a payload into a wire-ready buffer (4-byte LE length + payload).
// Returns false iff payload exceeds kMaxPayloadBytes.
bool Encode(std::string_view payload, std::vector<std::uint8_t>& out);

// Stateful decoder: feed it bytes from a stream; pop complete payloads with
// TryPop until it returns std::nullopt. Disconnects the stream if a single
// frame exceeds kMaxPayloadBytes (signaled via HasError()).
class Decoder {
public:
    Decoder() = default;

    // Append raw bytes received from the pipe.
    void Feed(std::span<const std::uint8_t> bytes);

    // Pop one complete payload as a UTF-8 string. Returns nullopt if no
    // complete frame is available yet.
    std::optional<std::string> TryPop();

    // Set after a frame whose declared length exceeds the maximum. Once true
    // the decoder refuses further input; the pipe layer should disconnect.
    bool HasError() const noexcept { return m_error; }

    // Bytes still buffered (incomplete frame fragment).
    std::size_t BufferedBytes() const noexcept { return m_buf.size(); }

private:
    std::vector<std::uint8_t> m_buf;
    bool m_error = false;
};

} // namespace windbgmcp::ipc
