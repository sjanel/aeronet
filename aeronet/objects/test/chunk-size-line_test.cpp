#include "aeronet/chunk-size-line.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <string_view>

#include "aeronet/http-status-code.hpp"

namespace aeronet::http {

namespace {

using State = ChunkSizeLine::State;

constexpr ChunkSizeSyntax kSyntaxes[]{ChunkSizeSyntax::Strict, ChunkSizeSyntax::Lenient};

ChunkSizeLine Parse(std::string_view input, ChunkSizeSyntax syntax, std::size_t maxLineSize = kNoChunkSizeLineLimit,
                    std::size_t maxChunkSize = kNoChunkSizeLineLimit) {
  return ParseChunkSizeLine(input.data(), input.data() + input.size(), maxLineSize, maxChunkSize, syntax);
}

// 'line' is complete, followed by data or not, and every prefix of it is incomplete.
void ExpectComplete(std::string_view line, std::size_t chunkSize, ChunkSizeSyntax syntax) {
  SCOPED_TRACE(line);
  for (const std::string& input : {std::string(line), std::string(line).append("data\r\n")}) {
    const ChunkSizeLine parsed = Parse(input, syntax);
    ASSERT_EQ(parsed.state, State::Complete);
    EXPECT_EQ(parsed.chunkSize, chunkSize);
    EXPECT_EQ(parsed.size, line.size());
  }
  for (std::size_t prefixSize = 0; prefixSize < line.size(); ++prefixSize) {
    EXPECT_EQ(Parse(line.substr(0, prefixSize), syntax).state, State::Incomplete) << prefixSize;
  }
}

// 'invalid' is rejected as soon as its last byte is received: every prefix of it is incomplete, and it is rejected
// whatever follows.
void ExpectRejected(std::string_view invalid, ChunkSizeSyntax syntax, StatusCode status = StatusCodeBadRequest) {
  SCOPED_TRACE(invalid);
  for (const std::string& input : {
           std::string(invalid),
           std::string(invalid).append("\r\n"),
           std::string(invalid).append(";ext\r\n"),
           std::string(invalid).append("5\r\n"),
       }) {
    const ChunkSizeLine parsed = Parse(input, syntax);
    ASSERT_EQ(parsed.state, State::Rejected) << input;
    EXPECT_EQ(parsed.status, status);
    EXPECT_NE(parsed.pMessage, nullptr);
  }
  for (std::size_t prefixSize = 0; prefixSize < invalid.size(); ++prefixSize) {
    EXPECT_EQ(Parse(invalid.substr(0, prefixSize), syntax).state, State::Incomplete) << prefixSize;
  }
}

}  // namespace

TEST(ChunkSizeLineTest, HexadecimalSizes) {
  for (const ChunkSizeSyntax syntax : kSyntaxes) {
    ExpectComplete("0\r\n", 0, syntax);
    ExpectComplete("5\r\n", 5, syntax);
    ExpectComplete("aBcDeF\r\n", 0xABCDEF, syntax);
    ExpectComplete("10\r\n", 16, syntax);
    // Leading zeros do not count toward the digits of the size.
    ExpectComplete(std::string(40, '0').append("ff\r\n"), 255, syntax);
    ExpectComplete(std::string(sizeof(std::size_t) * 2U, 'f').append("\r\n"), std::numeric_limits<std::size_t>::max(),
                   syntax);
  }
}

TEST(ChunkSizeLineTest, ExtensionsAreValidatedAndIgnored) {
  for (const ChunkSizeSyntax syntax : kSyntaxes) {
    ExpectComplete("5;ext\r\n", 5, syntax);
    ExpectComplete("5;\r\n", 5, syntax);
    ExpectComplete("5;name=value;flag\r\n", 5, syntax);
    ExpectComplete("5;name=\"a\tb;c\"\r\n", 5, syntax);
    // obs-text, as in a quoted-string (RFC 9112 §7.1.1).
    ExpectComplete("5;name=\"caf\xC3\xA9\"\r\n", 5, syntax);
    // BWS ";" BWS chunk-ext-name (RFC 9112 §7.1.1).
    ExpectComplete("5 ;ext\r\n", 5, syntax);
    ExpectComplete("5\t;ext\r\n", 5, syntax);
    ExpectComplete("5 \t ;\t ext = v\r\n", 5, syntax);
    ExpectComplete("0 ;last\r\n", 0, syntax);
  }
}

TEST(ChunkSizeLineTest, WhitespaceAroundTheSizeIsLenient) {
  for (const std::string_view line : {" 5\r\n", "\t5\r\n", "5 \r\n", "5\t \r\n", " \t5 \t\r\n", "\t5 \t;ext\r\n"}) {
    ExpectComplete(line, 5, ChunkSizeSyntax::Lenient);
  }
  for (const std::string_view invalid : {" ", "\t", "5 \r", "5\t\r", "5 \t\r"}) {
    ExpectRejected(invalid, ChunkSizeSyntax::Strict);
  }
}

TEST(ChunkSizeLineTest, InvalidSizesAreRejected) {
  for (const ChunkSizeSyntax syntax : kSyntaxes) {
    // An empty chunk-size is not a last-chunk.
    for (const std::string_view invalid : {"\r", ";", "\n", "x", "0x", "+", "-", "5x", "5 5", "10 0", "5 x"}) {
      ExpectRejected(invalid, syntax);
    }
  }
  // Only whitespace before the end of the line or the extension.
  for (const std::string_view invalid : {" \t\r", " ;", "\t\n"}) {
    ExpectRejected(invalid, ChunkSizeSyntax::Lenient);
  }
}

TEST(ChunkSizeLineTest, BareCrAndLfAreRejected) {
  for (const ChunkSizeSyntax syntax : kSyntaxes) {
    for (const std::string_view invalid : {"5\n", "5 \n", "5\rX", "5\r\r", "5;ext\n", "5;ext\rX", "5 ;\n", "5;\r;"}) {
      ExpectRejected(invalid, syntax);
    }
  }
}

TEST(ChunkSizeLineTest, ControlCharactersInExtensionsAreRejected) {
  for (const ChunkSizeSyntax syntax : kSyntaxes) {
    for (const std::string_view invalid : {
             std::string_view("5;\x01"),
             std::string_view("5;a=\x7F"),
             std::string_view("5 ;\x1F"),
             std::string_view("5;\0", 3),
         }) {
      ExpectRejected(invalid, syntax);
    }
  }
}

TEST(ChunkSizeLineTest, OverflowIsRejectedWithoutLimit) {
  constexpr std::size_t kMaxHexDigits = sizeof(std::size_t) * 2U;
  for (const ChunkSizeSyntax syntax : kSyntaxes) {
    ExpectRejected(std::string(kMaxHexDigits + 1U, 'f'), syntax, StatusCodePayloadTooLarge);
    ExpectRejected(std::string("1").append(kMaxHexDigits, '0'), syntax, StatusCodePayloadTooLarge);
  }
}

TEST(ChunkSizeLineTest, MaxChunkSize) {
  for (const ChunkSizeSyntax syntax : kSyntaxes) {
    for (const std::size_t maxChunkSize : {std::size_t{0}, std::size_t{0x10}, std::size_t{0x1F}, std::size_t{0xABC}}) {
      SCOPED_TRACE(maxChunkSize);
      for (std::size_t chunkSize = 0; chunkSize <= maxChunkSize + 0x20; ++chunkSize) {
        const std::string line = std::format("{:x}\r\n", chunkSize);
        const ChunkSizeLine parsed = Parse(line, syntax, kNoChunkSizeLineLimit, maxChunkSize);
        if (chunkSize <= maxChunkSize) {
          ASSERT_EQ(parsed.state, State::Complete) << line;
          EXPECT_EQ(parsed.chunkSize, chunkSize);
        } else {
          ASSERT_EQ(parsed.state, State::Rejected) << line;
          EXPECT_EQ(parsed.status, StatusCodePayloadTooLarge);
        }
      }
    }
    // Rejected at the digit making the size too large, before the end of the line.
    EXPECT_EQ(Parse("100", syntax, kNoChunkSizeLineLimit, 0xFF).state, State::Rejected);
    EXPECT_EQ(Parse("0000ff", syntax, kNoChunkSizeLineLimit, 0xFF).state, State::Incomplete);
  }
}

// The line, CRLF included, is bounded by maxLineSize whether it is received at once or byte by byte.
TEST(ChunkSizeLineTest, MaxLineSize) {
  for (const ChunkSizeSyntax syntax : kSyntaxes) {
    for (const std::string_view line : {"5;ext=value\r\n", "abc\r\n", "5 ;\r\n", "0\r\n"}) {
      SCOPED_TRACE(line);
      for (const std::size_t maxLineSize : {line.size() - 1, line.size(), line.size() + 1}) {
        SCOPED_TRACE(maxLineSize);
        for (std::size_t prefixSize = 0; prefixSize <= line.size(); ++prefixSize) {
          SCOPED_TRACE(prefixSize);
          const ChunkSizeLine parsed = Parse(line.substr(0, prefixSize), syntax, maxLineSize);
          if (prefixSize == line.size() && line.size() <= maxLineSize) {
            EXPECT_EQ(parsed.state, State::Complete);
            EXPECT_EQ(parsed.size, line.size());
          } else if (prefixSize >= maxLineSize) {
            ASSERT_EQ(parsed.state, State::Rejected);
            EXPECT_EQ(parsed.status, StatusCodeBadRequest);
            EXPECT_EQ(std::string_view(parsed.pMessage), "Chunk size line too long");
          } else {
            EXPECT_EQ(parsed.state, State::Incomplete);
          }
        }
        // The bytes following the line do not count.
        const std::string withData = std::string(line).append(64, 'x');
        EXPECT_EQ(Parse(withData, syntax, maxLineSize).state,
                  line.size() <= maxLineSize ? State::Complete : State::Rejected);
      }
    }
    EXPECT_EQ(Parse("", syntax, 0).state, State::Rejected);
  }
}

// A byte that the line cannot have is reported as such, wherever the line would end.
TEST(ChunkSizeLineTest, InvalidByteBeforeTheLimitIsNotATooLongLine) {
  for (const ChunkSizeSyntax syntax : kSyntaxes) {
    const std::string input = std::string("5x").append(100, 'a');
    const ChunkSizeLine parsed = Parse(input, syntax, 10);
    ASSERT_EQ(parsed.state, State::Rejected);
    EXPECT_EQ(std::string_view(parsed.pMessage), "Invalid chunk size");
  }
}

}  // namespace aeronet::http
