#include "url-decode.hpp"

#include "char-hexadecimal-converter.hpp"
#include "raw-chars.hpp"

namespace aeronet {

bool URLDecodeInPlace(RawChars &str, bool plusAsSpace) {
  RawChars::size_type readPos = 0;
  RawChars::size_type writePos = 0;
  const auto length = str.size();
  while (readPos < length) {
    char ch = str[readPos++];
    if (ch == '+') {
      if (plusAsSpace) {
        str[writePos++] = ' ';
      } else {
        str[writePos++] = '+';
      }
    } else if (ch == '%') {
      if (readPos + 1 >= length) {
        return false;  // truncated
      }
      char h1 = str[readPos++];
      char h2 = str[readPos++];
      int v1 = from_hex_digit(h1);
      int v2 = from_hex_digit(h2);
      if (v1 < 0 || v2 < 0) {
        return false;  // invalid
      }
      str[writePos++] = static_cast<char>((v1 << 4) | v2);
    } else {
      str[writePos++] = ch;
    }
  }
  str.resize_down(writePos);
  return true;
}

}  // namespace aeronet
