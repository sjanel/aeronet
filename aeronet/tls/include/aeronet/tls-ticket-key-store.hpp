#pragma once

#include <openssl/types.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>

#include "aeronet/tls-config.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

// Thread-safe store of TLS session ticket keys shared across SingleHttpServer instances (and
// across MultiHttpServer workers). Provides automatic rotation for randomly generated keys
// and supports injecting deterministic key material from configuration.
class TlsTicketKeyStore {
 public:
  TlsTicketKeyStore(std::chrono::seconds lifetime, std::uint32_t maxKeys);

  // Replace internal key material with the provided static keys (first entry is primary).
  // Supplying at least one key automatically enables session tickets even if auto-rotation
  // is disabled. Keys are truncated to the configured maxKeys.
  void loadStaticKeys(std::span<const TLSConfig::SessionTicketKey> keys);

  // Entry point used by the OpenSSL session ticket callback (EVP variant for OpenSSL 3.0+).
  // When enc==1 a new ticket is issued (populate keyName/iv and initialize cipher/MAC contexts).
  // When enc==0 the ticket must be decrypted using the matching keyName.
  int processTicket(unsigned char keyName[16], unsigned char* iv, int ivLen, EVP_CIPHER_CTX* cctx, EVP_MAC_CTX* mctx,
                    int enc);

 private:
  struct KeyMaterial {
    static constexpr std::size_t kPartSize = 16;
    using Bytes = std::array<unsigned char, TLSConfig::kSessionTicketKeySize>;

    KeyMaterial() = default;
    KeyMaterial(const KeyMaterial&) = delete;
    KeyMaterial& operator=(const KeyMaterial&) = delete;
    KeyMaterial(KeyMaterial&& other) noexcept;
    KeyMaterial& operator=(KeyMaterial&& other) noexcept;
    ~KeyMaterial();

    [[nodiscard]] auto data() noexcept { return std::span<unsigned char, TLSConfig::kSessionTicketKeySize>{bytes}; }
    [[nodiscard]] auto name() noexcept { return std::span<unsigned char, kPartSize>{bytes.data(), kPartSize}; }
    [[nodiscard]] auto name() const noexcept {
      return std::span<const unsigned char, kPartSize>{bytes.data(), kPartSize};
    }
    [[nodiscard]] auto hmacKey() noexcept {
      return std::span<unsigned char, kPartSize>{bytes.data() + kPartSize, kPartSize};
    }
    [[nodiscard]] auto hmacKey() const noexcept {
      return std::span<const unsigned char, kPartSize>{bytes.data() + kPartSize, kPartSize};
    }
    [[nodiscard]] auto aesKey() noexcept {
      return std::span<unsigned char, kPartSize>{bytes.data() + 2U * kPartSize, kPartSize};
    }
    [[nodiscard]] auto aesKey() const noexcept {
      return std::span<const unsigned char, kPartSize>{bytes.data() + 2U * kPartSize, kPartSize};
    }

    void scrub() noexcept;

    static_assert(TLSConfig::kSessionTicketKeySize == 3U * kPartSize, "Session ticket key size mismatch");
    Bytes bytes;
    std::chrono::steady_clock::time_point created;
  };

  static KeyMaterial GenerateRandomKeyUnlocked();
  void rotateIfNeededUnlocked();
  const KeyMaterial* findKeyUnlocked(const unsigned char keyName[16]) const;

  std::chrono::seconds _lifetime;
  std::uint32_t _maxKeys;
  bool _autoRotate{true};
  mutable std::mutex _mutex;
  vector<KeyMaterial> _keys;
};

}  // namespace aeronet
