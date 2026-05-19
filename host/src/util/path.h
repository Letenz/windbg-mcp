// SPDX-License-Identifier: MIT
//
// Path validation helpers for output_file. Must be absolute, must not
// contain '..', UTF-8 in, wide out.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace wmh::path {

// Returns the validated path or an error string describing why it was
// rejected.
struct Validated {
    std::wstring wide;
    std::string  utf8;
};

std::optional<Validated> ValidateOutputFile(std::string_view utf8, std::string& err);

} // namespace wmh::path
