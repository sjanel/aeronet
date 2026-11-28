#pragma once

// std::format at a fraction of its compile time: a format string checked at compile time, its arguments held by
// FormatArg, and the formatting done by src/format-lite.cpp, the only source that includes <format> (about 220 ms of
// parsing per translation unit, plus 100 ms of instantiations of std::vformat_to). The output is the one of
// std::format.
//
// Arguments: bool, char, the integer types, float, double, strings (std::string, std::string_view, C strings) and void
// pointers. Replacement fields: '{}' and '{:spec}', with the standard format specification of the argument type,
// except the locale-specific form 'L', the debug form '?', dynamic width and precision ('{:{}}') and argument indexes
// ('{0}'), and the options that the standard libraries do not all accept ('{:+c}' of an integer, '{:0}' of a pointer).
// Each argument has its replacement field: more arguments than fields is an error.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

/// The buffers that the formatting functions append to.
template <class Out>
concept FormatOutput = std::is_same_v<Out, std::string> || std::is_same_v<Out, RawChars>;

// An argument of a format call: its value, or the view of a string, which must outlive the call.
class FormatArg {
 public:
  enum class Type : std::uint8_t { kNone, kBool, kChar, kInt, kUInt, kFloat, kDouble, kString, kPointer };

  /// The type of a format argument of type T, kNone when T cannot be formatted.
  template <class T>
  static consteval Type TypeOf() noexcept {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, bool>) {
      return Type::kBool;
    } else if constexpr (std::is_same_v<U, char>) {
      return Type::kChar;
    } else if constexpr (std::is_same_v<U, signed char> || std::is_same_v<U, short> || std::is_same_v<U, int> ||
                         std::is_same_v<U, long> || std::is_same_v<U, long long>) {
      return Type::kInt;
    } else if constexpr (std::is_same_v<U, unsigned char> || std::is_same_v<U, unsigned short> ||
                         std::is_same_v<U, unsigned int> || std::is_same_v<U, unsigned long> ||
                         std::is_same_v<U, unsigned long long>) {
      return Type::kUInt;
    } else if constexpr (std::is_same_v<U, float>) {
      return Type::kFloat;
    } else if constexpr (std::is_same_v<U, double>) {
      return Type::kDouble;
    } else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view> ||
                         std::is_same_v<std::decay_t<U>, const char*> || std::is_same_v<std::decay_t<U>, char*>) {
      return Type::kString;
    } else if constexpr (std::is_same_v<U, const void*> || std::is_same_v<U, void*> ||
                         std::is_same_v<U, std::nullptr_t>) {
      return Type::kPointer;
    } else {
      return Type::kNone;
    }
  }

  /// An empty argument, ending an array of arguments that would be empty otherwise.
  FormatArg() noexcept = default;

  /// Holds 'value', whose type must be supported (TypeOf<T>() != kNone).
  template <class T>
  explicit FormatArg(const T& value) noexcept : _type(TypeOf<T>()) {
    if constexpr (TypeOf<T>() == Type::kBool) {
      _value.boolean = value;
    } else if constexpr (TypeOf<T>() == Type::kChar) {
      _value.character = value;
    } else if constexpr (TypeOf<T>() == Type::kInt) {
      _value.integer = value;
    } else if constexpr (TypeOf<T>() == Type::kUInt) {
      _value.unsignedInteger = value;
    } else if constexpr (TypeOf<T>() == Type::kFloat) {
      _value.floatValue = value;
    } else if constexpr (TypeOf<T>() == Type::kDouble) {
      _value.doubleValue = value;
    } else if constexpr (TypeOf<T>() == Type::kString) {
      const std::string_view str(value);
      _value.string = {str.data(), str.size()};
    } else {
      static_assert(TypeOf<T>() == Type::kPointer, "type not supported by FormatArg");
      _value.pointer = value;
    }
  }

  /// Appends the argument to 'out', formatted as std::format formats it for 'field', its replacement field ('{}' or
  /// '{:spec}', braces included). Throws std::format_error when it cannot ('{:c}' of an integer out of the range of
  /// char).
  template <FormatOutput Out>
  void formatTo(Out& out, std::string_view field) const;

 private:
  struct StringValue {
    const char* pData;
    std::size_t size;
  };

  union Value {
    bool boolean;
    char character;
    std::int64_t integer;
    std::uint64_t unsignedInteger;
    float floatValue;
    double doubleValue;
    StringValue string;
    const void* pointer;
  };

  Value _value;
  Type _type{Type::kNone};
};

namespace detail {

// Not constexpr: the compile-time check of a format string calls it on an error, which fails the compilation with the
// line of the error in the diagnostic. At run time (the tests of the check), throws std::invalid_argument('reason').
[[noreturn]] void FormatStringError(const char* reason);

constexpr bool IsFormatDigit(char ch) noexcept { return ch >= '0' && ch <= '9'; }

// Skips the decimal number at 'pos' of 'spec' (a width or a precision, at most 65535 as for std::format).
constexpr std::size_t SkipFormatCount(std::string_view spec, std::size_t pos) {
  std::uint32_t value = 0;
  for (; pos < spec.size() && IsFormatDigit(spec[pos]); ++pos) {
    value = (value * 10U) + static_cast<std::uint32_t>(spec[pos] - '0');
    if (value > 65535U) {
      FormatStringError("width or precision larger than 65535 in format string");
    }
  }
  return pos;
}

// Checks the standard format specification 'spec' (after the ':' of its replacement field) for an argument of 'type':
// [[fill]align][sign]['#']['0'][width]['.' precision][type].
constexpr void CheckFormatSpec(std::string_view spec, FormatArg::Type type) {
  const auto isAlign = [](char ch) { return ch == '<' || ch == '>' || ch == '^'; };
  std::size_t pos = 0;
  if (spec.size() >= 2 && isAlign(spec[1])) {
    pos = 2;  // fill and align
  } else if (!spec.empty() && isAlign(spec[0])) {
    pos = 1;
  }
  bool hasNumericOption = false;  // sign, '#' or '0'
  if (pos < spec.size() && (spec[pos] == '+' || spec[pos] == '-' || spec[pos] == ' ')) {
    hasNumericOption = true;
    ++pos;
  }
  if (pos < spec.size() && spec[pos] == '#') {
    hasNumericOption = true;
    ++pos;
  }
  if (pos < spec.size() && spec[pos] == '0') {
    hasNumericOption = true;
    ++pos;
    if (pos < spec.size() && spec[pos] == '0') {
      FormatStringError("width must be non-zero in format string");
    }
  }
  pos = SkipFormatCount(spec, pos);
  bool hasPrecision = false;
  if (pos < spec.size() && spec[pos] == '.') {
    ++pos;
    if (pos == spec.size() || !IsFormatDigit(spec[pos])) {
      FormatStringError("missing precision after '.' in format string");
    }
    pos = SkipFormatCount(spec, pos);
    hasPrecision = true;
  }
  if (pos < spec.size() && spec[pos] == 'L') {
    FormatStringError("locale-specific form 'L' not supported in format string");
  }
  const char presentation = pos < spec.size() ? spec[pos++] : '\0';
  if (pos != spec.size()) {
    FormatStringError("invalid format specification in format string");
  }
  const std::string_view kIntegerPresentations = "bBdoxX";
  const bool asInteger = presentation != '\0' && kIntegerPresentations.contains(presentation);
  bool valid = false;
  switch (type) {
    case FormatArg::Type::kBool:
      valid = !hasPrecision && (asInteger || ((presentation == '\0' || presentation == 's') && !hasNumericOption));
      break;
    case FormatArg::Type::kChar:
      valid = !hasPrecision && (asInteger || ((presentation == '\0' || presentation == 'c') && !hasNumericOption));
      break;
    case FormatArg::Type::kInt:
      [[fallthrough]];
    case FormatArg::Type::kUInt:
      valid = !hasPrecision && (presentation == '\0' || asInteger || (presentation == 'c' && !hasNumericOption));
      break;
    case FormatArg::Type::kFloat:
      [[fallthrough]];
    case FormatArg::Type::kDouble:
      valid = presentation == '\0' || std::string_view("aAeEfFgG").contains(presentation);
      break;
    case FormatArg::Type::kString:
      valid = !hasNumericOption && (presentation == '\0' || presentation == 's');
      break;
    case FormatArg::Type::kPointer:
      valid = !hasNumericOption && !hasPrecision && (presentation == '\0' || presentation == 'p');
      break;
    case FormatArg::Type::kNone:
      break;
  }
  if (!valid) {
    FormatStringError("format specification not valid for the type of its argument");
  }
}

// Checks 'fmt' for arguments of the types pTypes[0] to pTypes[nbArgs - 1].
constexpr void CheckFormatString(std::string_view fmt, const FormatArg::Type* pTypes, std::size_t nbArgs) {
  std::size_t argPos = 0;
  for (std::size_t pos = 0; pos < fmt.size(); ++pos) {
    if (fmt[pos] == '}') {
      if (pos + 1 == fmt.size() || fmt[pos + 1] != '}') {
        FormatStringError("unmatched '}' in format string (write '}}' for '}')");
      }
      ++pos;
    } else if (fmt[pos] == '{') {
      ++pos;
      if (pos < fmt.size() && fmt[pos] == '{') {
        continue;
      }
      const std::size_t endPos = fmt.find('}', pos);
      if (endPos == std::string_view::npos) {
        FormatStringError("unmatched '{' in format string (write '{{' for '{')");
      }
      if (fmt[pos] != '}' && fmt[pos] != ':') {
        FormatStringError("invalid replacement field, '{}' or '{:spec}' expected (argument indexes are not supported)");
      }
      if (argPos == nbArgs) {
        FormatStringError("more replacement fields than arguments in format string");
      }
      const std::string_view spec = fmt.substr(pos, endPos - pos);
      if (spec.contains('{')) {
        FormatStringError("nested replacement fields (dynamic width or precision) are not supported");
      }
      CheckFormatSpec(spec.substr(spec.empty() ? 0 : 1), pTypes[argPos]);
      ++argPos;
      pos = endPos;
    }
  }
  if (argPos != nbArgs) {
    FormatStringError("more arguments than replacement fields in format string");
  }
}

template <class... Args>
inline constexpr FormatArg::Type kFormatArgTypes[]{FormatArg::TypeOf<Args>()..., FormatArg::Type::kNone};

}  // namespace detail

// A format string for arguments of types Args, checked at compile time. Use FormatString, which does not take part in
// the deduction of Args.
template <class... Args>
class CheckedFormatString {
 public:
  static_assert(((FormatArg::TypeOf<Args>() != FormatArg::Type::kNone) && ...),
                "aeronet::FormatString: argument type not supported (bool, char, integers, float, double, strings and "
                "void pointers are)");

  /// Checks the constant 'str' at compile time. Implicit, as std::format_string: the string of a call converts.
  consteval CheckedFormatString(const char* str)  // NOLINT(*-explicit-constructor)
      : CheckedFormatString(std::string_view(str)) {}

  /// Checks the constant 'str' at compile time.
  consteval CheckedFormatString(std::string_view str) : _str(str) {  // NOLINT(*-explicit-constructor)
    detail::CheckFormatString(_str, detail::kFormatArgTypes<Args...>, sizeof...(Args));
  }

  /// The format string.
  [[nodiscard]] constexpr std::string_view get() const noexcept { return _str; }

 private:
  std::string_view _str;
};

/// The format string of a call taking arguments of types Args, as std::format_string.
template <class... Args>
using FormatString = CheckedFormatString<std::type_identity_t<Args>...>;

/// Appends 'fmt' formatted with the 'nbArgs' arguments of 'pArgs' to 'out', as std::format would. 'fmt' must be checked
/// for them (FormatString). Throws std::format_error as FormatArg::formatTo().
template <FormatOutput Out>
void VFormatTo(Out& out, std::string_view fmt, const FormatArg* pArgs, std::size_t nbArgs);

/// Appends 'fmt' formatted with 'args' to 'out', as std::format_to(std::back_inserter(out), fmt, args...) would.
template <FormatOutput Out, class... Args>
void FormatTo(Out& out, FormatString<Args...> fmt, const Args&... args) {
  const FormatArg formatArgs[]{FormatArg(args)..., FormatArg()};
  VFormatTo(out, fmt.get(), formatArgs, sizeof...(Args));
}

/// 'fmt' formatted with 'args', as std::format would.
template <class... Args>
[[nodiscard]] std::string Format(FormatString<Args...> fmt, const Args&... args) {
  std::string out;
  FormatTo(out, fmt, args...);
  return out;
}

}  // namespace aeronet
