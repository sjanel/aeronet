#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-status-code.hpp"

namespace aeronet::http {

// Where a chunk-size line accepts whitespace.
enum class ChunkSizeSyntax : uint8_t {
  // RFC 9112 §7.1: chunk-size = 1*HEXDIG, spaces and tabs only before the ";" of a chunk extension (BWS).
  Strict,
  // Also before the chunk size, and after it without extension, as some senders pad it.
  Lenient,
};

// A chunk-size line (RFC 9112 §7.1: chunk-size [ chunk-ext ] CRLF), as parsed by ParseChunkSizeLine().
struct ChunkSizeLine {
  enum class State : uint8_t { Complete, Incomplete, Rejected };

  std::size_t chunkSize{0};        // Complete only
  std::size_t size{0};             // bytes of the line, CRLF included (Complete only)
  const char* pMessage = nullptr;  // Rejected only: why, as the body of an error response
  StatusCode status{};             // Rejected only: 413 for a chunk size above maxChunkSize, 400 otherwise
  State state{State::Complete};
};

// Lifts a limit of ParseChunkSizeLine().
inline constexpr std::size_t kNoChunkSizeLineLimit = std::numeric_limits<std::size_t>::max();

// Parses the chunk-size line starting at pFirst, whose bytes received so far end at pLast. Chunk extensions are
// validated and ignored.
// The line is rejected as soon as it holds a byte that no continuation can make valid (a bare CR or LF, misplaced
// whitespace...), and when its first maxLineSize bytes are received without its end (CRLF included): whether received
// at once or not, a line without end cannot make its receiver buffer it (and parse it again at each read) without
// limit. Chunk sizes above maxChunkSize are rejected, and so are those overflowing std::size_t whatever the limits.
constexpr ChunkSizeLine ParseChunkSizeLine(const char* pFirst, const char* pLast, std::size_t maxLineSize,
                                           std::size_t maxChunkSize, ChunkSizeSyntax syntax) noexcept {
  const auto rejected = [](StatusCode status, const char* pMessage) {
    return ChunkSizeLine{.pMessage = pMessage, .status = status, .state = ChunkSizeLine::State::Rejected};
  };
  // Only the first maxLineSize bytes can hold the line.
  const bool isBounded = std::cmp_greater_equal(pLast - pFirst, maxLineSize);
  const char* const pEnd = isBounded ? pFirst + maxLineSize : pLast;
  // All the bytes the line can have are valid, but do not end it.
  const auto unterminated = [isBounded, &rejected] {
    return isBounded ? rejected(StatusCodeBadRequest, "Chunk size line too long")
                     : ChunkSizeLine{.state = ChunkSizeLine::State::Incomplete};
  };
  // Plain loops rather than std::find_if_not, whose unrolling costs more than it saves for the usual 0 or 1 blank.
  const auto skipBlanks = [pEnd](const char* pChar) {
    while (pChar != pEnd && (*pChar == ' ' || *pChar == '\t')) {
      ++pChar;
    }
    return pChar;
  };

  const char* pCur = pFirst;
  if (syntax == ChunkSizeSyntax::Lenient) {
    pCur = skipBlanks(pCur);
  }
  const char* const pSizeBeg = pCur;
  std::size_t chunkSize = 0;
  for (; pCur != pEnd; ++pCur) {
    // The position of the next chunk depends on this size: a table lookup here would delay it.
    const int8_t digit = FromHexDigitLowLatency(*pCur);
    if (digit < 0) {
      break;
    }
    // Checked before shifting, so that the chunk size never wraps around (it could become 0, ending the body early).
    if (chunkSize > (maxChunkSize >> 4U)) {
      return rejected(StatusCodePayloadTooLarge, "Chunk size too large");
    }
    chunkSize = (chunkSize << 4U) | static_cast<std::size_t>(digit);
    if (chunkSize > maxChunkSize) {
      return rejected(StatusCodePayloadTooLarge, "Chunk size too large");
    }
  }
  if (pCur == pEnd) {
    return unterminated();
  }
  // An empty chunk-size is not a last-chunk.
  if (pCur == pSizeBeg) {
    return rejected(StatusCodeBadRequest, "Invalid chunk size");
  }

  // The CRLF usually follows the size: whitespace and extensions are only looked for when it does not.
  if (*pCur != '\r') {
    const char* const pSizeEnd = pCur;
    pCur = skipBlanks(pCur);
    if (pCur == pEnd) {
      return unterminated();
    }
    if (*pCur == ';') {
      // Chunk extensions are ignored, but must not contain control characters: a parser accepting a bare LF as chunk
      // line terminator would otherwise not agree with us on where the chunk data starts. Bytes above 0x7F are
      // allowed, as in a quoted-string (RFC 9112 §7.1.1).
      const char* const pExtEnd = std::find(pCur, pEnd, '\r');
      if (!IsValidReceivedHeaderValue(std::string_view(pCur, pExtEnd))) {
        return rejected(StatusCodeBadRequest, "Invalid chunk extension");
      }
      pCur = pExtEnd;
      if (pCur == pEnd) {
        return unterminated();
      }
    } else if (pCur != pSizeEnd && syntax == ChunkSizeSyntax::Strict) {
      return rejected(StatusCodeBadRequest, "Invalid chunk size");
    }
    if (*pCur != '\r') {
      return rejected(StatusCodeBadRequest, *pCur == '\n' ? "Malformed chunk size line" : "Invalid chunk size");
    }
  }
  if (++pCur == pEnd) {
    return unterminated();
  }
  if (*pCur != '\n') {
    return rejected(StatusCodeBadRequest, "Malformed chunk size line");
  }
  return {.chunkSize = chunkSize, .size = static_cast<std::size_t>(pCur + 1 - pFirst)};
}

}  // namespace aeronet::http
