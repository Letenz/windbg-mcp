// SPDX-License-Identifier: MIT
#include "util/path.h"

#include <Windows.h>
#include <cctype>

namespace wmh::path {

namespace {
std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}
} // namespace

std::optional<Validated> ValidateOutputFile(std::string_view utf8, std::string& err) {
    if (utf8.empty()) {
        err = "output_file is empty";
        return std::nullopt;
    }
    // Reject path traversal.
    if (utf8.find("..") != std::string_view::npos) {
        err = "output_file may not contain '..'";
        return std::nullopt;
    }
    // Require absolute path: either "X:" or "\\".
    bool ok = false;
    if (utf8.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(utf8[0])) &&
        utf8[1] == ':') {
        ok = true;
    } else if (utf8.size() >= 2 && (utf8[0] == '\\' || utf8[0] == '/') &&
               (utf8[1] == '\\' || utf8[1] == '/')) {
        ok = true;     // UNC path
    }
    if (!ok) {
        err = "output_file must be an absolute path";
        return std::nullopt;
    }
    Validated v;
    v.utf8 = std::string(utf8);
    v.wide = Utf8ToWide(utf8);
    return v;
}

} // namespace wmh::path
