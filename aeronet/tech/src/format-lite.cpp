#include "aeronet/format-lite.hpp"

#include <cassert>
#include <charconv>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aeronet {

namespace {

// Appends 'value' formatted by std::format for 'field', a replacement field of one argument.
template <class T>
void StdFormatTo(std::string& out, std::string_view field, T value) {
  std::vformat_to(std::back_inserter(out), field, std::make_format_args(value));
}

template <class Int>
void AppendInteger(std::string& out, Int value) {
  char buf[std::numeric_limits<Int>::digits10 + 2];  // all the digits and the sign
  const std::to_chars_result result = std::to_chars(buf, buf + sizeof(buf), value);
  out.append(buf, result.ptr);
}

}  // namespace

namespace detail {

void FormatStringError(const char* reason) { throw std::invalid_argument(reason); }

}  // namespace detail

void FormatArg::formatTo(std::string& out, std::string_view field) const {
  // '{}' or '{:}' is the default format, written here for the usual types, with the output of std::format.
  const bool defaultFormat = field.size() <= 3U;
  switch (_type) {
    case Type::kBool:
      if (defaultFormat) {
        out.append(_value.boolean ? std::string_view("true") : std::string_view("false"));
      } else {
        StdFormatTo(out, field, _value.boolean);
      }
      break;
    case Type::kChar:
      if (defaultFormat) {
        out.push_back(_value.character);
      } else {
        StdFormatTo(out, field, _value.character);
      }
      break;
    case Type::kInt:
      if (defaultFormat) {
        AppendInteger(out, _value.integer);
      } else {
        StdFormatTo(out, field, _value.integer);
      }
      break;
    case Type::kUInt:
      if (defaultFormat) {
        AppendInteger(out, _value.unsignedInteger);
      } else {
        StdFormatTo(out, field, _value.unsignedInteger);
      }
      break;
    case Type::kFloat:
      StdFormatTo(out, field, _value.floatValue);
      break;
    case Type::kDouble:
      StdFormatTo(out, field, _value.doubleValue);
      break;
    case Type::kString:
      if (defaultFormat) {
        out.append(_value.string.pData, _value.string.size);
      } else {
        StdFormatTo(out, field, std::string_view(_value.string.pData, _value.string.size));
      }
      break;
    default:
      // kNone is never formatted: the check of the format string gives each replacement field an argument.
      assert(_type == Type::kPointer);
      StdFormatTo(out, field, _value.pointer);
      break;
  }
}

void VFormatTo(std::string& out, std::string_view fmt, const FormatArg* pArgs, [[maybe_unused]] std::size_t nbArgs) {
  // 'fmt' is valid (checked at compile time): its braces are paired, and it has a replacement field per argument.
  const FormatArg* pArg = pArgs;
  for (std::size_t bracePos = fmt.find_first_of("{}"); bracePos != std::string_view::npos;
       bracePos = fmt.find_first_of("{}")) {
    out.append(fmt.substr(0, bracePos));
    if (fmt[bracePos + 1] == fmt[bracePos]) {
      // '{{' or '}}'
      out.push_back(fmt[bracePos]);
      fmt.remove_prefix(bracePos + 2U);
    } else {
      const std::size_t fieldEnd = fmt.find('}', bracePos) + 1U;
      assert(pArg != pArgs + nbArgs);
      pArg->formatTo(out, fmt.substr(bracePos, fieldEnd - bracePos));
      ++pArg;
      fmt.remove_prefix(fieldEnd);
    }
  }
  out.append(fmt);
}

}  // namespace aeronet
