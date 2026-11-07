#include "assigned-json.hpp"

#include <cstddef>
#include <string_view>

namespace aeronet::internal {

namespace {

// Characters that can be part of a JavaScript identifier: ASCII letters, digits, '_', '$', and any byte of a non-ASCII
// UTF-8 sequence.
constexpr bool IsIdentifierChar(char ch) noexcept {
  const auto byte = static_cast<unsigned char>(ch);
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '_' ||
         byte == '$' || byte >= 0x80U;
}

constexpr bool IsSpace(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

constexpr std::size_t SkipSpaces(std::string_view text, std::size_t pos) noexcept {
  while (pos < text.size() && IsSpace(text[pos])) {
    ++pos;
  }
  return pos;
}

// The position after the bracket closing the one at 'open' ('{' or '['), or npos if it is not closed. The brackets in
// single or double quoted strings (with their backslash escapes) are ignored; the kind of the brackets is not checked.
constexpr std::size_t ClosingBracketEnd(std::string_view text, std::size_t open) noexcept {
  std::size_t depth = 0;
  for (std::size_t pos = open; pos < text.size(); ++pos) {
    switch (text[pos]) {
      case '{':
        [[fallthrough]];
      case '[':
        ++depth;
        break;
      case '}':
        [[fallthrough]];
      case ']':
        if (--depth == 0) {
          return pos + 1;
        }
        break;
      case '"':
        [[fallthrough]];
      case '\'': {
        const char quote = text[pos];
        for (++pos; pos < text.size() && text[pos] != quote; ++pos) {
          if (text[pos] == '\\') {
            ++pos;
          }
        }
        break;
      }
      default:
        break;
    }
  }
  return std::string_view::npos;
}

}  // namespace

std::string_view FindAssignedJson(std::string_view script, std::string_view variable) noexcept {
  if (variable.empty()) {
    return {};
  }
  for (std::size_t pos = script.find(variable); pos != std::string_view::npos; pos = script.find(variable, pos + 1)) {
    if (pos != 0 && IsIdentifierChar(script[pos - 1])) {
      continue;
    }
    std::size_t valuePos = SkipSpaces(script, pos + variable.size());
    // '=', not '==' (a comparison).
    if (valuePos == script.size() || script[valuePos] != '=' ||
        (valuePos + 1 < script.size() && script[valuePos + 1] == '=')) {
      continue;
    }
    valuePos = SkipSpaces(script, valuePos + 1);
    if (valuePos == script.size() || (script[valuePos] != '{' && script[valuePos] != '[')) {
      continue;
    }
    const std::size_t end = ClosingBracketEnd(script, valuePos);
    if (end == std::string_view::npos) {
      return {};
    }
    return script.substr(valuePos, end - valuePos);
  }
  return {};
}

}  // namespace aeronet::internal
