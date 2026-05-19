// SPDX-License-Identifier: MIT
#include "ipc/frame_codec.h"

#include <cstring>

namespace windbgmcp::ipc {

namespace {
constexpr std::size_t kHeaderBytes = 4;

inline std::uint32_t ReadLeU32(const std::uint8_t* p) noexcept {
    return  static_cast<std::uint32_t>(p[0])        |
           (static_cast<std::uint32_t>(p[1]) << 8)  |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

inline void WriteLeU32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>( v        & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >>  8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}
} // namespace

bool Encode(std::string_view payload, std::vector<std::uint8_t>& out) {
    if (payload.size() > kMaxPayloadBytes) {
        return false;
    }
    out.resize(kHeaderBytes + payload.size());
    WriteLeU32(out.data(), static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) {
        std::memcpy(out.data() + kHeaderBytes, payload.data(), payload.size());
    }
    return true;
}

void Decoder::Feed(std::span<const std::uint8_t> bytes) {
    if (m_error || bytes.empty()) return;
    m_buf.insert(m_buf.end(), bytes.begin(), bytes.end());
}

std::optional<std::string> Decoder::TryPop() {
    if (m_error || m_buf.size() < kHeaderBytes) {
        return std::nullopt;
    }
    const std::uint32_t len = ReadLeU32(m_buf.data());
    if (len > kMaxPayloadBytes) {
        m_error = true;
        m_buf.clear();
        return std::nullopt;
    }
    if (m_buf.size() < kHeaderBytes + len) {
        return std::nullopt;
    }
    std::string out(reinterpret_cast<const char*>(m_buf.data() + kHeaderBytes), len);
    m_buf.erase(m_buf.begin(), m_buf.begin() + kHeaderBytes + len);
    return out;
}

} // namespace windbgmcp::ipc
