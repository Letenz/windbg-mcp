// SPDX-License-Identifier: MIT
//
// Public protocol constants shared between the C++ extension and any other
// consumer (tests, future native clients). Mirror of docs/protocol.md. Do
// not put logic here — only literals and small enums.

#pragma once

#include <cstddef>
#include <cstdint>

namespace windbgmcp {

// On-the-wire protocol version. Bump only on a breaking change.
inline constexpr int kProtocolVersion = 2;

// Default local named-pipe endpoint. A session may select a different
// endpoint at startup; the current wire format is protocol v2.
inline constexpr char    kDefaultPipeEndpoint[]     = "\\\\.\\pipe\\windbgmcp";
inline constexpr wchar_t kDefaultPipeEndpointWide[] = L"\\\\.\\pipe\\windbgmcp";
inline constexpr char    kPipeEndpointPrefix[]      = "\\\\.\\pipe\\";

// Backward-compatible source alias for v1 consumers. New code should use
// kDefaultPipeEndpointWide because the endpoint is no longer globally fixed.
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\windbgmcp";

// Host-side environment fallback. Command-line --pipe takes precedence.
inline constexpr char kPipeEnvironmentVariable[] = "WINDBGMCP_PIPE";

// Single-frame hard cap. Anything larger is a protocol error.
inline constexpr std::uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

// Soft cap for inline `output` field in run_cmd responses. Above this we
// truncate to head + tail and tell the caller to use output_file next time.
inline constexpr std::uint32_t kInlineOutputSoftLimit = 256u * 1024u;

// Default preview head bytes when streaming to file.
inline constexpr std::uint32_t kDefaultPreviewHead = 8u * 1024u;
inline constexpr std::uint32_t kPreviewTail        = 2u * 1024u;

// Timeout tiers (ms).
inline constexpr std::uint32_t kTimeoutInteractiveMs = 5'000;
inline constexpr std::uint32_t kTimeoutStandardMs    = 30'000;
inline constexpr std::uint32_t kTimeoutHeavyMs       = 120'000;

// Event ring buffer sizing.
inline constexpr std::size_t kEventRingCapacity  = 64;
inline constexpr std::uint64_t kEventRingMaxAgeMs = 30'000;

// Error codes. Keep in sync with docs/protocol.md and server/errors.py.
namespace err {
inline constexpr char kDisconnected[]   = "disconnected";
inline constexpr char kNotAttached[]    = "not_attached";
inline constexpr char kTargetRunning[]  = "target_running";
inline constexpr char kTimeout[]        = "timeout";
inline constexpr char kEngineError[]    = "engine_error";
inline constexpr char kInvalidArg[]     = "invalid_arg";
inline constexpr char kProtocolError[]  = "protocol_error";
} // namespace err

} // namespace windbgmcp
