// SPDX-License-Identifier: MIT
#include "analysis/bugcheck_parser.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace wmh::analysis::bugcheck {

using json = nlohmann::json;

namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string StripTick(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '`'), s.end());
    return s;
}

std::string HexWithPrefix(std::string addr) {
    auto a = Lower(StripTick(std::move(addr)));
    if (a.rfind("0x", 0) != 0) a = "0x" + a;
    return a;
}

// Split text into lines, stripping trailing CR. Empty lines are preserved
// because they're meaningful (paragraph breaks in !analyze output).
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

// Build a KEY -> VALUE table from "KEY: value" lines. We iterate per-line
// instead of using std::regex_constants::multiline (MSVC 16.x lacks it).
std::unordered_map<std::string, std::string> KvTable(const std::vector<std::string>& lines) {
    static const std::regex re(R"(^([A-Z_]+):\s+(.+?)\s*$)");
    std::unordered_map<std::string, std::string> out;
    for (const auto& line : lines) {
        std::smatch m;
        if (std::regex_match(line, m, re)) {
            out[m[1].str()] = m[2].str();
        }
    }
    return out;
}

struct SymbolSplit {
    std::string module;
    std::string function;
    std::string offset;  // includes leading '+' when present
};
SymbolSplit SplitSymbol(const std::string& sym) {
    SymbolSplit r;
    auto bang = sym.find('!');
    if (bang != std::string::npos) {
        r.module = sym.substr(0, bang);
        std::string rest = sym.substr(bang + 1);
        auto plus = rest.find('+');
        if (plus != std::string::npos) {
            r.function = rest.substr(0, plus);
            r.offset   = "+" + rest.substr(plus + 1);
        } else {
            r.function = rest;
        }
        return r;
    }
    auto plus = sym.find('+');
    if (plus != std::string::npos) {
        r.module = sym.substr(0, plus);
        r.offset = "+" + sym.substr(plus + 1);
        return r;
    }
    r.module = sym;
    return r;
}

} // namespace

json ParseBugcheck(std::string_view text) {
    auto lines = Split_lines(text);

    // Find the bugcheck head line: NAME (hex)
    static const std::regex head_re(R"(^([A-Z][A-Z0-9_]+)\s*\(([0-9a-fA-F]+)\)\s*$)");
    std::size_t head_idx = std::string::npos;
    std::string name;
    unsigned long long code_int = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::smatch m;
        if (std::regex_match(lines[i], m, head_re)) {
            head_idx = i;
            name = m[1].str();
            code_int = std::stoull(m[2].str(), nullptr, 16);
            break;
        }
    }
    if (head_idx == std::string::npos) {
        return json(nullptr);
    }
    char code_buf[16];
    std::snprintf(code_buf, sizeof(code_buf), "0x%08llX", code_int);

    // Args (Arg1..Arg4)
    static const std::regex arg_re(R"(^Arg[1-4]:\s+([0-9a-fA-F`]+))");
    std::vector<std::string> params;
    for (const auto& line : lines) {
        std::smatch m;
        if (std::regex_search(line, m, arg_re)) {
            params.push_back(HexWithPrefix(m[1].str()));
        }
    }
    while (params.size() < 4) {
        params.push_back("0x0000000000000000");
    }

    // Summary: lines after the head, until the first blank line.
    std::string summary;
    for (std::size_t i = head_idx + 1; i < lines.size(); ++i) {
        std::string trimmed = lines[i];
        auto first_non = trimmed.find_first_not_of(" \t");
        if (first_non == std::string::npos) {
            if (!summary.empty()) break;
            continue;
        }
        if (!summary.empty()) summary.push_back(' ');
        summary.append(trimmed.begin() + first_non, trimmed.end());
    }

    json out = json::object();
    out["code"]    = code_buf;
    out["name"]    = name;
    out["params"]  = params;
    out["summary"] = summary.empty() ? json(nullptr) : json(summary);
    return out;
}

json ParseFaulting(std::string_view text) {
    auto lines = Split_lines(text);

    // Classic: a line "FAULTING_IP:" followed by symbol then address.
    for (std::size_t i = 0; i + 2 < lines.size(); ++i) {
        if (lines[i] == "FAULTING_IP:") {
            const std::string& sym  = lines[i + 1];
            const std::string& addr = lines[i + 2];
            // addr line begins with hex (possibly with `).
            static const std::regex addr_re(R"(^([0-9a-fA-F`]+))");
            std::smatch m;
            if (std::regex_search(addr, m, addr_re)) {
                auto sp = SplitSymbol(sym);
                json out = json::object();
                out["ip"]       = HexWithPrefix(m[1].str());
                out["module"]   = sp.module.empty()   ? json(nullptr) : json(sp.module);
                out["function"] = sp.function.empty() ? json(nullptr) : json(sp.function);
                out["offset"]   = sp.offset.empty()   ? json(nullptr) : json(sp.offset);
                out["source"]   = json(nullptr);
                return out;
            }
        }
    }

    auto kv = KvTable(lines);
    auto pick = [&](std::initializer_list<const char*> keys) -> std::string {
        for (auto k : keys) {
            auto it = kv.find(k);
            if (it != kv.end()) return it->second;
        }
        return {};
    };
    std::string sym  = pick({"FAILURE_SYMBOL_NAME", "FAILURE_FUNC_NAME"});
    std::string addr = pick({"FAULT_INSTR_ADDR_HEX", "FAULTING_INSTR_ADDR"});
    if (!sym.empty()) {
        auto sp = SplitSymbol(sym);
        json out = json::object();
        out["ip"]       = addr.empty() ? json(nullptr) : json(HexWithPrefix(addr));
        out["module"]   = sp.module.empty()   ? json(nullptr) : json(sp.module);
        out["function"] = sp.function.empty() ? json(nullptr) : json(sp.function);
        out["offset"]   = sp.offset.empty()   ? json(nullptr) : json(sp.offset);
        out["source"]   = json(nullptr);
        return out;
    }
    return json(nullptr);
}

json ParseProbableCulprit(std::string_view text) {
    auto kv = KvTable(Split_lines(text));
    auto pick = [&](std::initializer_list<const char*> keys) -> std::string {
        for (auto k : keys) {
            auto it = kv.find(k);
            if (it != kv.end()) return it->second;
        }
        return {};
    };
    std::string img = pick({"IMAGE_NAME", "MODULE_NAME"});
    if (img.empty()) return json(nullptr);
    json out = json::object();
    out["module"]    = img;
    std::string version = pick({"IMAGE_VERSION"});
    std::string ts      = pick({"DEBUG_FLR_IMAGE_TIMESTAMP", "BUGCHECK_BUILD_TIMESTAMP"});
    out["version"]   = version.empty() ? json(nullptr) : json(version);
    out["timestamp"] = ts.empty()      ? json(nullptr) : json(ts);
    return out;
}

json ParseContext(std::string_view text) {
    auto lines = Split_lines(text);

    // Find the CONTEXT: or REGISTERS: header.
    std::size_t start = std::string::npos;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].rfind("CONTEXT:", 0) == 0 ||
            lines[i].rfind("REGISTERS:", 0) == 0) {
            start = i + 1;
            break;
        }
    }
    if (start == std::string::npos) return json(nullptr);

    json out = json::object();
    static const std::regex reg_re(R"(\b([a-z][a-z0-9]{1,5})=([0-9a-fA-F`]+))");
    bool any = false;
    // Walk forward; stop at the next blank line after we've collected at
    // least one register hit (CONTEXT blocks are typically 3-5 dense lines).
    for (std::size_t i = start; i < lines.size(); ++i) {
        if (lines[i].empty()) {
            if (any) break;
            continue;
        }
        for (auto it = std::sregex_iterator(lines[i].begin(), lines[i].end(), reg_re);
             it != std::sregex_iterator(); ++it) {
            std::string k = (*it)[1].str();
            if (!out.contains(k)) {
                out[k] = HexWithPrefix((*it)[2].str());
                any = true;
            }
        }
    }
    return out.empty() ? json(nullptr) : out;
}

json ParseIretFrame(std::string_view text) {
    auto lines = Split_lines(text);
    static const std::regex trap_re(R"(^TRAP_FRAME:\s*([0-9a-fA-F`]+))");
    for (const auto& line : lines) {
        std::smatch m;
        if (std::regex_search(line, m, trap_re)) {
            json out = json::object();
            out["trap_frame"] = HexWithPrefix(m[1].str());
            return out;
        }
    }
    return json(nullptr);
}

} // namespace wmh::analysis::bugcheck

