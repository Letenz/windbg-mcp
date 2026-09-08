// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace windbgmcp::handlers {

inline std::string AsciiLowerCopy(std::string_view value) {
    std::string out(value);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return out;
}

inline std::string_view TrimAscii(std::string_view value) noexcept {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

inline std::string FirstCommandToken(std::string_view value) {
    value = TrimAscii(value);
    return AsciiLowerCopy(value.substr(0, value.find_first_of(" \t\r\n;{}()")));
}

inline bool OwnsRemainingLine(std::string_view token) {
    return token.starts_with("*") || token == "as" || token.starts_with("$<") || token.starts_with("$$<");
}

struct CommandBatch {
    std::vector<std::string> commands;
    std::string error;
};

// Split only top-level separators. DbgEng remains the parser for expressions,
// quoted strings and control blocks; alias/comment commands own their line.
inline CommandBatch ParseCommandBatch(std::string_view input) {
    CommandBatch result;
    std::string part;
    std::vector<char> nesting;
    char quote = 0;
    bool escaped = false;
    bool owns_line = false;
    auto emit = [&] {
        auto trimmed = TrimAscii(part);
        if (!trimmed.empty()) result.commands.emplace_back(trimmed);
        part.clear();
        owns_line = false;
    };
    for (char c : input) {
        if (c == '\0') { result.error = "cmd contains a NUL character"; return result; }
        if (quote) {
            part += c;
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (nesting.empty() && (c == '\r' || c == '\n')) { emit(); continue; }
        if (owns_line) { part += c; continue; }
        if (TrimAscii(part).empty() && c == '*') owns_line = true;
        const auto token = FirstCommandToken(part);
        if (nesting.empty() && (c == ' ' || c == '\t' || c == ';') && OwnsRemainingLine(token)) {
            owns_line = true;
            part += c;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        else if (c == '{' || c == '(' || c == '[') nesting.push_back(c);
        else if (c == '}' || c == ')' || c == ']') {
            const char expected = c == '}' ? '{' : c == ')' ? '(' : '[';
            if (nesting.empty() || nesting.back() != expected) {
                result.error = "unbalanced command delimiters"; return result;
            }
            nesting.pop_back();
        } else if (c == ';' && nesting.empty()) { emit(); continue; }
        part += c;
    }
    if (quote || !nesting.empty()) { result.error = "unbalanced quotes or command delimiters"; return result; }
    emit();
    if (result.commands.size() > 64) result.error = "at most 64 top-level commands are allowed";
    for (const auto& command : result.commands) {
        if (FirstCommandToken(command) == "j")
            result.error = "j command-string branches are not supported here; use explicit .if blocks";
    }
    return result;
}

// Enumerate explicit statement heads, excluding quoted text and comments.
// This is a lifecycle guard, not a sandbox for arbitrary debugger scripts.
inline std::vector<std::string> StatementTokens(std::string_view command) {
    std::vector<std::string> tokens;
    bool head = true, escaped = false, comment = false;
    char quote = 0;
    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (comment) { if (c == '\r' || c == '\n') { comment = false; head = true; } continue; }
        if (quote) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; head = false; continue; }
        if (c == ';' || c == '{' || c == '}' || c == '\r' || c == '\n') { head = true; continue; }
        if (c == ' ' || c == '\t') continue;
        if (!head) continue;
        if (c == '*') { comment = true; continue; }
        auto token = FirstCommandToken(command.substr(i));
        if (!token.empty()) {
            tokens.push_back(token);
            i += token.size() - 1;
            if (OwnsRemainingLine(token)) comment = true;
        }
        head = false;
    }
    return tokens;
}

inline bool IsRunControl(std::string_view token) {
    for (auto name : {"g", "gh", "gn", "gc", "gu", "p", "pa", "pc", "pct", "ph",
                      "pt", "t", "ta", "tb", "tc", "tct", "th", "tt", "wt"})
        if (token == name) return true;
    return false;
}

inline std::optional<std::string> UnsafeRunControl(const CommandBatch& batch) {
    for (std::size_t i = 0; i < batch.commands.size(); ++i) {
        const auto& command = batch.commands[i];
        const auto tokens = StatementTokens(command);
        for (std::size_t j = 0; j < tokens.size(); ++j) {
            if (tokens[j] == "j")
                return "j command-string branches are not supported here; use explicit .if blocks";
            if (IsRunControl(tokens[j]) &&
                (i + 1 != batch.commands.size() || j != 0 || tokens.size() != 1 ||
                 FirstCommandToken(command) != tokens[j]))
                return "run-control commands must be a standalone final statement, never inside a block";
        }
    }
    return std::nullopt;
}

} // namespace windbgmcp::handlers
