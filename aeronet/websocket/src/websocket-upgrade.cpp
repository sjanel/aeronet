#include "aeronet/websocket-upgrade.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <string_view>

#include "aeronet/base64.hpp"
#include "aeronet/websocket-constants.hpp"
#include "sha1.hpp"

namespace aeronet {

namespace {

[[nodiscard]] constexpr bool IsBase64Symbol(char ch) noexcept {
  // Tells whether a byte is one of the 64 symbols of the base64 alphabet ('=' padding excluded).
  static constexpr auto kIsBase64SymbolTable = [] {
    // One entry per byte value: header values may hold bytes 0x80-0xFF (obs-text, RFC 9110 §5.5).
    std::array<bool, std::numeric_limits<unsigned char>::max() + 1> table{};
    for (char ch : kB64Table) {
      table[static_cast<unsigned char>(ch)] = true;
    }
    return table;
  }();

  return kIsBase64SymbolTable[static_cast<unsigned char>(ch)];
}
}  // namespace

bool IsValidWebSocketKey(std::string_view key) {
  // A base64-encoded 16-byte value (RFC 6455 §4.2.1): 22 symbols holding the 128 bits, then 2 '=' padding characters
  // (RFC 4648 §4). '=' anywhere else cannot be decoded.
  static constexpr std::size_t kNbSymbols = 22;
  return key.size() == kNbSymbols + 2 && key[kNbSymbols] == '=' && key[kNbSymbols + 1] == '=' &&
         std::ranges::all_of(key.substr(0, kNbSymbols), IsBase64Symbol);
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

  B64Encode(std::string_view(hash.data(), hash.size()), ret.data(), ret.data() + ret.size());

  return ret;
}

}  // namespace aeronet