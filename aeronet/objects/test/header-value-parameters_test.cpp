#include "aeronet/header-value-parameters.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace aeronet::http {

namespace {

std::vector<HeaderParameter> Parameters(std::string_view headerValue) {
  std::vector<HeaderParameter> parameters;
  for (HeaderParametersIterator it(headerValue); it.hasNext();) {
    parameters.push_back(it.next());
  }
  return parameters;
}

void ExpectParameter(const HeaderParameter& parameter, std::string_view name, std::string_view value) {
  EXPECT_EQ(parameter.name, name);
  EXPECT_TRUE(parameter.hasValue()) << name;
  EXPECT_EQ(parameter.value, value);
}

void ExpectParameterWithoutValue(const HeaderParameter& parameter, std::string_view name) {
  EXPECT_EQ(parameter.name, name);
  EXPECT_FALSE(parameter.hasValue()) << name;
}

}  // namespace

TEST(ValueWithoutParameters, Basic) {
  static_assert(ValueWithoutParameters("text/html; charset=utf-8") == "text/html");
  EXPECT_EQ(ValueWithoutParameters("text/html"), "text/html");
  EXPECT_EQ(ValueWithoutParameters(" \ttext/html \t; charset=utf-8"), "text/html");
  EXPECT_EQ(ValueWithoutParameters("gzip;q=0.5"), "gzip");
  EXPECT_EQ(ValueWithoutParameters(""), "");
  EXPECT_EQ(ValueWithoutParameters(";q=1"), "");
  EXPECT_EQ(ValueWithoutParameters(" ; "), "");
}

TEST(ParseHeaderParameter, NameAndValue) {
  static_assert(ParseHeaderParameter("q=0.5").value == "0.5");
  ExpectParameter(ParseHeaderParameter("charset=utf-8"), "charset", "utf-8");
  ExpectParameter(ParseHeaderParameter(" \tcharset \t= \tutf-8 \t"), "charset", "utf-8");
  ExpectParameter(ParseHeaderParameter("x=a=b"), "x", "a=b");
}

TEST(ParseHeaderParameter, QuotedValueIsUnquotedNotUnescaped) {
  ExpectParameter(ParseHeaderParameter(R"(boundary="a b")"), "boundary", "a b");
  ExpectParameter(ParseHeaderParameter(R"(x="a\"b")"), "x", R"(a\"b)");
  ExpectParameter(ParseHeaderParameter(R"(x="")"), "x", "");
  ExpectParameter(ParseHeaderParameter(R"(x=")"), "x", R"(")");
}

// An empty value given with '=' has data, unlike the value of a parameter without '='.
TEST(ParseHeaderParameter, EmptyValueAndNoValue) {
  ExpectParameter(ParseHeaderParameter("x="), "x", "");
  ExpectParameter(ParseHeaderParameter("x= "), "x", "");
  ExpectParameter(ParseHeaderParameter("=v"), "", "v");
  ExpectParameterWithoutValue(ParseHeaderParameter("server_no_context_takeover"), "server_no_context_takeover");
  ExpectParameterWithoutValue(ParseHeaderParameter(" x "), "x");
  ExpectParameterWithoutValue(ParseHeaderParameter(""), "");
  ExpectParameterWithoutValue(ParseHeaderParameter(std::string_view{}), "");
}

TEST(HeaderParametersIterator, FirstElement) {
  for (std::string_view headerValue : {"text/html", " text/html ", "text/html;", " text/html\t;x=1;y=\"a;b\""}) {
    EXPECT_EQ(HeaderParametersIterator(headerValue).firstElement(), "text/html") << headerValue;
    EXPECT_EQ(HeaderParametersIterator(headerValue).firstElement(), ValueWithoutParameters(headerValue));
  }
  EXPECT_EQ(HeaderParametersIterator("").firstElement(), "");
  EXPECT_EQ(HeaderParametersIterator(" ;x=1").firstElement(), "");
}

// The name ends at the first '=' that is not in a quoted string.
TEST(HeaderParametersIterator, EqualInQuotedString) {
  const auto parameters = Parameters(R"(t; x="a=b"; "c=d"; e="f")");
  ASSERT_EQ(parameters.size(), 3U);
  ExpectParameter(parameters[0], "x", "a=b");
  ExpectParameterWithoutValue(parameters[1], R"("c=d")");
  ExpectParameter(parameters[2], "e", "f");
}

TEST(HeaderParametersIterator, NoParameters) {
  EXPECT_TRUE(Parameters("").empty());
  EXPECT_TRUE(Parameters("text/html").empty());
  EXPECT_TRUE(Parameters(" text/html ").empty());
}

TEST(HeaderParametersIterator, Parameters) {
  const auto parameters = Parameters(R"(multipart/form-data ; boundary="xyz" ;charset=utf-8;  flag)");
  ASSERT_EQ(parameters.size(), 3U);
  ExpectParameter(parameters[0], "boundary", "xyz");
  ExpectParameter(parameters[1], "charset", "utf-8");
  ExpectParameterWithoutValue(parameters[2], "flag");
}

// RFC 9110 §5.6.6 allows empty parameters. Nothing follows a ';' ending the value, unlike a ';' followed by whitespace.
TEST(HeaderParametersIterator, EmptyParameters) {
  EXPECT_TRUE(Parameters("text/html;").empty());

  auto parameters = Parameters("text/html; ");
  ASSERT_EQ(parameters.size(), 1U);
  ExpectParameterWithoutValue(parameters[0], "");

  parameters = Parameters("form-data;; name=a;");
  ASSERT_EQ(parameters.size(), 2U);
  ExpectParameterWithoutValue(parameters[0], "");
  ExpectParameter(parameters[1], "name", "a");

  parameters = Parameters(";;");
  ASSERT_EQ(parameters.size(), 1U);
  ExpectParameterWithoutValue(parameters[0], "");
}

TEST(HeaderParametersIterator, SemicolonInQuotedStringDoesNotEndTheParameter) {
  auto parameters = Parameters(R"(form-data; name="a;b"; filename="c; d.txt";x=1)");
  ASSERT_EQ(parameters.size(), 3U);
  ExpectParameter(parameters[0], "name", "a;b");
  ExpectParameter(parameters[1], "filename", "c; d.txt");
  ExpectParameter(parameters[2], "x", "1");

  // An escaped quote does not end the quoted string.
  parameters = Parameters(R"(t; x="a\";b"; y=2)");
  ASSERT_EQ(parameters.size(), 2U);
  ExpectParameter(parameters[0], "x", R"(a\";b)");
  ExpectParameter(parameters[1], "y", "2");

  // An escaped backslash does not escape the quote after it.
  parameters = Parameters(R"(t; x="a\\"; y=2)");
  ASSERT_EQ(parameters.size(), 2U);
  ExpectParameter(parameters[0], "x", R"(a\\)");
  ExpectParameter(parameters[1], "y", "2");
}

// An unterminated quoted string, or a trailing backslash, extend to the end of the value.
TEST(HeaderParametersIterator, UnterminatedQuotedString) {
  auto parameters = Parameters(R"(t; x="a;b)");
  ASSERT_EQ(parameters.size(), 1U);
  ExpectParameter(parameters[0], "x", R"("a;b)");

  parameters = Parameters(R"(t; x="a\)");
  ASSERT_EQ(parameters.size(), 1U);
  ExpectParameter(parameters[0], "x", R"("a\)");
}

}  // namespace aeronet::http
