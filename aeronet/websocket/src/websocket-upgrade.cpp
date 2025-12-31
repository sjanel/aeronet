#include "aeronet/websocket-upgrade.hpp"

#include <algorithm>
#include <string_view>

#include "aeronet/base64-encode.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sha1.hpp"
#include "aeronet/websocket-constants.hpp"

namespace aeronet {

namespace {
// Check if a character is valid base64
[[nodiscard]] constexpr bool IsBase64Char(char ch) noexcept {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/' ||
         ch == '=';
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
  // Concatenate key with WebSocket GUID
  RawChars concat(key.size() + websocket::kGUID.size());
  concat.unchecked_append(key);
  concat.unchecked_append(websocket::kGUID);

  // Compute SHA-1 hash
  SHA1 sha1Ctx;
  sha1Ctx.update(concat.data(), concat.size());
  const Sha1Digest hash = sha1Ctx.final();

  static constexpr auto b64EncodedSz = B64EncodedLen(sizeof(hash));

  static_assert(b64EncodedSz == B64EncodedSha1{}.size(), "Unexpected B64EncodedSha1 size");

  B64EncodedSha1 ret;

  B64Encode(hash, ret.data(), ret.data() + ret.size());

  return ret;
}

}  // namespace aeronet