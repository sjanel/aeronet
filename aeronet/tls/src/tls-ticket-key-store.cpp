#include "aeronet/tls-ticket-key-store.hpp"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <stdexcept>

#include "aeronet/log.hpp"
#include "aeronet/tls-config.hpp"

namespace aeronet {

TlsTicketKeyStore::TlsTicketKeyStore(std::chrono::seconds lifetime, std::uint32_t maxKeys)
    : _lifetime(lifetime), _maxKeys(std::max(1U, maxKeys)) {}

void TlsTicketKeyStore::loadStaticKeys(std::span<const TLSConfig::SessionTicketKey> keys) {
  std::scoped_lock<std::mutex> lock(_mutex);
  _keys.clear();
  _autoRotate = keys.empty();
  auto now = std::chrono::steady_clock::now();
  for (const auto& raw : keys) {
    KeyMaterial& mat = _keys.emplace_back();
    const auto matData = mat.data();
    const auto* ptr = reinterpret_cast<const unsigned char*>(raw.data());
    std::memcpy(matData.data(), ptr, matData.size());
    mat.created = now;

    if (_keys.size() == _maxKeys) {
      log::warn("Ignoring excess {} TLS session ticket keys beyond configured maxKeys={}", _keys.size() - _maxKeys,
                _maxKeys);
      break;
    }
  }
  if (_autoRotate) {
    _keys.emplace_back(GenerateRandomKeyUnlocked());
  }
}

namespace {
bool InitMacContext(EVP_MAC_CTX* mctx, const unsigned char* hmacKey, std::size_t hmacKeyLen) {
  static const OSSL_PARAM params[]{
      ::OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, const_cast<char*>("SHA256"), 0),
      ::OSSL_PARAM_construct_end()};
  if (::EVP_MAC_CTX_set_params(mctx, params) != 1) {
    return false;
  }
  return ::EVP_MAC_init(mctx, hmacKey, hmacKeyLen, nullptr) == 1;
}

}  // namespace

int TlsTicketKeyStore::processTicket(unsigned char keyName[16], unsigned char* iv, int ivLen, EVP_CIPHER_CTX* cctx,
                                     EVP_MAC_CTX* mctx, int enc) {
  std::scoped_lock<std::mutex> lock(_mutex);
  if (enc == 1) {
    rotateIfNeededUnlocked();
    if (_keys.empty()) {
      _keys.emplace_back(GenerateRandomKeyUnlocked());
    }
    auto& key = _keys.front();
    if (::RAND_bytes(iv, ivLen) != 1) {
      return -1;
    }
    std::memcpy(keyName, key.name.data(), key.name.size());
    if (::EVP_EncryptInit_ex(cctx, ::EVP_aes_128_cbc(), nullptr, key.aesKey.data(), iv) != 1) {
      return -1;
    }
    if (!InitMacContext(mctx, key.hmacKey.data(), key.hmacKey.size())) {
      return -1;
    }
    return 1;
  }

  const KeyMaterial* key = findKeyUnlocked(keyName);
  if (key == nullptr) {
    return 0;  // trigger full handshake
  }
  if (::EVP_DecryptInit_ex(cctx, ::EVP_aes_128_cbc(), nullptr, key->aesKey.data(), iv) != 1) {
    return -1;
  }
  if (!InitMacContext(mctx, key->hmacKey.data(), key->hmacKey.size())) {
    return -1;
  }
  return 1;
}

TlsTicketKeyStore::KeyMaterial TlsTicketKeyStore::GenerateRandomKeyUnlocked() {
  KeyMaterial key;
  auto data = key.data();
  if (::RAND_bytes(data.data(), static_cast<int>(data.size())) != 1) {
    throw std::runtime_error("RAND_bytes failed generating ticket");
  }
  key.created = std::chrono::steady_clock::now();
  return key;
}

void TlsTicketKeyStore::rotateIfNeededUnlocked() {
  if (_autoRotate && _lifetime.count() > 0) {
    if (_keys.empty() || std::chrono::steady_clock::now() - _keys.front().created >= _lifetime) {
      _keys.emplace(_keys.begin(), GenerateRandomKeyUnlocked());
      if (_keys.size() > _maxKeys) {
        _keys.pop_back();
      }
    }
  }
}

const TlsTicketKeyStore::KeyMaterial* TlsTicketKeyStore::findKeyUnlocked(const unsigned char keyName[16]) const {
  for (const auto& mat : _keys) {
    if (std::memcmp(mat.name.data(), keyName, mat.name.size()) == 0) {
      return &mat;
    }
  }
  return nullptr;
}

}  // namespace aeronet
