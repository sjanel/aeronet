#include "aeronet/search-crlf.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

namespace aeronet {

TEST(MemoryUtilsSearchCRLF, FindsAndReportsAbsence) {
  // SearchCRLF operates on raw char pointers, so use data()/data()+size(); MSVC string_view::iterator is a
  // wrapper class that does not convert to const char*.
  std::string_view withCrlf = "abc\r\ndef";
  const char* withCrlfBeg = withCrlf.data();
  const char* withCrlfEnd = withCrlfBeg + withCrlf.size();
  const char* it = SearchCRLF(withCrlfBeg, withCrlfEnd);
  ASSERT_NE(it, withCrlfEnd);
  EXPECT_EQ(static_cast<std::size_t>(it - withCrlfBeg), 3U);

  // A lone CR (no following LF) must not be reported as a CRLF.
  std::string_view loneCr = "abc\rdef";
  const char* loneCrBeg = loneCr.data();
  const char* loneCrEnd = loneCrBeg + loneCr.size();
  EXPECT_EQ(SearchCRLF(loneCrBeg, loneCrEnd), loneCrEnd);

  std::string_view none = "no line break here";
  const char* noneBeg = none.data();
  const char* noneEnd = noneBeg + none.size();
  EXPECT_EQ(SearchCRLF(noneBeg, noneEnd), noneEnd);

  // CR as the final byte: there is no room for a following LF.
  std::string_view trailingCr = "abc\r";
  const char* trailingCrBeg = trailingCr.data();
  const char* trailingCrEnd = trailingCrBeg + trailingCr.size();
  EXPECT_EQ(SearchCRLF(trailingCrBeg, trailingCrEnd), trailingCrEnd);

  std::string vectorTrailingCr(16, 'x');
  vectorTrailingCr.back() = '\r';
  const char* const vectorTrailingCrBegin = vectorTrailingCr.data();
  const char* const vectorTrailingCrEnd = vectorTrailingCrBegin + vectorTrailingCr.size();
  EXPECT_EQ(SearchCRLF(vectorTrailingCrBegin, vectorTrailingCrEnd), vectorTrailingCrEnd);
}

TEST(MemoryUtilsSearchCRLF, FindsAcrossSimdPrefixAndFallbackBoundaries) {
  for (const std::size_t offset : {
           std::size_t{0},
           std::size_t{14},
           std::size_t{15},
           std::size_t{16},
           std::size_t{31},
           std::size_t{32},
           std::size_t{63},
           std::size_t{64},
           std::size_t{95},
           std::size_t{96},
           std::size_t{123},
           std::size_t{124},
           std::size_t{126},
           std::size_t{127},
           std::size_t{128},
           std::size_t{129},
           std::size_t{255},
           std::size_t{16'383},
       }) {
    std::string input(offset + 34, 'x');
    input[offset] = '\r';
    input[offset + 1] = '\n';
    const char* const begin = input.data();
    EXPECT_EQ(SearchCRLF(begin, begin + input.size()), begin + offset) << "offset=" << offset;
  }
}

TEST(MemoryUtilsSearchCRLF, SkipsFalseCarriageReturnsBeforeFallbackMatch) {
  std::string input(192, 'x');
  for (const std::size_t offset : {
           std::size_t{1},
           std::size_t{5},
           std::size_t{15},
           std::size_t{31},
           std::size_t{63},
           std::size_t{95},
           std::size_t{127},
           std::size_t{140},
       }) {
    input[offset] = '\r';
  }
  input[150] = '\r';
  input[151] = '\n';

  const char* const begin = input.data();
  EXPECT_EQ(SearchCRLF(begin, begin + input.size()), begin + 150);
}

TEST(MemoryUtilsSearchCRLF, ReportsLargeFallbackMissAndPreservesMutablePointer) {
  std::string noCrlf(4096, 'x');
  EXPECT_EQ(SearchCRLF(noCrlf.data(), noCrlf.data() + noCrlf.size()), noCrlf.data() + noCrlf.size());

  std::string mutableInput = "Header: value\r\n";
  char* const begin = mutableInput.data();
  char* const result = SearchCRLF(begin, begin + mutableInput.size());
  EXPECT_EQ(result, begin + 13);
}

}  // namespace aeronet
