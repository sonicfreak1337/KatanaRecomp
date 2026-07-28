#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace katana::codegen::detail {

inline bool cpp_identifier_character(const char value) noexcept {
    return std::isalnum(static_cast<unsigned char>(value)) != 0 ||
           value == '_';
}

inline void skip_quoted_cpp_literal(const std::string_view text,
                                    std::size_t& offset,
                                    const char quote) noexcept {
    ++offset;
    while (offset < text.size()) {
        if (text[offset] == '\\') {
            ++offset;
            if (offset < text.size()) ++offset;
            continue;
        }
        if (text[offset++] == quote) return;
    }
}

inline bool skip_raw_cpp_literal(const std::string_view text,
                                 std::size_t& offset) {
    if (offset + 1u >= text.size() || text[offset] != 'R' ||
        text[offset + 1u] != '"')
        return false;
    const auto delimiter_begin = offset + 2u;
    auto opening_parenthesis = delimiter_begin;
    while (opening_parenthesis < text.size() &&
           text[opening_parenthesis] != '(') {
        const auto value =
            static_cast<unsigned char>(text[opening_parenthesis]);
        if (opening_parenthesis - delimiter_begin >= 16u ||
            value <= 0x20u || value == '\\' || value == ')')
            return false;
        ++opening_parenthesis;
    }
    if (opening_parenthesis == text.size()) return false;
    const auto delimiter =
        text.substr(delimiter_begin, opening_parenthesis - delimiter_begin);
    const auto terminator = ")" + std::string(delimiter) + '"';
    const auto closing =
        text.find(terminator, opening_parenthesis + 1u);
    offset = closing == std::string_view::npos
                 ? text.size()
                 : closing + terminator.size();
    return true;
}

// Replaces an exact C++ token sequence only in lexically active source.
// Generated diagnostics and comments remain byte-stable even if they mention
// an architectural register spelling.
inline void replace_cpp_code_token(std::string& text,
                                   const std::string_view from,
                                   const std::string_view to) {
    if (from.empty()) return;
    std::size_t offset = 0u;
    while (offset < text.size()) {
        if (text[offset] == '/' && offset + 1u < text.size()) {
            if (text[offset + 1u] == '/') {
                offset += 2u;
                while (offset < text.size()) {
                    if (text[offset] == '\\' &&
                        offset + 1u < text.size() &&
                        (text[offset + 1u] == '\n' ||
                         text[offset + 1u] == '\r')) {
                        offset += text[offset + 1u] == '\r' &&
                                          offset + 2u < text.size() &&
                                          text[offset + 2u] == '\n'
                                      ? 3u
                                      : 2u;
                        continue;
                    }
                    if (text[offset++] == '\n') break;
                }
                continue;
            }
            if (text[offset + 1u] == '*') {
                offset += 2u;
                while (offset + 1u < text.size() &&
                       !(text[offset] == '*' &&
                         text[offset + 1u] == '/'))
                    ++offset;
                offset = std::min(text.size(), offset + 2u);
                continue;
            }
        }
        if (skip_raw_cpp_literal(text, offset)) continue;
        if (text[offset] == '"' || text[offset] == '\'') {
            const auto quote = text[offset];
            skip_quoted_cpp_literal(text, offset, quote);
            continue;
        }
        if (offset + from.size() <= text.size() &&
            text.compare(offset, from.size(), from) == 0) {
            const auto end = offset + from.size();
            const auto touches_identifier =
                (offset != 0u &&
                 cpp_identifier_character(text[offset - 1u])) ||
                (end < text.size() &&
                 cpp_identifier_character(text[end]));
            if (!touches_identifier) {
                text.replace(offset, from.size(), to);
                offset += to.size();
                continue;
            }
            offset = end;
            continue;
        }
        ++offset;
    }
}

} // namespace katana::codegen::detail
