#include "aeronet/url-decode.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "aeronet/char-hexadecimal-converter.hpp"

namespace aeronet::url {

namespace {

// Percent-encoded sequences that are invalid, or that decode to a byte for which keepEncoded() returns true, make the
// decoding fail if strictInvalid, or are kept as is otherwise.
char* DecodeInPlaceImpl(char* first, const char* last, char plusAs, bool strictInvalid, auto keepEncoded) {
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
          *out++ = '%';
          break;  // keep '%' literal and exit (drop trailing)? Best effort: just literal '%'
        }
        const char c1 = *++first;
        const char c2 = *++first;
        const int8_t v1 = from_hex_digit(c1);
        const int8_t v2 = from_hex_digit(c2);
        // only meaningful if both digits are valid
        const char decoded =
            static_cast<char>(static_cast<uint8_t>(static_cast<uint8_t>(v1) << 4U) | static_cast<uint8_t>(v2));
        if (v1 < 0 || v2 < 0 || keepEncoded(decoded)) {
          if (strictInvalid) {
            return nullptr;
          }
          *out++ = '%';
          *out++ = c1;
          *out++ = c2;
          break;
        }
        *out++ = decoded;
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
  return DecodeInPlaceImpl(first, last, plusAs, strictInvalid, [](char) { return false; });
}

char* DecodeQueryParamsInPlace(char* first, char* last) {
  // The decoded key / value pairs are delimited by kNewKeyValueSep and kNewPairSep: these bytes must not be produced by
  // the decoding, otherwise "a=1%00b%1F2" would be read as the two parameters a=1 and b=2.
  const auto isSeparator = [](char ch) { return ch == kNewKeyValueSep || ch == kNewPairSep; };
  while (first < last) {
    // Find '=' and '&' within [first, last)
    char* keyEnd = static_cast<char*>(std::memchr(first, '=', static_cast<std::size_t>(last - first)));
    char* pairEnd = static_cast<char*>(std::memchr(first, '&', static_cast<std::size_t>(last - first)));

    if (pairEnd == nullptr) {
      pairEnd = last;  // last pair
    } else {
      *pairEnd = kNewPairSep;
    }

    if (keyEnd == nullptr || keyEnd > pairEnd) {
      // no '=' in this pair → key only
      keyEnd = pairEnd;
    } else {
      *keyEnd = kNewKeyValueSep;
    }

    char* keyBegin = first;
    char* valueBegin = (keyEnd < pairEnd) ? keyEnd + 1 : pairEnd;
    char* valueEnd = pairEnd;

    // --- Decode key ---
    {
      char* newEnd = DecodeInPlaceImpl(keyBegin, keyEnd, '+', /*strictInvalid*/ false, isSeparator);
      const auto decodedLen = static_cast<std::size_t>(newEnd - keyBegin);
      const auto origLen = static_cast<std::size_t>(keyEnd - keyBegin);
      if (decodedLen < origLen) {
        const auto shift = origLen - decodedLen;
        std::memmove(newEnd, keyEnd, static_cast<std::size_t>(last - keyEnd));
        last -= shift;
        // adjust valueBegin/valueEnd/pairEnd
        valueBegin -= shift;
        valueEnd -= shift;
        pairEnd -= shift;
      }
      keyEnd = newEnd;
    }

    // --- Decode value (if any) ---
    if (valueBegin < valueEnd) {
      char* newEnd = DecodeInPlaceImpl(valueBegin, valueEnd, /*plusAs*/ ' ', /*strictInvalid*/ false, isSeparator);
      const auto decodedLen = static_cast<std::size_t>(newEnd - valueBegin);
      const auto origLen = static_cast<std::size_t>(valueEnd - valueBegin);
      if (decodedLen < origLen) {
        const auto shift = origLen - decodedLen;
        std::memmove(newEnd, valueEnd, static_cast<std::size_t>(last - valueEnd));
        last -= shift;
        pairEnd -= shift;
      }
      valueEnd = newEnd;
    }

    // Advance to next pair
    first = (pairEnd < last) ? pairEnd + 1 : last;
  }

  return last;
}

}  // namespace aeronet::url
