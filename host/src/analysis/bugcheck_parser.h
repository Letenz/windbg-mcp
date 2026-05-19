// SPDX-License-Identifier: MIT
//
// Parse `!analyze -v` output into structured fields. C++ port of the
// Python prototype; same patterns, same field semantics. Every helper is
// best-effort: returns std::nullopt / leaves fields null when the input
// doesn't match. Raw text is still carried through for the AI.

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace wmh::analysis::bugcheck {

// Extract bugcheck name + 0x-prefixed code + 4 Args + summary text.
// Returns nullptr (a json null) when no bugcheck header found.
nlohmann::json ParseBugcheck(std::string_view text);

// Extract faulting IP / module / function / offset. Handles "mod!func+off",
// "mod!func", and bare "mod+off".
nlohmann::json ParseFaulting(std::string_view text);

// Probable culprit from MODULE_NAME / IMAGE_NAME + timestamp.
nlohmann::json ParseProbableCulprit(std::string_view text);

// CONTEXT register dump as { "rax": "0x...", ... }.
nlohmann::json ParseContext(std::string_view text);

// TRAP_FRAME pointer if present.
nlohmann::json ParseIretFrame(std::string_view text);

} // namespace wmh::analysis::bugcheck
