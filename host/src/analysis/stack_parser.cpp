// SPDX-License-Identifier: MIT
#include "analysis/stack_parser.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace wmh::analysis::stack {

using json = nlohmann::json;

namespace {

std::string StripTick(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '`'), s.end());
    return s;
}
std::string LowerHex(std::string s) {
    auto t = StripTick(std::move(s));
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (t.rfind("0x", 0) != 0) t = "0x" + t;
    return t;
}

// kb frame layout (whitespace-tolerant), one regex applied per line.
//   <2-hex frame> <child SP> <return addr> [: <0..4 args>] : <callsite>
static const std::regex kFrameRe(
    R"(^\s*([0-9a-fA-F]{2})\s+([0-9a-fA-F`]+)\s+([0-9a-fA-F`]+)\s*(?::(?:\s+[0-9a-fA-F]+){0,4}\s*)?:\s*(.+?)\s*$)");

// Split text into lines (CR-stripped, empty preserved).
std::vector<std::string> Split_lines(std::string_view text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i <= text.size()) {
        std::size_t j = text.find('\n', i);
        std::string line(text.substr(i, (j == std::string_view::npos)
                                            ? text.size() - i
                                            : j - i));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(std::move(line));
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return out;
}

} // namespace

json ParseKb(std::string_view text) {
    json frames = json::array();
    auto lines = Split_lines(text);
    for (const auto& line : lines) {
        std::smatch m;
        if (!std::regex_match(line, m, kFrameRe)) continue;
        json frame = json::object();
        frame["frame"]  = std::stoi(m[1].str(), nullptr, 16);
        frame["addr"]   = LowerHex(m[3].str());
        const std::string callsite = m[4].str();

        std::string module, func, offset;
        auto bang = callsite.find('!');
        if (bang != std::string::npos) {
            module = callsite.substr(0, bang);
            std::string rest = callsite.substr(bang + 1);
            auto plus = rest.find('+');
            if (plus != std::string::npos) {
                func = rest.substr(0, plus);
                std::string off = rest.substr(plus + 1);
                if (off.rfind("0x", 0) == 0) off.erase(0, 2);
                offset = "+0x" + off;
            } else {
                func = rest;
            }
        } else {
            auto plus = callsite.find('+');
            if (plus != std::string::npos) {
                module = callsite.substr(0, plus);
                std::string off = callsite.substr(plus + 1);
                if (off.rfind("0x", 0) == 0) off.erase(0, 2);
                offset = "+0x" + off;
            } else {
                module = callsite;
            }
        }
        frame["module"] = module.empty() ? json(nullptr) : json(module);
        frame["func"]   = func.empty()   ? json(nullptr) : json(func);
        frame["offset"] = offset.empty() ? json(nullptr) : json(offset);
        frames.push_back(frame);
    }
    return frames;
}

} // namespace wmh::analysis::stack
