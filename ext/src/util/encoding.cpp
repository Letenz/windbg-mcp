// SPDX-License-Identifier: MIT
#include "util/encoding.h"

#include <Windows.h>

namespace windbgmcp::encoding {

namespace {

std::wstring CodepageToWide(unsigned int codepage, std::string_view in) {
    if (in.empty()) return {};
    int n = ::MultiByteToWideChar(codepage, 0, in.data(), static_cast<int>(in.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out;
    out.resize(static_cast<std::size_t>(n));
    ::MultiByteToWideChar(codepage, 0, in.data(), static_cast<int>(in.size()), out.data(), n);
    return out;
}

std::string WideToCodepage(unsigned int codepage, std::wstring_view in) {
    if (in.empty()) return {};
    int n = ::WideCharToMultiByte(codepage, 0, in.data(), static_cast<int>(in.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out;
    out.resize(static_cast<std::size_t>(n));
    ::WideCharToMultiByte(codepage, 0, in.data(), static_cast<int>(in.size()),
                          out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace

std::string AcpToUtf8(std::string_view acp) {
    auto wide = CodepageToWide(CP_ACP, acp);
    return WideToCodepage(CP_UTF8, wide);
}

std::wstring Utf8ToWide(std::string_view utf8) {
    return CodepageToWide(CP_UTF8, utf8);
}

std::string WideToUtf8(std::wstring_view wide) {
    return WideToCodepage(CP_UTF8, wide);
}

std::string WideToAcp(std::wstring_view wide) {
    return WideToCodepage(CP_ACP, wide);
}

} // namespace windbgmcp::encoding
