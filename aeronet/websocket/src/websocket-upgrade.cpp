#include "aeronet/websocket-upgrade.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

#include "aeronet/base64-encode.hpp"
#include "aeronet/sha1.hpp"
#include "aeronet/websocket-constants.hpp"

namespace aeronet {

namespace {
constexpr auto kIsBase64CharTable = []() {
  std::array<bool, std::numeric_limits<char>::max()> table{};
  for (unsigned char ch = 'A'; ch <= 'Z'; ++ch) {
    table[ch] = true;
  }
  for (unsigned char ch = 'a'; ch <= 'z'; ++ch) {
    table[ch] = true;
  }
  for (unsigned char ch = '0'; ch <= '9'; ++ch) {
    table[ch] = true;
  }
  table[static_cast<unsigned char>('+')] = true;
  table[static_cast<unsigned char>('/')] = true;
  table[static_cast<unsigned char>('=')] = true;
  return table;
}();

// Check if a character is valid base64
[[nodiscard]] constexpr bool IsBase64Char(char ch) noexcept {
  return kIsBase64CharTable[static_cast<unsigned char>(ch)];
}
}  // namespace

bool IsValidWebSocketKey(std::string_view key) {
  // Must be exactly 24 base64 characters (16 bytes -> 24 chars with padding)
  if (key.size() != 24) {
    return false;
  }

  // Verify all characters are valid base64
  if (!std::ranges::all_of(key, [](char ch) { return IsBase64Char(ch); })) {
    return false;
  }

  // The key should end with "==" as 16 bytes encodes to 22 chars + 2 padding
  return key[22] == '=' && key[23] == '=';
}

B64EncodedSha1 ComputeWebSocketAccept(std::string_view key) {
  // Compute SHA-1 hash
  SHA1 sha1Ctx;
  sha1Ctx.update(key.data(), key.size());
  sha1Ctx.update(websocket::kGUID.data(), websocket::kGUID.size());
  const Sha1Digest hash = sha1Ctx.final();

  static constexpr auto b64EncodedSz = B64EncodedLen(sizeof(hash));

  static_assert(b64EncodedSz == B64EncodedSha1{}.size(), "Unexpected B64EncodedSha1 size");

  B64EncodedSha1 ret;

  B64Encode(hash, ret.data(), ret.data() + ret.size());

  return ret;
}

}  // namespace aeronet