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

TEST(MemoryUtilsSearchCRLF, FindsAcrossLineLengths) {
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

TEST(MemoryUtilsSearchCRLF, ReportsLargeFallbackMissAndPreservesMutablePointer) {
  std::string noCrlf(4096, 'x');
  EXPECT_EQ(SearchCRLF(noCrlf.data(), noCrlf.data() + noCrlf.size()), noCrlf.data() + noCrlf.size());

  std::string mutableInput = "Header: value\r\n";
  char* const begin = mutableInput.data();
  char* const result = SearchCRLF(begin, begin + mutableInput.size());
  EXPECT_EQ(result, begin + 13);
}

TEST(MemoryUtilsSearchCRLF, StrictSearchDistinguishesIncompleteAndMalformedLines) {
  const std::string_view complete = "Header: value\r\nnext";
  const auto completeResult = SearchCRLF(complete.data(), complete.data() + complete.size());
  ASSERT_EQ(completeResult, complete.data() + 13);
  EXPECT_EQ(completeResult[1], '\n');

  const std::string_view noCr = "Header: value";
  const auto noCrResult = SearchCRLF(noCr.data(), noCr.data() + noCr.size());
  EXPECT_EQ(noCrResult, noCr.data() + noCr.size());

  const std::string_view trailingCr = "Header: value\r";
  const auto trailingCrResult = SearchCRLF(trailingCr.data(), trailingCr.data() + trailingCr.size());
  EXPECT_EQ(trailingCrResult, trailingCr.data() + trailingCr.size());

  const std::string_view malformed = "Header: bad\rvalue\r\n";
  const auto malformedResult = SearchCRLF(malformed.data(), malformed.data() + malformed.size());
  ASSERT_EQ(malformedResult, malformed.data() + 11);
  EXPECT_EQ(malformedResult[1], 'v');

  std::string mutableLine = "Header: value\r\n";
  const auto mutableResult = SearchCRLF(mutableLine.data(), mutableLine.data() + mutableLine.size());
  EXPECT_EQ(mutableResult, mutableLine.data() + 13);
}

namespace {

bool CanStillBeValid(std::string_view lines) { return CanStillBeValidLines(lines.data(), lines.data() + lines.size()); }

}  // namespace

TEST(MemoryUtilsSearchCRLF, CanStillBeValidLinesOfALineWithoutCRLF) {
  // The lines SearchCRLF() did not find the end of: only a bare LF makes them invalid.
  EXPECT_TRUE(CanStillBeValid(""));
  EXPECT_TRUE(CanStillBeValid("Header: val"));
  EXPECT_TRUE(CanStillBeValid("Header: value\r"));  // the CR of a CRLF still to come
  EXPECT_TRUE(CanStillBeValid("\r"));
  EXPECT_TRUE(CanStillBeValid(std::string(8192, 'x')));

  EXPECT_FALSE(CanStillBeValid("\n"));
  EXPECT_FALSE(CanStillBeValid("Header: value\n"));
  EXPECT_FALSE(CanStillBeValid("GET / HTTP/1.1\nHost: x\n\n"));
  EXPECT_FALSE(CanStillBeValid("Header: val\nue\r"));
  EXPECT_FALSE(CanStillBeValid(std::string(4096, 'x') + "\n" + std::string(4096, 'y')));
}

TEST(MemoryUtilsSearchCRLF, CanStillBeValidLinesWithABareCR) {
  // A CR followed by anything but its LF, whether it ends the first line or another one.
  EXPECT_FALSE(CanStillBeValid("\rX"));
  EXPECT_FALSE(CanStillBeValid("Header: bad\rvalue\r\n"));
  EXPECT_FALSE(CanStillBeValid("Header: value\r\r\n"));
  EXPECT_FALSE(CanStillBeValid("GET / HTTP/1.1\r\nHost: x\rX"));
  EXPECT_FALSE(CanStillBeValid("GET / HTTP/1.1\r\nHost: x\r\n\r\r"));
  EXPECT_FALSE(CanStillBeValid(std::string(4096, 'x') + "\r\n" + std::string(4096, 'y') + "\r\r"));
}

TEST(MemoryUtilsSearchCRLF, CanStillBeValidLinesOfSeveralLines) {
  // Every line but the last one ends with CRLF, an empty one included.
  EXPECT_TRUE(CanStillBeValid("\r\n"));
  EXPECT_TRUE(CanStillBeValid("\r\n\r\n"));
  EXPECT_TRUE(CanStillBeValid("GET / HTTP/1.1\r\nHost: x\r\n"));
  EXPECT_TRUE(CanStillBeValid("GET / HTTP/1.1\r\nHost: x\r\nX-Last: z"));
  EXPECT_TRUE(CanStillBeValid("GET / HTTP/1.1\r\nHost: x\r\n\r"));
  EXPECT_TRUE(CanStillBeValid("GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /next HTTP/1.1\r\n"));

  // A bare LF in a line ended by CRLF, or starting a line.
  EXPECT_FALSE(CanStillBeValid("GET / HTTP/1.1\r\nHo\nst: x\r\n"));
  EXPECT_FALSE(CanStillBeValid("GET / HTTP/1.1\r\n\nHost: x\r\n"));
  EXPECT_FALSE(CanStillBeValid("GET / HTTP/1.1\r\nHost: x\n\r\n"));
  EXPECT_FALSE(CanStillBeValid("GET / HTTP/1.1\r\nHost: x\r\n\n"));

  // Many lines, with the bare one at the end.
  std::string lines;
  for (int lineNb = 0; lineNb < 1000; ++lineNb) {
    lines.append("X-Field: value\r\n");
  }
  EXPECT_TRUE(CanStillBeValid(lines));
  EXPECT_FALSE(CanStillBeValid(lines + "X-Last: \n"));
  EXPECT_FALSE(CanStillBeValid(lines + "X-Last: \rz"));
}

}  // namespace aeronet
