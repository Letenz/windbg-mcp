// SPDX-License-Identifier: MIT
//
// Parse `kb` stack output into structured frames.

#pragma once

#include <nlohmann/json.hpp>
#include <string_view>

namespace wmh::analysis::stack {

// Returns a JSON array of frame objects. Empty array if nothing parses.
nlohmann::json ParseKb(std::string_view text);

} // namespace wmh::analysis::stack
