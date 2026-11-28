#include "aeronet/format-lite.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {

// Appends the chars that 'writer(first)' writes from 'first', at most 'maxSize', returning the end, to 'out'.
// resize_and_overwrite() grows the capacity exponentially, as append(), in libstdc++ (_M_create doubles it), libc++
// (__grow_by) and the MSVC STL (_Calculate_growth): no reserve() needed beforehand.
template <class Writer>
void AppendWritten(std::string& out, std::size_t maxSize, Writer writer) {
  const std::size_t oldSize = out.size();
  out.resize_and_overwrite(oldSize + maxSize, [oldSize, writer](char* pData, std::size_t /*size*/) {
    return static_cast<std::size_t>(writer(pData + oldSize) - pData);
  });
}

template <class Writer>
void AppendWritten(RawChars& out, std::size_t maxSize, Writer writer) {
  out.ensureAvailableCapacityExponential(maxSize);
  out.setEnd(writer(out.data() + out.size()));
}

template <class Int>
void AppendInteger(auto& out, Int value) {
  static constexpr std::size_t kMaxSize = std::numeric_limits<Int>::digits10 + 2U;  // all the digits and the sign
  AppendWritten(out, kMaxSize, [value](char* first) { return std::to_chars(first, first + kMaxSize, value).ptr; });
}

// Appends 'value' formatted by std::format for 'field', a replacement field of one argument.
template <class T>
void StdFormatTo(auto& out, std::string_view field, T value) {
  std::vformat_to(std::back_inserter(out), field, std::make_format_args(value));
}

// The standard format specification [[fill]align][sign]['#']['0'][width][type] of a replacement field formatted here
// (an integer presentation, or a pointer). Its precision is only valid for the arguments that std::format formats.
struct FormatSpec {
  std::size_t width{};
  char fill{' '};
  char align{};  // '<', '>', '^', or '\0' for the default alignment (right for integers and pointers)
  char sign{'-'};
  char presentation{};  // '\0' when absent
  bool alternate{};
  bool zeroPadding{};
};

// Parses the specification of 'field', "{:spec}", checked at compile time.
FormatSpec ParseFormatSpec(std::string_view field) {
  std::string_view spec = field.substr(2U, field.size() - 3U);
  const auto isAlign = [](char ch) { return ch == '<' || ch == '>' || ch == '^'; };
  FormatSpec res;
  // "{:}" is the default format, not parsed.
  assert(!spec.empty());
  if (spec.size() >= 2U && isAlign(spec[1])) {
    res.fill = spec[0];
    res.align = spec[1];
    spec.remove_prefix(2U);
  } else if (isAlign(spec[0])) {
    res.align = spec[0];
    spec.remove_prefix(1U);
  }
  if (!spec.empty() && (spec[0] == '+' || spec[0] == '-' || spec[0] == ' ')) {
    res.sign = spec[0];
    spec.remove_prefix(1U);
  }
  if (!spec.empty() && spec[0] == '#') {
    res.alternate = true;
    spec.remove_prefix(1U);
  }
  if (!spec.empty() && spec[0] == '0') {
    res.zeroPadding = true;
    spec.remove_prefix(1U);
  }
  // A width does not start with '0': without one, from_chars stops at once and leaves it at 0.
  const char* widthEnd = std::from_chars(spec.data(), spec.data() + spec.size(), res.width).ptr;
  spec.remove_prefix(static_cast<std::size_t>(widthEnd - spec.data()));
  if (!spec.empty()) {
    res.presentation = spec[0];
  }
  return res;
}

// Writes the digits of 'value' in base kBase before 'last', returning the first one.
template <unsigned kBase>
char* WriteDigitsBackward(char* last, std::uint64_t value, bool upperCase) {
  // String literals: the linkers split their section per literal. The local digit table of libstdc++'s hexadecimal
  // std::to_chars is not one: GCC 15 for macOS (-O0) emits it under an assembler-local label, which the linkers glue
  // to the preceding weak symbol of its object, so that a program can read the copy of this symbol of another object
  // instead (in the tests, "{:x}" of 333 gave "\0\07" or "\xFF\xFFn", depending on the layout of the program).
  const char* digits = upperCase ? "0123456789ABCDEF" : "0123456789abcdef";
  do {
    *--last = digits[value % kBase];
    value /= kBase;
  } while (value != 0);
  return last;
}

// Appends the integer of absolute value 'magnitude' formatted as std::format formats it for 'spec' (an integer
// presentation type, or none for decimal).
void AppendFormattedInteger(auto& out, const FormatSpec& spec, std::uint64_t magnitude, bool negative) {
  char digits[std::numeric_limits<std::uint64_t>::digits];  // at most 64 binary digits
  char* digitsEnd = digits + sizeof(digits);
  char* digitsBegin;
  char prefix[3];  // sign and base prefix
  std::size_t prefixSize = 0;
  if (negative) {
    prefix[prefixSize++] = '-';
  } else if (spec.sign != '-') {
    prefix[prefixSize++] = spec.sign;
  }
  switch (spec.presentation) {
    case 'b':
      [[fallthrough]];
    case 'B':
      digitsBegin = WriteDigitsBackward<2>(digitsEnd, magnitude, false);
      break;
    case 'o':
      digitsBegin = WriteDigitsBackward<8>(digitsEnd, magnitude, false);
      break;
    case 'x':
      [[fallthrough]];
    case 'X':
      digitsBegin = WriteDigitsBackward<16>(digitsEnd, magnitude, spec.presentation == 'X');
      break;
    default:
      assert(spec.presentation == '\0' || spec.presentation == 'd');
      digitsBegin = WriteDigitsBackward<10>(digitsEnd, magnitude, false);
      break;
  }
  if (spec.alternate && spec.presentation != 'd' && spec.presentation != '\0') {
    // The base prefix: "0b", "0B", "0x" or "0X", and "0" for a non-zero octal value.
    if (spec.presentation != 'o') {
      prefix[prefixSize++] = '0';
      prefix[prefixSize++] = spec.presentation;
    } else if (magnitude != 0) {
      prefix[prefixSize++] = '0';
    }
  }
  const std::size_t size = prefixSize + static_cast<std::size_t>(digitsEnd - digitsBegin);
  std::size_t leftFill = 0;
  std::size_t zeros = 0;
  std::size_t rightFill = 0;
  if (spec.width > size) {
    const std::size_t padding = spec.width - size;
    if (spec.align == '<') {
      rightFill = padding;
    } else if (spec.align == '^') {
      leftFill = padding / 2U;
      rightFill = padding - leftFill;
    } else if (spec.align == '\0' && spec.zeroPadding) {
      // '0' pads after the sign and the base prefix, unless an alignment is given.
      zeros = padding;
    } else {
      leftFill = padding;
    }
  }
  AppendWritten(out, size + leftFill + zeros + rightFill, [&](char* pos) {
    pos = std::fill_n(pos, leftFill, spec.fill);
    pos = std::copy(prefix, prefix + prefixSize, pos);
    pos = std::fill_n(pos, zeros, '0');
    pos = std::copy(digitsBegin, digitsEnd, pos);
    return std::fill_n(pos, rightFill, spec.fill);
  });
}

// Whether 'presentation' is an integer presentation type: 'b', 'B', 'd', 'o', 'x' or 'X'.
bool IsIntegerPresentation(char presentation) { return std::string_view("bBdoxX").contains(presentation); }

// Appends the integer 'value' formatted as std::format formats it for 'field', with an integer presentation type or
// none, written here: faster than std::format, and without the digit table of libstdc++ (see WriteDigitsBackward).
template <class Int>
void AppendFormattedInteger(auto& out, std::string_view field, Int value) {
  if constexpr (std::is_signed_v<Int>) {
    if (value < 0) {
      // Computed in the unsigned type: the magnitude of the smallest value has no signed representation.
      AppendFormattedInteger(out, ParseFormatSpec(field), std::uint64_t{0} - static_cast<std::uint64_t>(value), true);
      return;
    }
  }
  AppendFormattedInteger(out, ParseFormatSpec(field), static_cast<std::uint64_t>(value), false);
}

}  // namespace

namespace detail {

void FormatStringError(const char* reason) { throw std::invalid_argument(reason); }

}  // namespace detail

template <FormatOutput Out>
void FormatArg::formatTo(Out& out, std::string_view field) const {
  // '{}' or '{:}' is the default format, written here for the usual types, with the output of std::format.
  const bool defaultFormat = field.size() <= 3U;
  // The presentation type of "{:spec}" is the last char of 'spec' when it is a letter: no other part of the
  // specification ends with one (a fill char is followed by its alignment).
  const char presentation = defaultFormat ? '\0' : field[field.size() - 2U];
  switch (_type) {
    case Type::kBool:
      if (defaultFormat) {
        out.append(_value.boolean ? std::string_view("true") : std::string_view("false"));
      } else if (IsIntegerPresentation(presentation)) {
        AppendFormattedInteger(out, field, static_cast<unsigned>(_value.boolean));
      } else {
        StdFormatTo(out, field, _value.boolean);
      }
      break;
    case Type::kChar:
      if (defaultFormat) {
        out.push_back(_value.character);
      } else if (IsIntegerPresentation(presentation)) {
        // A char formatted as an integer is converted to unsigned char (P2909, a defect report against C++20).
        AppendFormattedInteger(out, field, static_cast<unsigned char>(_value.character));
      } else {
        StdFormatTo(out, field, _value.character);
      }
      break;
    case Type::kInt:
      if (defaultFormat) {
        AppendInteger(out, _value.integer);
      } else if (presentation != 'c') {
        AppendFormattedInteger(out, field, _value.integer);
      } else {
        StdFormatTo(out, field, _value.integer);
      }
      break;
    case Type::kUInt:
      if (defaultFormat) {
        AppendInteger(out, _value.unsignedInteger);
      } else if (presentation != 'c') {
        AppendFormattedInteger(out, field, _value.unsignedInteger);
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
        out.append(std::string_view(_value.string.pData, _value.string.size));
      } else {
        StdFormatTo(out, field, std::string_view(_value.string.pData, _value.string.size));
      }
      break;
    default:
      // kNone is never formatted: the check of the format string gives each replacement field an argument.
      assert(_type == Type::kPointer);
      {
        // "0x" and the lower case hexadecimal digits of the address, right-aligned by default ('p' or no type).
        FormatSpec spec = defaultFormat ? FormatSpec{} : ParseFormatSpec(field);
        spec.presentation = 'x';
        spec.alternate = true;
        AppendFormattedInteger(out, spec, std::bit_cast<std::uintptr_t>(_value.pointer), false);
      }
      break;
  }
}

template <FormatOutput Out>
void VFormatTo(Out& out, std::string_view fmt, const FormatArg* pArgs, [[maybe_unused]] std::size_t nbArgs) {
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

template void FormatArg::formatTo(std::string& out, std::string_view field) const;
template void FormatArg::formatTo(RawChars& out, std::string_view field) const;

template void VFormatTo(std::string& out, std::string_view fmt, const FormatArg* pArgs, std::size_t nbArgs);
template void VFormatTo(RawChars& out, std::string_view fmt, const FormatArg* pArgs, std::size_t nbArgs);

}  // namespace aeronet
