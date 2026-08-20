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

#include "aeronet/raw-bytes.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {

namespace {

std::span<const std::byte> AsBytes(std::span<const uint8_t> span) {
  return {reinterpret_cast<const std::byte*>(span.data()), span.size()};
}

}  // namespace

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
  EXPECT_EQ(table.currentSizeBytes(), 0U);
  EXPECT_EQ(table.maxSizeBytes(), 4096U);
}

TEST(HpackDynamicTable, AddEntry) {
  HpackDynamicTable table(4096);

  bool added = table.add("custom-header", "custom-value");

  EXPECT_TRUE(added);
  EXPECT_EQ(table.entryCount(), 1U);
  // Size = name length + value length + 32
  EXPECT_EQ(table.currentSizeBytes(), 13U + 12U + 32U);
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
  EXPECT_LE(table.currentSizeBytes(), 50U);
}

TEST(HpackDynamicTable, EvictingLastEntryResetsTable) {
  HpackDynamicTable table(4096);
  table.add("header", "value");

  table.setMaxSize(0);

  EXPECT_EQ(table.entryCount(), 0U);
  EXPECT_EQ(table.currentSizeBytes(), 0U);

  table.setMaxSize(4096);
  table.add("new-header", "new-value");

  EXPECT_EQ(table.entryCount(), 1U);
  EXPECT_EQ(table[0].name(), "new-header");
}

TEST(HpackDynamicTable, Clear) {
  HpackDynamicTable table(4096);
  table.add("header1", "value1");
  table.add("header2", "value2");
  EXPECT_EQ(table.entryCount(), 2U);

  table.clear();

  EXPECT_EQ(table.entryCount(), 0U);
  EXPECT_EQ(table.currentSizeBytes(), 0U);
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

namespace {

HpackDecoder CreateHpackDecoder() { return {4096, true}; }

}  // namespace

TEST(HpackDecoder, DecodeIndexedHeader) {
  auto decoder = CreateHpackDecoder();

  // 0x82 = indexed header field, index 2 (:method: GET)
  static constexpr uint8_t encoded[]{0x82};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.begin()->first, ":method");
  EXPECT_EQ(result.decodedHeaders.begin()->second, "GET");
}

TEST(HpackDecoder, DuplicateIndexedHeaderForbidden) {
  auto decoder = CreateHpackDecoder();

  // Indexed Header Field (1xxxxxxx) with 7-bit prefix. Static table index 28
  // corresponds to "content-length" in our static table (1-based index).
  // Encoded byte = 0x80 | 28 = 0x9C
  static constexpr uint8_t encoded[]{0x9C, 0x9C};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::DuplicateHeaderForbiddenToMerge);
}

TEST(HpackDecoder, DecodeLiteralWithIndexing) {
  auto decoder = CreateHpackDecoder();

  // Literal header with incremental indexing, new name
  // 0x40 = literal with indexing, index 0 (new name)
  // 0x0a = name length 10
  // "custom-key" = name
  // 0x0d = value length 13
  // "custom-header" = value
  static constexpr uint8_t encoded[]{
      0x40, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
      'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e',
  };

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.begin()->first, "custom-key");
  EXPECT_EQ(result.decodedHeaders.begin()->second, "custom-value");

  // Should be added to dynamic table
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 1U);
}

// ============================
// Field name / value validity (RFC 9113 §8.2.1)
// ============================

TEST(HpackDecoder, RejectsUppercaseInNewLiteralName) {
  auto decoder = CreateHpackDecoder();

  // Literal with incremental indexing, new name "Custom-Key" (uppercase C, K)
  static constexpr uint8_t encoded[]{
      0x40, 0x0A, 'C', 'u', 's', 't', 'o', 'm', '-', 'K', 'e', 'y', 0x0C,
      'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::MalformedFieldName);

  // HPACK compression state must stay in sync even though the message is malformed:
  // the entry must still have been added to the dynamic table.
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 1U);
}

TEST(HpackDecoder, RejectsInvalidByteInNewLiteralName) {
  auto decoder = CreateHpackDecoder();

  // Name "foo:bar" - colon is not a tchar and is not uppercase, checks the
  // validator isn't just an A-Z check.
  static constexpr uint8_t encoded[]{
      0x40, 0x07, 'f', 'o', 'o', ':', 'b', 'a', 'r', 0x01, 'x',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::MalformedFieldName);
}

TEST(HpackDecoder, RejectsEmptyNewLiteralName) {
  auto decoder = CreateHpackDecoder();

  // Literal with incremental indexing, new name of length 0
  static constexpr uint8_t encoded[]{0x40, 0x00, 0x01, 'x'};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::MalformedFieldName);
}

TEST(HpackDecoder, AcceptsPunctuationTcharInName) {
  auto decoder = CreateHpackDecoder();

  // Name "x-my_header!" uses only valid tchar punctuation ('-', '_', '!')
  static constexpr uint8_t encoded[]{
      0x40, 0x0C, 'x', '-', 'm', 'y', '_', 'h', 'e', 'a', 'd', 'e', 'r', '!', 0x02, 'o', 'k',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.begin()->first, "x-my_header!");
}

TEST(HpackDecoder, RejectsCrLfInValue) {
  auto decoder = CreateHpackDecoder();

  // Value "bad\r\nvalue" - CRLF injection attempt
  static constexpr uint8_t encoded[]{
      0x40, 0x06, 'x', '-', 't', 'e', 's', 't', 0x0A, 'b', 'a', 'd', 0x0D, 0x0A, 'v', 'a', 'l', 'u', 'e',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::MalformedFieldValue);
}

TEST(HpackDecoder, RejectsNulInValue) {
  auto decoder = CreateHpackDecoder();

  // Value contains an embedded NUL byte
  static constexpr uint8_t encoded[]{
      0x40, 0x06, 'x', '-', 't', 'e', 's', 't', 0x03, 'a', 0x00, 'b',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::MalformedFieldValue);
}

TEST(HpackDecoder, AcceptsPseudoHeaderViaFullIndex) {
  auto decoder = CreateHpackDecoder();

  // Indexed Header Field, static table index 2 = ":method" / "GET"
  static constexpr uint8_t encoded[]{0x82};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.begin()->first, ":method");
  EXPECT_EQ(result.decodedHeaders.begin()->second, "GET");
}

TEST(HpackDecoder, RejectsDuplicatePseudoHeader) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();
  RawBytes encoded;
  encoder.encode(encoded, ":method", "GET");
  encoder.encode(encoded, ":method", "GET");

  const auto result = decoder.decode(encoded);

  EXPECT_FALSE(result.isSuccess());
  EXPECT_FALSE(result.isCompressionError());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::DuplicatePseudoHeaderField);
}

TEST(HpackDecoder, RejectsPseudoHeaderAfterRegularField) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();
  RawBytes encoded;
  encoder.encode(encoded, "x-first", "value");
  encoder.encode(encoded, ":method", "GET");

  const auto result = decoder.decode(encoded);

  EXPECT_FALSE(result.isSuccess());
  EXPECT_FALSE(result.isCompressionError());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::PseudoHeaderFieldAfterRegularField);
}

TEST(HpackDecoder, RejectsUndefinedPseudoHeaderAndFinishesDecodingBlock) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();
  RawBytes encoded;
  encoder.encode(encoded, ":extension", "value");
  encoder.encode(encoded, "x-after", "value");

  const auto result = decoder.decode(encoded);

  EXPECT_FALSE(result.isSuccess());
  EXPECT_FALSE(result.isCompressionError());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::UndefinedPseudoHeaderField);
  // Even a malformed field section has to be decoded in full so incremental-indexing updates keep the HPACK
  // dynamic table synchronized with the peer (RFC 9113 §4.3).
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 2U);
}

TEST(HpackDecoder, AcceptsDefinedProtocolPseudoHeaderForContextValidation) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();
  RawBytes encoded;
  encoder.encode(encoded, ":protocol", "websocket");

  const auto result = decoder.decode(encoded);

  ASSERT_TRUE(result.isSuccess());
  ASSERT_TRUE(result.decodedHeaders.contains(":protocol"));
  EXPECT_EQ(result.decodedHeaders.at(":protocol"), "websocket");
}

TEST(HpackDecoder, WithoutIndexingStillValidatesButDoesNotPollutTable) {
  auto decoder = CreateHpackDecoder();

  // Literal WITHOUT indexing (0000xxxx), new name "Bad-Name" (uppercase)
  static constexpr uint8_t encoded[]{
      0x00, 0x08, 'B', 'a', 'd', '-', 'N', 'a', 'm', 'e', 0x01, 'x',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::MalformedFieldName);

  // withIndexing == false here: nothing should have been added to the dynamic table.
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 0U);
}

TEST(HpackDecoder, IndexedReferenceToPreviouslyPoisonedDynamicEntryIsRejected) {
  auto decoder = CreateHpackDecoder();

  // 1st block: literal with indexing, new (invalid) name "Custom-Key" -> rejected,
  // but per RFC 9113 §4.3 it must still land in the dynamic table at index 62
  // (61 static entries + 1).
  static constexpr uint8_t firstBlock[]{
      0x40, 0x0A, 'C', 'u', 's', 't', 'o', 'm', '-', 'K', 'e', 'y', 0x0C,
      'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e',
  };
  auto firstResult = decoder.decode(AsBytes(firstBlock));
  ASSERT_FALSE(firstResult.isSuccess());
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 1U);

  // 2nd block, same connection/decoder: Indexed Header Field referencing index 62
  // (0x80 | 62 = 0xBE), i.e. a pure back-reference to the poisoned entry above.
  // This must be caught too, not just the original literal-new-name path.
  static constexpr uint8_t secondBlock[]{0xBE};
  auto secondResult = decoder.decode(AsBytes(secondBlock));

  EXPECT_FALSE(secondResult.isSuccess());
  EXPECT_EQ(secondResult.error, HpackDecoder::DecodeResult::Error::MalformedFieldName);
}

TEST(HpackDecoder, DecodeLiteralNameIncomplete) {
  auto decoder = CreateHpackDecoder();

  // Literal with indexing, new name (index 0) but name length integer is incomplete
  // 0x40 = literal with indexing, index 0
  // Next byte: 0x7F -> length prefix all ones (127) indicating continuation required, but no continuation bytes
  // provided
  static constexpr uint8_t encoded[]{0x40, 0x7F};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderName);
}

TEST(HpackDecoder, DecodeLiteralHeaderNameInsufficientBytes) {
  auto decoder = CreateHpackDecoder();

  // Literal with indexing, new name (index 0)
  // Name length 5, but only provide 2 bytes -> should detect insufficient data
  static constexpr uint8_t encoded[]{0x40, 0x05, 'a', 'b'};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderName);
}

TEST(HpackDecoder, DecodeLiteralHeaderNameInvalidHuffman) {
  auto decoder = CreateHpackDecoder();

  // Literal with indexing, new name (index 0)
  // Name: Huffman-flag set, length 1, but provide a single byte that makes Huffman decoding fail
  // First byte: 0x40 = literal with indexing, next byte is name-length with Huffman bit set (0x81)
  // Next byte: 0x00 (invalid/insufficient Huffman data)
  static constexpr uint8_t encoded[]{0x40, 0x81, 0x00};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderName);
}

TEST(HpackDecoder, DecodeLiteralHeaderNameHuffmanEos) {
  auto decoder = CreateHpackDecoder();

  // Literal with indexing, new name (index 0)
  // Name: Huffman-flag set, length 4, payload all 0xFF (sequence of ones)
  // This should include the EOS code (30 ones) and be detected as an error
  static constexpr uint8_t encoded[]{0x40, 0x84, 0xFF, 0xFF, 0xFF, 0xFF};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderName);
}

TEST(HpackDecoder, DecodeLiteralHeaderValueHuffmanEos) {
  auto decoder = CreateHpackDecoder();

  // Literal with indexing, new name (index 0)
  // Name: raw string length 3 "k","e","y"
  // Value: Huffman-flag set, length 4, payload all 0xFF (contains EOS)
  static constexpr uint8_t encoded[]{0x40, 0x03, 'k', 'e', 'y', 0x84, 0xFF, 0xFF, 0xFF, 0xFF};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderValue);
}

TEST(HpackDecoder, DecodeLiteralHeaderNameInvalidEncoding) {
  auto decoder = CreateHpackDecoder();

  // Literal with indexing, new name (index 0)
  // Name: Huffman-flag set, length 4, payload 0x00 0x00 0x00 0x00
  // This produces many zero bits — decoder will try to decode up to 30 bits
  // and should return max when no symbol matches and bitsInBuffer >= 30
  static constexpr uint8_t encoded[]{0x40, 0x84, 0x00, 0x00, 0x00, 0x00};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderName);
}

TEST(HpackDecoder, DecodeLiteralHeaderNameTooManyLeftoverBits) {
  auto decoder = CreateHpackDecoder();

  // Literal with indexing, new name (index 0)
  // Name: Huffman-flag set, length 2, payload 0xFF 0xFF
  // This should leave >=8 leftover bits and trigger the 'too many leftover bits' path
  static constexpr uint8_t encoded[]{0x40, 0x82, 0xFF, 0xFF};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderName);
}

TEST(HpackDecoder, FindInvalidHuffmanEncoding) {
  auto decoder = CreateHpackDecoder();

  // We'll search for a 4-byte Huffman payload that causes the decoder to
  // fail decoding the Huffman-encoded name. This attempts a bounded search
  // across 2^16 candidates for the last two bytes while fixing first two
  // bytes to a few patterns. The goal is to find at least one input that
  // exercises the invalid-encoding path in the Huffman decoder.
  bool found = false;
  vector<uint8_t> encoded;

  const std::array<std::pair<uint8_t, uint8_t>, 4> prefixes{
      {
          {0x12, 0x34},
          {0xAA, 0x55},
          {0xF0, 0x0F},
          {0x99, 0x66},
      },
  };

  for (const auto& [b0, b1] : prefixes) {
    for (uint32_t tail = 0; tail <= 0xFFFF; ++tail) {
      const uint8_t b2 = static_cast<uint8_t>(tail >> 8U);
      const uint8_t b3 = static_cast<uint8_t>(tail & 0xFFU);

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
  auto decoder = CreateHpackDecoder();

  // Literal with indexing, new name (index 0)
  // Name: length 3, "k","e","y"
  // Value: length prefix 127 (incomplete)
  static constexpr uint8_t encoded[]{0x40, 0x03, 'k', 'e', 'y', 0x7F};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderValue);
}

TEST(HpackDecoder, DecodeLiteralWithoutIndexing) {
  auto decoder = CreateHpackDecoder();

  // Literal header without indexing, new name
  // 0x00 = literal without indexing, index 0 (new name)
  static constexpr uint8_t encoded[]{
      0x00, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
      'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.begin()->first, "custom-key");
  EXPECT_EQ(result.decodedHeaders.begin()->second, "custom-value");

  // Should NOT be added to dynamic table
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 0);
}

TEST(HpackDecoder, DecodeMultipleHeaders) {
  auto decoder = CreateHpackDecoder();

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
  auto decoder = CreateHpackDecoder();

  // Two literal headers with the same name "accept" -> should be merged with ','
  // Format: literal with indexing (0x40), name length, name, value length, value
  static constexpr uint8_t encoded[]{
      0x40, 0x06, 'a', 'c', 'c', 'e', 'p', 't', 0x01, 'a',  // accept: a
      0x40, 0x06, 'a', 'c', 'c', 'e', 'p', 't', 0x01, 'b',  // accept: b
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.size(), 1U);
  const auto it = result.decodedHeaders.find("accept");
  ASSERT_NE(it, result.decodedHeaders.end());
  EXPECT_EQ(it->second, "a,b");
}

TEST(HpackDecoder, DuplicateCookieMergesWithSemicolon) {
  auto decoder = CreateHpackDecoder();

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
  auto decoder = CreateHpackDecoder();

  // Content-Length duplicated should be rejected by storeHeader
  static constexpr uint8_t encoded[]{
      0x40, 0x0E, 'c', 'o', 'n', 't', 'e', 'n', 't', '-', 'l', 'e', 'n', 'g', 't', 'h', 0x01, '1',
      0x40, 0x0E, 'c', 'o', 'n', 't', 'e', 'n', 't', '-', 'l', 'e', 'n', 'g', 't', 'h', 0x01, '2',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::DuplicateHeaderForbiddenToMerge);
}

TEST(HpackDecoder, DecodeDynamicTableSizeUpdate) {
  auto decoder = CreateHpackDecoder();

  // Dynamic table size update to 1024: 0x3f 0xe1 0x07
  // (0x20 | 31) = 0x3f, then 1024 - 31 = 993 = 0x07e1 in varint
  static constexpr uint8_t encoded[]{0x3f, 0xe1, 0x07};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 1024U);
}

TEST(HpackDecoder, InvalidIndexedHeader) {
  auto decoder = CreateHpackDecoder();

  // Index 0 is invalid
  static constexpr uint8_t encoded[]{0x80};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
}

TEST(HpackDecoder, DecodeIndexedHeaderIntegerIncomplete) {
  auto decoder = CreateHpackDecoder();

  // Indexed header field prefix 1xxxxxxx, but integer continuation bytes are missing
  // Use first byte with prefix bits all ones (0xFF) so decodeInteger requires continuation bytes
  static constexpr uint8_t encoded[]{0xFF};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeIndexedHeaderFieldIndex);
}

TEST(HpackDecoder, DecodeIndexedHeaderIntegerOverflow) {
  auto decoder = CreateHpackDecoder();

  // Construct an indexed header field (1xxxxxxx). Use prefix 7 bits with
  // all ones to indicate continuation, then provide many continuation bytes
  // that keep the multiplier growing until it overflows to 0.
  vector<uint8_t> encoded;
  encoded.push_back(0x80U | 0x7FU);

  // Provide a large number of continuation bytes with MSB set to 1 to force
  // many iterations in decodeInteger. Each continuation byte has low 7 bits
  // set to 0x7F to maximize contributions. This should eventually overflow
  // the multiplier (multiplier *= 128) when multiplier exceeds 64-bit range.
  for (int i = 0; i < 200; ++i) {
    encoded.push_back(0xFF);  // 0x80 | 0x7F
  }

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeIndexedHeaderFieldIndex);
}

TEST(HpackDecoder, DecodeIndexedHeaderInvalidZero) {
  auto decoder = CreateHpackDecoder();

  // Indexed header with explicit zero (invalid): encode varint 0 using prefix bits
  // To get indexResult.index == 0 after decodeInteger, craft a prefix < prefixMask with value 0
  // 0x80 has prefix value 0 -> triggers invalid-index-0 path
  static constexpr uint8_t encoded[]{0x80};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  // Either 'Failed to decode indexed header field index' or 'Invalid index 0 in indexed header field'
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::InvalidIndexZeroInIndexedHeaderField);
}

TEST(HpackDecoder, DecodeIndexedHeaderOutOfBounds) {
  auto decoder = CreateHpackDecoder();

  // Indexed header with an index larger than static + dynamic table
  // Use a small decoder with no dynamic entries and index value 1000 encoded as varint
  vector<uint8_t> encoded;
  // First byte: 0x80 | prefix (7 bits) set to max to indicate continuation
  encoded.push_back(0x80U | 0x7FU);
  // continuation bytes for 1000 - 127 = 873 in base-128 varint
  uint32_t rem = 1000 - 127;
  while (rem >= 128) {
    encoded.push_back(static_cast<uint8_t>((rem & 0x7FU) | 0x80U));
    rem >>= 7U;
  }
  encoded.push_back(static_cast<uint8_t>(rem));

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::IndexOutOfBoundsInIndexedHeaderField);
}

TEST(HpackDecoder, DecodeDynamicTableSizeUpdateIncomplete) {
  auto decoder = CreateHpackDecoder();

  // Dynamic table size update prefix 001xxxxx (0x20). Use 0x3f (prefix all ones) and no continuation
  static constexpr uint8_t encoded[]{0x3f};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeDynamicTableSizeUpdate);
}

TEST(HpackDecoder, DecodeLiteralHeaderIndexIncomplete) {
  auto decoder = CreateHpackDecoder();

  // Literal header field without indexing (prefix 0000) uses 4-bit prefix for index.
  // Provide a byte where the lower 4 bits are all ones -> requires continuation, but none provided
  static constexpr uint8_t encoded[]{0x0F};

  auto result = decoder.decode(AsBytes(encoded));
  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderIndex);
}

TEST(HpackDecoder, DecodeLiteralHeaderNameOutOfBounds) {
  auto decoder = CreateHpackDecoder();

  // Literal header field with incremental indexing (01xxxxxx). Use the
  // 6-bit prefix with all ones to force varint continuation for the name index.
  vector<uint8_t> encoded;
  encoded.push_back(0x40U | 0x3FU);

  // Choose an index far beyond static + dynamic table sizes (e.g., 1000).
  // For prefix max 63, varint encodes (1000 - 63).
  uint32_t rem = 1000 - 63;
  while (rem >= 128) {
    encoded.push_back(static_cast<uint8_t>((rem & 0x7FU) | 0x80U));
    rem >>= 7U;
  }
  encoded.push_back(static_cast<uint8_t>(rem));

  // No name/value bytes are required because lookup should fail on name index.
  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_FALSE(result.isSuccess());
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::IndexOutOfBoundsForHeaderName);
}

TEST(HpackDecoder, SetMaxDynamicTableSize) {
  auto decoder = CreateHpackDecoder();

  // Add two entries via literal-with-indexing encoded blocks
  vector<uint8_t> encoded1 = {
      0x40, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
      'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e',
  };

  vector<uint8_t> encoded2 = {0x40, 0x04, 'h', 'e', 'a', 'd', 0x05, 'v', 'a', 'l', 'u', 'e'};

  auto r1 = decoder.decode(AsBytes(encoded1));
  EXPECT_TRUE(r1.isSuccess());
  auto r2 = decoder.decode(AsBytes(encoded2));
  EXPECT_TRUE(r2.isSuccess());

  EXPECT_EQ(decoder.dynamicTable().entryCount(), 2U);

  // Now reduce the max dynamic table size to force eviction
  decoder.setMaxDynamicTableSize(50);

  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 50U);
  EXPECT_LE(decoder.dynamicTable().currentSizeBytes(), 50U);
  EXPECT_LT(decoder.dynamicTable().entryCount(), 2U);
}

TEST(HpackDecoder, ClearDecodedStrings) {
  auto decoder = CreateHpackDecoder();

  // Use an encoded literal-with-indexing block to populate decoded strings
  static constexpr uint8_t encoded[]{
      0x40, 0x0a, 'c', 'u', 's', 't', 'o', 'm', '-', 'k', 'e', 'y', 0x0c,
      'c',  'u',  's', 't', 'o', 'm', '-', 'v', 'a', 'l', 'u', 'e',
  };

  auto res1 = decoder.decode(AsBytes(encoded));
  EXPECT_TRUE(res1.isSuccess());
  EXPECT_EQ(res1.decodedHeaders.size(), 1U);
  EXPECT_EQ(res1.decodedHeaders.begin()->first, "custom-key");

  auto res2 = decoder.decode(AsBytes(encoded));
  EXPECT_TRUE(res2.isSuccess());
  EXPECT_EQ(res2.decodedHeaders.size(), 1U);
  EXPECT_EQ(res2.decodedHeaders.begin()->first, "custom-key");
}

TEST(HpackDecoder, IndexedNameSelfEviction) {
  // Table just large enough for a single small entry: the slightest addition
  // will force its eviction.
  HpackDecoder decoder(40, true);  // kHpackOverhead(32) + "ck"(2) + "v1"(2) = 36 <= 40

  // 1) New literal-with-indexing entry: "ck" -> "v1"
  static constexpr uint8_t firstBlock[]{0x40, 0x02, 'c', 'k', 0x02, 'v', '1'};
  auto firstResult = decoder.decode(AsBytes(firstBlock));
  ASSERT_TRUE(firstResult.isSuccess());
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 1U);

  // 2) Literal header, INDEXED NAME (index 62 = "ck", the entry we just added),
  // new value "v2", WITH indexing. The name therefore comes from the entry that will
  // have to be evicted (only one entry in the table -> it must be it,
  // and it is reused as a buffer, not just freed) to make room
  // for the new entry "ck"->"v2". Byte 1: 0x40 | 62 = 0x7E.
  static constexpr uint8_t secondBlock[]{0x7E, 0x02, 'v', '2'};
  auto secondResult = decoder.decode(AsBytes(secondBlock));

  EXPECT_TRUE(secondResult.isSuccess());
  ASSERT_EQ(secondResult.decodedHeaders.size(), 1U);
  EXPECT_EQ(secondResult.decodedHeaders.begin()->first, "ck");
  EXPECT_EQ(secondResult.decodedHeaders.begin()->second, "v2");
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 1U);
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

TEST(HpackEncoder, FindSmallHeaderInDynamicTable) {
  HpackEncoder encoder(4096);

  // Add a custom header
  RawBytes output;
  encoder.encode(output, "hd", "custom-value");

  // Should be found in dynamic table (index 62)
  auto result = encoder.findHeader("hd", "custom-value");
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
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0xE0U, 0x20);
}

TEST(HpackEncoder, EncodeNeverIndexed_WithStaticName) {
  HpackEncoder encoder(4096);
  RawBytes output;

  // Use a known static name ':method' but different value -> name-only match
  encoder.encode(output, ":method", "PUT", HpackEncoder::IndexingMode::NeverIndexed);

  // First byte should have 0001xxxx pattern (0x10)
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0xF0U, 0x10U);
}

TEST(HpackEncoder, EncodeNeverIndexed_NewName) {
  HpackEncoder encoder(4096);
  RawBytes output;

  encoder.encode(output, "x-new-name", "v", HpackEncoder::IndexingMode::NeverIndexed);

  // First byte should be 0x10 when name is new (literal name encoded)
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0xF0U, 0x10);
}

TEST(HpackEncoder, EncodeWithoutIndexing_WithStaticName) {
  HpackEncoder encoder(4096);
  RawBytes output;

  // Known static name ':method' with a different value should use name-only without indexing
  encoder.encode(output, ":method", "PUT", HpackEncoder::IndexingMode::WithoutIndexing);

  // First byte should have 0000xxxx pattern (0x00)
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0xF0U, 0x00);
}

TEST(HpackEncoder, EncodeWithoutIndexing_NewName) {
  HpackEncoder encoder(4096);
  RawBytes output;

  encoder.encode(output, "x-new-name", "v", HpackEncoder::IndexingMode::WithoutIndexing);

  // First byte should be 0x00 when name is new (literal name encoded)
  EXPECT_EQ(static_cast<uint8_t>(output[0]) & 0xF0U, 0x00);
}

// ============================
// Round-trip Tests
// ============================

TEST(HpackRoundTrip, SimpleHeaders) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();

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
  auto decoder = CreateHpackDecoder();

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
  auto decoder = CreateHpackDecoder();

  static constexpr std::string_view kDate = "Thu, 01 Jan 1970 00:00:00 GMT";
  static_assert(kDate.size() == RFC7231DateStrLen);

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
  auto decoder = CreateHpackDecoder();

  const std::array<char, RFC7231DateStrLen> dateBuf = [] {
    std::array<char, RFC7231DateStrLen> buf{};
    (void)TimeToStringRFC7231(SysClock::now(), buf.data());
    return buf;
  }();

  const std::string_view dateSv{dateBuf.data(), RFC7231DateStrLen};
  ASSERT_EQ(dateSv.size(), RFC7231DateStrLen);

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
  EXPECT_EQ(dateIt->second.size(), RFC7231DateStrLen);
  EXPECT_TRUE(dateIt->second.ends_with("GMT"));
}

TEST(HpackRoundTrip, ResponseHeaderSetIncludesDate) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();

  const std::array<char, RFC7231DateStrLen> dateBuf = [] {
    std::array<char, RFC7231DateStrLen> buf;
    (void)TimeToStringRFC7231(SysClock::now(), buf.data());
    return buf;
  }();

  const std::string_view dateSv{dateBuf.data(), RFC7231DateStrLen};

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
  EXPECT_EQ(dateIt->second.size(), RFC7231DateStrLen);
  EXPECT_TRUE(dateIt->second.ends_with("GMT"));
}

TEST(HpackDecoderFuzz, RandomizedReserveFuzz) {
  using namespace aeronet::http2;

  auto decoder = CreateHpackDecoder();

  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937_64 rng(123456789);  // deterministic seed for reproducibility
  std::uniform_int_distribution<int> byteDist(0, 255);

  static constexpr int iterations = 59;
  static constexpr std::size_t kMaxLen = 1UL << 20U;  // 1 MiB

  static constexpr int kStep = kMaxLen / iterations;

  vector<uint8_t> buf;
  buf.reserve(kMaxLen);

  for (std::size_t len = 7; len < kMaxLen; len += kStep) {
    while (buf.size() < len) {
      buf.push_back(static_cast<uint8_t>(byteDist(rng)));
    }

    // Run decode; ensure it doesn't crash and returns a well-formed DecodeResult.
    // Any decode error is acceptable but must provide a non-null, non-empty error message.
    auto res = decoder.decode(AsBytes(buf));

    ASSERT_TRUE(res.isSuccess() || (res.error != HpackDecoder::DecodeResult::Error::None));
  }
}

// ============================
// Huffman Coverage Tests
// ============================
// The decoder pads its bit window with 1s once the input is exhausted so the fast table can serve the last symbol of a
// string. Since the RFC 7541 padding is itself a run of 1s (the EOS prefix), a tail of padding always looks like the
// start of EOS. These tests pin down that a valid stream is never mistaken for one containing EOS, whatever the number
// of leftover bits, and that every code length decodes.

namespace {

// Round-trip 'value' as a header value through the encoder and decoder and return what came back.
std::string HuffmanRoundTrip(std::string_view value) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();
  RawBytes encoded;
  encoder.encode(encoded, "x-rt", value);
  const auto result = decoder.decode(encoded);
  if (!result.isSuccess()) {
    return "<decode-failed>";
  }
  const auto it = result.decodedHeaders.find("x-rt");
  if (it == result.decodedHeaders.end()) {
    return "<missing>";
  }
  return std::string(it->second);
}

}  // namespace

TEST(HpackHuffman, RoundTripsEveryPaddingRemainder) {
  // 'a' is a 5-bit code, so growing a run of 'a's walks the leftover-bit count through all of 0..7 and every string
  // here ends on padding that is a prefix of EOS.
  for (std::size_t len = 1; len <= 64; ++len) {
    const std::string value(len, 'a');
    EXPECT_EQ(HuffmanRoundTrip(value), value) << "length " << len;
  }
}

TEST(HpackHuffman, RoundTripsEveryByteValue) {
  // Covers all 257 code lengths, including the 9..30 bit codes that bypass the fast table.
  for (int ch = 0; ch < 256; ++ch) {
    const std::string value(4, static_cast<char>(ch));

    if (ch == '\0' || ch == '\n' || ch == '\r') {
      // NUL, LF and CR are legal Huffman symbols - LF and CR in fact carry the
      // two longest codes in the whole table (30 bits), and NUL a 13-bit code,
      // so these three specifically exercise the tier-2 canonical decode path.
      // But RFC 9113 §8.2.1 makes a field VALUE containing any of them
      // malformed, so decode() must reject the header. Checking the exact
      // error message distinguishes "codec decoded fine, semantic check
      // caught it" (Malformed field value) from "codec itself is broken"
      // (Failed to decode literal header value) - which is what still lets
      // this loop assert Huffman correctness for these three without
      // expecting a successful round trip.
      HpackEncoder encoder(4096);
      auto decoder = CreateHpackDecoder();
      RawBytes encoded;
      encoder.encode(encoded, "x-rt", value);
      const auto result = decoder.decode(encoded);

      EXPECT_FALSE(result.isSuccess()) << "byte " << ch;
      EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::MalformedFieldValue) << "byte " << ch;
      continue;
    }

    EXPECT_EQ(HuffmanRoundTrip(value), value) << "byte " << ch;
  }
}

TEST(HpackHuffman, RoundTripsCharactersNeedingLongCodes) {
  // Every printable ASCII character whose code exceeds the fast-table window.
  static constexpr std::string_view kLongCoded = R"(!"#$'()+<>?@[\]^`{|}~)";
  EXPECT_EQ(HuffmanRoundTrip(kLongCoded), kLongCoded);
  for (char ch : kLongCoded) {
    const std::string value(1, ch);
    EXPECT_EQ(HuffmanRoundTrip(value), value) << "char " << ch;
  }
}

TEST(HpackHuffman, RoundTripsMixedLengthStrings) {
  static constexpr std::array<std::string_view, 8> kValues{
      "a",
      "ab",
      "abc",  // encodes to exactly 2 bytes, leaving zero padding bits
      "application/json",
      "Mon, 01 Jan 2024 00:00:00 GMT",
      R"(W/"abc123")",
      "https://bench.example.com/dashboard?q=1&r=2",
      "\x01\x7f\xff mixed \xc3\xa9",
  };
  for (auto value : kValues) {
    EXPECT_EQ(HuffmanRoundTrip(value), value) << "value " << value;
  }
}

}  // namespace aeronet::http2
