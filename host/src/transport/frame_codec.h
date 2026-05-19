// SPDX-License-Identifier: MIT
//
// Length-prefixed frame codec for the host. Same wire format as
// ext/src/ipc/frame_codec — duplicated rather than shared so the host
// stays a free-standing exe with no link dependency on the ext.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace wmh::transport {

inline constexpr std::uint32_t kMaxPayloadBytes = 16u * 1024u * 1024u;

bool Encode(std::string_view payload, std::vector<std::uint8_t>& out);

class Decoder {
public:
    void Feed(std::span<const std::uint8_t> bytes);
    std::optional<std::string> TryPop();
    bool HasError() const noexcept { return m_error; }

private:
    std::vector<std::uint8_t> m_buf;
    bool m_error = false;
};

} // namespace wmh::transport
