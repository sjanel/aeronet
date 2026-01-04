#include "aeronet/hpack.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet::http2 {

namespace {

std::span<const std::byte> AsBytes(std::span<const uint8_t> span) {
  return {reinterpret_cast<const std::byte*>(span.data()), span.size()};
}

// ============================
// Static Table Tests
// ============================

TEST(HpackStaticTable, HasCorrectSize) {
  auto table = GetHpackStaticTable();
  EXPECT_EQ(table.size(), 61U);
}

TEST(HpackStaticTable, ContainsExpectedEntries) {
  auto table = GetHpackStaticTable();

  // Index 1: :authority (empty value)
  EXPECT_EQ(table[0].name, ":authority");
  EXPECT_EQ(table[0].value, "");

  // Index 2: :method GET
  EXPECT_EQ(table[1].name, ":method");
  EXPECT_EQ(table[1].value, "GET");

  // Index 3: :method POST
  EXPECT_EQ(table[2].name, ":method");
  EXPECT_EQ(table[2].value, "POST");

  // Index 4: :path /
  EXPECT_EQ(table[3].name, ":path");
  EXPECT_EQ(table[3].value, "/");

  // Index 7: :scheme https
  EXPECT_EQ(table[6].name, ":scheme");
  EXPECT_EQ(table[6].value, "https");

  // Index 8: :status 200
  EXPECT_EQ(table[7].name, ":status");
  EXPECT_EQ(table[7].value, "200");
}

// ============================
// Dynamic Table Tests
// ============================

TEST(HpackDynamicTable, InitialState) {
  HpackDynamicTable table(4096);

  EXPECT_EQ(table.entryCount(), 0U);
  EXPECT_EQ(table.currentSize(), 0U);
  EXPECT_EQ(table.maxSize(), 4096U);
}

TEST(HpackDynamicTable, AddEntry) {
  HpackDynamicTable table(4096);

  bool added = table.add("custom-header", "custom-value");

  EXPECT_TRUE(added);
  EXPECT_EQ(table.entryCount(), 1U);
  // Size = name length + value length + 32
  EXPECT_EQ(table.currentSize(), 13U + 12U + 32U);
}

TEST(HpackDynamicTable, GetEntry) {
  HpackDynamicTable table(4096);
  table.add("header1", "value1");
  table.add("header2", "value2");

  // Index 0 is most recent (header2)
  auto& entry0 = table[0];
  EXPECT_EQ(entry0.name(), "header2");

  // Index 1 is older (header1)
  auto& entry1 = table[1];
  EXPECT_EQ(entry1.name(), "header1");
}

TEST(HpackDynamicTable, Eviction) {
  // Table can hold ~50 bytes (32 overhead + name + value)
  HpackDynamicTable table(100);

  // Add first entry: 7 + 6 + 32 = 45 bytes
  table.add("header1", "value1");
  EXPECT_EQ(table.entryCount(), 1U);

  // Add second entry: 7 + 6 + 32 = 45 bytes, total = 90
  table.add("header2", "value2");
  EXPECT_EQ(table.entryCount(), 2U);

  // Add third entry: would exceed 100, should evict first
  table.add("header3", "value3");
  EXPECT_EQ(table.entryCount(), 2U);
  // Most recent should be header3
  auto& entry = table[0];
  EXPECT_EQ(entry.name(), "header3");
  EXPECT_EQ(entry.value(), "value3");
}

TEST(HpackDynamicTable, SetMaxSize) {
  HpackDynamicTable table(4096);
  table.add("header1", "value1");  // 45 bytes
  table.add("header2", "value2");  // 45 bytes, total 90
  EXPECT_EQ(table.entryCount(), 2U);

  // Reduce max size to force eviction
  table.setMaxSize(50);

  EXPECT_EQ(table.entryCount(), 1U);
  EXPECT_LE(table.currentSize(), 50U);
}

TEST(HpackDynamicTable, Clear) {
  HpackDynamicTable table(4096);
  table.add("header1", "value1");
  table.add("header2", "value2");
  EXPECT_EQ(table.entryCount(), 2U);

  table.clear();

  EXPECT_EQ(table.entryCount(), 0U);
  EXPECT_EQ(table.currentSize(), 0U);
}

TEST(HpackDynamicTable, AddEntryTooLarge) {
  HpackDynamicTable table(50);  // Very small table

  // This entry is larger than the table
  std::string largeName(100, 'x');
  bool added = table.add(largeName, "value");

  EXPECT_FALSE(added);
  EXPECT_EQ(table.entryCount(), 0U);
}

// ============================
// Decoder Tests
// ============================

TEST(HpackDecoder, DecodeIndexedHeader) {
  HpackDecoder decoder(4096);

  // 0x82 = indexed header field, index 2 (:method: GET)
  static constexpr uint8_t encoded[]{0x82};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.begin()->first, ":method");
  EXPECT_EQ(result.decodedHeaders.begin()->second, "GET");
}

TEST(HpackDecoder, DuplicateIndexedHeaderForbidden) {
  HpackDecoder decoder(4096);

  // Indexed Header Field (1xxxxxxx) with 7-bit prefix. Static table index 28
  // corresponds to "content-length" in our static table (1-based index).
  // Encoded byte = 0x80 | 28 = 0x9C
  static constexpr uint8_t encoded[]{0x9C, 0x9C};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Duplicated header forbidden to merge");
}

TEST(HpackDecoder, DecodeLiteralWithIndexing) {
  HpackDecoder decoder(4096);

  // Literal header with incremental indexing, new name
  // 0x40 = literal with indexing, index 0 (new name)
  // 0x0a = name length 10
  // "custom-key" = name
  // 0x0d = value length 13
  // "custom-header" = value
  static constexpr uint8_t encoded[]{0x40, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
                                     'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e'};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.begin()->first, "custom-key");
  EXPECT_EQ(result.decodedHeaders.begin()->second, "custom-value");

  // Should be added to dynamic table
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 1U);
}

TEST(HpackDecoder, DecodeLiteralNameIncomplete) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0) but name length integer is incomplete
  // 0x40 = literal with indexing, index 0
  // Next byte: 0x7F -> length prefix all ones (127) indicating continuation required, but no continuation bytes
  // provided
  static constexpr uint8_t encoded[]{0x40, 0x7F};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header name");
}

TEST(HpackDecoder, DecodeLiteralHeaderNameInsufficientBytes) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name length 5, but only provide 2 bytes -> should detect insufficient data
  static constexpr uint8_t encoded[]{0x40, 0x05, 'a', 'b'};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header name");
}

TEST(HpackDecoder, DecodeLiteralHeaderNameInvalidHuffman) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name: Huffman-flag set, length 1, but provide a single byte that makes Huffman decoding fail
  // First byte: 0x40 = literal with indexing, next byte is name-length with Huffman bit set (0x81)
  // Next byte: 0x00 (invalid/insufficient Huffman data)
  static constexpr uint8_t encoded[]{0x40, 0x81, 0x00};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header name");
}

TEST(HpackDecoder, DecodeLiteralHeaderNameHuffmanEos) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name: Huffman-flag set, length 4, payload all 0xFF (sequence of ones)
  // This should include the EOS code (30 ones) and be detected as an error
  static constexpr uint8_t encoded[]{0x40, 0x84, 0xFF, 0xFF, 0xFF, 0xFF};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header name");
}

TEST(HpackDecoder, DecodeLiteralHeaderValueHuffmanEos) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name: raw string length 3 "k","e","y"
  // Value: Huffman-flag set, length 4, payload all 0xFF (contains EOS)
  static constexpr uint8_t encoded[]{0x40, 0x03, 'k', 'e', 'y', 0x84, 0xFF, 0xFF, 0xFF, 0xFF};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header value");
}

TEST(HpackDecoder, DecodeLiteralHeaderNameInvalidEncoding) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name: Huffman-flag set, length 4, payload 0x00 0x00 0x00 0x00
  // This produces many zero bits — decoder will try to decode up to 30 bits
  // and should return max when no symbol matches and bitsInBuffer >= 30
  static constexpr uint8_t encoded[]{0x40, 0x84, 0x00, 0x00, 0x00, 0x00};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header name");
}

TEST(HpackDecoder, DecodeLiteralHeaderNameTooManyLeftoverBits) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name: Huffman-flag set, length 2, payload 0xFF 0xFF
  // This should leave >=8 leftover bits and trigger the 'too many leftover bits' path
  static constexpr uint8_t encoded[]{0x40, 0x82, 0xFF, 0xFF};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header name");
}

TEST(HpackDecoder, FindInvalidHuffmanEncoding) {
  HpackDecoder decoder(4096);

  // We'll search for a 4-byte Huffman payload that causes the decoder to
  // fail decoding the Huffman-encoded name. This attempts a bounded search
  // across 2^16 candidates for the last two bytes while fixing first two
  // bytes to a few patterns. The goal is to find at least one input that
  // exercises the invalid-encoding path in the Huffman decoder.
  bool found = false;
  std::vector<uint8_t> encoded;

  const std::array<std::pair<uint8_t, uint8_t>, 4> prefixes = {
      {{0x12, 0x34}, {0xAA, 0x55}, {0xF0, 0x0F}, {0x99, 0x66}}};

  for (const auto& [b0, b1] : prefixes) {
    for (uint32_t tail = 0; tail <= 0xFFFF; ++tail) {
      const uint8_t b2 = static_cast<uint8_t>(tail >> 8);
      const uint8_t b3 = static_cast<uint8_t>(tail & 0xFF);

      // Literal with indexing, Huffman-flag set, length 4
      encoded = {0x40, 0x84, b0, b1, b2, b3};

      auto res = decoder.decode(AsBytes(encoded));
      if (!res.isSuccess()) {
        // We observed a decode failure for the literal header name.
        // Report and stop searching.
        found = true;
        break;
      }
    }
    if (found) {
      break;
    }
  }

  EXPECT_TRUE(found) << "Failed to find an invalid Huffman encoding in bounded search";
}

TEST(HpackDecoder, DecodeLiteralValueIncomplete) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name: length 3, "k","e","y"
  // Value: length prefix 127 (incomplete)
  static constexpr uint8_t encoded[]{0x40, 0x03, 'k', 'e', 'y', 0x7F};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header value");
}

TEST(HpackDecoder, DecodeLiteralWithoutIndexing) {
  HpackDecoder decoder(4096);

  // Literal header without indexing, new name
  // 0x00 = literal without indexing, index 0 (new name)
  static constexpr uint8_t encoded[]{0x00, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
                                     'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e'};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.begin()->first, "custom-key");
  EXPECT_EQ(result.decodedHeaders.begin()->second, "custom-value");

  // Should NOT be added to dynamic table
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 0);
}

TEST(HpackDecoder, DecodeMultipleHeaders) {
  HpackDecoder decoder(4096);

  // :method: GET (0x82) + :path: / (0x84) + :scheme: https (0x87)
  static constexpr uint8_t encoded[]{0x82, 0x84, 0x87};

  auto result = decoder.decode(AsBytes(encoded));

  const auto& headers = result.decodedHeaders;

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(headers.size(), 3U);

  const auto methodIt = headers.find(":method");
  ASSERT_NE(methodIt, headers.end());
  EXPECT_EQ(methodIt->second, "GET");

  const auto pathIt = headers.find(":path");
  ASSERT_NE(pathIt, headers.end());
  EXPECT_EQ(pathIt->second, "/");

  const auto schemeIt = headers.find(":scheme");
  ASSERT_NE(schemeIt, headers.end());
  EXPECT_EQ(schemeIt->second, "https");
}

TEST(HpackDecoder, DuplicateHeaderMergesWithComma) {
  HpackDecoder decoder(4096);

  // Two literal headers with the same name "accept" -> should be merged with ','
  // Format: literal with indexing (0x40), name length, name, value length, value
  static constexpr uint8_t encoded[]{
      0x40, 0x06, 'a', 'c', 'c', 'e', 'p', 't', 0x01, 'a',  // accept: a
      0x40, 0x06, 'a', 'c', 'c', 'e', 'p', 't', 0x01, 'b'   // accept: b
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.size(), 1U);
  const auto it = result.decodedHeaders.find("accept");
  ASSERT_NE(it, result.decodedHeaders.end());
  EXPECT_EQ(it->second, "a,b");
}

TEST(HpackDecoder, DuplicateCookieMergesWithSemicolon) {
  HpackDecoder decoder(4096);

  // Two Cookie headers should be merged with ';'
  static constexpr uint8_t encoded[]{
      0x40, 0x06, 'c', 'o', 'o', 'k', 'i', 'e', 0x03, 'o', 'n', 'e',
      0x40, 0x06, 'c', 'o', 'o', 'k', 'i', 'e', 0x03, 't', 'w', 'o',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.size(), 1U);
  const auto it = result.decodedHeaders.find("cookie");
  ASSERT_NE(it, result.decodedHeaders.end());
  EXPECT_EQ(it->second, "one;two");
}

TEST(HpackDecoder, DuplicateContentLengthIsForbidden) {
  HpackDecoder decoder(4096);

  // Content-Length duplicated should be rejected by storeHeader
  static constexpr uint8_t encoded[]{
      0x40, 0x0E, 'c', 'o', 'n', 't', 'e', 'n', 't', '-', 'l', 'e', 'n', 'g', 't', 'h', 0x01, '1',
      0x40, 0x0E, 'c', 'o', 'n', 't', 'e', 'n', 't', '-', 'l', 'e', 'n', 'g', 't', 'h', 0x01, '2',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Duplicated header forbidden to merge");
}

TEST(HpackDecoder, DecodeDynamicTableSizeUpdate) {
  HpackDecoder decoder(4096);

  // Dynamic table size update to 1024: 0x3f 0xe1 0x07
  // (0x20 | 31) = 0x3f, then 1024 - 31 = 993 = 0x07e1 in varint
  static constexpr uint8_t encoded[]{0x3f, 0xe1, 0x07};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(decoder.dynamicTable().maxSize(), 1024U);
}

TEST(HpackDecoder, InvalidIndexedHeader) {
  HpackDecoder decoder(4096);

  // Index 0 is invalid
  static constexpr uint8_t encoded[]{0x80};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
}

TEST(HpackDecoder, DecodeIndexedHeaderIntegerIncomplete) {
  HpackDecoder decoder(4096);

  // Indexed header field prefix 1xxxxxxx, but integer continuation bytes are missing
  // Use first byte with prefix bits all ones (0xFF) so decodeInteger requires continuation bytes
  static constexpr uint8_t encoded[]{0xFF};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode indexed header field index");
}

TEST(HpackDecoder, DecodeIndexedHeaderIntegerOverflow) {
  HpackDecoder decoder(4096);

  // Construct an indexed header field (1xxxxxxx). Use prefix 7 bits with
  // all ones to indicate continuation, then provide many continuation bytes
  // that keep the multiplier growing until it overflows to 0.
  std::vector<uint8_t> encoded;
  encoded.push_back(0x80 | 0x7F);

  // Provide a large number of continuation bytes with MSB set to 1 to force
  // many iterations in decodeInteger. Each continuation byte has low 7 bits
  // set to 0x7F to maximize contributions. This should eventually overflow
  // the multiplier (multiplier *= 128) when multiplier exceeds 64-bit range.
  for (int i = 0; i < 200; ++i) {
    encoded.push_back(0xFF);  // 0x80 | 0x7F
  }

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  // decodeInteger returns empty ret on overflow, which propagates to this error
  EXPECT_STREQ(result.errorMessage, "Failed to decode indexed header field index");
}

TEST(HpackDecoder, DecodeIndexedHeaderInvalidZero) {
  HpackDecoder decoder(4096);

  // Indexed header with explicit zero (invalid): encode varint 0 using prefix bits
  // To get indexResult.index == 0 after decodeInteger, craft a prefix < prefixMask with value 0
  // 0x80 has prefix value 0 -> triggers invalid-index-0 path
  static constexpr uint8_t encoded[]{0x80};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  // Either 'Failed to decode indexed header field index' or 'Invalid index 0 in indexed header field'
  ASSERT_NE(result.errorMessage, nullptr);
}

TEST(HpackDecoder, DecodeIndexedHeaderOutOfBounds) {
  HpackDecoder decoder(4096);

  // Indexed header with an index larger than static + dynamic table
  // Use a small decoder with no dynamic entries and index value 1000 encoded as varint
  std::vector<uint8_t> encoded;
  // First byte: 0x80 | prefix (7 bits) set to max to indicate continuation
  encoded.push_back(0x80 | 0x7F);
  // continuation bytes for 1000 - 127 = 873 in base-128 varint
  uint32_t rem = 1000 - 127;
  while (rem >= 128) {
    encoded.push_back(static_cast<uint8_t>((rem & 0x7F) | 0x80));
    rem >>= 7;
  }
  encoded.push_back(static_cast<uint8_t>(rem));

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Index out of bounds in indexed header field");
}

TEST(HpackDecoder, DecodeDynamicTableSizeUpdateIncomplete) {
  HpackDecoder decoder(4096);

  // Dynamic table size update prefix 001xxxxx (0x20). Use 0x3f (prefix all ones) and no continuation
  static constexpr uint8_t encoded[]{0x3f};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode dynamic table size update");
}

TEST(HpackDecoder, DecodeLiteralHeaderIndexIncomplete) {
  HpackDecoder decoder(4096);

  // Literal header field without indexing (prefix 0000) uses 4-bit prefix for index.
  // Provide a byte where the lower 4 bits are all ones -> requires continuation, but none provided
  static constexpr uint8_t encoded[]{0x0F};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header index");
}

TEST(HpackDecoder, DecodeLiteralHeaderNameOutOfBounds) {
  HpackDecoder decoder(4096);

  // Literal header field with incremental indexing (01xxxxxx). Use the
  // 6-bit prefix with all ones to force varint continuation for the name index.
  std::vector<uint8_t> encoded;
  encoded.push_back(0x40 | 0x3F);

  // Choose an index far beyond static + dynamic table sizes (e.g., 1000).
  // For prefix max 63, varint encodes (1000 - 63).
  uint32_t rem = 1000 - 63;
  while (rem >= 128) {
    encoded.push_back(static_cast<uint8_t>((rem & 0x7F) | 0x80));
    rem >>= 7;
  }
  encoded.push_back(static_cast<uint8_t>(rem));

  // No name/value bytes are required because lookup should fail on name index.
  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Index out of bounds for header name");
}

TEST(HpackDecoder, SetMaxDynamicTableSize) {
  HpackDecoder decoder(4096);

  // Add two entries via literal-with-indexing encoded blocks
  std::vector<uint8_t> encoded1 = {0x40, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
                                   'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e'};

  std::vector<uint8_t> encoded2 = {0x40, 0x04, 'h', 'e', 'a', 'd', 0x05, 'v', 'a', 'l', 'u', 'e'};

  auto r1 = decoder.decode(AsBytes(encoded1));
  EXPECT_TRUE(r1.isSuccess());
  auto r2 = decoder.decode(AsBytes(encoded2));
  EXPECT_TRUE(r2.isSuccess());

  EXPECT_EQ(decoder.dynamicTable().entryCount(), 2U);

  // Now reduce the max dynamic table size to force eviction
  decoder.setMaxDynamicTableSize(50);

  EXPECT_EQ(decoder.dynamicTable().maxSize(), 50U);
  EXPECT_LE(decoder.dynamicTable().currentSize(), 50U);
  EXPECT_LT(decoder.dynamicTable().entryCount(), 2U);
}

TEST(HpackDecoder, ClearDecodedStrings) {
  HpackDecoder decoder(4096);

  // Use an encoded literal-with-indexing block to populate decoded strings
  static constexpr uint8_t encoded[]{0x40, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
                                     'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e'};

  auto res1 = decoder.decode(AsBytes(encoded));
  EXPECT_TRUE(res1.isSuccess());
  EXPECT_EQ(res1.decodedHeaders.size(), 1U);
  EXPECT_EQ(res1.decodedHeaders.begin()->first, "custom-key");

  auto res2 = decoder.decode(AsBytes(encoded));
  EXPECT_TRUE(res2.isSuccess());
  EXPECT_EQ(res2.decodedHeaders.size(), 1U);
  EXPECT_EQ(res2.decodedHeaders.begin()->first, "custom-key");
}

// ============================
// Encoder Tests
// ============================

TEST(HpackEncoder, EncodeIndexedHeader) {
  HpackEncoder encoder(4096);
  RawBytes output;

  // :method: GET should use indexed representation (index 2)
  encoder.encode(output, ":method", "GET");

  // Should encode as 0x82 (indexed, index 2)
  EXPECT_EQ(output.size(), 1U);
  EXPECT_EQ(static_cast<uint8_t>(output[0]), 0x82);
}

TEST(HpackEncoder, EncodeLiteralNewName) {
  HpackEncoder encoder(4096);
  RawBytes output;

  encoder.encode(output, "custom-header", "custom-value");

  EXPECT_GT(output.size(), 0);

  // Verify it was added to dynamic table
  EXPECT_EQ(encoder.dynamicTable().entryCount(), 1U);
}

TEST(HpackEncoder, EncodeReuseDynamicTable) {
  HpackEncoder encoder(4096);

  // First encode adds to dynamic table
  RawBytes output1;
  encoder.encode(output1, "custom-header", "custom-value");
  std::size_t firstSize = output1.size();

  // Second encode should use indexed representation
  RawBytes output2;
  encoder.encode(output2, "custom-header", "custom-value");
  std::size_t secondSize = output2.size();

  // Second encoding should be smaller (just index reference)
  EXPECT_LT(secondSize, firstSize);
}

TEST(HpackEncoder, FindHeaderInStaticTable) {
  HpackEncoder encoder(4096);

  // :method: GET should be found with full match
  auto result = encoder.findHeader(":method", "GET");
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 2U);

  // :method: PUT should be found with name-only match
  result = encoder.findHeader(":method", "PUT");
  EXPECT_EQ(result.match, HpackLookupResult::Match::NameOnly);
  // Index should be one of the :method entries (2 or 3)
  EXPECT_TRUE(result.index == 2U || result.index == 3U);
}

TEST(HpackEncoder, FindHeaderInDynamicTable) {
  HpackEncoder encoder(4096);

  // Add a custom header
  RawBytes output;
  encoder.encode(output, "custom-header", "custom-value");

  // Should be found in dynamic table (index 62)
  auto result = encoder.findHeader("custom-header", "custom-value");
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 62U);  // First dynamic table entry
}

TEST(HpackEncoder, FindHeaderTooLongToBeAStaticHeader) {
  HpackEncoder encoder(4096);

  // Add a custom header
  RawBytes output;
  encoder.encode(output, "a-very-long-header-name-that-exceeds-static-table", "custom-value");

  // Search for name-only match with different value
  auto result = encoder.findHeader("a-very-long-header-name-that-exceeds-static-table", "different-value");
  EXPECT_EQ(result.match, HpackLookupResult::Match::NameOnly);
  EXPECT_EQ(result.index, 62U);  // First dynamic table entry
}

TEST(HpackEncoder, FindHeaderNotFound) {
  HpackEncoder encoder(4096);

  auto result = encoder.findHeader("x-nonexistent", "value");
  EXPECT_EQ(result.match, HpackLookupResult::Match::None);
}

TEST(HpackEncoder, EncodeDynamicTableSizeUpdate) {
  HpackEncoder encoder(4096);
  RawBytes output;

  encoder.encodeDynamicTableSizeUpdate(output, 1024);

  // Should encode as dynamic table size update
  EXPECT_GT(output.size(), 0);
  // First byte should have 001xxxxx pattern
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0xE0, 0x20);
}

TEST(HpackEncoder, EncodeNeverIndexed_WithStaticName) {
  HpackEncoder encoder(4096);
  RawBytes output;

  // Use a known static name ':method' but different value -> name-only match
  encoder.encode(output, ":method", "PUT", HpackEncoder::IndexingMode::NeverIndexed);

  // First byte should have 0001xxxx pattern (0x10)
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0xF0, 0x10);
}

TEST(HpackEncoder, EncodeNeverIndexed_NewName) {
  HpackEncoder encoder(4096);
  RawBytes output;

  encoder.encode(output, "x-new-name", "v", HpackEncoder::IndexingMode::NeverIndexed);

  // First byte should be 0x10 when name is new (literal name encoded)
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0xF0, 0x10);
}

TEST(HpackEncoder, EncodeWithoutIndexing_WithStaticName) {
  HpackEncoder encoder(4096);
  RawBytes output;

  // Known static name ':method' with a different value should use name-only without indexing
  encoder.encode(output, ":method", "PUT", HpackEncoder::IndexingMode::WithoutIndexing);

  // First byte should have 0000xxxx pattern (0x00)
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0xF0, 0x00);
}

TEST(HpackEncoder, EncodeWithoutIndexing_NewName) {
  HpackEncoder encoder(4096);
  RawBytes output;

  encoder.encode(output, "x-new-name", "v", HpackEncoder::IndexingMode::WithoutIndexing);

  // First byte should be 0x00 when name is new (literal name encoded)
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0xF0, 0x00);
}

// ============================
// Round-trip Tests
// ============================

TEST(HpackRoundTrip, SimpleHeaders) {
  HpackEncoder encoder(4096);
  HpackDecoder decoder(4096);

  RawBytes encoded;
  encoder.encode(encoded, ":method", "GET");
  encoder.encode(encoded, ":path", "/index.html");
  encoder.encode(encoded, ":scheme", "https");
  encoder.encode(encoded, "custom-header", "custom-value");

  auto result = decoder.decode(encoded);

  const auto& headers = result.decodedHeaders;

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(headers.size(), 4U);

  const auto methodIt = headers.find(":method");
  ASSERT_NE(methodIt, headers.end());
  EXPECT_EQ(methodIt->second, "GET");

  const auto pathIt = headers.find(":path");
  ASSERT_NE(pathIt, headers.end());
  EXPECT_EQ(pathIt->second, "/index.html");

  const auto schemeIt = headers.find(":scheme");
  ASSERT_NE(schemeIt, headers.end());
  EXPECT_EQ(schemeIt->second, "https");

  const auto customIt = headers.find("custom-header");
  ASSERT_NE(customIt, headers.end());
  EXPECT_EQ(customIt->second, "custom-value");
}

TEST(HpackRoundTrip, RepeatedHeaders) {
  HpackEncoder encoder(4096);
  HpackDecoder decoder(4096);

  // Encode same header multiple times
  RawBytes encoded1;
  encoder.encode(encoded1, "x-custom", "value1");

  RawBytes encoded2;
  encoder.encode(encoded2, "x-custom", "value1");

  // Second encoding should be smaller due to dynamic table
  EXPECT_LT(encoded2.size(), encoded1.size());

  // Both should decode correctly
  {
    auto result = decoder.decode(encoded1);
    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(result.decodedHeaders.size(), 1U);

    auto [name, value] = *result.decodedHeaders.begin();
    EXPECT_EQ(name, "x-custom");
    EXPECT_EQ(value, "value1");
  }

  {
    auto result = decoder.decode(encoded2);
    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(result.decodedHeaders.size(), 1U);

    auto [name, value] = *result.decodedHeaders.begin();
    EXPECT_EQ(name, "x-custom");
    EXPECT_EQ(value, "value1");
  }
}

TEST(HpackRoundTrip, DateHeaderValue) {
  HpackEncoder encoder(4096);
  HpackDecoder decoder(4096);

  static constexpr std::string_view kDate = "Thu, 01 Jan 1970 00:00:00 GMT";
  static_assert(kDate.size() == kRFC7231DateStrLen);

  RawBytes encoded;
  encoder.encode(encoded, ":status", "200");
  encoder.encode(encoded, "date", kDate);
  encoder.encode(encoded, "content-length", "1");

  auto result = decoder.decode(encoded);
  ASSERT_TRUE(result.isSuccess());

  const auto dateIt = result.decodedHeaders.find("date");
  ASSERT_NE(dateIt, result.decodedHeaders.end());
  EXPECT_EQ(dateIt->second, kDate);
}

TEST(HpackRoundTrip, CurrentDateHeaderValue) {
  HpackEncoder encoder(4096);
  HpackDecoder decoder(4096);

  const std::array<char, kRFC7231DateStrLen> dateBuf = [] {
    std::array<char, kRFC7231DateStrLen> buf{};
    (void)TimeToStringRFC7231(SysClock::now(), buf.data());
    return buf;
  }();

  const std::string_view dateSv{dateBuf.data(), kRFC7231DateStrLen};
  ASSERT_EQ(dateSv.size(), kRFC7231DateStrLen);

  RawBytes encoded;
  encoder.encode(encoded, ":status", "200");
  encoder.encode(encoded, "date", dateSv);
  encoder.encode(encoded, "content-length", "1");

  auto result = decoder.decode(encoded);
  ASSERT_TRUE(result.isSuccess());

  const auto dateIt = result.decodedHeaders.find("date");
  if (dateIt == result.decodedHeaders.end()) {
    for (const auto& [name, value] : result.decodedHeaders) {
      ADD_FAILURE() << "Decoded header: '" << name << "'='" << value << "'";
    }
    // Also check if the date value got associated with a different name.
    for (const auto& [name, value] : result.decodedHeaders) {
      if (value.size() == dateSv.size() && std::memcmp(value.data(), dateSv.data(), value.size()) == 0) {
        ADD_FAILURE() << "Date value decoded under name: '" << name << "'";
      }
    }
    FAIL() << "Missing 'date' in decoded headers";
  }
  EXPECT_EQ(dateIt->second.size(), kRFC7231DateStrLen);
  EXPECT_TRUE(dateIt->second.ends_with("GMT"));
}

TEST(HpackRoundTrip, ResponseHeaderSetIncludesDate) {
  HpackEncoder encoder(4096);
  HpackDecoder decoder(4096);

  const std::array<char, kRFC7231DateStrLen> dateBuf = [] {
    std::array<char, kRFC7231DateStrLen> buf;
    (void)TimeToStringRFC7231(SysClock::now(), buf.data());
    return buf;
  }();

  const std::string_view dateSv{dateBuf.data(), kRFC7231DateStrLen};

  RawBytes encoded;
  encoder.encode(encoded, ":status", "200");
  encoder.encode(encoded, "content-type", "text/plain");
  encoder.encode(encoded, "x-custom", "original");
  encoder.encode(encoded, "x-another", "anothervalue");
  encoder.encode(encoded, "x-global", "gvalue");
  encoder.encode(encoded, "date", dateSv);
  encoder.encode(encoded, "content-length", "1");

  auto result = decoder.decode(encoded);
  ASSERT_TRUE(result.isSuccess());

  const auto dateIt = result.decodedHeaders.find("date");
  ASSERT_NE(dateIt, result.decodedHeaders.end());
  EXPECT_EQ(dateIt->second.size(), kRFC7231DateStrLen);
  EXPECT_TRUE(dateIt->second.ends_with("GMT"));
}

}  // namespace
}  // namespace aeronet::http2

TEST(HpackDecoderFuzz, RandomizedReserveFuzz) {
  using namespace aeronet::http2;

  HpackDecoder decoder;

  std::mt19937_64 rng(123456789);  // deterministic seed for reproducibility
  std::uniform_int_distribution<int> byteDist(0, 255);

  static constexpr int iterations = 59;
  static constexpr std::size_t kMaxLen = 1 << 20;  // 1 MiB

  static constexpr int kStep = kMaxLen / iterations;

  std::vector<uint8_t> buf;
  buf.reserve(kMaxLen);

  for (std::size_t len = 7; len < kMaxLen; len += kStep) {
    while (buf.size() < len) {
      buf.push_back(static_cast<uint8_t>(byteDist(rng)));
    }

    // Run decode; ensure it doesn't crash and returns a well-formed DecodeResult.
    // Any decode error is acceptable but must provide a non-null, non-empty error message.
    auto res = decoder.decode(AsBytes(buf));

    ASSERT_TRUE(res.isSuccess() || (res.errorMessage != nullptr && res.errorMessage[0] != '\0'));
  }
}