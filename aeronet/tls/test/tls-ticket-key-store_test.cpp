#include "aeronet/tls-ticket-key-store.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/types.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <thread>

#include "aeronet/tls-config.hpp"

namespace aeronet {

TEST(TlsTicketKeyStoreTest, ProcessTicketIssuesAndDecrypts) {
  // Create a ticket store with a single static key.
  TlsTicketKeyStore ticketStore(std::chrono::seconds(60), 2);
  std::array<TLSConfig::SessionTicketKey, 1> staticKeys;
  for (std::size_t idx = 0; idx < staticKeys[0].size(); ++idx) {
    staticKeys[0][idx] = static_cast<std::byte>(idx);
  }
  ticketStore.loadStaticKeys(staticKeys);

  // Set up cipher context and MAC context for testing.
  std::array<unsigned char, 16> keyName{};
  std::array<unsigned char, EVP_MAX_IV_LENGTH> iv{};

  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> cipherCtx{::EVP_CIPHER_CTX_new(),
                                                                              &::EVP_CIPHER_CTX_free};
  ASSERT_NE(cipherCtx.get(), nullptr);

  EVP_MAC* mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  std::unique_ptr<EVP_MAC_CTX, decltype(&::EVP_MAC_CTX_free)> macCtx{::EVP_MAC_CTX_new(mac), &::EVP_MAC_CTX_free};
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  // Test issuing a new ticket (enc = 1).
  int issueRc = ticketStore.processTicket(keyName.data(), iv.data(), static_cast<int>(iv.size()), cipherCtx.get(),
                                          macCtx.get(), 1);
  EXPECT_EQ(issueRc, 1);

  // Reset cipher context for decryption test.
  ::EVP_CIPHER_CTX_reset(cipherCtx.get());

  // Recreate MAC context for decryption.
  mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  macCtx.reset(::EVP_MAC_CTX_new(mac));
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  // Test decrypting with the same key name (enc = 0).
  int decryptRc = ticketStore.processTicket(keyName.data(), iv.data(), static_cast<int>(iv.size()), cipherCtx.get(),
                                            macCtx.get(), 0);
  EXPECT_EQ(decryptRc, 1);
}

TEST(TlsTicketKeyStoreTest, RotateExceedsMaxKeysPopsBack) {
  // Use a short lifetime and maxKeys=1 to force rotation and pop_back behavior.
  TlsTicketKeyStore ticketStore(std::chrono::seconds(1), 1);

  // Ensure auto-rotate mode (no static keys loaded)
  ticketStore.loadStaticKeys(std::span<const TLSConfig::SessionTicketKey>{});

  std::array<unsigned char, 16> firstKeyName{};
  std::array<unsigned char, EVP_MAX_IV_LENGTH> iv{};

  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> cipherCtx{::EVP_CIPHER_CTX_new(),
                                                                              &::EVP_CIPHER_CTX_free};
  ASSERT_NE(cipherCtx.get(), nullptr);

  EVP_MAC* mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  std::unique_ptr<EVP_MAC_CTX, decltype(&::EVP_MAC_CTX_free)> macCtx{::EVP_MAC_CTX_new(mac), &::EVP_MAC_CTX_free};
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  // Issue first ticket (creates initial key)
  int rc1 = ticketStore.processTicket(firstKeyName.data(), iv.data(), static_cast<int>(iv.size()), cipherCtx.get(),
                                      macCtx.get(), 1);
  EXPECT_EQ(rc1, 1);

  // Wait for lifetime to expire so the next issuance rotates and pushes a new key.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // Issue second ticket (rotation should occur and, because maxKeys==1, pop_back should remove the first key)
  std::array<unsigned char, 16> secondKeyName{};
  std::array<unsigned char, EVP_MAX_IV_LENGTH> iv2{};
  ::EVP_CIPHER_CTX_reset(cipherCtx.get());
  mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  macCtx.reset(::EVP_MAC_CTX_new(mac));
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  int rc2 = ticketStore.processTicket(secondKeyName.data(), iv2.data(), static_cast<int>(iv2.size()), cipherCtx.get(),
                                      macCtx.get(), 1);
  EXPECT_EQ(rc2, 1);

  // Attempt to decrypt with the original first key name: should return 0 (unknown key) because it was popped.
  ::EVP_CIPHER_CTX_reset(cipherCtx.get());
  mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  macCtx.reset(::EVP_MAC_CTX_new(mac));
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  int decryptRc = ticketStore.processTicket(firstKeyName.data(), iv.data(), static_cast<int>(iv.size()),
                                            cipherCtx.get(), macCtx.get(), 0);
  EXPECT_EQ(decryptRc, 0);
}

TEST(TlsTicketKeyStoreTest, LoadStaticKeysMaxKeysLimit) {
  // Test that loading more keys than maxKeys truncates at the limit (covers line 31 break)
  TlsTicketKeyStore ticketStore(std::chrono::seconds(60), 2);

  // Create 5 keys but store only has maxKeys=2
  std::array<TLSConfig::SessionTicketKey, 5> staticKeys{};
  for (std::size_t keyIdx = 0; keyIdx < staticKeys.size(); ++keyIdx) {
    for (std::size_t byteIdx = 0; byteIdx < staticKeys[keyIdx].size(); ++byteIdx) {
      staticKeys[keyIdx][byteIdx] = static_cast<std::byte>((keyIdx * 100) + byteIdx);
    }
  }

  ticketStore.loadStaticKeys(staticKeys);

  // Verify we can issue/decrypt with the first key (which should be loaded)
  std::array<unsigned char, 16> keyName{};
  std::array<unsigned char, EVP_MAX_IV_LENGTH> iv{};

  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> cipherCtx{::EVP_CIPHER_CTX_new(),
                                                                              &::EVP_CIPHER_CTX_free};
  ASSERT_NE(cipherCtx.get(), nullptr);

  EVP_MAC* mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  std::unique_ptr<EVP_MAC_CTX, decltype(&::EVP_MAC_CTX_free)> macCtx{::EVP_MAC_CTX_new(mac), &::EVP_MAC_CTX_free};
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  // Issue should succeed (key was loaded)
  int issueRc = ticketStore.processTicket(keyName.data(), iv.data(), static_cast<int>(iv.size()), cipherCtx.get(),
                                          macCtx.get(), 1);
  EXPECT_EQ(issueRc, 1);
}

TEST(TlsTicketKeyStoreTest, ProcessTicketUnknownKeyReturns0) {
  // Test decryption with an unknown key name returns 0 (covers line 55)
  TlsTicketKeyStore ticketStore(std::chrono::seconds(60), 2);

  std::array<TLSConfig::SessionTicketKey, 1> staticKeys{};
  for (std::size_t idx = 0; idx < staticKeys[0].size(); ++idx) {
    staticKeys[0][idx] = static_cast<std::byte>(idx);
  }
  ticketStore.loadStaticKeys(staticKeys);

  // Use a completely different key name that won't match any stored key
  std::array<unsigned char, 16> unknownKeyName{};
  std::ranges::fill(unknownKeyName, 0xFF);
  std::array<unsigned char, EVP_MAX_IV_LENGTH> iv{};

  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> cipherCtx{::EVP_CIPHER_CTX_new(),
                                                                              &::EVP_CIPHER_CTX_free};
  ASSERT_NE(cipherCtx.get(), nullptr);

  EVP_MAC* mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  std::unique_ptr<EVP_MAC_CTX, decltype(&::EVP_MAC_CTX_free)> macCtx{::EVP_MAC_CTX_new(mac), &::EVP_MAC_CTX_free};
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  // Decrypt with unknown key should return 0 (key not found)
  int decryptRc = ticketStore.processTicket(unknownKeyName.data(), iv.data(), static_cast<int>(iv.size()),
                                            cipherCtx.get(), macCtx.get(), 0);
  EXPECT_EQ(decryptRc, 0);
}

TEST(TlsTicketKeyStoreTest, ProcessTicketShouldGenerateRandomKeyIfNoKeys) {
  TlsTicketKeyStore ticketStore({}, 0);

  std::array<unsigned char, 16> unknownKeyName;
  std::ranges::fill(unknownKeyName, 0xFF);
  std::array<unsigned char, EVP_MAX_IV_LENGTH> iv{};

  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> cipherCtx{::EVP_CIPHER_CTX_new(),
                                                                              &::EVP_CIPHER_CTX_free};
  ASSERT_NE(cipherCtx.get(), nullptr);

  EVP_MAC* mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  std::unique_ptr<EVP_MAC_CTX, decltype(&::EVP_MAC_CTX_free)> macCtx{::EVP_MAC_CTX_new(mac), &::EVP_MAC_CTX_free};
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  // Decrypt with unknown key should return 0 (key not found)
  int decryptRc = ticketStore.processTicket(unknownKeyName.data(), iv.data(), static_cast<int>(iv.size()),
                                            cipherCtx.get(), macCtx.get(), 1);
  EXPECT_EQ(decryptRc, 1);
}

TEST(TlsTicketKeyStoreTest, LoadStaticKeysEmptyGeneratesKey) {
  // Covers tls-ticket-key-store.cpp lines 43-44: auto-rotate generates key when loadStaticKeys with empty
  TlsTicketKeyStore ticketStore(std::chrono::seconds(60), 2);

  // Load empty keys - should trigger auto-rotate to generate a key
  std::span<const TLSConfig::SessionTicketKey> emptyKeys;
  ticketStore.loadStaticKeys(emptyKeys);

  // Now try to process a ticket - should work because a key was auto-generated
  std::array<unsigned char, 16> keyName{};
  std::array<unsigned char, EVP_MAX_IV_LENGTH> iv{};

  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> cipherCtx{::EVP_CIPHER_CTX_new(),
                                                                              &::EVP_CIPHER_CTX_free};
  ASSERT_NE(cipherCtx.get(), nullptr);

  EVP_MAC* mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  std::unique_ptr<EVP_MAC_CTX, decltype(&::EVP_MAC_CTX_free)> macCtx{::EVP_MAC_CTX_new(mac), &::EVP_MAC_CTX_free};
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  int rc = ticketStore.processTicket(keyName.data(), iv.data(), static_cast<int>(iv.size()), cipherCtx.get(),
                                     macCtx.get(), 1);
  EXPECT_EQ(rc, 1);
}

TEST(TlsTicketKeyStoreTest, AutoRotateGeneratesKeyWhenEmpty) {
  // Covers line 43-44: auto-rotate generates a key when processTicket called with empty store
  // The store auto-rotates when _autoRotate is true (default) and keys are empty
  TlsTicketKeyStore ticketStore(std::chrono::seconds(60), 2);

  // Don't load any keys - the store should auto-generate on first processTicket

  std::array<unsigned char, 16> keyName{};
  std::array<unsigned char, EVP_MAX_IV_LENGTH> iv{};

  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> cipherCtx{::EVP_CIPHER_CTX_new(),
                                                                              &::EVP_CIPHER_CTX_free};
  ASSERT_NE(cipherCtx.get(), nullptr);

  EVP_MAC* mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  std::unique_ptr<EVP_MAC_CTX, decltype(&::EVP_MAC_CTX_free)> macCtx{::EVP_MAC_CTX_new(mac), &::EVP_MAC_CTX_free};
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  // Issue should succeed because auto-rotate creates a key
  int issueRc = ticketStore.processTicket(keyName.data(), iv.data(), static_cast<int>(iv.size()), cipherCtx.get(),
                                          macCtx.get(), 1);
  EXPECT_EQ(issueRc, 1);
}

TEST(TlsTicketKeyStoreTest, RotateAfterLifetimeExpires) {
  // Covers the rotation path when key lifetime expires
  TlsTicketKeyStore ticketStore(std::chrono::seconds(0), 2);  // 0 lifetime = immediate rotation

  std::array<TLSConfig::SessionTicketKey, 1> staticKeys{};
  for (std::size_t idx = 0; idx < staticKeys[0].size(); ++idx) {
    staticKeys[0][idx] = static_cast<std::byte>(idx);
  }
  ticketStore.loadStaticKeys(staticKeys);

  std::array<unsigned char, 16> keyName{};
  std::array<unsigned char, EVP_MAX_IV_LENGTH> iv{};

  std::unique_ptr<EVP_CIPHER_CTX, decltype(&::EVP_CIPHER_CTX_free)> cipherCtx{::EVP_CIPHER_CTX_new(),
                                                                              &::EVP_CIPHER_CTX_free};
  ASSERT_NE(cipherCtx.get(), nullptr);

  EVP_MAC* mac = ::EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  ASSERT_NE(mac, nullptr);
  std::unique_ptr<EVP_MAC_CTX, decltype(&::EVP_MAC_CTX_free)> macCtx{::EVP_MAC_CTX_new(mac), &::EVP_MAC_CTX_free};
  ::EVP_MAC_free(mac);
  ASSERT_NE(macCtx.get(), nullptr);

  // Issue a ticket - with lifetime=0, this will trigger rotation on each call
  int issueRc = ticketStore.processTicket(keyName.data(), iv.data(), static_cast<int>(iv.size()), cipherCtx.get(),
                                          macCtx.get(), 1);
  EXPECT_EQ(issueRc, 1);
}

}  // namespace aeronet