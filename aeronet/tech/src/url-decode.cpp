#include "aeronet/url-decode.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "aeronet/char-hexadecimal-converter.hpp"

namespace aeronet::url {

namespace {

// Percent-encoded sequences that are invalid, or that decode to a byte listed in keepEncoded, make the decoding fail
// if strictInvalid, or are kept as is otherwise.
char* DecodeInPlaceImpl(char* first, const char* last, char plusAs, bool strictInvalid, int keep1, int keep2) {
  char* out = first;
  for (; first < last; ++first) {
    const char ch = *first;
    switch (ch) {
      case '+':
        *out++ = plusAs;
        break;
      case '%': {
        if (first + 2 >= last) {
          if (strictInvalid) {
            return nullptr;
          }
          *out++ = '%';  // truncated sequence: kept verbatim, the following chars (if any) are copied as is
          break;
        }
        const char c1 = *++first;
        const char c2 = *++first;
        const int8_t v1 = FromHexDigit(c1);
        const int8_t v2 = FromHexDigit(c2);
        // only meaningful if both digits are valid
        const uint8_t decoded =
            static_cast<uint8_t>(static_cast<uint8_t>(static_cast<uint8_t>(v1) << 4U) | static_cast<uint8_t>(v2));
        if (v1 < 0 || v2 < 0 || std::cmp_equal(decoded, keep1) || std::cmp_equal(decoded, keep2)) {
          if (strictInvalid) {
            return nullptr;
          }
          *out++ = '%';
          *out++ = c1;
          *out++ = c2;
          break;
        }
        *out++ = static_cast<char>(decoded);
        break;
      }
      default:
        *out++ = ch;
        break;
    }
  }
  return out;
}

}  // namespace

char* DecodeInPlace(char* first, const char* last, char plusAs, bool strictInvalid) {
  // In strict mode, a decoded NUL makes the decoding fail: it would truncate the decoded string for C APIs. So does a
  // decoded '/': an encoded slash is data, not a path separator (RFC 3986 §2.2 and §6.2.2.2), and decoding it would
  // route "/a%2Fb" as "/a/b".
  return DecodeInPlaceImpl(first, last, plusAs, strictInvalid, strictInvalid ? 0 : -1, strictInvalid ? '/' : -1);
}

char* DecodeQueryParamsInPlace(char* first, char* last) {
  // The decoded key / value pairs are delimited by kNewKeyValueSep and kNewPairSep: these bytes must not be produced by
  // the decoding, otherwise "a=1%00b%1F2" would be read as the two parameters a=1 and b=2.
  static constexpr int kSep1 = static_cast<uint8_t>(kNewKeyValueSep);
  static constexpr int kSep2 = static_cast<uint8_t>(kNewPairSep);
  while (first < last) {
    // Find '=' and '&' within [first, last)
    char* pKeyEnd = static_cast<char*>(std::memchr(first, '=', static_cast<std::size_t>(last - first)));
    char* pPairEnd = static_cast<char*>(std::memchr(first, '&', static_cast<std::size_t>(last - first)));

    if (pPairEnd == nullptr) {
      pPairEnd = last;  // last pair
    } else {
      *pPairEnd = kNewPairSep;
    }

    if (pKeyEnd == nullptr || pKeyEnd > pPairEnd) {
      // no '=' in this pair → key only
      pKeyEnd = pPairEnd;
    } else {
      *pKeyEnd = kNewKeyValueSep;
    }

    char* pValueBegin = (pKeyEnd < pPairEnd) ? pKeyEnd + 1 : pPairEnd;
    char* pValueEnd = pPairEnd;

    // --- Decode key ---
    {
      char* pNewEnd = DecodeInPlaceImpl(first, pKeyEnd, '+', false, kSep1, kSep2);
      if (pNewEnd < pKeyEnd) {
        const std::ptrdiff_t shift = pKeyEnd - pNewEnd;
        std::memmove(pNewEnd, pKeyEnd, static_cast<std::size_t>(last - pKeyEnd));
        last -= shift;
        // adjust valueBegin/valueEnd/pairEnd
        pValueBegin -= shift;
        pValueEnd -= shift;
        pPairEnd -= shift;
      }
    }

    // --- Decode value (if any) ---
    if (pValueBegin < pValueEnd) {
      char* pNewEnd = DecodeInPlaceImpl(pValueBegin, pValueEnd, /*plusAs*/ ' ', /*strictInvalid*/ false, kSep1, kSep2);
      if (pNewEnd < pValueEnd) {
        const std::ptrdiff_t shift = pValueEnd - pNewEnd;
        std::memmove(pNewEnd, pValueEnd, static_cast<std::size_t>(last - pValueEnd));
        last -= shift;
        pPairEnd -= shift;
      }
    }

    // Advance to next pair
    first = (pPairEnd < last) ? pPairEnd + 1 : last;
  }

  return last;
}

}  // namespace aeronet::url
