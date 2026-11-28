#include "aeronet/format-lite.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

namespace {

// A format string as a template argument, so that a test checks it at compile time for both aeronet::Format and
// std::format.
template <std::size_t N>
struct FixedFormat {
  consteval FixedFormat(const char (&str)[N]) { std::copy_n(str, N, chars); }  // NOLINT(*-explicit-constructor)

  [[nodiscard]] constexpr std::string_view view() const { return {chars, N - 1}; }

  char chars[N]{};
};

template <FixedFormat kFmt, class... Args>
void ExpectSameAsStdFormat(const Args&... args) {
  EXPECT_EQ(Format(kFmt.view(), args...), std::format(kFmt.view(), args...)) << kFmt.view();
}

// The reason why 'fmt' is invalid for arguments of 'types' (checked at run time), empty if it is valid.
std::string CheckError(std::string_view fmt, std::initializer_list<FormatArg::Type> types) {
  try {
    detail::CheckFormatString(fmt, types.begin(), types.size());
  } catch (const std::invalid_argument& ex) {
    return ex.what();
  }
  return {};
}

// Replacement fields '{:spec}' combining the parts of the standard format specification.
std::vector<std::string> SpecificationFields() {
  static constexpr std::string_view kAligns[]{"", "<", "*^", "0>"};
  static constexpr std::string_view kSigns[]{"", "+", "-", " "};
  static constexpr std::string_view kAlternates[]{"", "#"};
  static constexpr std::string_view kZeros[]{"", "0"};
  static constexpr std::string_view kWidths[]{"", "7"};
  static constexpr std::string_view kPrecisions[]{"", ".3", ".0"};
  static constexpr std::string_view kPresentations[]{
      "", "s", "c", "b", "B", "d", "o", "x", "X", "a", "A", "e", "E", "f", "F", "g", "G", "p",
  };
  std::vector<std::string> fields;
  for (std::string_view align : kAligns) {
    for (std::string_view sign : kSigns) {
      for (std::string_view alternate : kAlternates) {
        for (std::string_view zero : kZeros) {
          for (std::string_view width : kWidths) {
            for (std::string_view precision : kPrecisions) {
              for (std::string_view presentation : kPresentations) {
                std::string& field = fields.emplace_back("{:");
                field.append(align).append(sign).append(alternate).append(zero).append(width).append(precision);
                field.append(presentation).push_back('}');
              }
            }
          }
        }
      }
    }
  }
  return fields;
}

// Formats 'value' with each of the 'fields' accepted for it, as std::format. Returns the number of accepted fields.
template <class T>
std::size_t CheckAcceptedFields(const T& value, const std::vector<std::string>& fields) {
  const FormatArg arg(value);
  std::size_t nbAccepted = 0;
  for (const std::string& field : fields) {
    if (!CheckError(field, {FormatArg::TypeOf<T>()}).empty()) {
      continue;
    }
    ++nbAccepted;
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char>) {
      // '{:c}' of an integer out of the range of char (a negative one where char is unsigned, as on ARM) throws at run
      // time, as std::format.
      using CharRange = std::conditional_t<std::is_signed_v<char>, signed char, unsigned char>;
      if (field.ends_with("c}") && !std::in_range<CharRange>(value)) {
        EXPECT_THROW(static_cast<void>(std::vformat(field, std::make_format_args(value))), std::format_error) << field;
        std::string formatted;
        EXPECT_THROW(VFormatTo(formatted, field, &arg, 1), std::format_error) << field;
        continue;
      }
    }
    std::string expected;
    try {
      expected = std::vformat(field, std::make_format_args(value));
    } catch (const std::format_error& ex) {
      ADD_FAILURE() << field << " accepted, rejected by std::format: " << ex.what();
      continue;
    }
    std::string formatted;
    VFormatTo(formatted, field, &arg, 1);
    EXPECT_EQ(formatted, expected) << field;
    RawChars rawFormatted;
    VFormatTo(rawFormatted, field, &arg, 1);
    EXPECT_EQ(std::string_view(rawFormatted), expected) << field;
  }
  return nbAccepted;
}
}  // namespace

TEST(FormatLiteTest, DefaultFormatOfEachType) {
  const int value = 0;
  char mutableChars[] = "mutable";
  ExpectSameAsStdFormat<"{} {}">(true, false);
  ExpectSameAsStdFormat<"{}{}">('a', '\0');
  ExpectSameAsStdFormat<"{} {} {} {} {}">(static_cast<signed char>(-5), static_cast<short>(-300), -42, -42L,
                                          std::numeric_limits<long long>::min());
  ExpectSameAsStdFormat<"{} {} {} {} {}">(static_cast<unsigned char>(200), static_cast<unsigned short>(65535), 42U,
                                          std::numeric_limits<unsigned long>::max(),
                                          std::numeric_limits<unsigned long long>::max());
  ExpectSameAsStdFormat<"{} {} {} {}">(1.5F, 0.1F, -0.0F, std::numeric_limits<float>::infinity());
  ExpectSameAsStdFormat<"{} {} {} {}">(0.1, 1e300, -2.5, std::numeric_limits<double>::quiet_NaN());
  ExpectSameAsStdFormat<"{} {} {} {} {}">(std::string("string"), std::string_view("view"), "literal",
                                          static_cast<const char*>("pointer"), mutableChars);
  ExpectSameAsStdFormat<"{} {} {}">(static_cast<const void*>(&value), nullptr,
                                    static_cast<void*>(const_cast<int*>(&value)));
  ExpectSameAsStdFormat<"{:} {:}">(42, "empty spec");
}

TEST(FormatLiteTest, FormatSpecifications) {
  ExpectSameAsStdFormat<"{:x} {:#010x} {:05} {:08d} {:04x} {:02x} {:+} {: } {:b} {:#B} {:o} {:X} {:c}">(
      255, 255, -42, 42, 10U, 7U, 3, 3L, 5U, 5, 8, 255UL, 65);
  ExpectSameAsStdFormat<"[{:>8}] [{:*^9}] [{:<5}] [{:0>6}]">(42, -42, 7U, 1);
  ExpectSameAsStdFormat<"{:d} {:#x} {:>6} {:s} {:*^7}">(true, true, false, false, true);
  ExpectSameAsStdFormat<"{:c} {:d} {:#x} {:>3} {:*^5}">('a', 'a', 'a', 'a', 'a');
  ExpectSameAsStdFormat<"{:.2f} {:e} {:10.3g} {:+a} {:F} {:#G} {:010.4f}">(1.23456F, 1.5, 0.000123456, 2.0, -1.0, 100.0,
                                                                           -2.5);
  ExpectSameAsStdFormat<"[{:>10}] [{:.3}] [{:*^9.2}] [{:<6s}]">("text", std::string("truncated"), "ab", "cd");
  int value = 0;
  ExpectSameAsStdFormat<"[{:p}] [{:>20}] [{:<20p}]">(static_cast<const void*>(&value), nullptr,
                                                     static_cast<void*>(&value));
}

TEST(FormatLiteTest, EscapedBracesAndLiteralText) {
  ExpectSameAsStdFormat<"{{}}">();
  ExpectSameAsStdFormat<"">();
  ExpectSameAsStdFormat<"no field">();
  ExpectSameAsStdFormat<"a{{b}}c {} d">(1);
  ExpectSameAsStdFormat<"}}{}{{">(2);
  ExpectSameAsStdFormat<"{}{}{}">(1, "x", 'y');
  ExpectSameAsStdFormat<"{{{}}}">("braced");
  EXPECT_EQ(Format("{{literal}}"), "{literal}");
}

TEST(FormatLiteTest, AppendsToExistingContent) {
  std::string out = "prefix ";
  FormatTo(out, "{} and {:>6}", 42, std::string_view("text"));
  EXPECT_EQ(out, "prefix 42 and   text");

  RawChars rawOut(std::string_view("prefix "));
  FormatTo(rawOut, "{} and {:>6} {} {}", -42, "text", true, 'c');
  EXPECT_EQ(std::string_view(rawOut), "prefix -42 and   text true c");
}

TEST(FormatLiteTest, LargeArguments) {
  const std::string large(std::size_t{1} << 20U, 'x');
  const std::string formatted = Format("[{}] [{:.5}] {}", large, large, 7);
  EXPECT_EQ(formatted, std::format("[{}] [{:.5}] {}", large, large, 7));
  EXPECT_EQ(Format("{:>65535}", 'c'), std::format("{:>65535}", 'c'));
  EXPECT_EQ(Format("{} {} {} {} {} {} {} {} {} {} {} {} {} {} {} {}", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                   "sixteen"),
            "1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 sixteen");
}

// An integer out of the range of char has no '{:c}' form: std::format throws, at run time.
TEST(FormatLiteTest, CharacterOfAnIntegerOutOfRangeThrows) {
  EXPECT_THROW(static_cast<void>(Format("{:c}", 1000000)), std::format_error);
  EXPECT_EQ(Format("{:c}", 65), "A");
}

TEST(FormatLiteTest, InvalidFormatStrings) {
  using enum FormatArg::Type;
  EXPECT_EQ(CheckError("a}b", {}), "unmatched '}' in format string (write '}}' for '}')");
  EXPECT_EQ(CheckError("}", {}), "unmatched '}' in format string (write '}}' for '}')");
  EXPECT_EQ(CheckError("{", {kInt}), "unmatched '{' in format string (write '{{' for '{')");
  EXPECT_EQ(CheckError("{:x", {kInt}), "unmatched '{' in format string (write '{{' for '{')");
  EXPECT_EQ(CheckError("{0}", {kInt}),
            "invalid replacement field, '{}' or '{:spec}' expected (argument indexes are not supported)");
  EXPECT_EQ(CheckError("{x}", {kInt}),
            "invalid replacement field, '{}' or '{:spec}' expected (argument indexes are not supported)");
  EXPECT_EQ(CheckError("{} {}", {kInt}), "more replacement fields than arguments in format string");
  EXPECT_EQ(CheckError("{}", {kInt, kInt}), "more arguments than replacement fields in format string");
  EXPECT_EQ(CheckError("{{}}", {kInt}), "more arguments than replacement fields in format string");
  EXPECT_EQ(CheckError("{:{}}", {kInt, kInt}),
            "nested replacement fields (dynamic width or precision) are not supported");
  EXPECT_EQ(CheckError("{:65536}", {kInt}), "width or precision larger than 65535 in format string");
  EXPECT_EQ(CheckError("{:.70000}", {kDouble}), "width or precision larger than 65535 in format string");
  EXPECT_EQ(CheckError("{:00}", {kInt}), "width must be non-zero in format string");
  EXPECT_EQ(CheckError("{:.}", {kDouble}), "missing precision after '.' in format string");
  EXPECT_EQ(CheckError("{:.f}", {kDouble}), "missing precision after '.' in format string");
  EXPECT_EQ(CheckError("{:L}", {kInt}), "locale-specific form 'L' not supported in format string");
  EXPECT_EQ(CheckError("{:Ld}", {kInt}), "locale-specific form 'L' not supported in format string");
  EXPECT_EQ(CheckError("{:?}", {kString}), "format specification not valid for the type of its argument");
  EXPECT_EQ(CheckError("{:dd}", {kInt}), "invalid format specification in format string");
  EXPECT_EQ(CheckError("{:x}", {kString}), "format specification not valid for the type of its argument");
  EXPECT_EQ(CheckError("{}", {kNone}), "format specification not valid for the type of its argument");
  EXPECT_EQ(CheckError("{:>10} {{}} {:#x}", {kString, kUInt}), "");
}

// Every specification accepted for an argument is accepted by std::format, with the same output.
TEST(FormatLiteTest, AcceptedSpecificationsFormatAsStdFormat) {
  const std::vector<std::string> fields = SpecificationFields();
  const int pointee = 0;
  EXPECT_GT(CheckAcceptedFields(true, fields), 100U);
  EXPECT_GT(CheckAcceptedFields('a', fields), 100U);
  EXPECT_GT(CheckAcceptedFields(-42, fields), 500U);
  EXPECT_GT(CheckAcceptedFields(42U, fields), 500U);
  EXPECT_GT(CheckAcceptedFields(1000, fields), 500U);
  // Zero (no octal base prefix), the extreme values, and a char above 127 (converted to unsigned char as an integer).
  EXPECT_GT(CheckAcceptedFields(0, fields), 500U);
  EXPECT_GT(CheckAcceptedFields(std::numeric_limits<long long>::min(), fields), 500U);
  EXPECT_GT(CheckAcceptedFields(std::numeric_limits<unsigned long long>::max(), fields), 500U);
  EXPECT_GT(CheckAcceptedFields(false, fields), 100U);
  EXPECT_GT(CheckAcceptedFields(static_cast<char>(200), fields), 100U);
  EXPECT_GT(CheckAcceptedFields(1.5F, fields), 1000U);
  EXPECT_GT(CheckAcceptedFields(-0.125, fields), 1000U);
  EXPECT_GT(CheckAcceptedFields(std::string_view("text"), fields), 40U);
  EXPECT_GT(CheckAcceptedFields(static_cast<const void*>(&pointee), fields), 10U);
  EXPECT_GT(CheckAcceptedFields(nullptr, fields), 10U);
}

// The widest integer fields: 64 binary digits with sign and base prefix, and the largest width.
TEST(FormatLiteTest, WidestIntegerFields) {
  ExpectSameAsStdFormat<"{:#b} {:+#066B} {:#o} {:#X}">(std::numeric_limits<long long>::min(),
                                                       std::numeric_limits<unsigned long long>::max(), -1, 255U);
  ExpectSameAsStdFormat<"{:>65535} {:*<65535x} {:^65535} {:065535}">(42, 255U, -7, -3);
  int value = 0;
  ExpectSameAsStdFormat<"{:^65535p}">(static_cast<const void*>(&value));
}

}  // namespace aeronet
