#include "aeronet/hpack.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/raw-bytes.hpp"

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
  std::vector<uint8_t> encoded = {0x82};

  std::string name;
  std::string value;
  auto result = decoder.decode(AsBytes(encoded), [&](std::string_view nm, std::string_view val) {
    name = std::string(nm);
    value = std::string(val);
  });

  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(name, ":method");
  EXPECT_EQ(value, "GET");
}

TEST(HpackDecoder, DecodeLiteralWithIndexing) {
  HpackDecoder decoder(4096);

  // Literal header with incremental indexing, new name
  // 0x40 = literal with indexing, index 0 (new name)
  // 0x0a = name length 10
  // "custom-key" = name
  // 0x0d = value length 13
  // "custom-header" = value
  std::vector<uint8_t> encoded = {0x40, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
                                  'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e'};

  std::string name;
  std::string value;
  auto result = decoder.decode(AsBytes(encoded), [&](std::string_view n, std::string_view val) {
    name = std::string(n);
    value = std::string(val);
  });

  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(name, "custom-key");
  EXPECT_EQ(value, "custom-value");

  // Should be added to dynamic table
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 1U);
}

TEST(HpackDecoder, DecodeLiteralNameIncomplete) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0) but name length integer is incomplete
  // 0x40 = literal with indexing, index 0
  // Next byte: 0x7F -> length prefix all ones (127) indicating continuation required, but no continuation bytes
  // provided
  std::vector<uint8_t> encoded = {0x40, 0x7F};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header name");
}

TEST(HpackDecoder, DecodeLiteralHeaderNameInsufficientBytes) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name length 5, but only provide 2 bytes -> should detect insufficient data
  std::vector<uint8_t> encoded = {0x40, 0x05, 'a', 'b'};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});
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
  std::vector<uint8_t> encoded = {0x40, 0x81, 0x00};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header name");
}

TEST(HpackDecoder, DecodeLiteralHeaderNameHuffmanEos) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name: Huffman-flag set, length 4, payload all 0xFF (sequence of ones)
  // This should include the EOS code (30 ones) and be detected as an error
  std::vector<uint8_t> encoded = {0x40, 0x84, 0xFF, 0xFF, 0xFF, 0xFF};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});

  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header name");
}

TEST(HpackDecoder, DecodeLiteralHeaderValueHuffmanEos) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name: raw string length 3 "k","e","y"
  // Value: Huffman-flag set, length 4, payload all 0xFF (contains EOS)
  std::vector<uint8_t> encoded = {0x40, 0x03, 'k', 'e', 'y', 0x84, 0xFF, 0xFF, 0xFF, 0xFF};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});

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
  std::vector<uint8_t> encoded = {0x40, 0x84, 0x00, 0x00, 0x00, 0x00};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});

  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header name");
}

TEST(HpackDecoder, DecodeLiteralHeaderNameTooManyLeftoverBits) {
  HpackDecoder decoder(4096);

  // Literal with indexing, new name (index 0)
  // Name: Huffman-flag set, length 2, payload 0xFF 0xFF
  // This should leave >=8 leftover bits and trigger the 'too many leftover bits' path
  std::vector<uint8_t> encoded = {0x40, 0x82, 0xFF, 0xFF};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});

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

      auto res = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});
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
  std::vector<uint8_t> encoded = {0x40, 0x03, 'k', 'e', 'y', 0x7F};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode literal header value");
}

TEST(HpackDecoder, DecodeLiteralWithoutIndexing) {
  HpackDecoder decoder(4096);

  // Literal header without indexing, new name
  // 0x00 = literal without indexing, index 0 (new name)
  std::vector<uint8_t> encoded = {0x00, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
                                  'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e'};

  std::string name;
  std::string value;
  auto result = decoder.decode(AsBytes(encoded), [&](std::string_view nm, std::string_view val) {
    name = std::string(nm);
    value = std::string(val);
  });

  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(name, "custom-key");
  EXPECT_EQ(value, "custom-value");

  // Should NOT be added to dynamic table
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 0);
}

TEST(HpackDecoder, DecodeMultipleHeaders) {
  HpackDecoder decoder(4096);

  // :method: GET (0x82) + :path: / (0x84) + :scheme: https (0x87)
  std::vector<uint8_t> encoded = {0x82, 0x84, 0x87};

  std::vector<std::pair<std::string, std::string>> headers;
  auto result = decoder.decode(AsBytes(encoded),
                               [&](std::string_view nm, std::string_view val) { headers.emplace_back(nm, val); });

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(headers.size(), 3U);
  EXPECT_EQ(headers[0].first, ":method");
  EXPECT_EQ(headers[0].second, "GET");
  EXPECT_EQ(headers[1].first, ":path");
  EXPECT_EQ(headers[1].second, "/");
  EXPECT_EQ(headers[2].first, ":scheme");
  EXPECT_EQ(headers[2].second, "https");
}

TEST(HpackDecoder, DecodeDynamicTableSizeUpdate) {
  HpackDecoder decoder(4096);

  // Dynamic table size update to 1024: 0x3f 0xe1 0x07
  // (0x20 | 31) = 0x3f, then 1024 - 31 = 993 = 0x07e1 in varint
  std::vector<uint8_t> encoded = {0x3f, 0xe1, 0x07};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});

  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(decoder.dynamicTable().maxSize(), 1024U);
}

TEST(HpackDecoder, InvalidIndexedHeader) {
  HpackDecoder decoder(4096);

  // Index 0 is invalid
  std::vector<uint8_t> encoded = {0x80};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});

  EXPECT_FALSE(result.isSuccess());
}

TEST(HpackDecoder, DecodeIndexedHeaderIntegerIncomplete) {
  HpackDecoder decoder(4096);

  // Indexed header field prefix 1xxxxxxx, but integer continuation bytes are missing
  // Use first byte with prefix bits all ones (0xFF) so decodeInteger requires continuation bytes
  std::vector<uint8_t> encoded = {0xFF};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});
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

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});

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
  std::vector<uint8_t> encoded = {0x80};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});
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

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Index out of bounds in indexed header field");
}

TEST(HpackDecoder, DecodeDynamicTableSizeUpdateIncomplete) {
  HpackDecoder decoder(4096);

  // Dynamic table size update prefix 001xxxxx (0x20). Use 0x3f (prefix all ones) and no continuation
  std::vector<uint8_t> encoded = {0x3f};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});
  EXPECT_FALSE(result.isSuccess());
  ASSERT_NE(result.errorMessage, nullptr);
  EXPECT_STREQ(result.errorMessage, "Failed to decode dynamic table size update");
}

TEST(HpackDecoder, DecodeLiteralHeaderIndexIncomplete) {
  HpackDecoder decoder(4096);

  // Literal header field without indexing (prefix 0000) uses 4-bit prefix for index.
  // Provide a byte where the lower 4 bits are all ones -> requires continuation, but none provided
  std::vector<uint8_t> encoded = {0x0F};

  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});
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
  auto result = decoder.decode(AsBytes(encoded), [](std::string_view, std::string_view) {});

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

  auto r1 = decoder.decode(AsBytes(encoded1), [](std::string_view, std::string_view) {});
  EXPECT_TRUE(r1.isSuccess());
  auto r2 = decoder.decode(AsBytes(encoded2), [](std::string_view, std::string_view) {});
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
  std::vector<uint8_t> encoded = {0x40, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
                                  'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e'};

  std::string name1;
  auto res1 = decoder.decode(AsBytes(encoded), [&](std::string_view n, std::string_view) { name1 = std::string(n); });
  EXPECT_TRUE(res1.isSuccess());
  EXPECT_EQ(name1, "custom-key");

  // Clear decoded strings buffer (should not affect future decodes)
  decoder.clearDecodedStrings();

  std::string name2;
  auto res2 = decoder.decode(AsBytes(encoded), [&](std::string_view n, std::string_view) { name2 = std::string(n); });
  EXPECT_TRUE(res2.isSuccess());
  EXPECT_EQ(name2, "custom-key");
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

TEST(HpackEncoder, UnreasonableHeaderNameLen) {
  HpackEncoder encoder(4096);
  char ch{};
  std::string_view hugeHeader(&ch,
                              static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()) + 1);  // 1 GiB header name
  EXPECT_THROW((void)encoder.findHeader(hugeHeader, "GET"), std::overflow_error);
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
  auto result = encoder.findHeader("custom-Header", "custom-value");
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 62U);  // First dynamic table entry
}

TEST(HpackEncoder, FindHeaderTooLongToBeAStaticHeader) {
  HpackEncoder encoder(4096);

  // Add a custom header
  RawBytes output;
  encoder.encode(output, "A-Very-Long-Header-Name-That-Exceeds-Static-Table", "custom-value");

  // Search for name-only match with different value
  auto result = encoder.findHeader("A-Very-Long-Header-Name-That-Exceeds-Static-Table", "different-value");
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

  std::vector<std::pair<std::string, std::string>> decoded;
  auto result = decoder.decode(
      encoded, [&](std::string_view name, std::string_view value) { decoded.emplace_back(name, value); });

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(decoded.size(), 4U);
  EXPECT_EQ(decoded[0].first, ":method");
  EXPECT_EQ(decoded[0].second, "GET");
  EXPECT_EQ(decoded[1].first, ":path");
  EXPECT_EQ(decoded[1].second, "/index.html");
  EXPECT_EQ(decoded[2].first, ":scheme");
  EXPECT_EQ(decoded[2].second, "https");
  EXPECT_EQ(decoded[3].first, "custom-header");
  EXPECT_EQ(decoded[3].second, "custom-value");
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
  std::string value;
  auto result = decoder.decode(encoded1, [&](std::string_view, std::string_view val) { value = std::string(val); });
  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(value, "value1");

  result = decoder.decode(encoded2, [&](std::string_view, std::string_view val) { value = std::string(val); });
  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(value, "value1");
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
    size_t calls = 0;
    auto res = decoder.decode(AsBytes(buf), [&](std::string_view, std::string_view) { ++calls; });

    ASSERT_TRUE(res.isSuccess() || (res.errorMessage != nullptr && res.errorMessage[0] != '\0'));
    decoder.clearDecodedStrings();
  }
}