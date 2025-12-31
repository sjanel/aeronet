#include "aeronet/sha1.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/sha.h>
#endif

namespace aeronet {
namespace {

// Helper: convert raw digest to hex string for display/comparison
std::string ToHex(const Sha1Digest& digest) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (char ch : digest) {
    oss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(ch));
  }
  return oss.str();
}

#ifdef AERONET_ENABLE_OPENSSL
// Compute SHA1 using OpenSSL as reference implementation
Sha1Digest OpenSSLSha1(std::string_view data) {
  Sha1Digest result;
  ::SHA1(reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         reinterpret_cast<unsigned char*>(result.data()));
  return result;
}

// Compare our implementation against OpenSSL
void ValidateAgainstOpenSSL(std::string_view data) {
  SHA1 hasher;
  hasher.update(data.data(), data.size());
  Sha1Digest ours = hasher.final();
  Sha1Digest ref = OpenSSLSha1(data);
  EXPECT_EQ(ours, ref) << "Mismatch for data: \"" << std::string(data.substr(0, 64)) << (data.size() > 64 ? "..." : "")
                       << "\" (size=" << data.size() << ")\n"
                       << "  ours:    " << ToHex(ours) << "\n"
                       << "  openssl: " << ToHex(ref);
}

void ValidateAgainstOpenSSLChunked(std::string_view data, std::size_t chunkSize) {
  SHA1 hasher;
  for (std::size_t off = 0; off < data.size(); off += chunkSize) {
    std::size_t len = std::min(chunkSize, data.size() - off);
    hasher.update(data.data() + off, len);
  }
  Sha1Digest ours = hasher.final();
  Sha1Digest ref = OpenSSLSha1(data);
  EXPECT_EQ(ours, ref) << "Mismatch (chunked " << chunkSize << ") for size=" << data.size() << "\n"
                       << "  ours:    " << ToHex(ours) << "\n"
                       << "  openssl: " << ToHex(ref);
}
#endif

// ============================================================================
// Known Test Vectors (from FIPS 180-1 / RFC 3174)
// ============================================================================

TEST(SHA1Test, EmptyString) {
  SHA1 hasher;
  hasher.update("", 0);
  Sha1Digest digest = hasher.final();
  // SHA1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709
  EXPECT_EQ(ToHex(digest), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL("");
#endif
}

TEST(SHA1Test, Abc) {
  SHA1 hasher;
  hasher.update("abc", sizeof("abc") - 1);
  Sha1Digest digest = hasher.final();
  // SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
  EXPECT_EQ(ToHex(digest), "a9993e364706816aba3e25717850c26c9cd0d89d");
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL("abc");
#endif
}

TEST(SHA1Test, MessageDigest) {
  SHA1 hasher;
  hasher.update("message digest", sizeof("message digest") - 1);
  Sha1Digest digest = hasher.final();
  // SHA1("message digest") = c12252ceda8be8994d5fa0290a47231c1d16aae3
  EXPECT_EQ(ToHex(digest), "c12252ceda8be8994d5fa0290a47231c1d16aae3");
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL("message digest");
#endif
}

TEST(SHA1Test, AlphabetLower) {
  SHA1 hasher;
  hasher.update("abcdefghijklmnopqrstuvwxyz", sizeof("abcdefghijklmnopqrstuvwxyz") - 1);
  Sha1Digest digest = hasher.final();
  // SHA1("abcdefghijklmnopqrstuvwxyz") = 32d10c7b8cf96570ca04ce37f2a19d84240d3a89
  EXPECT_EQ(ToHex(digest), "32d10c7b8cf96570ca04ce37f2a19d84240d3a89");
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL("abcdefghijklmnopqrstuvwxyz");
#endif
}

TEST(SHA1Test, AlphabetMixedWithDigits) {
  SHA1 hasher;
  hasher.update("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
                sizeof("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") - 1);
  Sha1Digest digest = hasher.final();
  // SHA1 of above = 761c457bf73b14d27e9e9265c46f4b4dda11f940
  EXPECT_EQ(ToHex(digest), "761c457bf73b14d27e9e9265c46f4b4dda11f940");
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
#endif
}

TEST(SHA1Test, LongRepeatedDigits) {
  // "1234567890" repeated 8 times = 80 chars
  std::string input;
  for (int ii = 0; ii < 8; ++ii) {
    input += "1234567890";
  }
  SHA1 hasher;
  hasher.update(input.data(), input.size());
  Sha1Digest digest = hasher.final();
  // SHA1 of "12345678901234567890123456789012345678901234567890123456789012345678901234567890"
  // = 50abf5706a150990a08b2c5ea40fa0e585554732
  EXPECT_EQ(ToHex(digest), "50abf5706a150990a08b2c5ea40fa0e585554732");
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL(input);
#endif
}

TEST(SHA1Test, FIPS180MillionAs) {
  // SHA1 of one million 'a' characters = 34aa973cd4c4daa4f61eeb2bdbad27316534016f
  std::string input(1000000, 'a');
  SHA1 hasher;
  hasher.update(input.data(), input.size());
  Sha1Digest digest = hasher.final();
  EXPECT_EQ(ToHex(digest), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL(input);
#endif
}

// ============================================================================
// Incremental Update Tests
// ============================================================================

TEST(SHA1Test, IncrementalUpdateSingleBytes) {
  std::string_view data = "The quick brown fox jumps over the lazy dog";
  // SHA1 = 2fd4e1c67a2d28fced849ee1bb76e7391b93eb12
  SHA1 hasher;
  for (char ch : data) {
    hasher.update(&ch, 1);
  }
  Sha1Digest digest = hasher.final();
  EXPECT_EQ(ToHex(digest), "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL(data);
#endif
}

TEST(SHA1Test, IncrementalUpdateVaryingChunks) {
  std::string_view data = "The quick brown fox jumps over the lazy dog";
  // Feed in various chunk sizes
  SHA1 hasher;
  hasher.update(data.begin(), 10);                    // "The quick "
  hasher.update(data.data() + 10, 6);                 // "brown "
  hasher.update(data.data() + 16, 20);                // "fox jumps over the "
  hasher.update(data.data() + 36, data.size() - 36);  // "lazy dog"
  Sha1Digest digest = hasher.final();
  EXPECT_EQ(ToHex(digest), "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST(SHA1Test, IncrementalUpdateExactBlockBoundary) {
  // 64-byte block boundary test
  std::string data64(64, 'X');
  SHA1 hasher;
  hasher.update(data64.data(), data64.size());  // Exactly one block
  hasher.update("Y", 1);                        // One more byte
  Sha1Digest digest = hasher.final();

#ifdef AERONET_ENABLE_OPENSSL
  Sha1Digest ref = OpenSSLSha1(data64 + "Y");
  EXPECT_EQ(digest, ref);
#else
  // Just verify it doesn't crash and produces consistent output
  SHA1 hasher2;
  hasher2.update(data64.data(), data64.size());  // Exactly one block
  hasher2.update("Y", 1);                        // One more byte
  EXPECT_EQ(digest, hasher2.final());
#endif
}

TEST(SHA1Test, IncrementalUpdateMultipleBlocks) {
  // 200 bytes = 3+ blocks
  std::string data(200, 'Z');
  SHA1 hasher;
  // Feed in chunks of 13 bytes
  for (std::size_t off = 0; off < data.size(); off += 13) {
    std::size_t len = std::min<std::size_t>(13, data.size() - off);
    hasher.update(data.data() + off, len);
  }
  Sha1Digest digest = hasher.final();

  SHA1 hasher2;
  hasher2.update(data.data(), data.size());
  EXPECT_EQ(digest, hasher2.final());
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSLChunked(data, 13);
#endif
}

// ============================================================================
// Reset and Reuse Tests
// ============================================================================

TEST(SHA1Test, ResetAndReuse) {
  SHA1 hasher;
  hasher.update("first message", sizeof("first message") - 1);
  Sha1Digest first = hasher.final();  // final() resets

  hasher.update("second message", sizeof("second message") - 1);
  Sha1Digest second = hasher.final();

  // Verify they're different
  EXPECT_NE(first, second);

  // Verify second is correct
  SHA1 fresh;
  fresh.update("second message", sizeof("second message") - 1);
  EXPECT_EQ(second, fresh.final());
}

TEST(SHA1Test, ExplicitReset) {
  SHA1 hasher;
  hasher.update("partial data", sizeof("partial data") - 1);
  hasher.reset();  // Discard partial state
  hasher.update("abc", sizeof("abc") - 1);
  Sha1Digest digest = hasher.final();
  EXPECT_EQ(ToHex(digest), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

// ============================================================================
// Binary Data Tests
// ============================================================================

TEST(SHA1Test, BinaryDataWithNulls) {
  // Data with embedded null bytes
  const char rawData[] = "\x00\x01\x02\x00\xff\xfe\x00\x00";
  std::string_view data(rawData, sizeof(rawData) - 1);  // 8 bytes

  SHA1 hasher;
  hasher.update(data.data(), data.size());
  Sha1Digest digest = hasher.final();

#ifdef AERONET_ENABLE_OPENSSL
  Sha1Digest ref = OpenSSLSha1(data);
  EXPECT_EQ(digest, ref);
#else
  // Consistency check
  SHA1 hasher2;
  hasher2.update(data.data(), data.size());
  EXPECT_EQ(digest, hasher2.final());
#endif
}

TEST(SHA1Test, AllByteValues) {
  // Test with all possible byte values 0-255
  std::string data;
  data.reserve(256);
  for (int ii = 0; ii < 256; ++ii) {
    data.push_back(static_cast<char>(ii));
  }

  SHA1 hasher;
  hasher.update(data.data(), data.size());
  Sha1Digest digest = hasher.final();

  SHA1 hasher2;
  hasher2.update(data.data(), data.size());
  EXPECT_EQ(digest, hasher2.final());
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL(data);
#endif
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST(SHA1Test, Exactly55Bytes) {
  // 55 bytes: exactly fills block with padding byte, length fits in same block
  std::string data(55, 'A');
  SHA1 hasher;
  hasher.update(data.data(), data.size());
  Sha1Digest digest = hasher.final();

  SHA1 hasher2;
  hasher2.update(data.data(), data.size());
  EXPECT_EQ(digest, hasher2.final());
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL(data);
#endif
}

TEST(SHA1Test, Exactly56Bytes) {
  // 56 bytes: needs extra block for padding
  std::string data(56, 'B');
  SHA1 hasher;
  hasher.update(data.data(), data.size());
  Sha1Digest digest = hasher.final();

  SHA1 hasher2;
  hasher2.update(data.data(), data.size());
  EXPECT_EQ(digest, hasher2.final());
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL(data);
#endif
}

TEST(SHA1Test, Exactly63Bytes) {
  // 63 bytes: one less than a full block
  std::string data(63, 'C');
  SHA1 hasher;
  hasher.update(data.data(), data.size());
  Sha1Digest digest = hasher.final();

  SHA1 hasher2;
  hasher2.update(data.data(), data.size());
  EXPECT_EQ(digest, hasher2.final());
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL(data);
#endif
}

TEST(SHA1Test, Exactly64Bytes) {
  // 64 bytes: exactly one full block
  std::string data(64, 'D');
  SHA1 hasher;
  hasher.update(data.data(), data.size());
  Sha1Digest digest = hasher.final();

  SHA1 hasher2;
  hasher2.update(data.data(), data.size());
  EXPECT_EQ(digest, hasher2.final());
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL(data);
#endif
}

TEST(SHA1Test, Exactly65Bytes) {
  // 65 bytes: one full block + 1
  std::string data(65, 'E');
  SHA1 hasher;
  hasher.update(data.data(), data.size());
  Sha1Digest digest = hasher.final();

  SHA1 hasher2;
  hasher2.update(data.data(), data.size());
  EXPECT_EQ(digest, hasher2.final());
#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL(data);
#endif
}

TEST(SHA1Test, EmptyUpdates) {
  SHA1 hasher;
  hasher.update("", 0);
  hasher.update("", 0);
  hasher.update("abc", sizeof("abc") - 1);
  hasher.update("", 0);
  Sha1Digest digest = hasher.final();
  EXPECT_EQ(ToHex(digest), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

// ============================================================================
// WebSocket Key Acceptance Test (Real Use Case)
// ============================================================================

TEST(SHA1Test, WebSocketAcceptKey) {
  // WebSocket handshake: SHA1(client_key + magic_guid)
  // This is the actual use case for this SHA1 implementation
  std::string_view clientKey = "dGhlIHNhbXBsZSBub25jZQ==";
  std::string_view magicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

  std::string combined;
  combined.reserve(clientKey.size() + magicGuid.size());
  combined += clientKey;
  combined += magicGuid;

  SHA1 hasher;
  hasher.update(combined.data(), combined.size());
  Sha1Digest digest = hasher.final();

  // Expected SHA1: b37a4f2cc0624f1690f64606cf385945b2bec4ea
  // (Base64 of this = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=" which is the Sec-WebSocket-Accept)
  EXPECT_EQ(ToHex(digest), "b37a4f2cc0624f1690f64606cf385945b2bec4ea");

#ifdef AERONET_ENABLE_OPENSSL
  ValidateAgainstOpenSSL(combined);
#endif
}

// ============================================================================
// Large Data Tests
// ============================================================================

#ifdef AERONET_ENABLE_OPENSSL
TEST(SHA1Test, LargeDataVaryingChunkSizes) {
  // Test with larger data and various chunk sizes
  std::string data(100000, 'L');

  // Vary the data slightly
  for (std::size_t ii = 0; ii < data.size(); ++ii) {
    data[ii] = static_cast<char>('A' + (ii % 26));
  }

  // Test with different chunk sizes
  for (std::size_t chunkSize : {1UL, 7UL, 13UL, 31UL, 63UL, 64UL, 65UL, 100UL, 1000UL}) {
    SCOPED_TRACE("chunkSize=" + std::to_string(chunkSize));
    ValidateAgainstOpenSSLChunked(data, chunkSize);
  }
}

TEST(SHA1Test, VeryLargeData) {
  // 10 MB of data
  std::string data(static_cast<std::size_t>(10 * 1024 * 1024), 'a');
  for (std::size_t ii = 0; ii < data.size(); ++ii) {
    data[ii] = static_cast<char>(ii % 256);
  }

  ValidateAgainstOpenSSL(data);
}
#endif

// ============================================================================
// Convenience Function Test
// ============================================================================

Sha1Digest ComputeSha1(std::string_view data) {
  SHA1 hasher;
  hasher.update(data.data(), data.size());
  return hasher.final();
}

TEST(SHA1Test, ConvenienceFunction) {
  EXPECT_EQ(ToHex(ComputeSha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  EXPECT_EQ(ToHex(ComputeSha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(ToHex(ComputeSha1("The quick brown fox jumps over the lazy dog")),
            "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

}  // namespace
}  // namespace aeronet
