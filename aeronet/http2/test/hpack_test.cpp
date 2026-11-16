#include "aeronet/hpack.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/city-hash.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-view.hpp"
#include "aeronet/is-header-whitespace.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/sv-to-sv-map.hpp"
#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {

struct HpackEncoderTestAccess {
  static void redirectFullLookup(HpackEncoder& encoder, std::string_view name, std::string_view value,
                                 std::size_t tableIdx) {
    const auto nameHash = CityHash{}(name);
    encoder._dynamicIndex[HpackEncoder::dynamicFullHash(nameHash, value)] = encoder._newestDynamicSerial - tableIdx;
  }

  static void redirectNameLookup(HpackEncoder& encoder, std::string_view name, std::size_t tableIdx) {
    const auto nameHash = CityHash{}(name);
    encoder._dynamicIndex[HpackEncoder::dynamicNameHash(nameHash)] = encoder._newestDynamicSerial - tableIdx;
  }
};

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

TEST(HpackDynamicTable, AddWithTheNameOfTheEvictedEntry) {
  // A literal field with incremental indexing may take its name from the oldest entry, evicted by its own insertion,
  // whose buffer is reused for the new entry: large enough, or reallocated.
  for (const std::size_t valueSize : {1U, 20U}) {
    HpackDynamicTable table(200);
    ASSERT_TRUE(table.add("x-oldest", "abcdef"));              // 46 bytes
    ASSERT_TRUE(table.add("x-newer", std::string(100, 'n')));  // 139 bytes
    const std::string value(valueSize, 'v');

    ASSERT_TRUE(table.add(table[1].name(), value));

    ASSERT_EQ(table.entryCount(), 2U);
    EXPECT_EQ(table[0].name(), "x-oldest");
    EXPECT_EQ(table[0].value(), value);
    EXPECT_EQ(table[1].name(), "x-newer");
    EXPECT_EQ(table.currentSizeBytes(), 139U + 8U + valueSize + 32U);
  }
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

HpackDecoder CreateHpackDecoder() { return {4096, std::numeric_limits<uint32_t>::max(), true}; }

// Append the representation of an integer (RFC 7541 §5.1), whose first byte starts with the bits of 'flags'.
void AppendInteger(vector<uint8_t>& out, uint8_t flags, uint8_t prefixBits, uint64_t value) {
  const uint8_t prefixMask = static_cast<uint8_t>((1U << prefixBits) - 1U);
  if (value < prefixMask) {
    out.push_back(static_cast<uint8_t>(flags | value));
    return;
  }
  out.push_back(static_cast<uint8_t>(flags | prefixMask));
  for (value -= prefixMask; value >= 128U; value >>= 7U) {
    out.push_back(static_cast<uint8_t>((value & 0x7FU) | 0x80U));
  }
  out.push_back(static_cast<uint8_t>(value));
}

}  // namespace

TEST(HpackDecoder, DecodeIndexedHeader) {
  auto decoder = CreateHpackDecoder();

  // 0x82 = indexed header field, index 2 (:method: GET)
  static constexpr uint8_t encoded[]{0x82};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  EXPECT_TRUE(result.decodedHeaders.headers.empty());
  EXPECT_TRUE(result.decodedHeaders.hasPseudoHeader(PseudoHeader::Method));
  EXPECT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Method), "GET");
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
  EXPECT_EQ(result.decodedHeaders.headers.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.headers.begin()->first, "custom-key");
  EXPECT_EQ(result.decodedHeaders.headers.begin()->second, "custom-value");

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
  ASSERT_EQ(result.decodedHeaders.headers.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.headers.begin()->first, "x-my_header!");
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

// RFC 9113 §8.2.1: a field value must not start or end with SP or HTAB. Such a value would be silently altered when
// translated to HTTP/1.1 (HTTP Garden "bonus bonus" bug #1: whitespace not stripped in HTTP/2 to HTTP/1.1 downgrades).
TEST(HpackDecoder, RejectsLeadingOrTrailingWhitespaceInValue) {
  for (std::string_view value : {"\ta\t", " a", "a ", "\ta", "a\t", " ", "\t"}) {
    HpackEncoder encoder(4096);
    auto decoder = CreateHpackDecoder();
    RawBytes encoded;
    encoder.encode(encoded, "test1", value);

    const auto result = decoder.decode(encoded);

    EXPECT_FALSE(result.isSuccess()) << '[' << value << ']';
    EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::MalformedFieldValue) << '[' << value << ']';
  }
  // pseudo-header values are field values too
  {
    HpackEncoder encoder(4096);
    auto decoder = CreateHpackDecoder();
    RawBytes encoded;
    encoder.encode(encoded, ":path", "/ ");
    EXPECT_EQ(decoder.decode(encoded).error, HpackDecoder::DecodeResult::Error::MalformedFieldValue);
  }
  // inner whitespace and empty values are fine
  for (std::string_view value : {"a b", "a\tb", ""}) {
    HpackEncoder encoder(4096);
    auto decoder = CreateHpackDecoder();
    RawBytes encoded;
    encoder.encode(encoded, "test1", value);

    const auto result = decoder.decode(encoded);

    ASSERT_TRUE(result.isSuccess()) << '[' << value << ']';
    EXPECT_EQ(result.decodedHeaders.headers.find("test1")->second, value);
  }
}

TEST(HpackDecoder, AcceptsPseudoHeaderViaFullIndex) {
  auto decoder = CreateHpackDecoder();

  // Indexed Header Field, static table index 2 = ":method" / "GET"
  static constexpr uint8_t encoded[]{0x82U};

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  EXPECT_TRUE(result.decodedHeaders.headers.empty());
  EXPECT_EQ(result.decodedHeaders.pseudoHeadersMask, DecodedHeaders::PseudoHeaderBit(PseudoHeader::Method));
  EXPECT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Method), "GET");
  EXPECT_EQ(result.decodedHeaders.namesAndValuesSize, std::string_view(":methodGET").size());
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
  ASSERT_TRUE(result.decodedHeaders.hasPseudoHeader(PseudoHeader::Protocol));
  EXPECT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Protocol), "websocket");
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
  EXPECT_EQ(result.decodedHeaders.headers.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.headers.begin()->first, "custom-key");
  EXPECT_EQ(result.decodedHeaders.headers.begin()->second, "custom-value");

  // Should NOT be added to dynamic table
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 0);
}

TEST(HpackDecoder, DecodeMultipleHeaders) {
  auto decoder = CreateHpackDecoder();

  // :method: GET (0x82) + :path: / (0x84) + :scheme: https (0x87)
  static constexpr uint8_t encoded[]{0x82, 0x84, 0x87};

  auto result = decoder.decode(AsBytes(encoded));

  const auto& decodedHeaders = result.decodedHeaders;

  EXPECT_TRUE(result.isSuccess());
  EXPECT_TRUE(decodedHeaders.headers.empty());
  EXPECT_EQ(decodedHeaders.pseudoHeader(PseudoHeader::Method), "GET");
  EXPECT_EQ(decodedHeaders.pseudoHeader(PseudoHeader::Path), "/");
  EXPECT_EQ(decodedHeaders.pseudoHeader(PseudoHeader::Scheme), "https");
  EXPECT_FALSE(decodedHeaders.hasPseudoHeader(PseudoHeader::Authority));
  EXPECT_TRUE(decodedHeaders.pseudoHeader(PseudoHeader::Authority).empty());
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
  ASSERT_EQ(result.decodedHeaders.headers.size(), 1U);
  const auto it = result.decodedHeaders.headers.find("accept");
  ASSERT_NE(it, result.decodedHeaders.headers.end());
  EXPECT_EQ(it->second, "a,b");
}

TEST(HpackDecoder, DuplicateCookieMergesWithSemicolon) {
  auto decoder = CreateHpackDecoder();

  // Two Cookie headers are joined with "; " (RFC 9113 §8.2.3)
  static constexpr uint8_t encoded[]{
      0x40, 0x06, 'c', 'o', 'o', 'k', 'i', 'e', 0x03, 'o', 'n', 'e',
      0x40, 0x06, 'c', 'o', 'o', 'k', 'i', 'e', 0x03, 't', 'w', 'o',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.headers.size(), 1U);
  const auto it = result.decodedHeaders.headers.find("cookie");
  ASSERT_NE(it, result.decodedHeaders.headers.end());
  EXPECT_EQ(it->second, "one; two");
}

// Repeated Set-Cookie fields of a response cannot be joined with commas, which their Expires dates hold: the decoder of
// a client joins them with a line feed, which no field value can hold, for the client to split them back.
TEST(HpackDecoder, DuplicateSetCookieOfResponsesMergesWithLineFeed) {
  static constexpr uint8_t encoded[]{
      0x40, 0x0a, 's',  'e',  't', '-', 'c', 'o', 'o', 'k', 'i', 'e', 0x07, 'a', '=',  '1', ',', ' ',
      'x',  'y',  0x40, 0x0a, 's', 'e', 't', '-', 'c', 'o', 'o', 'k', 'i',  'e', 0x03, 'b', '=', '2',
  };

  HpackDecoder responseDecoder(4096, std::numeric_limits<uint32_t>::max(), true, HpackDecoder::Messages::Responses);
  auto result = responseDecoder.decode(AsBytes(encoded));
  ASSERT_TRUE(result.isSuccess());
  auto it = result.decodedHeaders.headers.find("set-cookie");
  ASSERT_NE(it, result.decodedHeaders.headers.end());
  EXPECT_EQ(it->second, "a=1, xy\nb=2");

  // The decoder of a server keeps the rules of the requests.
  auto requestDecoder = CreateHpackDecoder();
  auto requestResult = requestDecoder.decode(AsBytes(encoded));
  ASSERT_TRUE(requestResult.isSuccess());
  it = requestResult.decodedHeaders.headers.find("set-cookie");
  ASSERT_NE(it, requestResult.decodedHeaders.headers.end());
  EXPECT_EQ(it->second, "a=1, xy,b=2");
}

TEST(HpackDecoder, ContentEncodingValueIsLowerCased) {
  auto decoder = CreateHpackDecoder();

  // content-encoding is static table index 26. Its codings are case-insensitive: they are lower-cased when decoded
  // (including merged repeated fields), whereas other values keep their case.
  static constexpr uint8_t encoded[]{
      0x5A, 0x04, 'G',  'Z', 'I', 'P',                                           // content-encoding: GZIP (indexed)
      0x0F, 0x0B, 0x02, 'B', 'r',                                                // content-encoding: Br (not indexed)
      0x40, 0x06, 'x',  '-', 'c', 'a', 's', 'e', 0x05, 'M', 'i', 'X', 'e', 'D',  // x-case: MiXeD (indexed)
  };

  auto result = decoder.decode(AsBytes(encoded));
  ASSERT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.headers.size(), 2U);
  const auto encodingIt = result.decodedHeaders.headers.find("content-encoding");
  ASSERT_NE(encodingIt, result.decodedHeaders.headers.end());
  EXPECT_EQ(encodingIt->second, "gzip,br");
  const auto caseIt = result.decodedHeaders.headers.find("x-case");
  ASSERT_NE(caseIt, result.decodedHeaders.headers.end());
  EXPECT_EQ(caseIt->second, "MiXeD");

  // The dynamic table keeps the field as received, and each decoded reference to it is lower-cased.
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 2U);
  EXPECT_EQ(decoder.dynamicTable()[1].value(), "GZIP");
  static constexpr uint8_t indexed[]{0xBF};  // dynamic index 63 (oldest entry): content-encoding: GZIP

  const auto indexedResult = decoder.decode(AsBytes(indexed));
  ASSERT_TRUE(indexedResult.isSuccess());
  ASSERT_EQ(indexedResult.decodedHeaders.headers.size(), 1U);
  EXPECT_EQ(indexedResult.decodedHeaders.headers.begin()->first, "content-encoding");
  EXPECT_EQ(indexedResult.decodedHeaders.headers.begin()->second, "gzip");
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

TEST(HpackDecoder, DuplicateSingletonHeaderIsRejected) {
  // authorization (static name index 23) is a singleton field: its duplicates are rejected, like for HTTP/1.1, instead
  // of keeping one of them (a proxy in front may keep another one).
  {
    auto decoder = CreateHpackDecoder();
    static constexpr uint8_t encoded[]{
        0x57, 0x01, 'a',  // authorization: a
        0x57, 0x01, 'b',  // authorization: b
    };
    const auto result = decoder.decode(AsBytes(encoded));
    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::DuplicateHeaderForbiddenToMerge);
  }
  {
    // content-type (static name index 31), the first one being empty
    auto decoder = CreateHpackDecoder();
    static constexpr uint8_t encoded[]{
        0x5F, 0x00,                 // content-type: (empty)
        0x5F, 0x03, 't', 'e', 'x',  // content-type: tex
    };
    const auto result = decoder.decode(AsBytes(encoded));
    EXPECT_FALSE(result.isSuccess());
    EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::DuplicateHeaderForbiddenToMerge);
  }
}

namespace {

RawBytes EncodeLiteralField(std::string_view name, std::string_view value) {
  HpackEncoder encoder(4096);
  RawBytes encoded;
  encoder.encode(encoded, name, value, HpackEncoder::IndexingMode::WithoutIndexing);
  return encoded;
}

}  // namespace

// RFC 9113 §8.2.2: the request is malformed, which does not concern the compression state of the connection.
TEST(HpackDecoder, ConnectionSpecificRequestFieldsAreRejected) {
  for (std::string_view name : {"connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade"}) {
    for (std::string_view value : {"close", ""}) {
      auto decoder = CreateHpackDecoder();
      const RawBytes encoded = EncodeLiteralField(name, value);

      const auto result = decoder.decode(encoded);

      EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::ConnectionSpecificField) << name << ": " << value;
      EXPECT_FALSE(result.isCompressionError()) << name;
    }
  }
}

TEST(HpackDecoder, TeRequestFieldOnlyAcceptsTrailers) {
  for (std::string_view value : {"trailers", "Trailers", "TRAILERS"}) {
    auto decoder = CreateHpackDecoder();
    const RawBytes encoded = EncodeLiteralField("te", value);

    const auto result = decoder.decode(encoded);

    ASSERT_TRUE(result.isSuccess()) << value;
    EXPECT_EQ(result.decodedHeaders.headers.find("te")->second, value);
  }
  for (std::string_view value : {"", "gzip", "trailer", "trailerz", "trailers,gzip", "trailers, deflate;q=0.5"}) {
    auto decoder = CreateHpackDecoder();
    const RawBytes encoded = EncodeLiteralField("te", value);

    EXPECT_EQ(decoder.decode(encoded).error, HpackDecoder::DecodeResult::Error::ConnectionSpecificField) << value;
  }
}

TEST(HpackDecoder, FieldsWithTheLengthOfAConnectionSpecificOneAreAccepted) {
  for (std::string_view name : {"tf", "referer", "user-agent", "proxy-connectiom", "transfer-encodinh"}) {
    auto decoder = CreateHpackDecoder();
    const RawBytes encoded = EncodeLiteralField(name, "value");

    EXPECT_TRUE(decoder.decode(encoded).isSuccess()) << name;
  }
}

TEST(HpackDecoder, ConnectionSpecificRequestFieldsAreRejectedFromTheTables) {
  {
    auto decoder = CreateHpackDecoder();
    static constexpr uint8_t indexed[]{0xB9};  // transfer-encoding: (empty), index 57 of the static table
    EXPECT_EQ(decoder.decode(AsBytes(indexed)).error, HpackDecoder::DecodeResult::Error::ConnectionSpecificField);
  }

  auto decoder = CreateHpackDecoder();
  // transfer-encoding: chunked, with incremental indexing and a name from the static table
  static constexpr uint8_t literal[]{0x79, 0x07, 'c', 'h', 'u', 'n', 'k', 'e', 'd'};
  EXPECT_EQ(decoder.decode(AsBytes(literal)).error, HpackDecoder::DecodeResult::Error::ConnectionSpecificField);

  // The dynamic table is updated all the same (RFC 9113 §4.3), and a reference to this entry is rejected as well.
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 1U);
  EXPECT_EQ(decoder.dynamicTable()[0].name(), "transfer-encoding");
  static constexpr uint8_t reference[]{0xBE};  // index 62: the dynamic table entry
  EXPECT_EQ(decoder.decode(AsBytes(reference)).error, HpackDecoder::DecodeResult::Error::ConnectionSpecificField);
}

// The client reports such a response as malformed: the decoder only tells it, for the block that holds the field.
TEST(HpackDecoder, ConnectionSpecificFieldsOfResponsesAreRecorded) {
  for (std::string_view name : {"connection", "keep-alive", "proxy-connection", "transfer-encoding", "upgrade", "te"}) {
    HpackDecoder decoder(4096, std::numeric_limits<uint32_t>::max(), true, HpackDecoder::Messages::Responses);

    const auto result = decoder.decode(EncodeLiteralField(name, "value"));

    ASSERT_TRUE(result.isSuccess()) << name;
    EXPECT_TRUE(result.decodedHeaders.hasConnectionSpecificField) << name;

    const auto nextResult = decoder.decode(EncodeLiteralField("x-field", "value"));

    ASSERT_TRUE(nextResult.isSuccess()) << name;
    EXPECT_FALSE(nextResult.decodedHeaders.hasConnectionSpecificField) << name;
  }

  HpackDecoder decoder(4096, std::numeric_limits<uint32_t>::max(), true, HpackDecoder::Messages::Responses);
  EXPECT_FALSE(decoder.decode(EncodeLiteralField("te", "trailers")).decodedHeaders.hasConnectionSpecificField);
}

TEST(HpackDecoder, DuplicateWithEmptyValueAddsNoSeparator) {
  auto decoder = CreateHpackDecoder();

  // accept (static name index 19), repeated with empty values around a non-empty one.
  static constexpr uint8_t encoded[]{
      0x53, 0x00,       // accept: (empty)
      0x53, 0x01, 'x',  // accept: x
      0x53, 0x00,       // accept: (empty)
      0x53, 0x01, 'y',  // accept: y
  };

  const auto result = decoder.decode(AsBytes(encoded));

  ASSERT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.headers.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.headers.at("accept"), "x,y");
  EXPECT_EQ(result.decodedHeaders.namesAndValuesSize, (4U * std::string_view("accept").size()) + 2U);
}

TEST(HpackDecoder, StaticTableFieldsAreNotCopied) {
  auto decoder = CreateHpackDecoder();

  // :method: GET, :scheme: https, :authority (empty), accept-encoding: gzip, deflate - all from the static table.
  static constexpr uint8_t staticOnly[]{0x82, 0x87, 0x81, 0x90};

  const auto result = decoder.decode(AsBytes(staticOnly));

  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.storage.capacity(), 0U);  // nothing was written to the storage
  EXPECT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Method), "GET");
  EXPECT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Scheme), "https");
  // Present, with an empty value.
  EXPECT_TRUE(result.decodedHeaders.hasPseudoHeader(PseudoHeader::Authority));
  EXPECT_TRUE(result.decodedHeaders.pseudoHeader(PseudoHeader::Authority).empty());
  EXPECT_EQ(result.decodedHeaders.headers.at("accept-encoding"), "gzip, deflate");

  // :path: / from the static table is copied: the server decodes it in place.
  static constexpr uint8_t path[]{0x84};
  const auto pathResult = decoder.decode(AsBytes(path));
  ASSERT_TRUE(pathResult.isSuccess());
  EXPECT_NE(pathResult.decodedHeaders.storage.capacity(), 0U);
  const std::string_view pathValue = pathResult.decodedHeaders.pseudoHeader(PseudoHeader::Path);
  ASSERT_EQ(pathValue, "/");
  const_cast<char*>(pathValue.data())[0] = '/';  // writable
}

TEST(HpackDecoder, TakenOverHeadersOutliveTheNextDecodes) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();

  const std::string token(200, 't');  // Huffman-encoded by the encoder, then referenced from the dynamic table

  // The second block references the dynamic table entries added by the first one.
  RawBytes blocks[2];
  for (RawBytes& block : blocks) {
    encoder.encode(block, ":method", "GET");
    encoder.encode(block, ":path", "/api/items?id=42");
    encoder.encode(block, "x-token", token);
    encoder.encode(block, "x-trace", "abc");
  }
  EXPECT_LT(blocks[1].size(), blocks[0].size());

  // Owners of the decoded headers, like the requests of a server.
  struct Owner {
    SvToSvMap headers;
    DecodedHeaders::Storage storage{DecodedHeaders::kStorageInitialCapacity};
    std::string_view path;
  };
  Owner owners[2];
  for (std::size_t idx = 0; idx < std::size(blocks); ++idx) {
    const auto result = decoder.decode(blocks[idx]);
    ASSERT_TRUE(result.isSuccess());
    owners[idx].path = result.decodedHeaders.pseudoHeader(PseudoHeader::Path);
    owners[idx].headers.swap(result.decodedHeaders.headers);
    owners[idx].storage.swap(result.decodedHeaders.storage);
    // The decoder got the empty map and storage of the owner instead.
    EXPECT_TRUE(result.decodedHeaders.headers.empty());
    EXPECT_EQ(result.decodedHeaders.storage.capacity(), 0U);
  }

  // Evict the entries of the dynamic table, then decode a block with other values in the storage of the decoder.
  decoder.setMaxDynamicTableSize(0);
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 0U);
  HpackEncoder otherEncoder(0);
  RawBytes other;
  otherEncoder.encode(other, ":path", "/zzzzzzzzzzzzzzzz");
  otherEncoder.encode(other, "x-token", std::string(200, 'z'));
  const auto otherResult = decoder.decode(other);
  ASSERT_TRUE(otherResult.isSuccess());
  EXPECT_EQ(otherResult.decodedHeaders.headers.at("x-token"), std::string(200, 'z'));

  for (const Owner& owner : owners) {
    EXPECT_EQ(owner.path, "/api/items?id=42");
    ASSERT_EQ(owner.headers.size(), 2U);
    EXPECT_EQ(owner.headers.at("x-token"), token);
    EXPECT_EQ(owner.headers.at("x-trace"), "abc");
  }
}

TEST(HpackDecoder, HeaderListBeyondTheLimitIsNotStored) {
  static constexpr uint32_t kMaxHeaderListSize = 8192;
  HpackDecoder decoder(4096, kMaxHeaderListSize, true);
  HpackEncoder encoder(4096);

  // A 4000-byte field added to the dynamic table, then referenced by each of the next 1000 bytes of the block: a 4 KB
  // block for a 4 MB header list, whose repeated values, joined as they came, took gigabytes to store.
  const std::string value(4000, 'v');
  RawBytes block;
  encoder.encode(block, "x-a", value);
  ASSERT_EQ(encoder.dynamicTable().entryCount(), 1U);
  for (int idx = 0; idx < 1000; ++idx) {
    block.push_back(std::byte{0xBE});  // indexed field 62: x-a
  }

  const auto result = decoder.decode(block);

  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::HeaderListTooLarge);
  EXPECT_FALSE(result.isCompressionError());
  EXPECT_EQ(result.headerListSize, 1001U * (std::string_view("x-a").size() + value.size() + 32U));
  // Only the fields within the limit were stored.
  EXPECT_LE(result.decodedHeaders.storage.capacity(), 8U * kMaxHeaderListSize);

  // The dynamic table stays in sync with the encoder: the next block can reference the field.
  static constexpr uint8_t reference[]{0xBE};
  const auto next = decoder.decode(AsBytes(reference));
  ASSERT_TRUE(next.isSuccess());
  EXPECT_EQ(next.decodedHeaders.headers.at("x-a"), value);
}

TEST(HpackDecoder, HeaderListAtTheLimitIsAccepted) {
  static constexpr uint32_t kMaxHeaderListSize = 100;
  HpackDecoder decoder(4096, kMaxHeaderListSize, true);
  HpackEncoder encoder(4096);
  static constexpr auto kWithoutIndexing = HpackEncoder::IndexingMode::WithoutIndexing;

  // name + value + 32 bytes: exactly the limit, then one byte more.
  RawBytes atLimit;
  encoder.encode(atLimit, "x", std::string(kMaxHeaderListSize - 1U - 32U, 'a'), kWithoutIndexing);
  const auto atLimitResult = decoder.decode(atLimit);
  ASSERT_TRUE(atLimitResult.isSuccess());
  EXPECT_EQ(atLimitResult.headerListSize, kMaxHeaderListSize);
  EXPECT_EQ(atLimitResult.decodedHeaders.headers.at("x").size(), kMaxHeaderListSize - 1U - 32U);

  RawBytes overLimit;
  encoder.encode(overLimit, "x", std::string(kMaxHeaderListSize - 32U, 'a'), kWithoutIndexing);
  const auto overLimitResult = decoder.decode(overLimit);
  EXPECT_EQ(overLimitResult.error, HpackDecoder::DecodeResult::Error::HeaderListTooLarge);
  EXPECT_TRUE(overLimitResult.decodedHeaders.headers.empty());

  // A malformed field within the limit is reported rather than the size of the list.
  RawBytes malformedFirst;
  encoder.encode(malformedFirst, "x", " a", kWithoutIndexing);  // no leading whitespace (RFC 9113 §8.2.1)
  encoder.encode(malformedFirst, "y", std::string(kMaxHeaderListSize, 'b'), kWithoutIndexing);
  EXPECT_EQ(decoder.decode(malformedFirst).error, HpackDecoder::DecodeResult::Error::MalformedFieldValue);
}

TEST(HpackDecoder, RepeatedFieldsAreJoinedInOrder) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();

  RawBytes block;
  encoder.encode(block, "x-list", "1");
  encoder.encode(block, "cookie", "a=1");
  encoder.encode(block, "x-list", "2");
  encoder.encode(block, "accept", "");
  encoder.encode(block, "cookie", "b=2");
  encoder.encode(block, "accept", "text/html");
  encoder.encode(block, "x-list", "3");
  encoder.encode(block, "accept", "");
  encoder.encode(block, "accept", "*/*");

  const auto result = decoder.decode(block);

  ASSERT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.headers.size(), 3U);
  EXPECT_EQ(result.decodedHeaders.headers.at("x-list"), "1,2,3");
  EXPECT_EQ(result.decodedHeaders.headers.at("cookie"), "a=1; b=2");
  EXPECT_EQ(result.decodedHeaders.headers.at("accept"), "text/html,*/*");
}

TEST(HpackDecoder, ManyRepeatedFieldsAreJoinedOnceInOrder) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();

  // Two fields repeated in turn, 1000 times each: joining each value as it comes would copy the previous ones each
  // time, about 4 MB of copies. The values of each field are joined in their order.
  static constexpr int kNbValues = 1000;
  RawBytes block;
  std::string expected;
  for (int idx = 0; idx < kNbValues; ++idx) {
    const std::string value = std::to_string(idx);
    encoder.encode(block, "x-a", value);
    encoder.encode(block, "x-b", value);
    if (idx != 0) {
      expected.push_back(',');
    }
    expected.append(value);
  }

  const auto result = decoder.decode(block);

  ASSERT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.headers.size(), 2U);
  EXPECT_EQ(result.decodedHeaders.headers.at("x-a"), expected);
  EXPECT_EQ(result.decodedHeaders.headers.at("x-b"), expected);
  EXPECT_LT(result.decodedHeaders.storage.capacity(), 64U << 10U);
}

TEST(HpackDecoder, HuffmanContentEncodingKeepsItsCaseInTheDynamicTable) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();

  static constexpr std::string_view kValue = "GZIP-AND-SOME-MORE-UPPERCASE";
  RawBytes block;
  encoder.encode(block, "content-encoding", kValue);
  // Literal with incremental indexing of static name 26, then its value, Huffman-encoded.
  ASSERT_EQ(static_cast<uint8_t>(block[0]), 0x5AU);
  ASSERT_NE(static_cast<uint8_t>(block[1]) & 0x80U, 0U);

  const auto result = decoder.decode(block);

  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.headers.at("content-encoding"), "gzip-and-some-more-uppercase");
  // The dynamic table keeps the field as received, in sync with the encoder.
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 1U);
  EXPECT_EQ(decoder.dynamicTable()[0].value(), kValue);
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

// RFC 7541 §6.3: a size update beyond the limit of the decoder would let the encoder make the table hold up to 4 GiB.
TEST(HpackDecoder, DynamicTableSizeUpdateAboveTheLimitIsACompressionError) {
  auto decoder = CreateHpackDecoder();
  HpackEncoder encoder;

  RawBytes block;
  encoder.encodeDynamicTableSizeUpdate(block, 0);
  encoder.encodeDynamicTableSizeUpdate(block, 4096U);
  EXPECT_TRUE(decoder.decode(block).isSuccess());
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 4096U);

  for (const uint32_t size : {4097U, 1U << 20U, std::numeric_limits<uint32_t>::max() - 1U}) {
    block.clear();
    encoder.encodeDynamicTableSizeUpdate(block, size);
    encoder.encode(block, "x-filler", std::string(4000U, 'f'));
    const auto result = decoder.decode(block);
    EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::DynamicTableSizeUpdateAboveLimit) << size;
    EXPECT_TRUE(result.isCompressionError());
    EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 4096U);
    EXPECT_EQ(decoder.dynamicTable().entryCount(), 0U);
  }
}

// The length of a string close to 2^32 must not wrap around the check that its bytes are in the block.
TEST(HpackDecoder, StringLongerThanTheBlockIsAnError) {
  for (const uint8_t huffmanFlag : {uint8_t{0}, uint8_t{0x80}}) {
    auto decoder = CreateHpackDecoder();
    // Literal field without indexing with a new name, whose value claims 2^32 - 6 bytes after its 6 length bytes.
    vector<uint8_t> encoded{0x00, 0x01, 'a'};
    AppendInteger(encoded, huffmanFlag, 7, (uint64_t{1} << 32U) - 6U);
    encoded.resize(encoded.size() + 64U, 0x00);  // valid Huffman codes ('0')
    const auto result = decoder.decode(AsBytes(encoded));
    EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderValue) << int{huffmanFlag};
  }
}

// RFC 7541 §5.1: an integer beyond the limits of the implementation is a decoding error, not a truncated value.
TEST(HpackDecoder, IntegerNotFittingIn32BitsIsAnError) {
  using Error = HpackDecoder::DecodeResult::Error;
  static constexpr uint64_t k2Pow32 = uint64_t{1} << 32U;
  struct Case {
    uint8_t flags;
    uint8_t prefixBits;
    uint64_t value;  // Truncated to 32 bits, a valid one
    Error expected;
  };
  for (const Case& testCase : {
           Case{0x80, 7, k2Pow32 + 2U, Error::FailedToDecodeIndexedHeaderFieldIndex},  // :method GET
           Case{0x20, 5, k2Pow32 + 100U, Error::FailedToDecodeDynamicTableSizeUpdate},
           Case{0x40, 6, k2Pow32 + 1U, Error::FailedToDecodeLiteralHeaderIndex},  // :authority
           Case{0x80, 7, k2Pow32 - 1U, Error::FailedToDecodeIndexedHeaderFieldIndex},
       }) {
    auto decoder = CreateHpackDecoder();
    vector<uint8_t> encoded;
    AppendInteger(encoded, testCase.flags, testCase.prefixBits, testCase.value);
    encoded.push_back(0x00);  // an empty value for the literal field
    const auto result = decoder.decode(AsBytes(encoded));
    EXPECT_EQ(result.error, testCase.expected) << testCase.value;
    EXPECT_TRUE(result.isCompressionError());
  }

  // The length of a string.
  auto decoder = CreateHpackDecoder();
  vector<uint8_t> encoded{0x00};  // literal field without indexing, new name
  AppendInteger(encoded, 0x00, 7, k2Pow32 + 1U);
  encoded.push_back('a');
  EXPECT_EQ(decoder.decode(AsBytes(encoded)).error, Error::FailedToDecodeLiteralHeaderName);

  // The largest value.
  encoded.clear();
  AppendInteger(encoded, 0x80, 7, k2Pow32 - 2U);
  EXPECT_EQ(decoder.decode(AsBytes(encoded)).error, Error::IndexOutOfBoundsInIndexedHeaderField);
}

// RFC 7541 §4.2: dynamic table size updates only come at the beginning of a header block.
TEST(HpackDecoder, DynamicTableSizeUpdateAfterAFieldIsACompressionError) {
  auto decoder = CreateHpackDecoder();

  static constexpr uint8_t kAtTheBeginning[]{0x20, 0x3f, 0xe1, 0x1f, 0x82};  // 0, 4096, then :method GET
  const auto success = decoder.decode(AsBytes(kAtTheBeginning));
  EXPECT_TRUE(success.isSuccess());
  EXPECT_EQ(success.decodedHeaders.pseudoHeader(PseudoHeader::Method), "GET");

  static constexpr uint8_t kAfterAField[]{0x82, 0x3f, 0xe1, 0x07};  // :method GET, then 1024
  const auto result = decoder.decode(AsBytes(kAfterAField));
  EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::DynamicTableSizeUpdateAfterField);
  EXPECT_TRUE(result.isCompressionError());
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 4096U);
}

TEST(HpackDecoder, SetMaxDynamicTableSizeBoundsTheSizeUpdates) {
  auto decoder = CreateHpackDecoder();
  HpackEncoder encoder;
  RawBytes block;

  // A lower limit reduces the table right away.
  decoder.setMaxDynamicTableSize(1024U);
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 1024U);
  encoder.encodeDynamicTableSizeUpdate(block, 1025U);
  EXPECT_EQ(decoder.decode(block).error, HpackDecoder::DecodeResult::Error::DynamicTableSizeUpdateAboveLimit);

  // A higher one leaves the table size to the size updates of the encoder.
  decoder.setMaxDynamicTableSize(65536U);
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 1024U);
  block.clear();
  encoder.encodeDynamicTableSizeUpdate(block, 65536U);
  EXPECT_TRUE(decoder.decode(block).isSuccess());
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 65536U);
  block.clear();
  encoder.encodeDynamicTableSizeUpdate(block, 65537U);
  EXPECT_EQ(decoder.decode(block).error, HpackDecoder::DecodeResult::Error::DynamicTableSizeUpdateAboveLimit);
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 65536U);
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
  EXPECT_EQ(res1.decodedHeaders.headers.size(), 1U);
  EXPECT_EQ(res1.decodedHeaders.headers.begin()->first, "custom-key");

  auto res2 = decoder.decode(AsBytes(encoded));
  EXPECT_TRUE(res2.isSuccess());
  EXPECT_EQ(res2.decodedHeaders.headers.size(), 1U);
  EXPECT_EQ(res2.decodedHeaders.headers.begin()->first, "custom-key");
}

TEST(HpackDecoder, IndexedNameSelfEviction) {
  // Table just large enough for a single small entry: the slightest addition
  // will force its eviction.
  HpackDecoder decoder(40, std::numeric_limits<uint32_t>::max(),
                       true);  // kHpackOverhead(32) + "ck"(2) + "v1"(2) = 36 <= 40

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
  ASSERT_EQ(secondResult.decodedHeaders.headers.size(), 1U);
  EXPECT_EQ(secondResult.decodedHeaders.headers.begin()->first, "ck");
  EXPECT_EQ(secondResult.decodedHeaders.headers.begin()->second, "v2");
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

TEST(HpackEncoder, LargeDynamicTableIndexedLookup) {
  HpackEncoder encoder(65536);
  RawBytes output;
  for (std::size_t idx = 0; idx < 160U; ++idx) {
    const auto suffix = std::to_string(idx);
    encoder.encode(output, "x-indexed-" + suffix, "value-" + suffix);
  }

  auto result = encoder.findHeader("x-indexed-159", "value-159");
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 62U);

  result = encoder.findHeader("x-indexed-0", "value-0");
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 221U);

  result = encoder.findHeader("x-indexed-80", "different");
  EXPECT_EQ(result.match, HpackLookupResult::Match::NameOnly);
  EXPECT_EQ(result.index, 141U);

  result = encoder.findHeader("x-indexed-absent", "value");
  EXPECT_EQ(result.match, HpackLookupResult::Match::None);
}

TEST(HpackEncoder, LargeDynamicTableIndexTracksChurn) {
  HpackEncoder encoder(16384);
  RawBytes output;
  const std::string value(64U, 'v');
  for (std::size_t idx = 0; idx < 200U; ++idx) {
    encoder.encode(output, "x-churn-" + std::to_string(idx), value);
  }

  // The index deliberately keeps stale hashes until its periodic rebuild. Both exact and name-only probes must detect
  // the stale serial and preserve lookup correctness through their linear fallback.
  auto result = encoder.findHeader("x-churn-0", value);
  EXPECT_EQ(result.match, HpackLookupResult::Match::None);
  result = encoder.findHeader("x-churn-0", "different");
  EXPECT_EQ(result.match, HpackLookupResult::Match::None);

  for (std::size_t idx = 200U; idx < 1000U; ++idx) {
    encoder.encode(output, "x-churn-" + std::to_string(idx), value);
  }

  result = encoder.findHeader("x-churn-999", value);
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 62U);

  result = encoder.findHeader("x-churn-0", value);
  EXPECT_EQ(result.match, HpackLookupResult::Match::None);
}

TEST(HpackEncoder, LargeDynamicTableIndexHandlesHashCollisions) {
  HpackEncoder encoder(65536);
  RawBytes output;
  for (std::size_t idx = 0; idx < 128U; ++idx) {
    const auto suffix = std::to_string(idx);
    encoder.encode(output, "x-collision-" + suffix, "value-" + suffix);
  }
  encoder.encode(output, "x-collision-target", "older");
  encoder.encode(output, "x-collision-target", "newer");

  // Redirecting a hash slot models collisions without depending on a particular hash implementation's output.
  // A same-name collision must scan past the newer value and find the older exact entry.
  HpackEncoderTestAccess::redirectFullLookup(encoder, "x-collision-target", "older", 0U);
  auto result = encoder.findHeader("x-collision-target", "older");
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 63U);

  // A full-hash collision with a different name must also recover the exact entry through the linear fallback.
  HpackEncoderTestAccess::redirectFullLookup(encoder, "x-collision-0", "value-0", 0U);
  result = encoder.findHeader("x-collision-0", "value-0");
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 191U);

  // A name-hash collision must preserve a valid name-only result.
  HpackEncoderTestAccess::redirectNameLookup(encoder, "x-collision-target", 2U);
  result = encoder.findHeader("x-collision-target", "absent");
  EXPECT_EQ(result.match, HpackLookupResult::Match::NameOnly);
  EXPECT_EQ(result.index, 62U);
}

TEST(HpackEncoder, LargeDynamicTableIndexHandlesResizeAndClear) {
  HpackEncoder encoder(65536);
  RawBytes output;
  const std::string value(64U, 'v');
  for (std::size_t idx = 0; idx < 500U; ++idx) {
    encoder.encode(output, "x-resize-" + std::to_string(idx), value);
  }

  encoder.encodeDynamicTableSizeUpdate(output, 60000U);
  encoder.encodeDynamicTableSizeUpdate(output, 16384U);
  auto result = encoder.findHeader("x-resize-499", value);
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 62U);
  result = encoder.findHeader("x-resize-0", value);
  EXPECT_EQ(result.match, HpackLookupResult::Match::None);

  encoder.encode(output, "x-oversized", std::string(16384U, 'x'));
  EXPECT_EQ(encoder.dynamicTable().entryCount(), 0U);
  result = encoder.findHeader("x-resize-499", value);
  EXPECT_EQ(result.match, HpackLookupResult::Match::None);
}

TEST(HpackEncoder, LargeDynamicTableIndexResetsBelowActivationThreshold) {
  HpackEncoder encoder(65536);
  RawBytes output;
  const std::string value(64U, 'v');
  for (std::size_t idx = 0; idx < 140U; ++idx) {
    encoder.encode(output, "x-shrink-" + std::to_string(idx), value);
  }

  encoder.encodeDynamicTableSizeUpdate(output, 8192U);
  EXPECT_LT(encoder.dynamicTable().entryCount(), 128U);
  const auto result = encoder.findHeader("x-shrink-139", value);
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 62U);
}

TEST(HpackEncoder, CopiesAndMovesLargeDynamicTableIndex) {
  HpackEncoder encoder(65536);
  RawBytes output;
  for (std::size_t idx = 0; idx < 140U; ++idx) {
    const auto suffix = std::to_string(idx);
    encoder.encode(output, "x-copy-" + suffix, "value-" + suffix);
  }

  HpackEncoder copied(encoder);
  EXPECT_EQ(copied.findHeader("x-copy-0", "value-0").match, HpackLookupResult::Match::Full);

  HpackEncoder copyAssigned;
  copyAssigned = encoder;
  EXPECT_EQ(copyAssigned.findHeader("x-copy-139", "value-139").index, 62U);

  HpackEncoder moved(std::move(copied));
  EXPECT_EQ(moved.findHeader("x-copy-0", "value-0").match, HpackLookupResult::Match::Full);

  HpackEncoder moveAssigned;
  moveAssigned = std::move(copyAssigned);
  EXPECT_EQ(moveAssigned.findHeader("x-copy-139", "value-139").index, 62U);

  HpackEncoder small;
  HpackEncoder smallCopy(small);
  copyAssigned = small;
  EXPECT_EQ(smallCopy.findHeader("x-copy-0", "value-0").match, HpackLookupResult::Match::None);
  EXPECT_EQ(copyAssigned.findHeader("x-copy-0", "value-0").match, HpackLookupResult::Match::None);
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

TEST(HpackEncoder, SetMaxDynamicTableSizeSignalsOnlyAChange) {
  HpackEncoder encoder;
  RawBytes output;

  encoder.setMaxDynamicTableSize(4096U);
  encoder.encode(output, ":method", "GET");
  EXPECT_EQ(output.size(), 1U);  // 0x82 only

  output.clear();
  encoder.setMaxDynamicTableSize(1024U);
  encoder.encode(output, ":method", "GET");
  ASSERT_EQ(output.size(), 4U);  // 0x3f 0xe1 0x07 (1024), then 0x82
  EXPECT_EQ(static_cast<uint8_t>(output[0]), 0x3FU);
  EXPECT_EQ(encoder.dynamicTable().maxSizeBytes(), 1024U);
}

// RFC 7541 §4.2: the sizes set between two header blocks are signaled by at most two updates, the smallest one then the
// last one.
TEST(HpackEncoder, SizesSetBetweenTwoBlocksSignalTheSmallestThenTheLast) {
  static constexpr uint32_t kLargest = std::numeric_limits<uint32_t>::max();

  struct Case {
    vector<uint32_t> sizes;
    vector<uint32_t> updates;
  };
  const std::array cases{
      Case{{0U, 4096U}, {0U, 4096U}},               // Back to the size of the table
      Case{{1024U, 0U, 2048U}, {0U, 2048U}},        // The smallest one is in the middle
      Case{{1024U, 8192U, 2048U}, {1024U, 2048U}},  // Neither the first nor the last set size is lost
      Case{{6000U, 8192U}, {6000U, 8192U}},         // Only increases: the smallest one is still signaled
      Case{{2048U, 1024U}, {1024U}},                // Decreases: the smallest one is the last one
      Case{{0U, 0U}, {0U}},                         // The empty table, the value of no pending last size
      Case{{1024U}, {1024U}},                       // A single change
      Case{{8192U, 4096U}, {4096U}},                // A reverted change is still a change
      Case{{4096U}, {}},                            // No change
      Case{{kLargest}, {kLargest}},                 // The value of no pending smallest size
      Case{{0U, kLargest}, {0U, kLargest}},         // Largest possible difference
      Case{{kLargest, 1024U, kLargest}, {1024U, kLargest}},
  };

  // Compared as vectors, whose bytes gtest prints when they differ.
  const auto bytesOf = [](const RawBytes& block) { return vector<std::byte>(block.begin(), block.end()); };

  // The block of a single indexed field, starting with the given updates.
  const auto expectedBlock = [&bytesOf](const vector<uint32_t>& updates) {
    RawBytes block;
    HpackEncoder encoder;
    for (const uint32_t size : updates) {
      encoder.encodeDynamicTableSizeUpdate(block, size);
    }
    block.push_back(std::byte{0x82});  // :method: GET
    return bytesOf(block);
  };

  for (std::size_t idx = 0; idx < cases.size(); ++idx) {
    SCOPED_TRACE(idx);
    const auto& [sizes, updates] = cases[idx];
    HpackEncoder encoder;
    for (const uint32_t size : sizes) {
      encoder.setMaxDynamicTableSize(size);
    }

    RawBytes output;
    encoder.encode(output, ":method", "GET");
    EXPECT_EQ(bytesOf(output), expectedBlock(updates));
    EXPECT_EQ(encoder.dynamicTable().maxSizeBytes(), sizes.back());

    // The sizes of the previous interval are forgotten once signaled.
    output.clear();
    encoder.encode(output, ":method", "GET");
    EXPECT_EQ(bytesOf(output), expectedBlock({}));

    output.clear();
    encoder.setMaxDynamicTableSize(512U);
    encoder.encode(output, ":method", "GET");
    EXPECT_EQ(bytesOf(output), expectedBlock({512U}));
    EXPECT_EQ(encoder.dynamicTable().maxSizeBytes(), 512U);
  }
}

// A decoder reduces its table as soon as a smaller limit applies (HpackDecoder::setMaxDynamicTableSize()): the encoder
// has to evict the same entries, even when the size is raised again before its next block.
TEST(HpackEncoder, SizeRaisedBeforeTheNextBlockStaysInSyncWithTheDecoder) {
  HpackEncoder encoder;
  auto decoder = CreateHpackDecoder();
  const auto setSizes = [&](uint32_t smallest) {
    for (const uint32_t size : {smallest, 4096U}) {
      encoder.setMaxDynamicTableSize(size);
      decoder.setMaxDynamicTableSize(size);
    }
  };

  RawBytes block;
  encoder.encode(block, "x-a", "value");
  encoder.encode(block, "x-b", "value");
  encoder.encode(block, "x-c", "value");
  ASSERT_TRUE(decoder.decode(block).isSuccess());

  // Each entry takes 40 bytes: only the two newest ones fit.
  setSizes(80U);
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 2U);
  block.clear();
  encoder.encode(block, "x-c", "value");
  encoder.encode(block, "x-a", "value");  // No longer in the table: not a reference to it
  {
    const auto result = decoder.decode(block);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.decodedHeaders.headers.at("x-c"), "value");
    EXPECT_EQ(result.decodedHeaders.headers.at("x-a"), "value");
  }
  EXPECT_EQ(encoder.dynamicTable().entryCount(), 3U);
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 3U);
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 4096U);

  // The table cleared, then restored.
  setSizes(0U);
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 0U);
  block.clear();
  encoder.encode(block, "x-a", "value");
  {
    const auto result = decoder.decode(block);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.decodedHeaders.headers.at("x-a"), "value");
  }
  EXPECT_EQ(encoder.dynamicTable().entryCount(), 1U);
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 1U);
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 4096U);
}

TEST(HpackEncoder, LargeDynamicTableIndexFollowsTheSmallestSize) {
  HpackEncoder encoder(65536);
  RawBytes output;
  const std::string value(64U, 'v');
  for (std::size_t idx = 0; idx < 500U; ++idx) {
    encoder.encode(output, "x-resize-" + std::to_string(idx), value);
  }

  encoder.setMaxDynamicTableSize(16384U);
  encoder.setMaxDynamicTableSize(65536U);
  encoder.encode(output, ":method", "GET");
  EXPECT_EQ(encoder.dynamicTable().maxSizeBytes(), 65536U);
  auto result = encoder.findHeader("x-resize-499", value);
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 62U);
  result = encoder.findHeader("x-resize-0", value);
  EXPECT_EQ(result.match, HpackLookupResult::Match::None);

  // Below the number of entries of the index.
  encoder.setMaxDynamicTableSize(1024U);
  encoder.setMaxDynamicTableSize(65536U);
  encoder.encode(output, ":method", "GET");
  EXPECT_LE(encoder.dynamicTable().currentSizeBytes(), 1024U);
  result = encoder.findHeader("x-resize-499", value);
  EXPECT_EQ(result.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(result.index, 62U);
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

TEST(HpackEncoder, FieldsWhoseValueVariesPerMessageAreNotIndexed) {
  for (const std::string_view name : {
           "age",
           "content-length",
           "etag",
           "if-modified-since",
           "if-none-match",
           "last-modified",
           "location",
           "set-cookie",
       }) {
    EXPECT_EQ(HpackEncoder::IndexingModeOf(name, "v"), HpackEncoder::IndexingMode::WithoutIndexing) << name;
  }
  // Including names of the same length as those (and as the credentials).
  for (const std::string_view name : {
           ":status",
           "content-type",
           "date",
           "cache-control",
           "server",
           "vary",
           "host",
           "via",
           "x-etag",
           "content-range",
           "last-modifiex",
           "if-none-matcx",
           "content-lengtx",
           "x-custom-header",
           "if-modified-sincx",
           "locatiox",
           "set-cookix",
           "cookix",
           "authorizatiox",
           "proxy-authorizatiox",
           "content-disposition",
           "x",
       }) {
    EXPECT_EQ(HpackEncoder::IndexingModeOf(name, "v"), HpackEncoder::IndexingMode::Indexed) << name;
  }
}

TEST(HpackEncoder, CredentialsAreNeverIndexed) {
  const std::string longValue(100, 'a');
  for (const std::string_view name : {"authorization", "proxy-authorization"}) {
    for (const std::string_view value :
         {std::string_view{}, std::string_view{"Basic dXNlcjpwYXNz"}, std::string_view{longValue}}) {
      EXPECT_EQ(HpackEncoder::IndexingModeOf(name, value), HpackEncoder::IndexingMode::NeverIndexed) << name;
    }
  }
  // Short cookie values may be guessed, long ones are indexed.
  for (std::size_t valueSize = 0; valueSize < 20U; ++valueSize) {
    EXPECT_EQ(HpackEncoder::IndexingModeOf("cookie", std::string_view(longValue).substr(0, valueSize)),
              HpackEncoder::IndexingMode::NeverIndexed)
        << valueSize;
  }
  EXPECT_EQ(HpackEncoder::IndexingModeOf("cookie", std::string_view(longValue).substr(0, 20)),
            HpackEncoder::IndexingMode::Indexed);
  EXPECT_EQ(HpackEncoder::IndexingModeOf("cookie", longValue), HpackEncoder::IndexingMode::Indexed);
}

TEST(HpackEncoder, CredentialsAreSentAsNeverIndexedLiterals) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();
  const std::string longCookie = "session=" + std::string(32, 'c');
  const std::pair<std::string_view, std::string_view> fields[]{
      {"authorization", "Bearer abc"},
      {"proxy-authorization", "Basic dXNlcjpwYXNz"},
      {"cookie", "id=42"},
      {"cookie", longCookie},
  };
  for (int requestIdx = 0; requestIdx < 2; ++requestIdx) {
    for (const auto& [name, value] : fields) {
      RawBytes encoded;
      encoder.encode(encoded, name, value, HpackEncoder::IndexingModeOf(name, value));
      const bool isLongCookie = value.size() == longCookie.size();
      if (isLongCookie && requestIdx == 1) {
        // Indexed by the first request: a single byte referencing the dynamic table entry.
        EXPECT_EQ(encoded.size(), 1U);
        EXPECT_EQ(static_cast<uint8_t>(encoded[0]) & 0x80U, 0x80U);
      } else if (!isLongCookie) {
        // Literal never indexed (0001xxxx), on each request.
        EXPECT_EQ(static_cast<uint8_t>(encoded[0]) & 0xF0U, 0x10U) << name;
      }
      const auto result = decoder.decode(encoded);
      ASSERT_TRUE(result.isSuccess());
      ASSERT_EQ(result.decodedHeaders.headers.size(), 1U);
      EXPECT_EQ(result.decodedHeaders.headers.begin()->first, name);
      EXPECT_EQ(result.decodedHeaders.headers.begin()->second, value);
    }
    // Only the long cookie is in the dynamic tables.
    ASSERT_EQ(encoder.dynamicTable().entryCount(), 1U);
    EXPECT_EQ(encoder.dynamicTable()[0].value(), longCookie);
    EXPECT_EQ(decoder.dynamicTable().entryCount(), 1U);
  }
}

TEST(HpackEncoder, FieldsWithoutIndexingLeaveTheDynamicTableToRepeatedOnes) {
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();
  for (int responseIdx = 0; responseIdx < 3; ++responseIdx) {
    const std::string length = std::to_string(1000 + responseIdx);
    const std::pair<std::string_view, std::string_view> fields[]{
        {"content-type", "text/plain"},
        {"content-length", length},
        {"etag", length},
    };
    RawBytes encoded;
    for (const auto& [name, value] : fields) {
      encoder.encode(encoded, name, value, HpackEncoder::IndexingModeOf(name, value));
    }
    const auto result = decoder.decode(encoded);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.decodedHeaders.headers.find("content-length")->second, length);
    EXPECT_EQ(result.decodedHeaders.headers.find("etag")->second, length);
    EXPECT_EQ(result.decodedHeaders.headers.find("content-type")->second, "text/plain");

    // Only content-type is added, by the first response: the next ones reference it.
    ASSERT_EQ(encoder.dynamicTable().entryCount(), 1U);
    EXPECT_EQ(encoder.dynamicTable()[0].name(), "content-type");
    EXPECT_EQ(decoder.dynamicTable().entryCount(), 1U);
  }
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

  const auto& headers = result.decodedHeaders.headers;

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(headers.size(), 1U);

  EXPECT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Method), "GET");
  EXPECT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Path), "/index.html");
  EXPECT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Scheme), "https");

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
    ASSERT_EQ(result.decodedHeaders.headers.size(), 1U);

    auto [name, value] = *result.decodedHeaders.headers.begin();
    EXPECT_EQ(name, "x-custom");
    EXPECT_EQ(value, "value1");
  }

  {
    auto result = decoder.decode(encoded2);
    EXPECT_TRUE(result.isSuccess());
    ASSERT_EQ(result.decodedHeaders.headers.size(), 1U);

    auto [name, value] = *result.decodedHeaders.headers.begin();
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
  encoder.encode(encoded, http::ContentLength, "1");

  auto result = decoder.decode(encoded);
  ASSERT_TRUE(result.isSuccess());

  const auto dateIt = result.decodedHeaders.headers.find("date");
  ASSERT_NE(dateIt, result.decodedHeaders.headers.end());
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
  encoder.encode(encoded, http::ContentLength, "1");

  auto result = decoder.decode(encoded);
  ASSERT_TRUE(result.isSuccess());

  const auto dateIt = result.decodedHeaders.headers.find("date");
  if (dateIt == result.decodedHeaders.headers.end()) {
    for (const auto& [name, value] : result.decodedHeaders.headers) {
      ADD_FAILURE() << "Decoded header: '" << name << "'='" << value << "'";
    }
    // Also check if the date value got associated with a different name.
    for (const auto& [name, value] : result.decodedHeaders.headers) {
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
  encoder.encode(encoded, http::ContentType, "text/plain");
  encoder.encode(encoded, "x-custom", "original");
  encoder.encode(encoded, "x-another", "anothervalue");
  encoder.encode(encoded, "x-global", "gvalue");
  encoder.encode(encoded, http::Date, dateSv);
  encoder.encode(encoded, http::ContentLength, "1");

  auto result = decoder.decode(encoded);
  ASSERT_TRUE(result.isSuccess());

  const auto dateIt = result.decodedHeaders.headers.find("date");
  ASSERT_NE(dateIt, result.decodedHeaders.headers.end());
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
  const auto it = result.decodedHeaders.headers.find("x-rt");
  if (it == result.decodedHeaders.headers.end()) {
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
    std::string value(4, static_cast<char>(ch));
    if (http::IsHeaderWhitespace(static_cast<char>(ch))) {
      // A field value cannot start or end with whitespace (RFC 9113 §8.2.1): keep them inside the value.
      value.insert(value.begin(), 'a');
      value.push_back('a');
    }

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

// The decoder reads 8 input bytes at a time while at least 8 are left, then the last ones at once: these tests put the
// symbol under test at every bit position of strings long enough for both phases.
namespace {

// The bits of a Huffman string, written MSB first.
class HuffmanBitWriter {
 public:
  void append(uint32_t code, uint32_t nbBits) {
    for (uint32_t bit = nbBits; bit != 0; --bit) {
      appendBit(((code >> (bit - 1U)) & 1U) != 0);
    }
  }

  void appendRepeated(uint32_t code, uint32_t nbBits, std::size_t count) {
    for (std::size_t idx = 0; idx < count; ++idx) {
      append(code, nbBits);
    }
  }

  // Number of bits needed to complete the last byte.
  [[nodiscard]] uint32_t nbMissingBits() const noexcept { return static_cast<uint32_t>((8U - (_nbBits % 8U)) % 8U); }

  // Complete the last byte with 1s, as the EOS prefix a valid string ends with.
  [[nodiscard]] vector<uint8_t> finish() {
    append(0xFFU, nbMissingBits());
    return _bytes;
  }

 private:
  void appendBit(bool set) {
    if (_nbBits % 8U == 0) {
      _bytes.push_back(0);
    }
    if (set) {
      _bytes.back() = static_cast<uint8_t>(_bytes.back() | (0x80U >> (_nbBits % 8U)));
    }
    ++_nbBits;
  }

  vector<uint8_t> _bytes;
  std::size_t _nbBits{0};
};

// RFC 7541 Appendix B.
constexpr uint32_t kHuffmanCodeA = 0x3;  // 'a', 5 bits
constexpr uint32_t kHuffmanCodeABits = 5;
constexpr uint32_t kHuffmanCodeEos = 0x3fffffff;
constexpr uint32_t kHuffmanCodeEosBits = 30;

// A literal field without indexing named "x", its value being the given string (Huffman-encoded or not).
vector<uint8_t> LiteralFieldX(std::span<const uint8_t> valueBytes, bool huffman) {
  vector<uint8_t> block{0x00, 0x01, 'x'};
  // The string length, with a 7-bit prefix (RFC 7541 §5.1) after the Huffman bit.
  const uint8_t huffmanBit = huffman ? 0x80 : 0x00;
  std::size_t length = valueBytes.size();
  if (length < 127U) {
    block.push_back(static_cast<uint8_t>(huffmanBit | length));
  } else {
    block.push_back(static_cast<uint8_t>(huffmanBit | 0x7FU));
    for (length -= 127U; length >= 128U; length >>= 7U) {
      block.push_back(static_cast<uint8_t>((length & 0x7FU) | 0x80U));
    }
    block.push_back(static_cast<uint8_t>(length));
  }
  for (const uint8_t byte : valueBytes) {
    block.push_back(byte);
  }
  return block;
}

vector<uint8_t> LiteralFieldX(std::string_view value) {
  return LiteralFieldX(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(value.data()), value.size()), false);
}

}  // namespace

TEST(HpackHuffman, DecodesLongRunsAtEveryPaddingRemainder) {
  for (std::size_t nbSymbols = 1; nbSymbols <= 120; ++nbSymbols) {
    HuffmanBitWriter writer;
    writer.appendRepeated(kHuffmanCodeA, kHuffmanCodeABits, nbSymbols);
    const auto block = LiteralFieldX(writer.finish(), true);
    auto decoder = CreateHpackDecoder();
    const auto result = decoder.decode(AsBytes(block));
    ASSERT_TRUE(result.isSuccess()) << nbSymbols << " symbols";
    EXPECT_EQ(result.decodedHeaders.headers.find("x")->second, std::string(nbSymbols, 'a')) << nbSymbols << " symbols";
  }
}

TEST(HpackHuffman, RejectsEosAtAnyPosition) {
  for (std::size_t nbBefore = 0; nbBefore <= 40; ++nbBefore) {
    HuffmanBitWriter writer;
    writer.appendRepeated(kHuffmanCodeA, kHuffmanCodeABits, nbBefore);
    writer.append(kHuffmanCodeEos, kHuffmanCodeEosBits);
    writer.appendRepeated(kHuffmanCodeA, kHuffmanCodeABits, 12);
    const auto block = LiteralFieldX(writer.finish(), true);
    auto decoder = CreateHpackDecoder();
    const auto result = decoder.decode(AsBytes(block));
    EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderValue)
        << nbBefore << " symbols before EOS";
  }
}

TEST(HpackHuffman, RejectsPaddingNotMadeOfOnesAfterAnyLength) {
  for (std::size_t nbSymbols = 1; nbSymbols <= 120; ++nbSymbols) {
    HuffmanBitWriter writer;
    writer.appendRepeated(kHuffmanCodeA, kHuffmanCodeABits, nbSymbols);
    const uint32_t nbPaddingBits = writer.nbMissingBits();
    if (nbPaddingBits == 0) {
      continue;
    }
    // 1s then a 0: no code is made of 1s then a single 0 in 7 bits or less, so it can only be read as padding.
    writer.append((1U << nbPaddingBits) - 2U, nbPaddingBits);
    const auto block = LiteralFieldX(writer.finish(), true);
    auto decoder = CreateHpackDecoder();
    const auto result = decoder.decode(AsBytes(block));
    EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderValue)
        << nbSymbols << " symbols";
  }
}

TEST(HpackHuffman, RejectsAWholeByteOfPaddingAfterAnyLength) {
  for (std::size_t nbSymbols = 0; nbSymbols <= 120; ++nbSymbols) {
    HuffmanBitWriter writer;
    writer.appendRepeated(kHuffmanCodeA, kHuffmanCodeABits, nbSymbols);
    const uint32_t nbPaddingBits = 8U + writer.nbMissingBits();
    writer.append((1U << nbPaddingBits) - 1U, nbPaddingBits);
    const auto block = LiteralFieldX(writer.finish(), true);
    auto decoder = CreateHpackDecoder();
    const auto result = decoder.decode(AsBytes(block));
    EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderValue)
        << nbSymbols << " symbols";
  }
}

TEST(HpackHuffman, RoundTripsLongCodesAtEveryBitPosition) {
  // 0xFF has a 26-bit code, 0x01 a 23-bit one, '!' a 10-bit one: all decoded by the tier-2 path.
  for (const char rare : {'\xff', '\x01', '!'}) {
    for (std::size_t pos = 1; pos < 47; ++pos) {
      std::string value(48, 'a');
      value[pos] = rare;
      EXPECT_EQ(HuffmanRoundTrip(value), value)
          << "byte " << static_cast<int>(static_cast<uint8_t>(rare)) << " at " << pos;
    }
  }
}

TEST(HpackHuffman, RoundTripsRandomTextOfEveryLength) {
  std::mt19937 rng(42);  // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
  static constexpr std::string_view kCommon = "abcdefghijklmnopqrstuvwxyz0123456789-_./=;,";
  std::uniform_int_distribution<std::size_t> commonDist(0, kCommon.size() - 1U);
  std::uniform_int_distribution<int> byteDist(1, 255);
  std::uniform_int_distribution<int> pickDist(0, 9);
  for (std::size_t len = 1; len <= 300; ++len) {
    std::string value(len, 'a');
    // Inner characters only: a value cannot start or end with whitespace.
    for (std::size_t pos = 1; pos + 1 < len; ++pos) {
      char ch = kCommon[commonDist(rng)];
      if (pickDist(rng) == 0) {
        ch = static_cast<char>(byteDist(rng));
        if (ch == '\r' || ch == '\n') {
          ch = 'z';
        }
      }
      value[pos] = ch;
    }
    EXPECT_EQ(HuffmanRoundTrip(value), value) << "length " << len;
  }
}

// The value validation tests 8 bytes at a time: a forbidden byte is found at any position, and the bytes allowed close
// to the forbidden ones are accepted at any position.
TEST(HpackDecoder, RejectsNulCrLfAtAnyPositionOfTheValue) {
  for (std::size_t len = 1; len <= 40; ++len) {
    for (std::size_t pos = 0; pos < len; ++pos) {
      for (const uint8_t forbidden : {uint8_t{0x00}, uint8_t{'\n'}, uint8_t{'\r'}}) {
        std::string value(len, 'v');
        value[pos] = static_cast<char>(forbidden);
        const auto block = LiteralFieldX(value);
        auto decoder = CreateHpackDecoder();
        EXPECT_EQ(decoder.decode(AsBytes(block)).error, HpackDecoder::DecodeResult::Error::MalformedFieldValue)
            << "byte " << static_cast<int>(forbidden) << " at " << pos << " of " << len;
      }
    }
  }
}

TEST(HpackDecoder, AcceptsOtherControlAndHighBytesAtAnyPositionOfTheValue) {
  for (std::size_t len = 1; len <= 40; ++len) {
    for (std::size_t pos = 0; pos < len; ++pos) {
      for (const uint8_t allowed : {
               uint8_t{0x01},
               uint8_t{'\t'},
               uint8_t{0x0B},
               uint8_t{0x0C},
               uint8_t{0x0E},
               uint8_t{0x7F},
               uint8_t{0x80},
               uint8_t{0xFF},
           }) {
        if (allowed == '\t' && (pos == 0 || pos + 1 == len)) {
          continue;  // leading or trailing whitespace
        }
        std::string value(len, 'v');
        value[pos] = static_cast<char>(allowed);
        const auto block = LiteralFieldX(value);
        auto decoder = CreateHpackDecoder();
        const auto result = decoder.decode(AsBytes(block));
        ASSERT_TRUE(result.isSuccess()) << "byte " << static_cast<int>(allowed) << " at " << pos << " of " << len;
        EXPECT_EQ(result.decodedHeaders.headers.find("x")->second, value);
      }
    }
  }
}

// A field that is not indexed is encoded with the index of its static name, even when the dynamic table holds the same
// field (indexed by an earlier one): the dynamic table is not scanned for it.
TEST(HpackEncoder, FieldWithoutIndexingUsesItsStaticNameIndex) {
  HpackEncoder encoder(4096);
  RawBytes indexed;
  encoder.encode(indexed, "content-length", "10");
  ASSERT_EQ(encoder.dynamicTable().entryCount(), 1U);

  const auto fullMatch = encoder.findHeader("content-length", "10");
  EXPECT_EQ(fullMatch.match, HpackLookupResult::Match::Full);
  EXPECT_EQ(fullMatch.index, 62U);
  for (const auto mode : {HpackEncoder::IndexingMode::WithoutIndexing, HpackEncoder::IndexingMode::NeverIndexed}) {
    const auto lookup = encoder.findHeader("content-length", "10", mode);
    EXPECT_EQ(lookup.match, HpackLookupResult::Match::NameOnly);
    EXPECT_EQ(lookup.index, 28U);
  }

  RawBytes encoded;
  encoder.encode(encoded, "content-length", "10", HpackEncoder::IndexingMode::WithoutIndexing);
  // Literal without indexing, name index 28 on a 4-bit prefix (15 + 13), raw value "10".
  static constexpr uint8_t kExpected[]{0x0F, 0x0D, 0x02, '1', '0'};
  ASSERT_EQ(encoded.size(), std::size(kExpected));
  EXPECT_EQ(std::memcmp(encoded.data(), kExpected, std::size(kExpected)), 0);

  auto decoder = CreateHpackDecoder();
  const auto first = decoder.decode(indexed);
  ASSERT_TRUE(first.isSuccess());
  const auto second = decoder.decode(encoded);
  ASSERT_TRUE(second.isSuccess());
  EXPECT_EQ(second.decodedHeaders.headers.find("content-length")->second, "10");
}

// ============================
// Plain fields of the dynamic table: stored without checking them again
// ============================

TEST(HpackDynamicTable, KeepsTheTagOfEachEntry) {
  // Room for 3 entries of 2 + 2 + 32 bytes.
  HpackDynamicTable table(108);
  ASSERT_TRUE(table.add("n0", "v0"));
  EXPECT_EQ(table.tag(0), 0U);

  // Evictions and the reclaiming of the evicted entries keep each tag with its entry.
  for (std::size_t idx = 1; idx < 64; ++idx) {
    const std::string name = "n" + std::to_string(idx % 10);
    const std::string value = "v" + std::to_string(idx % 10);
    ASSERT_TRUE(table.add(name, value, idx));
    const std::size_t nbEntries = std::min<std::size_t>(idx + 1U, 3U);
    ASSERT_EQ(table.entryCount(), nbEntries);
    for (std::size_t tableIdx = 0; tableIdx < nbEntries; ++tableIdx) {
      const std::size_t entryIdx = idx - tableIdx;
      EXPECT_EQ(table.tag(static_cast<uint32_t>(tableIdx)), entryIdx);
      EXPECT_EQ(table[static_cast<uint32_t>(tableIdx)].name(), "n" + std::to_string(entryIdx % 10));
    }
  }

  table.setMaxSize(40);
  ASSERT_EQ(table.entryCount(), 1U);
  EXPECT_EQ(table.tag(0), 63U);
}

// A block referencing the fields of the dynamic table decodes like the block that added them.
TEST(HpackDecoder, IndexedFieldsDecodeLikeTheirLiterals) {
  static constexpr http::HeaderView kFields[]{
      {"user-agent", "Mozilla/5.0 (X11; Linux x86_64; rv:131.0) Gecko/20100101 Firefox/131.0"},
      {"accept", "*/*"},
      {"x", ""},
      {"x-custom-field-with-a-long-name", "v"},
      {"cookie", "session=7f3a9c2e5b1d4f6a8e0c; theme=dark"},
      {"x-binary", "\x01\x7f\xff value"},
      {"te", "trailers"},
  };
  auto decoder = CreateHpackDecoder();
  HpackEncoder encoder(4096);

  std::size_t namesAndValuesSize = 0;
  for (const http::HeaderView& field : kFields) {
    namesAndValuesSize += field.name.size() + field.value.size();
  }

  // The first block adds the fields to the dynamic table, the next ones reference them.
  for (int blockIdx = 0; blockIdx < 4; ++blockIdx) {
    RawBytes block;
    for (const http::HeaderView& field : kFields) {
      encoder.encode(block, field.name, field.value);
    }
    if (blockIdx != 0) {
      ASSERT_EQ(block.size(), std::size(kFields));  // one byte per indexed field
    }

    const auto result = decoder.decode(block);

    ASSERT_TRUE(result.isSuccess()) << blockIdx;
    EXPECT_EQ(result.headerListSize, namesAndValuesSize + (32U * std::size(kFields)));
    EXPECT_EQ(result.decodedHeaders.namesAndValuesSize, namesAndValuesSize);
    ASSERT_EQ(result.decodedHeaders.headers.size(), std::size(kFields));
    for (const http::HeaderView& field : kFields) {
      // Found by a lookup hashing its name: the field was stored with the same hash.
      const auto it = result.decodedHeaders.headers.find(field.name);
      ASSERT_NE(it, result.decodedHeaders.headers.end()) << field.name;
      EXPECT_EQ(it->second, field.value);
      // The dynamic table entry may be evicted while the decoded headers are in use: they do not point to it.
      for (uint32_t tableIdx = 0; tableIdx < decoder.dynamicTable().entryCount(); ++tableIdx) {
        EXPECT_NE(it->first.data(), decoder.dynamicTable()[tableIdx].name().data());
        EXPECT_NE(it->second.data(), decoder.dynamicTable()[tableIdx].value().data());
      }
    }
  }
}

TEST(HpackDecoder, RepeatedIndexedFieldsAreMerged) {
  auto decoder = CreateHpackDecoder();
  HpackEncoder encoder(4096);
  RawBytes block;
  encoder.encode(block, "accept", "text/html");
  encoder.encode(block, "cookie", "a=1; session=0123456789abcdef");
  encoder.encode(block, "x-empty", "");
  ASSERT_TRUE(decoder.decode(block).isSuccess());

  // Indexes 64 (accept), 63 (cookie) and 62 (x-empty), each twice, then a literal accept.
  static constexpr uint8_t references[]{0xC0, 0xBF, 0xBE, 0xC0, 0xBF, 0xBE, 0x53, 0x03, '*', '/', '*'};

  const auto result = decoder.decode(AsBytes(references));

  ASSERT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.headers.size(), 3U);
  EXPECT_EQ(result.decodedHeaders.headers.at("accept"), "text/html,text/html,*/*");
  EXPECT_EQ(result.decodedHeaders.headers.at("cookie"), "a=1; session=0123456789abcdef; a=1; session=0123456789abcdef");
  EXPECT_EQ(result.decodedHeaders.headers.at("x-empty"), "");
  EXPECT_EQ(result.decodedHeaders.namesAndValuesSize,
            (2U * (std::string_view("accepttext/html").size() + std::string_view("x-empty").size() +
                   std::string_view("cookiea=1; session=0123456789abcdef").size())) +
                std::string_view("accept*/*").size());
}

TEST(HpackDecoder, RepeatedIndexedSingletonFieldIsRejected) {
  auto decoder = CreateHpackDecoder();
  HpackEncoder encoder(4096);
  RawBytes block;
  encoder.encode(block, "content-length", "12");
  ASSERT_TRUE(decoder.decode(block).isSuccess());

  static constexpr uint8_t once[]{0xBE};
  const auto single = decoder.decode(AsBytes(once));
  ASSERT_TRUE(single.isSuccess());
  EXPECT_EQ(single.decodedHeaders.headers.at("content-length"), "12");

  static constexpr uint8_t twice[]{0xBE, 0xBE};
  EXPECT_EQ(decoder.decode(AsBytes(twice)).error, HpackDecoder::DecodeResult::Error::DuplicateHeaderForbiddenToMerge);

  // The set-cookie fields of a response are joined with a line feed.
  HpackDecoder responseDecoder(4096, std::numeric_limits<uint32_t>::max(), true, HpackDecoder::Messages::Responses);
  HpackEncoder responseEncoder(4096);
  RawBytes responseBlock;
  responseEncoder.encode(responseBlock, "set-cookie", "a=1; Expires=Wed, 21 Oct 2026 07:28:00 GMT");
  ASSERT_TRUE(responseDecoder.decode(responseBlock).isSuccess());
  const auto cookies = responseDecoder.decode(AsBytes(twice));
  ASSERT_TRUE(cookies.isSuccess());
  EXPECT_EQ(cookies.decodedHeaders.headers.at("set-cookie"),
            "a=1; Expires=Wed, 21 Oct 2026 07:28:00 GMT\na=1; Expires=Wed, 21 Oct 2026 07:28:00 GMT");
}

// A pseudo-header field cannot follow a regular field, whichever way this one is stored.
TEST(HpackDecoder, PseudoHeaderAfterAnIndexedRegularFieldIsRejected) {
  auto decoder = CreateHpackDecoder();
  HpackEncoder encoder(4096);
  RawBytes block;
  encoder.encode(block, ":path", "/some/path");
  encoder.encode(block, "x-field", "value");
  ASSERT_TRUE(decoder.decode(block).isSuccess());

  static constexpr uint8_t inOrder[]{0x82, 0xBF, 0xBE};  // :method: GET, :path (63), x-field (62)
  const auto result = decoder.decode(AsBytes(inOrder));
  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Path), "/some/path");
  EXPECT_EQ(result.decodedHeaders.headers.at("x-field"), "value");

  for (const uint8_t pseudoHeaderIndex : {uint8_t{0x82}, uint8_t{0xBF}}) {
    const uint8_t outOfOrder[]{0xBE, pseudoHeaderIndex};
    EXPECT_EQ(decoder.decode(AsBytes(outOfOrder)).error,
              HpackDecoder::DecodeResult::Error::PseudoHeaderFieldAfterRegularField);
  }
}

// The fields of the dynamic table that are not stored as received, or that are refused, are checked each time.
TEST(HpackDecoder, EachReferenceToAFieldThatIsNotPlainIsChecked) {
  auto decoder = CreateHpackDecoder();
  HpackEncoder encoder(4096);
  RawBytes block;
  encoder.encode(block, "x-bad-value", "line\nfeed");
  ASSERT_EQ(decoder.decode(block).error, HpackDecoder::DecodeResult::Error::MalformedFieldValue);
  block.clear();
  encoder.encode(block, "content-encoding", "GZip");
  ASSERT_TRUE(decoder.decode(block).isSuccess());
  block.clear();
  encoder.encode(block, "te", "trailers");
  encoder.encode(block, "x-good", "value");
  ASSERT_TRUE(decoder.decode(block).isSuccess());

  for (int attempt = 0; attempt < 3; ++attempt) {
    static constexpr uint8_t badValue[]{0xC1};  // index 65
    EXPECT_EQ(decoder.decode(AsBytes(badValue)).error, HpackDecoder::DecodeResult::Error::MalformedFieldValue);

    static constexpr uint8_t contentEncoding[]{0xC0, 0xBE};  // indexes 64 and 62
    const auto result = decoder.decode(AsBytes(contentEncoding));
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.decodedHeaders.headers.at("content-encoding"), "gzip");
    EXPECT_EQ(result.decodedHeaders.headers.at("x-good"), "value");
    EXPECT_EQ(decoder.dynamicTable()[2].value(), "GZip");
  }

  // The name of a plain field is known to be valid, not the value given to it by another field.
  static constexpr uint8_t teGzip[]{0x0F, 0x30, 0x04, 'g', 'z', 'i', 'p'};  // te (index 63): gzip, without indexing
  EXPECT_EQ(decoder.decode(AsBytes(teGzip)).error, HpackDecoder::DecodeResult::Error::ConnectionSpecificField);
  static constexpr uint8_t goodWithNul[]{0x7E, 0x03, 'a', 0x00, 'b'};  // x-good (index 62): a NUL b, indexed
  EXPECT_EQ(decoder.decode(AsBytes(goodWithNul)).error, HpackDecoder::DecodeResult::Error::MalformedFieldValue);
  static constexpr uint8_t newEntry[]{0xBE};  // index 62: the entry just added
  EXPECT_EQ(decoder.decode(AsBytes(newEntry)).error, HpackDecoder::DecodeResult::Error::MalformedFieldValue);

  // A valid value with the name of a plain field makes another plain field.
  static constexpr uint8_t goodOther[]{0x7F, 0x00, 0x05, 'o', 't', 'h', 'e', 'r'};  // x-good (index 63): other, indexed
  const auto literal = decoder.decode(AsBytes(goodOther));
  ASSERT_TRUE(literal.isSuccess());
  EXPECT_EQ(literal.decodedHeaders.headers.at("x-good"), "other");
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto reference = decoder.decode(AsBytes(newEntry));
    ASSERT_TRUE(reference.isSuccess());
    EXPECT_EQ(reference.decodedHeaders.headers.at("x-good"), "other");
  }
}

TEST(HpackDecoder, EachReferenceToAConnectionSpecificFieldOfAResponseIsRecorded) {
  HpackDecoder decoder(4096, std::numeric_limits<uint32_t>::max(), true, HpackDecoder::Messages::Responses);
  HpackEncoder encoder(4096);
  RawBytes block;
  encoder.encode(block, "connection", "close");
  encoder.encode(block, "x-field", "value");
  ASSERT_TRUE(decoder.decode(block).decodedHeaders.hasConnectionSpecificField);

  for (int attempt = 0; attempt < 2; ++attempt) {
    static constexpr uint8_t plainField[]{0xBE};
    const auto plainResult = decoder.decode(AsBytes(plainField));
    ASSERT_TRUE(plainResult.isSuccess());
    EXPECT_FALSE(plainResult.decodedHeaders.hasConnectionSpecificField);

    static constexpr uint8_t bothFields[]{0xBE, 0xBF};
    const auto result = decoder.decode(AsBytes(bothFields));
    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(result.decodedHeaders.hasConnectionSpecificField);
    EXPECT_EQ(result.decodedHeaders.headers.at("connection"), "close");
  }
}

// A field beyond the header list size limit is added to the dynamic table without being checked: it is checked when
// a later block references it.
TEST(HpackDecoder, FieldAddedBeyondTheHeaderListLimitIsCheckedWhenReferenced) {
  static constexpr uint32_t kMaxHeaderListSize = 100;
  HpackDecoder decoder(4096, kMaxHeaderListSize, true);
  HpackEncoder encoder(4096);
  RawBytes block;
  encoder.encode(block, "x-large", std::string(kMaxHeaderListSize, 'v'), HpackEncoder::IndexingMode::WithoutIndexing);
  encoder.encode(block, "x-bad", "line\nfeed");
  encoder.encode(block, "x-good", "value");

  ASSERT_EQ(decoder.decode(block).error, HpackDecoder::DecodeResult::Error::HeaderListTooLarge);
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 2U);

  for (int attempt = 0; attempt < 2; ++attempt) {
    static constexpr uint8_t bad[]{0xBF};
    EXPECT_EQ(decoder.decode(AsBytes(bad)).error, HpackDecoder::DecodeResult::Error::MalformedFieldValue);

    static constexpr uint8_t good[]{0xBE};
    const auto result = decoder.decode(AsBytes(good));
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.decodedHeaders.headers.at("x-good"), "value");
  }

  // Indexed plain fields count in the header list size, and are not stored beyond its limit.
  HpackDecoder smallDecoder(4096, kMaxHeaderListSize, true);
  block.clear();
  HpackEncoder smallEncoder(4096);
  smallEncoder.encode(block, "x-plain", "0123456789");  // 7 + 10 + 32 bytes
  ASSERT_TRUE(smallDecoder.decode(block).isSuccess());
  static constexpr uint8_t twice[]{0xBE, 0xBE};
  const auto withinLimit = smallDecoder.decode(AsBytes(twice));
  ASSERT_TRUE(withinLimit.isSuccess());
  EXPECT_EQ(withinLimit.headerListSize, 98U);
  EXPECT_EQ(withinLimit.decodedHeaders.headers.at("x-plain"), "0123456789,0123456789");
  static constexpr uint8_t thrice[]{0xBE, 0xBE, 0xBE};
  const auto beyondLimit = smallDecoder.decode(AsBytes(thrice));
  EXPECT_EQ(beyondLimit.error, HpackDecoder::DecodeResult::Error::HeaderListTooLarge);
  EXPECT_EQ(beyondLimit.headerListSize, 147U);
  EXPECT_EQ(beyondLimit.decodedHeaders.namesAndValuesSize, 34U);
}

// The decoder remembers which entries of its dynamic table are plain fields: this must follow their evictions.
TEST(HpackDecoder, PlainFieldsAreRememberedThroughEvictions) {
  // Room for 4 entries of 5 + 5 + 32 bytes.
  HpackDecoder decoder(168, std::numeric_limits<uint32_t>::max(), true);
  HpackEncoder encoder(168);
  static constexpr std::size_t kNbEntries = 4;

  const auto isValid = [](std::size_t idx) { return idx % 3U != 1U; };
  const auto nameOf = [](std::size_t idx) { return "x-" + std::to_string(100 + idx); };
  const auto valueOf = [&isValid](std::size_t idx) {
    return (isValid(idx) ? "v-" : "v\n") + std::to_string(100 + idx);
  };

  for (std::size_t idx = 0; idx < 100; ++idx) {
    RawBytes block;
    encoder.encode(block, nameOf(idx), valueOf(idx));
    ASSERT_EQ(decoder.decode(block).isSuccess(), isValid(idx)) << idx;
    ASSERT_EQ(decoder.dynamicTable().entryCount(), std::min(idx + 1U, kNbEntries));

    // Each entry of the table is referenced twice: the valid ones are decoded, the other ones refused.
    for (std::size_t tableIdx = 0; tableIdx < decoder.dynamicTable().entryCount(); ++tableIdx) {
      const std::size_t entryIdx = idx - tableIdx;
      const uint8_t reference[]{static_cast<uint8_t>(0x80U | (62U + tableIdx))};
      for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = decoder.decode(AsBytes(reference));
        if (isValid(entryIdx)) {
          ASSERT_TRUE(result.isSuccess()) << idx << " " << tableIdx;
          ASSERT_EQ(result.decodedHeaders.headers.size(), 1U);
          const std::string name = nameOf(entryIdx);
          const auto it = result.decodedHeaders.headers.find(std::string_view(name));
          ASSERT_NE(it, result.decodedHeaders.headers.end()) << idx << " " << tableIdx;
          EXPECT_EQ(it->second, valueOf(entryIdx));
        } else {
          EXPECT_EQ(result.error, HpackDecoder::DecodeResult::Error::MalformedFieldValue) << idx << " " << tableIdx;
        }
      }
    }
  }
}

// ============================
// Fields found again in the dynamic table of the encoder without scanning it
// ============================

namespace {

// Decode 'block' and check that it holds exactly these fields.
void ExpectDecodesTo(HpackDecoder& decoder, const RawBytes& block, std::span<const http::HeaderView> fields) {
  const auto result = decoder.decode(block);
  ASSERT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.headers.size(), fields.size());
  for (const http::HeaderView& field : fields) {
    const auto it = result.decodedHeaders.headers.find(field.name);
    ASSERT_NE(it, result.decodedHeaders.headers.end()) << field.name;
    EXPECT_EQ(it->second, field.value) << field.name;
  }
}

}  // namespace

// The steady state of a long-lived connection: the fields of each message are in the dynamic table, after the values
// that changed since they were added.
TEST(HpackEncoder, FieldsDeepInTheDynamicTableAreFoundAgain) {
  static constexpr http::HeaderView kFields[]{
      {"content-type", "application/json"},
      {"cache-control", "public, max-age=3600"},
      {"server", "aeronet"},
      {"x-frame-options", "DENY"},
      {"strict-transport-security", "max-age=31536000; includeSubDomains"},
  };
  HpackEncoder encoder(4096);
  auto decoder = CreateHpackDecoder();
  RawBytes block;
  for (const http::HeaderView& field : kFields) {
    encoder.encode(block, field.name, field.value);
  }
  ExpectDecodesTo(decoder, block, kFields);

  for (uint32_t second = 0; second < 40; ++second) {
    // A new date each second, added before the fields above.
    const std::string date = "Mon, 12 Oct 2026 08:45:" + std::to_string(10U + second) + " GMT";
    block.clear();
    encoder.encode(block, "date", date);
    const http::HeaderView dateField[]{{"date", date}};
    ExpectDecodesTo(decoder, block, dateField);

    for (int message = 0; message < 2; ++message) {
      block.clear();
      uint32_t expectedIndex = 62U + second + static_cast<uint32_t>(std::size(kFields));
      for (const http::HeaderView& field : kFields) {
        const auto lookup = encoder.findHeader(field.name, field.value);
        EXPECT_EQ(lookup.match, HpackLookupResult::Match::Full);
        EXPECT_EQ(lookup.index, expectedIndex);
        --expectedIndex;
        encoder.encode(block, field.name, field.value);
      }
      EXPECT_EQ(encoder.findHeader("date", date).index, 62U);
      EXPECT_EQ(encoder.findHeader("server", "other").match, HpackLookupResult::Match::NameOnly);
      EXPECT_EQ(encoder.findHeader("x-other", "aeronet").match, HpackLookupResult::Match::None);
      ASSERT_EQ(encoder.dynamicTable().entryCount(), std::size(kFields) + second + 1U);
      ExpectDecodesTo(decoder, block, kFields);
    }
  }
}

// More fields than the encoder remembers the position of, some of them sharing their slots: all are found.
TEST(HpackEncoder, ManyRepeatedFieldsAreAllFound) {
  static constexpr std::size_t kNbFields = 60;
  std::string names[kNbFields];
  std::string values[kNbFields];
  http::HeaderView fields[kNbFields];
  for (std::size_t idx = 0; idx < kNbFields; ++idx) {
    names[idx] = "x-" + std::to_string(idx % 7U) + std::string(idx % 5U, 'n') + std::to_string(idx);
    values[idx] = std::string(idx % 3U, 'v');
    fields[idx] = {names[idx], values[idx]};
  }

  HpackEncoder encoder(8192);
  HpackDecoder decoder(8192, std::numeric_limits<uint32_t>::max(), true);
  for (int message = 0; message < 4; ++message) {
    RawBytes block;
    for (const http::HeaderView& field : fields) {
      encoder.encode(block, field.name, field.value);
    }
    if (message != 0) {
      // Each field is encoded as its index (62 to 121) on one byte, no entry being added.
      EXPECT_EQ(block.size(), kNbFields);
    }
    ASSERT_EQ(encoder.dynamicTable().entryCount(), kNbFields);
    ExpectDecodesTo(decoder, block, fields);
  }
}

// A field is not found where it was once its entry is evicted, or after the table was emptied.
TEST(HpackEncoder, EvictedFieldsAreNotFoundAgain) {
  // Room for 3 entries of 3 + 1 + 32 bytes.
  HpackEncoder encoder(108);
  HpackDecoder decoder(108, std::numeric_limits<uint32_t>::max(), true);
  static constexpr http::HeaderView kFields[]{{"x-a", "1"}, {"x-b", "2"}, {"x-c", "3"}, {"x-d", "4"}, {"x-e", "5"}};

  // Each of the 5 fields in turn: the table holds the last 3 of them.
  for (std::size_t step = 0; step < 100; ++step) {
    const http::HeaderView field = kFields[step % std::size(kFields)];
    for (std::size_t age = 0; age < std::size(kFields); ++age) {
      const http::HeaderView older = kFields[(step + std::size(kFields) - 1U - age) % std::size(kFields)];
      const auto lookup = encoder.findHeader(older.name, older.value);
      if (age < std::min<std::size_t>(step, 3U)) {
        EXPECT_EQ(lookup.match, HpackLookupResult::Match::Full) << step << " " << age;
        EXPECT_EQ(lookup.index, 62U + age) << step << " " << age;
      } else {
        EXPECT_EQ(lookup.match, HpackLookupResult::Match::None) << step << " " << age;
      }
    }
    RawBytes block;
    encoder.encode(block, field.name, field.value);
    const http::HeaderView expected[]{field};
    ExpectDecodesTo(decoder, block, expected);
  }

  // An entry larger than the table empties it (RFC 7541 §4.4).
  ASSERT_EQ(encoder.findHeader("x-e", "5").match, HpackLookupResult::Match::Full);
  RawBytes block;
  const std::string largeValue(200, 'v');
  encoder.encode(block, "x-large", largeValue);
  ASSERT_EQ(encoder.dynamicTable().entryCount(), 0U);
  const http::HeaderView large[]{{"x-large", largeValue}};
  ExpectDecodesTo(decoder, block, large);
  for (const http::HeaderView& field : kFields) {
    EXPECT_EQ(encoder.findHeader(field.name, field.value).match, HpackLookupResult::Match::None);
  }

  // Added again, a field is found at its new position, and no more once a smaller table size evicted it.
  block.clear();
  encoder.encode(block, "x-e", "5");
  encoder.encode(block, "x-a", "1");
  EXPECT_EQ(encoder.findHeader("x-e", "5").index, 63U);
  EXPECT_EQ(encoder.findHeader("x-a", "1").index, 62U);
  const http::HeaderView both[]{{"x-e", "5"}, {"x-a", "1"}};
  ExpectDecodesTo(decoder, block, both);

  // The smaller size is signaled at the beginning of the next block.
  block.clear();
  encoder.setMaxDynamicTableSize(40);
  encoder.encode(block, "x-a", "1");
  EXPECT_EQ(encoder.findHeader("x-e", "5").match, HpackLookupResult::Match::None);
  EXPECT_EQ(encoder.findHeader("x-a", "1").index, 62U);
  const http::HeaderView last[]{{"x-a", "1"}};
  ExpectDecodesTo(decoder, block, last);
}

// Random messages from a few fields, through tables of several sizes: the decoder gets what was encoded, whichever
// way each field is found.
TEST(HpackRoundTrip, RandomRepeatedFieldsThroughSmallAndLargeTables) {
  static constexpr std::string_view kNames[]{"x-a", "x-b", "x-cc", "x-dd", "accept", "x-custom-name", "y", "x-z"};
  static constexpr std::string_view kValues[]{"", "1", "22", "value", "a longer value than the other ones", "v-6"};
  std::mt19937 rng(20261114);  // NOLINT(bugprone-random-generator-seed)

  for (const uint32_t tableSize : {0U, 64U, 150U, 400U, 4096U}) {
    HpackEncoder encoder(tableSize);
    HpackDecoder decoder(tableSize, std::numeric_limits<uint32_t>::max(), true);
    for (int message = 0; message < 500; ++message) {
      RawBytes block;
      // One value per name in a message: the repeated fields of a block are merged by the decoder.
      http::HeaderView fields[std::size(kNames)];
      std::size_t nbFields = 0;
      for (const std::string_view name : kNames) {
        if (rng() % 4U != 0U) {
          fields[nbFields] = {name, kValues[rng() % std::size(kValues)]};
          encoder.encode(block, fields[nbFields].name, fields[nbFields].value);
          ++nbFields;
        }
      }
      ASSERT_NO_FATAL_FAILURE(ExpectDecodesTo(decoder, block, std::span<const http::HeaderView>(fields, nbFields)))
          << tableSize << " " << message;
      ASSERT_EQ(encoder.dynamicTable().entryCount(), decoder.dynamicTable().entryCount());
    }
  }
}

namespace {

// What a lookup finds by comparing the strings of every entry of the static and dynamic tables.
HpackLookupResult LookUpByComparingEveryEntry(const HpackDynamicTable& table, std::string_view name,
                                              std::string_view value) {
  HpackLookupResult result;
  const auto staticTable = GetHpackStaticTable();
  for (uint32_t idx = 0; idx < staticTable.size(); ++idx) {
    if (staticTable[idx].name == name) {
      if (staticTable[idx].value == value) {
        return {HpackLookupResult::Match::Full, idx + 1U};
      }
      if (result.match == HpackLookupResult::Match::None) {
        result = {HpackLookupResult::Match::NameOnly, idx + 1U};
      }
    }
  }
  for (uint32_t idx = 0; idx < table.entryCount(); ++idx) {
    if (table[idx].name() == name) {
      if (table[idx].value() == value) {
        return {HpackLookupResult::Match::Full, static_cast<uint32_t>(staticTable.size()) + 1U + idx};
      }
      if (result.match == HpackLookupResult::Match::None) {
        result = {HpackLookupResult::Match::NameOnly, static_cast<uint32_t>(staticTable.size()) + 1U + idx};
      }
    }
  }
  return result;
}

}  // namespace

// The encoder compares the strings of an entry only if the fingerprint of its field, from the sizes and the first and
// last 8 bytes of its name and value, is the one of the field looked up. Names and values differing in their middle
// bytes only have the same fingerprints: a lookup still finds what comparing every entry finds, through small tables,
// tables whose lookups are indexed (128 entries and more), the newest entries and the remembered ones.
TEST(HpackEncoder, FieldsWithTheSameFingerprintAreToldApart) {
  vector<std::string> names;
  for (const char middle : {'a', 'b', 'c'}) {
    names.push_back(std::string("x-header") + middle + "-suffix1");
  }
  names.emplace_back("x-other");
  names.emplace_back("content-type");  // in the static table
  vector<std::string> values;
  for (int middle = 0; middle < 40; ++middle) {
    values.push_back("value-12" + std::to_string(1000 + middle) + "-34-tail");
  }
  values.emplace_back("value-12-34-tail");  // the same edges, another size
  std::mt19937 rng(20261116);               // NOLINT(bugprone-random-generator-seed)

  for (const uint32_t tableSize : {300U, 4096U, 16384U}) {
    HpackEncoder encoder(tableSize);
    HpackDecoder decoder(tableSize, std::numeric_limits<uint32_t>::max(), true);
    std::size_t nbFull = 0;
    std::size_t nbNameOnly = 0;
    for (int message = 0; message < 1500; ++message) {
      // One draw per statement: the order of the operands of % is unspecified, the draws would differ per compiler.
      const std::string& name = names[rng() % names.size()];
      // A few values more often than the others: some are found again while still in the table.
      const std::mt19937::result_type valueDraw = rng();
      const std::size_t nbCandidateValues = rng() % 2U == 0 ? 4U : values.size();
      const std::string& value = values[static_cast<uint32_t>(valueDraw % nbCandidateValues)];
      const HpackLookupResult expected = LookUpByComparingEveryEntry(encoder.dynamicTable(), name, value);
      const HpackLookupResult lookup = encoder.findHeader(name, value);
      ASSERT_EQ(lookup.match, expected.match) << tableSize << " " << message << " " << name << ": " << value;
      ASSERT_EQ(lookup.index, expected.index) << tableSize << " " << message << " " << name << ": " << value;
      nbFull += static_cast<std::size_t>(lookup.match == HpackLookupResult::Match::Full);
      nbNameOnly += static_cast<std::size_t>(lookup.match == HpackLookupResult::Match::NameOnly);

      RawBytes block;
      encoder.encode(block, name, value);
      const http::HeaderView fields[]{{name, value}};
      ASSERT_NO_FATAL_FAILURE(ExpectDecodesTo(decoder, block, fields)) << tableSize << " " << message;
    }
    EXPECT_GT(nbFull, 100U) << tableSize;
    EXPECT_GT(nbNameOnly, 100U) << tableSize;
    if (tableSize == 16384U) {
      EXPECT_GE(encoder.dynamicTable().entryCount(), 128U);
    }
  }
}

// ============================
// RFC 7541 and RFC 9113 §8.2 audit (2026-11-16)
// ============================
// Suite HpackAppendixC: the examples of RFC 7541 Appendix C, as test vectors.
// Suite HpackAudit: the requirements checked by the audit, and the fixes of its findings
// (CookieFieldsAreJoinedWithSemicolonAndSpace, ReferenceToAnEntryWithAnEmptyNameIsAMalformedFieldNotACompressionError,
// HuffmanLiteralsBeyondTheHeaderListLimitAreNotStored, EncoderKeepsTheStaticIndexOfANameAlsoInTheDynamicTable,
// EncoderEvictionBelowTheLookupIndexThreshold).

namespace {

using AuditError = HpackDecoder::DecodeResult::Error;

// The bytes of a hex dump without spaces, as the RFC prints its examples.
vector<uint8_t> FromHex(std::string_view hex) {
  const auto nibble = [](char ch) { return static_cast<uint32_t>(ch <= '9' ? ch - '0' : ch - 'a' + 10); };
  vector<uint8_t> bytes;
  for (std::size_t pos = 0; pos + 1U < hex.size(); pos += 2U) {
    bytes.push_back(static_cast<uint8_t>((nibble(hex[pos]) << 4U) | nibble(hex[pos + 1U])));
  }
  return bytes;
}

// Check that a block was decoded to exactly these fields, pseudo-header fields included.
void ExpectFields(const HpackDecoder::DecodeResult& result, std::span<const http::HeaderView> fields) {
  ASSERT_TRUE(result.isSuccess()) << static_cast<int>(result.error);
  const DecodedHeaders& decoded = result.decodedHeaders;
  EXPECT_EQ(decoded.headers.size() + static_cast<std::size_t>(std::popcount(decoded.pseudoHeadersMask)), fields.size());
  for (const http::HeaderView& field : fields) {
    if (field.name.front() == ':') {
      const auto pseudoHeaderIdx =
          static_cast<std::size_t>(std::ranges::find(kPseudoHeaderNames, field.name) - kPseudoHeaderNames);
      ASSERT_LT(pseudoHeaderIdx, std::size(kPseudoHeaderNames)) << field.name;
      EXPECT_TRUE(decoded.hasPseudoHeader(static_cast<PseudoHeader>(pseudoHeaderIdx))) << field.name;
      EXPECT_EQ(decoded.pseudoHeader(static_cast<PseudoHeader>(pseudoHeaderIdx)), field.value) << field.name;
    } else {
      const auto it = decoded.headers.find(field.name);
      ASSERT_NE(it, decoded.headers.end()) << field.name;
      EXPECT_EQ(it->second, field.value) << field.name;
    }
  }
}

// Check the entries of a dynamic table, the newest one first as the RFC numbers them, and its size.
void ExpectDynamicTable(const HpackDynamicTable& table, std::span<const http::HeaderView> entries,
                        std::size_t sizeBytes) {
  ASSERT_EQ(table.entryCount(), entries.size());
  for (std::size_t idx = 0; idx < entries.size(); ++idx) {
    EXPECT_EQ(table[static_cast<uint32_t>(idx)].name(), entries[idx].name) << idx;
    EXPECT_EQ(table[static_cast<uint32_t>(idx)].value(), entries[idx].value) << idx;
  }
  EXPECT_EQ(table.currentSizeBytes(), sizeBytes);
}

// A message of an example of RFC 7541 Appendix C: its encoded block, its fields, and the dynamic table after it.
struct AppendixCMessage {
  std::string_view hex;
  std::span<const http::HeaderView> fields;
  std::span<const http::HeaderView> table;
  std::size_t tableSize;
};

void ExpectDecodesAppendixC(HpackDecoder& decoder, std::span<const AppendixCMessage> messages) {
  for (const AppendixCMessage& message : messages) {
    const vector<uint8_t> block = FromHex(message.hex);
    const auto result = decoder.decode(AsBytes(block));
    ASSERT_NO_FATAL_FAILURE(ExpectFields(result, message.fields)) << message.hex;
    ASSERT_NO_FATAL_FAILURE(ExpectDynamicTable(decoder.dynamicTable(), message.table, message.tableSize))
        << message.hex;
  }
}

// The fields and dynamic tables of the request examples (C.3 and C.4).
constexpr http::HeaderView kRequest1[]{
    {":method", "GET"},
    {":scheme", "http"},
    {":path", "/"},
    {":authority", "www.example.com"},
};
constexpr http::HeaderView kRequest2[]{
    {":method", "GET"},
    {":scheme", "http"},
    {":path", "/"},
    {":authority", "www.example.com"},
    {"cache-control", "no-cache"},
};
constexpr http::HeaderView kRequest3[]{
    {":method", "GET"},
    {":scheme", "https"},
    {":path", "/index.html"},
    {":authority", "www.example.com"},
    {"custom-key", "custom-value"},
};
constexpr http::HeaderView kRequestTable1[]{{":authority", "www.example.com"}};
constexpr http::HeaderView kRequestTable2[]{
    {"cache-control", "no-cache"},
    {":authority", "www.example.com"},
};
constexpr http::HeaderView kRequestTable3[]{
    {"custom-key", "custom-value"},
    {"cache-control", "no-cache"},
    {":authority", "www.example.com"},
};

// The fields and dynamic tables of the response examples (C.5 and C.6), with a dynamic table of 256 bytes.
constexpr std::string_view kExampleSetCookie = "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1";
constexpr http::HeaderView kResponse1[]{
    {":status", "302"},
    {"cache-control", "private"},
    {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
    {"location", "https://www.example.com"},
};
constexpr http::HeaderView kResponse2[]{
    {":status", "307"},
    {"cache-control", "private"},
    {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
    {"location", "https://www.example.com"},
};
constexpr http::HeaderView kResponse3[]{
    {":status", "200"},
    {"cache-control", "private"},
    {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
    {"location", "https://www.example.com"},
    {"content-encoding", "gzip"},
    {"set-cookie", kExampleSetCookie},
};
constexpr http::HeaderView kResponseTable1[]{
    {"location", "https://www.example.com"},
    {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
    {"cache-control", "private"},
    {":status", "302"},
};
constexpr http::HeaderView kResponseTable2[]{
    {":status", "307"},
    {"location", "https://www.example.com"},
    {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
    {"cache-control", "private"},
};
constexpr http::HeaderView kResponseTable3[]{
    {"set-cookie", kExampleSetCookie},
    {"content-encoding", "gzip"},
    {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
};

constexpr uint32_t kResponseExamplesTableSize = 256;

// C.4.1 to C.4.3.
constexpr std::string_view kRequestsWithHuffmanHex[]{
    "828684418cf1e3c2e5f23a6ba0ab90f4ff",
    "828684be5886a8eb10649cbf",
    "828785bf408825a849e95ba97d7f8925a849e95bb8e8b4bf",
};

// C.5.1 to C.5.3.
constexpr std::string_view kResponsesWithoutHuffmanHex[]{
    "4803333032580770726976617465611d4d6f6e2c203231204f637420323031332032303a31333a323120474d546e1768747470733a"
    "2f2f7777772e6578616d706c652e636f6d",
    "4803333037c1c0bf",
    "88c1611d4d6f6e2c203231204f637420323031332032303a31333a323220474d54c05a04677a69707738666f6f3d4153444a4b4851"
    "4b425a584f5157454f50495541585157454f49553b206d61782d6167653d333630303b2076657273696f6e3d31",
};

// C.6.1 to C.6.3.
constexpr std::string_view kResponsesWithHuffmanHex[]{
    "488264025885aec3771a4b6196d07abe941054d444a8200595040b8166e082a62d1bff6e919d29ad171863c78f0b97c8e9ae82ae43d3",
    "4883640effc1c0bf",
    "88c16196d07abe941054d444a8200595040b8166e084a62d1bffc05a839bd9ab77ad94e7821dd7f2e6c7b335dfdfcd5b3960d5af2708"
    "7f3672c1ab270fb5291f9587316065c003ed4ee5b1063d5007",
};

}  // namespace

// C.1: the integer examples, read as dynamic table size updates (5-bit prefix) and as an index (C.1.3).
TEST(HpackAppendixC, IntegerExamples) {
  HpackDecoder decoder(4096, std::numeric_limits<uint32_t>::max(), true);
  static constexpr uint8_t ten[]{0x2A};  // C.1.1: 10 fits in the prefix
  ASSERT_TRUE(decoder.decode(AsBytes(ten)).isSuccess());
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 10U);

  static constexpr uint8_t leet[]{0x3F, 0x9A, 0x0A};  // C.1.2: 1337 = 31 + 26 + 10 * 128
  ASSERT_TRUE(decoder.decode(AsBytes(leet)).isSuccess());
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 1337U);

  RawBytes encoded;
  HpackEncoder encoder(4096);
  encoder.encodeDynamicTableSizeUpdate(encoded, 10);
  encoder.encodeDynamicTableSizeUpdate(encoded, 1337);
  static constexpr std::byte expected[]{std::byte{0x2A}, std::byte{0x3F}, std::byte{0x9A}, std::byte{0x0A}};
  ASSERT_EQ(encoded.size(), std::size(expected));
  EXPECT_EQ(std::memcmp(encoded.data(), expected, std::size(expected)), 0);

  // C.1.3: 42 starting at an octet boundary is how the length of a 42-byte string would be written with a 7-bit prefix:
  // here, the index 42 (if-range) of an indexed field.
  static constexpr uint8_t fortyTwo[]{0x80 | 42};
  const auto result = decoder.decode(AsBytes(fortyTwo));
  ASSERT_TRUE(result.isSuccess());
  EXPECT_TRUE(result.decodedHeaders.headers.contains("if-range"));
}

// C.2.1 to C.2.4: one test vector per field representation.
TEST(HpackAppendixC, HeaderFieldRepresentationExamples) {
  static constexpr http::HeaderView kCustom[]{{"custom-key", "custom-header"}};
  static constexpr http::HeaderView kPath[]{{":path", "/sample/path"}};
  static constexpr http::HeaderView kPassword[]{{"password", "secret"}};
  static constexpr http::HeaderView kMethod[]{{":method", "GET"}};
  for (const AppendixCMessage& message : {
           AppendixCMessage{"400a637573746f6d2d6b65790d637573746f6d2d686561646572", kCustom, kCustom, 55},  // C.2.1
           AppendixCMessage{"040c2f73616d706c652f70617468", kPath, {}, 0},                                  // C.2.2
           AppendixCMessage{"100870617373776f726406736563726574", kPassword, {}, 0},                        // C.2.3
           AppendixCMessage{"82", kMethod, {}, 0},                                                          // C.2.4
       }) {
    auto decoder = CreateHpackDecoder();
    ASSERT_NO_FATAL_FAILURE(ExpectDecodesAppendixC(decoder, std::span<const AppendixCMessage>(&message, 1U)));
  }
}

// C.3.1 to C.3.3: three requests on one connection, without Huffman coding.
TEST(HpackAppendixC, RequestExamplesWithoutHuffmanCoding) {
  static constexpr AppendixCMessage kMessages[]{
      {"828684410f7777772e6578616d706c652e636f6d", kRequest1, kRequestTable1, 57},
      {"828684be58086e6f2d6361636865", kRequest2, kRequestTable2, 110},
      {"828785bf400a637573746f6d2d6b65790c637573746f6d2d76616c7565", kRequest3, kRequestTable3, 164},
  };
  auto decoder = CreateHpackDecoder();
  ExpectDecodesAppendixC(decoder, kMessages);
}

// C.4.1 to C.4.3: the same requests, with Huffman coding.
TEST(HpackAppendixC, RequestExamplesWithHuffmanCoding) {
  static constexpr AppendixCMessage kMessages[]{
      {kRequestsWithHuffmanHex[0], kRequest1, kRequestTable1, 57},
      {kRequestsWithHuffmanHex[1], kRequest2, kRequestTable2, 110},
      {kRequestsWithHuffmanHex[2], kRequest3, kRequestTable3, 164},
  };
  auto decoder = CreateHpackDecoder();
  ExpectDecodesAppendixC(decoder, kMessages);
}

// C.5.1 to C.5.3: three responses on one connection whose dynamic table is limited to 256 bytes, which evicts entries,
// without Huffman coding.
TEST(HpackAppendixC, ResponseExamplesWithoutHuffmanCoding) {
  static constexpr AppendixCMessage kMessages[]{
      {kResponsesWithoutHuffmanHex[0], kResponse1, kResponseTable1, 222},
      {kResponsesWithoutHuffmanHex[1], kResponse2, kResponseTable2, 222},
      {kResponsesWithoutHuffmanHex[2], kResponse3, kResponseTable3, 215},
  };
  HpackDecoder decoder(kResponseExamplesTableSize, std::numeric_limits<uint32_t>::max(), true,
                       HpackDecoder::Messages::Responses);
  ExpectDecodesAppendixC(decoder, kMessages);
}

// C.6.1 to C.6.3: the same responses, with Huffman coding.
TEST(HpackAppendixC, ResponseExamplesWithHuffmanCoding) {
  static constexpr AppendixCMessage kMessages[]{
      {kResponsesWithHuffmanHex[0], kResponse1, kResponseTable1, 222},
      {kResponsesWithHuffmanHex[1], kResponse2, kResponseTable2, 222},
      {kResponsesWithHuffmanHex[2], kResponse3, kResponseTable3, 215},
  };
  HpackDecoder decoder(kResponseExamplesTableSize, std::numeric_limits<uint32_t>::max(), true,
                       HpackDecoder::Messages::Responses);
  ExpectDecodesAppendixC(decoder, kMessages);
}

namespace {

// Encode the messages of an example with every field indexed, as the encoder of the RFC does, and check the bytes.
void ExpectEncodesAppendixC(HpackEncoder& encoder, std::span<const std::span<const http::HeaderView>> messages,
                            std::span<const std::string_view> expectedHex) {
  ASSERT_EQ(messages.size(), expectedHex.size());
  for (std::size_t idx = 0; idx < messages.size(); ++idx) {
    RawBytes block;
    for (const http::HeaderView& field : messages[idx]) {
      encoder.encode(block, field.name, field.value, HpackEncoder::IndexingMode::Indexed);
    }
    const vector<uint8_t> expected = FromHex(expectedHex[idx]);
    ASSERT_EQ(block.size(), expected.size()) << idx;
    EXPECT_EQ(std::memcmp(block.data(), expected.data(), expected.size()), 0) << idx;
  }
}

}  // namespace

// C.4: the encoder makes the choices of the one of the RFC (Huffman coding when shorter, lowest matching index), so it
// writes the same bytes.
TEST(HpackAppendixC, EncoderWritesTheRequestExamplesWithHuffmanCoding) {
  static constexpr std::span<const http::HeaderView> kMessages[]{kRequest1, kRequest2, kRequest3};
  HpackEncoder encoder(4096);
  ASSERT_NO_FATAL_FAILURE(ExpectEncodesAppendixC(encoder, kMessages, kRequestsWithHuffmanHex));
  ExpectDynamicTable(encoder.dynamicTable(), kRequestTable3, 164);
}

// C.6.1: the first response, before the dynamic table holds any of the names (see
// HpackAudit.EncoderKeepsTheStaticIndexOfANameAlsoInTheDynamicTable for the next ones).
TEST(HpackAppendixC, EncoderWritesTheFirstResponseExampleWithHuffmanCoding) {
  static constexpr std::span<const http::HeaderView> kMessages[]{kResponse1};
  HpackEncoder encoder(kResponseExamplesTableSize);
  ASSERT_NO_FATAL_FAILURE(
      ExpectEncodesAppendixC(encoder, kMessages, std::span<const std::string_view>(kResponsesWithHuffmanHex, 1U)));
  ExpectDynamicTable(encoder.dynamicTable(), kResponseTable1, 222);
}

// C.6: the three responses leave the dynamic table of the encoder as in the RFC, with its evictions.
TEST(HpackAppendixC, EncoderEvictsLikeTheResponseExamples) {
  static constexpr AppendixCMessage kMessages[]{
      {{}, kResponse1, kResponseTable1, 222},
      {{}, kResponse2, kResponseTable2, 222},
      {{}, kResponse3, kResponseTable3, 215},
  };
  HpackEncoder encoder(kResponseExamplesTableSize);
  HpackDecoder decoder(kResponseExamplesTableSize, std::numeric_limits<uint32_t>::max(), true,
                       HpackDecoder::Messages::Responses);
  for (const AppendixCMessage& message : kMessages) {
    RawBytes block;
    for (const http::HeaderView& field : message.fields) {
      encoder.encode(block, field.name, field.value, HpackEncoder::IndexingMode::Indexed);
    }
    ASSERT_NO_FATAL_FAILURE(ExpectDynamicTable(encoder.dynamicTable(), message.table, message.tableSize));
    const auto result = decoder.decode(block);
    ASSERT_NO_FATAL_FAILURE(ExpectFields(result, message.fields));
    ASSERT_NO_FATAL_FAILURE(ExpectDynamicTable(decoder.dynamicTable(), message.table, message.tableSize));
  }
}

// ---- Integers (RFC 7541 §5.1)

// An integer may be written with more continuation bytes than needed (zero groups): the decoder accepts 8 of them, and
// fails without reading the others beyond, so that a run of 0x80 bytes costs nothing.
TEST(HpackAudit, IntegerContinuationBytesAreBounded) {
  for (uint32_t nbZeroGroups = 0; nbZeroGroups < 12U; ++nbZeroGroups) {
    HpackDecoder decoder(4096, std::numeric_limits<uint32_t>::max(), true);
    vector<uint8_t> block{0x3F};  // dynamic table size update, prefix full: 31 + ...
    block.insert(block.end(), nbZeroGroups, uint8_t{0x80});
    block.push_back(0x00);
    const auto result = decoder.decode(AsBytes(block));
    if (nbZeroGroups < 8U) {
      EXPECT_TRUE(result.isSuccess()) << nbZeroGroups;
      EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 31U) << nbZeroGroups;
    } else {
      EXPECT_EQ(result.error, AuditError::FailedToDecodeDynamicTableSizeUpdate) << nbZeroGroups;
      EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 4096U) << nbZeroGroups;
    }
  }

  // The same in each position of an integer: index, name index, string length.
  static constexpr uint32_t kRunSize = 200000;
  struct Case {
    uint8_t firstByte;
    AuditError expected;
  };
  for (const Case& testCase : {
           Case{0xFF, AuditError::FailedToDecodeIndexedHeaderFieldIndex},
           Case{0x7F, AuditError::FailedToDecodeLiteralHeaderIndex},
           Case{0x0F, AuditError::FailedToDecodeLiteralHeaderIndex},
           Case{0x1F, AuditError::FailedToDecodeLiteralHeaderIndex},
           Case{0x3F, AuditError::FailedToDecodeDynamicTableSizeUpdate},
       }) {
    auto decoder = CreateHpackDecoder();
    vector<uint8_t> block{testCase.firstByte};
    block.insert(block.end(), kRunSize, uint8_t{0x80});
    block.push_back(0x00);
    const auto result = decoder.decode(AsBytes(block));
    EXPECT_EQ(result.error, testCase.expected) << static_cast<int>(testCase.firstByte);
    EXPECT_TRUE(result.isCompressionError());
  }
  auto decoder = CreateHpackDecoder();
  vector<uint8_t> block{0x00, 0x7F};  // literal field without indexing, new name, whose length never ends
  block.insert(block.end(), kRunSize, uint8_t{0x80});
  block.push_back(0x00);
  EXPECT_EQ(decoder.decode(AsBytes(block)).error, AuditError::FailedToDecodeLiteralHeaderName);

  // An integer whose last byte is missing, at each length.
  for (uint32_t nbContinuationBytes = 0; nbContinuationBytes < 10U; ++nbContinuationBytes) {
    vector<uint8_t> truncated{0xFF};
    truncated.insert(truncated.end(), nbContinuationBytes, uint8_t{0xFF});
    EXPECT_EQ(decoder.decode(AsBytes(truncated)).error, AuditError::FailedToDecodeIndexedHeaderFieldIndex);
  }
}

// ---- Strings (RFC 7541 §5.2)

// An empty Huffman string is valid, in a name (a malformed field for HTTP, not a compression error) and in a value.
TEST(HpackAudit, EmptyHuffmanStringsAreDecoded) {
  auto decoder = CreateHpackDecoder();
  static constexpr uint8_t emptyValue[]{0x00, 0x01, 'x', 0x80};
  const auto result = decoder.decode(AsBytes(emptyValue));
  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.headers.at("x"), "");

  static constexpr uint8_t emptyName[]{0x00, 0x80, 0x01, 'v'};
  const auto nameResult = decoder.decode(AsBytes(emptyName));
  EXPECT_EQ(nameResult.error, AuditError::MalformedFieldName);
  EXPECT_FALSE(nameResult.isCompressionError());
}

// A Huffman string declares the length of its encoded bytes: a string cut by the end of the block, or whose last
// symbol misses bits, is an error, and the bytes after the declared length belong to the next field.
TEST(HpackAudit, HuffmanStringLengthIsTheOneOfItsEncodedBytes) {
  auto decoder = CreateHpackDecoder();
  // "custom-key" (8 bytes) and "custom-value" (9 bytes), as in C.4.3, in a field without indexing, then the index 16
  // (accept-encoding).
  const vector<uint8_t> block = FromHex("008825a849e95ba97d7f8925a849e95bb8e8b4bf90");
  const auto result = decoder.decode(AsBytes(block));
  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.headers.at("custom-key"), "custom-value");
  EXPECT_EQ(result.decodedHeaders.headers.at("accept-encoding"), "gzip, deflate");

  // Each strict prefix of the field is an error: a truncated integer, name or value.
  for (std::size_t size = 1; size + 1U < block.size(); ++size) {
    const auto truncated = decoder.decode(AsBytes(std::span<const uint8_t>(block.data(), size)));
    EXPECT_TRUE(truncated.isCompressionError()) << size;
  }

  // A declared length one byte shorter cuts the last symbol: its 2 bits left (00) are not a padding of 1s.
  vector<uint8_t> shorter = block;
  shorter[10] = 0x88;
  EXPECT_EQ(decoder.decode(AsBytes(shorter)).error, AuditError::FailedToDecodeLiteralHeaderValue);
}

// ---- Indexes (RFC 7541 §2.3.3, §6.1, §6.2)

TEST(HpackAudit, IndexesAtTheBoundaryOfTheStaticAndDynamicTables) {
  auto decoder = CreateHpackDecoder();

  static constexpr uint8_t zero[]{0x80};
  EXPECT_EQ(decoder.decode(AsBytes(zero)).error, AuditError::InvalidIndexZeroInIndexedHeaderField);

  static constexpr uint8_t last[]{0x80 | 61};
  const auto lastResult = decoder.decode(AsBytes(last));
  ASSERT_TRUE(lastResult.isSuccess());
  EXPECT_EQ(lastResult.decodedHeaders.headers.at("www-authenticate"), "");

  // 62 is the first entry of the dynamic table, empty for now: as an index and as a name index of each literal form.
  static constexpr uint8_t first[]{0x80 | 62};
  EXPECT_EQ(decoder.decode(AsBytes(first)).error, AuditError::IndexOutOfBoundsInIndexedHeaderField);
  for (const uint8_t flags : {uint8_t{0x40}, uint8_t{0x00}, uint8_t{0x10}}) {
    vector<uint8_t> block;
    AppendInteger(block, flags, flags == 0x40 ? 6 : 4, 62);
    block.push_back(0x01);
    block.push_back('v');
    const auto result = decoder.decode(AsBytes(block));
    EXPECT_EQ(result.error, AuditError::IndexOutOfBoundsForHeaderName) << static_cast<int>(flags);
    EXPECT_TRUE(result.isCompressionError());
    EXPECT_EQ(decoder.dynamicTable().entryCount(), 0U);
  }

  // Name index 61 (www-authenticate), the last of the static table, with indexing: it becomes the entry 62.
  static constexpr uint8_t literal[]{0x40 | 61, 0x01, 'v'};
  const auto literalResult = decoder.decode(AsBytes(literal));
  ASSERT_TRUE(literalResult.isSuccess());
  EXPECT_EQ(literalResult.decodedHeaders.headers.at("www-authenticate"), "v");

  const auto firstResult = decoder.decode(AsBytes(first));
  ASSERT_TRUE(firstResult.isSuccess());
  EXPECT_EQ(firstResult.decodedHeaders.headers.at("www-authenticate"), "v");

  static constexpr uint8_t second[]{0x80 | 63};
  EXPECT_EQ(decoder.decode(AsBytes(second)).error, AuditError::IndexOutOfBoundsInIndexedHeaderField);

  // The name of the entry 62 in each literal form: only the one with indexing adds an entry.
  std::size_t expectedEntryCount = 1;
  for (const uint8_t flags : {uint8_t{0x00}, uint8_t{0x10}, uint8_t{0x40}}) {
    vector<uint8_t> block;
    AppendInteger(block, flags, flags == 0x40 ? 6 : 4, 62);
    block.push_back(0x02);
    block.push_back('w');
    block.push_back(static_cast<uint8_t>('0' + (flags >> 4U)));
    const auto result = decoder.decode(AsBytes(block));
    ASSERT_TRUE(result.isSuccess()) << static_cast<int>(flags);
    EXPECT_EQ(result.decodedHeaders.headers.at("www-authenticate").front(), 'w');
    expectedEntryCount += flags == 0x40 ? 1U : 0U;
    EXPECT_EQ(decoder.dynamicTable().entryCount(), expectedEntryCount);
  }
  EXPECT_EQ(decoder.dynamicTable()[0].value(), "w4");
  EXPECT_EQ(decoder.dynamicTable()[1].value(), "v");
}

// ---- Dynamic table (RFC 7541 §4)

// §4.4: an entry larger than the table empties it, which is not an error. Its name is here the one of an entry that
// this empties: the decoded field keeps it.
TEST(HpackAudit, EntryLargerThanTheTableEmptiesItWithoutError) {
  HpackDecoder decoder(100, std::numeric_limits<uint32_t>::max(), true);
  static constexpr uint8_t small[]{0x40, 0x03, 'x', '-', 'a', 0x01, 'b'};
  ASSERT_TRUE(decoder.decode(AsBytes(small)).isSuccess());
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 1U);
  EXPECT_EQ(decoder.dynamicTable().currentSizeBytes(), 36U);

  // 3 + 66 + 32 = 101 bytes: one more than the table.
  vector<uint8_t> block{0x7E, 66};
  block.insert(block.end(), 66U, uint8_t{'v'});
  block.push_back(0x90);  // then accept-encoding: the block goes on
  const auto result = decoder.decode(AsBytes(block));
  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.headers.at("x-a"), std::string(66, 'v'));
  EXPECT_EQ(result.decodedHeaders.headers.at("accept-encoding"), "gzip, deflate");
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 0U);
  EXPECT_EQ(decoder.dynamicTable().currentSizeBytes(), 0U);

  // An entry of exactly the size of the table is kept: 3 + 65 + 32.
  vector<uint8_t> exact{0x40, 0x03, 'x', '-', 'a', 65};
  exact.insert(exact.end(), 65U, uint8_t{'v'});
  ASSERT_TRUE(decoder.decode(AsBytes(exact)).isSuccess());
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 1U);
  EXPECT_EQ(decoder.dynamicTable().currentSizeBytes(), 100U);
}

// §4.2, §6.3: several size updates may start a block (the smallest size then the last one, or more); none may follow a
// field, even a field that is malformed or beyond the header list size limit; the limit is the advertised one.
TEST(HpackAudit, DynamicTableSizeUpdatesOnlyStartABlock) {
  HpackDecoder decoder(4096, 100, true);
  static constexpr uint8_t fill[]{0x40, 0x03, 'x', '-', 'a', 0x01, 'b', 0x40, 0x03, 'x', '-', 'b', 0x01, 'c'};
  ASSERT_TRUE(decoder.decode(AsBytes(fill)).isSuccess());
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 2U);

  // 36, which evicts the oldest entry, 0, 4096 (31 + 4065), then the fields.
  static constexpr uint8_t updates[]{0x3F, 0x05, 0x20, 0x3F, 0xE1, 0x1F, 0x82, 0x40, 0x03, 'x', '-', 'c', 0x01, 'd'};
  const auto result = decoder.decode(AsBytes(std::span<const uint8_t>(updates, 2U)));
  ASSERT_TRUE(result.isSuccess());
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 1U);
  EXPECT_EQ(decoder.dynamicTable()[0].name(), "x-b");

  ASSERT_TRUE(decoder.decode(AsBytes(updates)).isSuccess());
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 4096U);
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 1U);
  EXPECT_EQ(decoder.dynamicTable()[0].name(), "x-c");

  // After a field of each kind.
  static constexpr uint8_t afterIndexed[]{0x82, 0x20};
  static constexpr uint8_t afterLiteral[]{0x00, 0x01, 'x', 0x01, 'v', 0x20};
  static constexpr uint8_t afterMalformed[]{0x00, 0x01, 'X', 0x01, 'v', 0x20};
  static constexpr uint8_t afterDynamic[]{0xBE, 0x20};
  const std::span<const uint8_t> blocks[]{afterIndexed, afterLiteral, afterMalformed, afterDynamic};
  for (const std::span<const uint8_t> block : blocks) {
    EXPECT_EQ(decoder.decode(AsBytes(block)).error, AuditError::DynamicTableSizeUpdateAfterField);
    EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 4096U);
  }
  // After a field beyond the header list size limit (100 bytes), which is not stored.
  vector<uint8_t> beyond{0x00, 0x01, 'x', 100};
  beyond.insert(beyond.end(), 100U, uint8_t{'v'});
  beyond.push_back(0x20);
  EXPECT_EQ(decoder.decode(AsBytes(beyond)).error, AuditError::DynamicTableSizeUpdateAfterField);

  // The limit: 4096 is accepted, 4097 is not, and the table is unchanged.
  static constexpr uint8_t above[]{0x3F, 0xE2, 0x1F};
  EXPECT_EQ(decoder.decode(AsBytes(above)).error, AuditError::DynamicTableSizeUpdateAboveLimit);
  EXPECT_EQ(decoder.dynamicTable().maxSizeBytes(), 4096U);
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 1U);
}

// ---- Lifetime of the decoded fields

// The fields of a block that reference the dynamic table keep their bytes when later fields of the same block evict
// their entries, overwrite them (the buffer of the oldest entry is reused for the new one) or empty the table.
TEST(HpackAudit, DecodedFieldsOutliveTheEntriesEvictedByTheirBlock) {
  // Room for two entries of 4 + 30 + 32 = 66 bytes.
  HpackDecoder decoder(140, std::numeric_limits<uint32_t>::max(), true);
  const std::string firstValue(30, 'a');
  const std::string secondValue(30, 'b');
  vector<uint8_t> fill;
  const http::HeaderView entries[]{{"x-01", firstValue}, {"x-02", secondValue}};
  for (const auto& [name, value] : entries) {
    fill.push_back(0x40);
    fill.push_back(static_cast<uint8_t>(name.size()));
    fill.insert(fill.end(), name.begin(), name.end());
    fill.push_back(static_cast<uint8_t>(value.size()));
    fill.insert(fill.end(), value.begin(), value.end());
  }
  ASSERT_TRUE(decoder.decode(AsBytes(fill)).isSuccess());
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 2U);

  // Both entries by index (63 is the oldest, x-01), then literals with indexing, all with the name of the newest entry
  // (62), which add() reads while it evicts:
  //  - 60 bytes: evicts x-01, whose buffer is reused, then x-02 itself, the entry of the name
  //  - 40 bytes: evicts the only entry, the one of the name, whose buffer is large enough for the new one
  //  - 90 bytes: the same with a buffer to enlarge
  //  - 120 bytes: an entry larger than the table, which empties it
  vector<uint8_t> block{0xBF, 0xBE};
  const std::string values[]{std::string(60, 'c'), std::string(40, 'd'), std::string(90, 'f'), std::string(120, 'e')};
  for (const std::string& value : values) {
    block.push_back(0x40 | 62);
    block.push_back(static_cast<uint8_t>(value.size()));
    block.insert(block.end(), value.begin(), value.end());
  }
  // The block is freed before the decoded fields are read: none points into it either.
  auto ownedBlock = std::make_unique<uint8_t[]>(block.size());
  std::memcpy(ownedBlock.get(), block.data(), block.size());
  const auto result = decoder.decode(AsBytes(std::span<const uint8_t>(ownedBlock.get(), block.size())));
  ownedBlock.reset();
  std::ranges::fill(block, uint8_t{0});

  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(decoder.dynamicTable().entryCount(), 0U);
  ASSERT_EQ(result.decodedHeaders.headers.size(), 2U);
  EXPECT_EQ(result.decodedHeaders.headers.at("x-01"), firstValue);
  EXPECT_EQ(result.decodedHeaders.headers.at("x-02"),
            secondValue + "," + values[0] + "," + values[1] + "," + values[2] + "," + values[3]);
  EXPECT_EQ(result.headerListSize, (2U * 66U) + 96U + 76U + 126U + 156U);
}

// The same for the pseudo-header fields, which are kept apart from the regular ones, and for the fields taken over by
// an owner (swap of the map and of the storage) before the table and the storage of the decoder are used again.
TEST(HpackAudit, PseudoHeadersAndTakenOverFieldsOutliveTheTableAndTheInput) {
  HpackDecoder decoder(200, std::numeric_limits<uint32_t>::max(), true);
  const std::string authority = "an-authority-of-forty-bytes.example.com";
  vector<uint8_t> fill{0x41, static_cast<uint8_t>(authority.size())};  // literal with indexing, name :authority
  fill.insert(fill.end(), authority.begin(), authority.end());
  ASSERT_TRUE(decoder.decode(AsBytes(fill)).isSuccess());

  SvToSvMap ownedHeaders;
  DecodedHeaders::Storage ownedStorage{DecodedHeaders::kStorageInitialCapacity};
  std::string_view ownedAuthority;
  std::string_view ownedMethod;
  {
    // :method GET (static), :authority of the dynamic table, then a field emptying the table and one from the block.
    vector<uint8_t> block{0x82, 0xBE, 0x40, 0x01, 'x', 0x7F, 0x49};  // value of 127 + 73 = 200 bytes
    block.insert(block.end(), 200U, uint8_t{'v'});
    static constexpr uint8_t kRawField[]{0x00, 0x01, 'y', 0x03, 'r', 'a', 'w'};
    block.insert(block.end(), std::begin(kRawField), std::end(kRawField));
    const auto result = decoder.decode(AsBytes(block));
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(decoder.dynamicTable().entryCount(), 0U);
    ownedAuthority = result.decodedHeaders.pseudoHeader(PseudoHeader::Authority);
    ownedMethod = result.decodedHeaders.pseudoHeader(PseudoHeader::Method);
    ownedHeaders.swap(result.decodedHeaders.headers);
    ownedStorage.swap(result.decodedHeaders.storage);
    std::ranges::fill(block, uint8_t{0});
  }

  // Other blocks, with other bytes at the same places of the table and of the storage of the decoder.
  for (int iter = 0; iter < 4; ++iter) {
    vector<uint8_t> other{0x41, static_cast<uint8_t>(authority.size())};
    other.insert(other.end(), static_cast<uint32_t>(authority.size()), static_cast<uint8_t>('0' + iter));
    static constexpr uint8_t kOtherFields[]{0x40, 0x01, 'x', 0x03, 'z', 'z', 'z', 0x00, 0x01, 'y', 0x03, 'z', 'z', 'z'};
    other.insert(other.end(), std::begin(kOtherFields), std::end(kOtherFields));
    ASSERT_TRUE(decoder.decode(AsBytes(other)).isSuccess());
  }

  EXPECT_EQ(ownedAuthority, authority);
  EXPECT_EQ(ownedMethod, "GET");
  ASSERT_EQ(ownedHeaders.size(), 2U);
  EXPECT_EQ(ownedHeaders.at("x"), std::string(200, 'v'));
  EXPECT_EQ(ownedHeaders.at("y"), "raw");
}

// ---- Header list size (RFC 9113 §6.5.2: names + values + 32 bytes per field, as received)

TEST(HpackAudit, HeaderListSizeCountsEachFieldAsReceived) {
  // :method GET (7 + 3 + 32), accept-encoding "gzip, deflate" twice (15 + 13 + 32 each), then "x: " (1 + 0 + 32).
  static constexpr uint8_t block[]{0x82, 0x90, 0x90, 0x00, 0x01, 'x', 0x00};
  static constexpr uint32_t kSize = 42U + 60U + 60U + 33U;

  HpackDecoder exact(4096, kSize, true);
  const auto result = exact.decode(AsBytes(block));
  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(result.headerListSize, kSize);
  EXPECT_EQ(result.decodedHeaders.namesAndValuesSize, kSize - (4U * 32U));
  // The joined value takes one byte for its separator, which the limit does not count.
  EXPECT_EQ(result.decodedHeaders.headers.at("accept-encoding"), "gzip, deflate,gzip, deflate");

  HpackDecoder below(4096, kSize - 1U, true);
  const auto refused = below.decode(AsBytes(block));
  EXPECT_EQ(refused.error, AuditError::HeaderListTooLarge);
  EXPECT_FALSE(refused.isCompressionError());
  EXPECT_EQ(refused.headerListSize, kSize);  // the whole block is decoded
  EXPECT_FALSE(refused.decodedHeaders.headers.contains("x"));

  // Beyond the limit, the block is still decoded for its effects on the dynamic table, and its encoding errors stay
  // compression errors.
  static constexpr uint8_t withEntry[]{0x82, 0x90, 0x90, 0x40, 0x01, 'x', 0x01, 'v'};
  HpackDecoder small(4096, 50, true);
  EXPECT_EQ(small.decode(AsBytes(withEntry)).error, AuditError::HeaderListTooLarge);
  EXPECT_EQ(small.dynamicTable().entryCount(), 1U);
  static constexpr uint8_t withBadIndex[]{0x82, 0x90, 0x90, 0xC0};
  const auto badIndex = small.decode(AsBytes(withBadIndex));
  EXPECT_EQ(badIndex.error, AuditError::IndexOutOfBoundsInIndexedHeaderField);
  EXPECT_TRUE(badIndex.isCompressionError());
}

// ---- Field validity at decode time (RFC 9113 §8.2, §8.3)

TEST(HpackAudit, FieldNamesAndValuesAreValidatedAtDecodeTime) {
  struct Case {
    std::string_view name;
    std::string_view value;
    AuditError expected;
  };
  RawBytes block;
  using namespace std::string_view_literals;
  for (const Case& testCase : {
           // §8.2.1: names are lower-case tokens.
           Case{"x-a", "v", AuditError::None},
           Case{"X-a", "v", AuditError::MalformedFieldName},
           Case{"x-A", "v", AuditError::MalformedFieldName},
           Case{"", "v", AuditError::MalformedFieldName},
           Case{"x a", "v", AuditError::MalformedFieldName},
           Case{"x:a", "v", AuditError::MalformedFieldName},
           Case{"x\x7f", "v", AuditError::MalformedFieldName},
           Case{"x\x80", "v", AuditError::MalformedFieldName},
           Case{"x\xff", "v", AuditError::MalformedFieldName},
           Case{"x\0a"sv, "v", AuditError::MalformedFieldName},
           Case{"x\ta", "v", AuditError::MalformedFieldName},
           // §8.3: only the defined pseudo-header fields, in lower case.
           Case{":method", "GET", AuditError::None},
           Case{":Method", "GET", AuditError::UndefinedPseudoHeaderField},
           Case{":", "v", AuditError::UndefinedPseudoHeaderField},
           Case{":unknown", "v", AuditError::UndefinedPseudoHeaderField},
           Case{":path ", "/", AuditError::UndefinedPseudoHeaderField},
           // §8.2.1: values without NUL, CR, LF, nor leading or trailing SP / HTAB. The other bytes are allowed.
           Case{"x-a", "", AuditError::None},
           Case{"x-a", "a b\tc", AuditError::None},
           Case{"x-a", "\x01\x7f\x80\xff", AuditError::None},
           Case{"x-a", " ", AuditError::MalformedFieldValue},
           Case{"x-a", "\t", AuditError::MalformedFieldValue},
           Case{"x-a", " v", AuditError::MalformedFieldValue},
           Case{"x-a", "v\t", AuditError::MalformedFieldValue},
           Case{"x-a", "a\rb", AuditError::MalformedFieldValue},
           Case{"x-a", "a\nb", AuditError::MalformedFieldValue},
           Case{"x-a", "a\0b"sv, AuditError::MalformedFieldValue},
           Case{":path", "/a\nb", AuditError::MalformedFieldValue},
           Case{":authority", " host", AuditError::MalformedFieldValue},
           // §8.2.2: connection-specific fields, TE excepted with the value "trailers".
           Case{"connection", "close", AuditError::ConnectionSpecificField},
           Case{"keep-alive", "timeout=5", AuditError::ConnectionSpecificField},
           Case{"proxy-connection", "keep-alive", AuditError::ConnectionSpecificField},
           Case{"transfer-encoding", "chunked", AuditError::ConnectionSpecificField},
           Case{"upgrade", "h2c", AuditError::ConnectionSpecificField},
           Case{"te", "trailers", AuditError::None},
           Case{"te", "TRAILERS", AuditError::None},
           Case{"te", "gzip", AuditError::ConnectionSpecificField},
           Case{"te", "trailers, gzip", AuditError::ConnectionSpecificField},
           Case{"te", "", AuditError::ConnectionSpecificField},
       }) {
    // As a literal with raw strings, as a literal with Huffman strings (the encoder chooses), then from the table.
    for (const bool withEncoder : {false, true}) {
      HpackDecoder decoder = CreateHpackDecoder();
      block.clear();
      if (withEncoder) {
        HpackEncoder encoder(4096);
        if (testCase.name.empty() ||
            std::ranges::any_of(testCase.name, [](char ch) { return ch >= 'A' && ch <= 'Z'; })) {
          // the encoder asserts on empty and upper-case names
          continue;
        }
        encoder.encode(block, testCase.name, testCase.value);
      } else {
        block.push_back(std::byte{0x40U});
        block.push_back(static_cast<std::byte>(testCase.name.size()));
        block.append(reinterpret_cast<const std::byte*>(testCase.name.data()), testCase.name.size());
        block.push_back(static_cast<std::byte>(testCase.value.size()));
        block.append(reinterpret_cast<const std::byte*>(testCase.value.data()), testCase.value.size());
      }
      const auto result = decoder.decode(block);
      EXPECT_EQ(result.error, testCase.expected) << testCase.name << ": " << testCase.value << " " << withEncoder;
      EXPECT_FALSE(result.isCompressionError());
      if (decoder.dynamicTable().entryCount() == 1U && !testCase.name.empty()) {
        // The field is in the dynamic table whatever its validity (RFC 9113 §4.3): each reference gets the same answer.
        static constexpr uint8_t indexed[]{0xBEU};
        EXPECT_EQ(decoder.decode(AsBytes(indexed)).error, testCase.expected) << testCase.name;
        EXPECT_EQ(decoder.decode(AsBytes(indexed)).error, testCase.expected) << testCase.name;
      }
    }
  }
}

TEST(HpackAudit, PseudoHeaderOrderAndDuplicates) {
  struct Case {
    std::string_view hex;
    AuditError expected;
  };
  for (const Case& testCase : {
           Case{"828486410168", AuditError::None},  // :method GET, :path /, :scheme http, :authority h (literal)
           Case{"8284", AuditError::None},
           Case{"8282", AuditError::DuplicatePseudoHeaderField},          // :method GET twice
           Case{"8283", AuditError::DuplicatePseudoHeaderField},          // :method GET then :method POST
           Case{"9082", AuditError::PseudoHeaderFieldAfterRegularField},  // accept-encoding then :method
           Case{"829084", AuditError::PseudoHeaderFieldAfterRegularField},
           Case{"88", AuditError::None},  // :status 200: defined, its context is checked by the owner of the message
       }) {
    auto decoder = CreateHpackDecoder();
    const vector<uint8_t> block = FromHex(testCase.hex);
    const auto result = decoder.decode(AsBytes(block));
    EXPECT_EQ(result.error, testCase.expected) << testCase.hex;
  }
}

// RFC 9113 §8.2.3: the cookie fields of a field block are concatenated with the two bytes "; " (0x3b, 0x20) before
// they reach a context that is not HTTP/2, like the handlers of a server. HttpClient sends one field per cookie-pair.
TEST(HpackAudit, CookieFieldsAreJoinedWithSemicolonAndSpace) {
  auto decoder = CreateHpackDecoder();
  // Three literal fields with the name index 32 (cookie).
  static constexpr uint8_t block[]{0x60, 0x03, 'a', '=', 'b', 0x60, 0x03, 'c', '=', 'd', 0x60, 0x03, 'e', '=', 'f'};
  const auto result = decoder.decode(AsBytes(block));
  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.headers.at("cookie"), "a=b; c=d; e=f");
}

// An empty name is a valid HPACK string and a malformed HTTP field (RFC 9113 §8.2.1): its field block is decoded, and
// its stream reset. A reference to the entry that such a field added to the dynamic table is the same malformed field,
// not an invalid index (RFC 7541 §2.3.3: an error only for an index beyond both tables).
TEST(HpackAudit, ReferenceToAnEntryWithAnEmptyNameIsAMalformedFieldNotACompressionError) {
  auto decoder = CreateHpackDecoder();
  static constexpr uint8_t literal[]{0x40, 0x00, 0x01, 'v'};
  const auto literalResult = decoder.decode(AsBytes(literal));
  EXPECT_EQ(literalResult.error, AuditError::MalformedFieldName);
  EXPECT_FALSE(literalResult.isCompressionError());
  ASSERT_EQ(decoder.dynamicTable().entryCount(), 1U);
  EXPECT_EQ(decoder.dynamicTable().currentSizeBytes(), 33U);

  static constexpr uint8_t indexed[]{0xBE};
  const auto indexedResult = decoder.decode(AsBytes(indexed));
  EXPECT_EQ(indexedResult.error, AuditError::MalformedFieldName);
  EXPECT_FALSE(indexedResult.isCompressionError());

  // As the name of a literal field without indexing (4-bit prefix: 15 + 47).
  static constexpr uint8_t named[]{0x0F, 0x2F, 0x01, 'w'};
  const auto namedResult = decoder.decode(AsBytes(named));
  EXPECT_EQ(namedResult.error, AuditError::MalformedFieldName);
  EXPECT_FALSE(namedResult.isCompressionError());
}

// The fields of a header list beyond the limit are decoded and not stored (docs/protocols/http2.md): this holds for
// raw strings and for references to the tables, and has to for Huffman strings, which are decoded into the storage of
// the decoded headers.
TEST(HpackAudit, HuffmanLiteralsBeyondTheHeaderListLimitAreNotStored) {
  static constexpr uint32_t kMaxHeaderListSize = 1024;
  static constexpr std::size_t kNbFields = 64;
  static constexpr std::size_t kValueSize = 4000;

  // Raw strings: 64 fields "x: 000...0" of 4000 bytes.
  {
    HpackDecoder decoder(4096, kMaxHeaderListSize, true);
    vector<uint8_t> block;
    for (std::size_t idx = 0; idx < kNbFields; ++idx) {
      const vector<uint8_t> field = LiteralFieldX(std::string(kValueSize, '0'));
      block.insert(block.end(), field.begin(), field.end());
    }
    const auto result = decoder.decode(AsBytes(block));
    EXPECT_EQ(result.error, AuditError::HeaderListTooLarge);
    EXPECT_EQ(result.headerListSize, kNbFields * (1U + kValueSize + 32U));
    EXPECT_LE(result.decodedHeaders.storage.capacity(), 8U * kMaxHeaderListSize);
  }

  // The same fields with Huffman strings: '0' is the code 00000, so 4000 of them are 2500 zero bytes.
  HpackDecoder decoder(4096, kMaxHeaderListSize, true);
  const vector<uint8_t> huffmanValue(kValueSize * 5U / 8U, uint8_t{0});
  vector<uint8_t> block;
  for (std::size_t idx = 0; idx < kNbFields; ++idx) {
    const vector<uint8_t> field = LiteralFieldX(huffmanValue, true);
    block.insert(block.end(), field.begin(), field.end());
  }
  ASSERT_LT(block.size(), 256U << 10U);  // within the bound of a header block
  const auto result = decoder.decode(AsBytes(block));
  EXPECT_EQ(result.error, AuditError::HeaderListTooLarge);
  EXPECT_EQ(result.headerListSize, kNbFields * (1U + kValueSize + 32U));
  EXPECT_LE(result.decodedHeaders.storage.capacity(), 8U * kMaxHeaderListSize);

  // The decoder keeps its storage for the next blocks of the connection.
  static constexpr uint8_t next[]{0x82};
  const auto nextResult = decoder.decode(AsBytes(next));
  ASSERT_TRUE(nextResult.isSuccess());
  EXPECT_LE(nextResult.decodedHeaders.storage.capacity(), 8U * kMaxHeaderListSize);
}

namespace {

// A string literal made of '0' characters: Huffman-coded, 8 of them are 5 zero bytes ('0' is the code 00000).
struct Zeros {
  std::size_t nbChars;  // A multiple of 8 when Huffman-coded
  bool huffman;
};

// Append a literal field with a new name (RFC 7541 §6.2), its name and value made of '0'.
void AppendZerosField(vector<uint8_t>& out, Zeros name, Zeros value, bool withIndexing = false) {
  out.push_back(withIndexing ? uint8_t{0x40} : uint8_t{0x00});
  for (const Zeros str : {name, value}) {
    const auto nbBytes = static_cast<vector<uint8_t>::size_type>(str.huffman ? str.nbChars * 5U / 8U : str.nbChars);
    AppendInteger(out, str.huffman ? uint8_t{0x80} : uint8_t{0x00}, 7, nbBytes);
    out.insert(out.end(), nbBytes, str.huffman ? uint8_t{0} : uint8_t{'0'});
  }
}

}  // namespace

// The Huffman strings of a field beyond the limit are given back to the storage of the decoded headers once the field
// is decoded (and added to the dynamic table), whichever of its name and value are Huffman-coded: the next fields reuse
// their bytes. A refused block that made the storage grow releases it.
TEST(HpackAudit, HuffmanStringsBeyondTheHeaderListLimitAreGivenBack) {
  static constexpr uint32_t kMaxHeaderListSize = 64;
  static constexpr Zeros kHuffman96{96, true};
  static constexpr Zeros kRaw96{96, false};
  HpackDecoder decoder(4096, kMaxHeaderListSize, true);

  // A field within the limit, decoded into the first block of the storage.
  vector<uint8_t> accepted;
  AppendZerosField(accepted, {8, true}, {8, true});
  ASSERT_TRUE(decoder.decode(AsBytes(accepted)).isSuccess());

  // 150 fields of 96 + 96 bytes beyond the limit (28 KiB if they were kept), Huffman-coded in name and value, in name
  // only, and in value only, then a field added to the dynamic table.
  vector<uint8_t> block;
  for (int idx = 0; idx < 50; ++idx) {
    AppendZerosField(block, kHuffman96, kHuffman96);
    AppendZerosField(block, kHuffman96, kRaw96);
    AppendZerosField(block, kRaw96, kHuffman96);
  }
  AppendZerosField(block, {8, true}, {16, true}, true);
  const auto refused = decoder.decode(AsBytes(block));
  EXPECT_EQ(refused.error, AuditError::HeaderListTooLarge);
  EXPECT_EQ(refused.headerListSize, (150U * (96U + 96U + 32U)) + 8U + 16U + 32U);
  EXPECT_EQ(refused.decodedHeaders.storage.capacity(), DecodedHeaders::kStorageInitialCapacity);

  // The entry was added before its strings were given back.
  static constexpr uint8_t reference[]{0xBE};
  const auto referenced = decoder.decode(AsBytes(reference));
  ASSERT_TRUE(referenced.isSuccess());
  EXPECT_EQ(referenced.decodedHeaders.headers.at("00000000"), "0000000000000000");

  // A Huffman name taking most of the first block of the storage, then a Huffman value that does not fit next to it
  // (400 + 200 > 512 bytes): the value takes a new block and is given back alone, the next field reusing that block.
  block.clear();
  AppendZerosField(block, {400, true}, {200, true});
  AppendZerosField(block, kHuffman96, kHuffman96);
  const auto grown = decoder.decode(AsBytes(block));
  EXPECT_EQ(grown.error, AuditError::HeaderListTooLarge);
  EXPECT_EQ(grown.decodedHeaders.storage.capacity(), 0U);  // released
  EXPECT_TRUE(grown.decodedHeaders.headers.empty());

  const auto next = decoder.decode(AsBytes(accepted));
  ASSERT_TRUE(next.isSuccess());
  EXPECT_EQ(next.decodedHeaders.headers.at("00000000"), "00000000");
}

// Not an RFC requirement, a compression loss: a literal field whose name is in the static table is written with the
// index of that name in the dynamic table when another value of the field is there, which takes one byte more from
// the second entry of the dynamic table on (an index of 63 and more does not fit in the 6-bit prefix), where the
// static index takes one. The responses of C.6.2 and C.6.3 show it (":status: 307" after ":status: 302", then "date").
TEST(HpackAudit, EncoderKeepsTheStaticIndexOfANameAlsoInTheDynamicTable) {
  static constexpr std::span<const http::HeaderView> kMessages[]{kResponse1, kResponse2, kResponse3};
  // The second response is the one of C.5.2: "307" takes 3 bytes with and without Huffman coding, which the encoder
  // only uses when shorter.
  static constexpr std::string_view kExpectedHex[]{
      kResponsesWithHuffmanHex[0],
      "4803333037c1c0bf",
      kResponsesWithHuffmanHex[2],
  };
  HpackEncoder encoder(kResponseExamplesTableSize);
  for (std::size_t idx = 0; idx < std::size(kMessages); ++idx) {
    RawBytes block;
    for (const http::HeaderView& field : kMessages[idx]) {
      encoder.encode(block, field.name, field.value, HpackEncoder::IndexingMode::Indexed);
    }
    const vector<uint8_t> expected = FromHex(kExpectedHex[idx]);
    EXPECT_EQ(block.size(), expected.size()) << idx;
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(block.data()), block.size()),
              std::string_view(reinterpret_cast<const char*>(expected.data()), expected.size()))
        << idx;
  }
}

// The lookup index of a large dynamic table (built at 128 entries, so for a table above 4096 bytes) is kept in sync by
// encode(), also when a field evicts enough entries to leave fewer than 128 of them (it asserted there).
TEST(HpackAudit, EncoderEvictionBelowTheLookupIndexThreshold) {
  HpackEncoder encoder(8192);
  HpackDecoder decoder(8192, std::numeric_limits<uint32_t>::max(), true);
  RawBytes block;
  for (int idx = 0; idx < 200; ++idx) {
    encoder.encode(block, "x-" + std::to_string(idx), "v");  // 200 entries of 36 to 38 bytes
  }
  ASSERT_TRUE(decoder.decode(block).isSuccess());
  ASSERT_EQ(encoder.dynamicTable().entryCount(), 200U);

  block.clear();
  const std::string large(6000, 'v');
  encoder.encode(block, "x-large", large);  // evicts about 160 entries
  EXPECT_LT(encoder.dynamicTable().entryCount(), 128U);
  const auto result = decoder.decode(block);
  ASSERT_TRUE(result.isSuccess());
  EXPECT_EQ(result.decodedHeaders.headers.at("x-large"), large);
  EXPECT_EQ(encoder.dynamicTable().entryCount(), decoder.dynamicTable().entryCount());

  // The encoder still finds the entries that are left.
  block.clear();
  encoder.encode(block, "x-199", "v");
  EXPECT_EQ(block.size(), 1U);
  ASSERT_TRUE(decoder.decode(block).isSuccess());
}

// ---- The encoder, read by a decoder model written from RFC 7541 alone

namespace {

// RFC 7541 Appendix B, generated from the text of the RFC: the code and its number of bits for each symbol, EOS last.
struct RfcHuffmanCode {
  uint32_t code;
  uint8_t nbBits;
};

constexpr RfcHuffmanCode kRfcHuffmanCodes[]{
    {0x1ff8, 13},    {0x7fffd8, 23},   {0xfffffe2, 28}, {0xfffffe3, 28}, {0xfffffe4, 28},  {0xfffffe5, 28},
    {0xfffffe6, 28}, {0xfffffe7, 28},  {0xfffffe8, 28}, {0xffffea, 24},  {0x3ffffffc, 30}, {0xfffffe9, 28},
    {0xfffffea, 28}, {0x3ffffffd, 30}, {0xfffffeb, 28}, {0xfffffec, 28}, {0xfffffed, 28},  {0xfffffee, 28},
    {0xfffffef, 28}, {0xffffff0, 28},  {0xffffff1, 28}, {0xffffff2, 28}, {0x3ffffffe, 30}, {0xffffff3, 28},
    {0xffffff4, 28}, {0xffffff5, 28},  {0xffffff6, 28}, {0xffffff7, 28}, {0xffffff8, 28},  {0xffffff9, 28},
    {0xffffffa, 28}, {0xffffffb, 28},  {0x14, 6},       {0x3f8, 10},     {0x3f9, 10},      {0xffa, 12},
    {0x1ff9, 13},    {0x15, 6},        {0xf8, 8},       {0x7fa, 11},     {0x3fa, 10},      {0x3fb, 10},
    {0xf9, 8},       {0x7fb, 11},      {0xfa, 8},       {0x16, 6},       {0x17, 6},        {0x18, 6},
    {0x0, 5},        {0x1, 5},         {0x2, 5},        {0x19, 6},       {0x1a, 6},        {0x1b, 6},
    {0x1c, 6},       {0x1d, 6},        {0x1e, 6},       {0x1f, 6},       {0x5c, 7},        {0xfb, 8},
    {0x7ffc, 15},    {0x20, 6},        {0xffb, 12},     {0x3fc, 10},     {0x1ffa, 13},     {0x21, 6},
    {0x5d, 7},       {0x5e, 7},        {0x5f, 7},       {0x60, 7},       {0x61, 7},        {0x62, 7},
    {0x63, 7},       {0x64, 7},        {0x65, 7},       {0x66, 7},       {0x67, 7},        {0x68, 7},
    {0x69, 7},       {0x6a, 7},        {0x6b, 7},       {0x6c, 7},       {0x6d, 7},        {0x6e, 7},
    {0x6f, 7},       {0x70, 7},        {0x71, 7},       {0x72, 7},       {0xfc, 8},        {0x73, 7},
    {0xfd, 8},       {0x1ffb, 13},     {0x7fff0, 19},   {0x1ffc, 13},    {0x3ffc, 14},     {0x22, 6},
    {0x7ffd, 15},    {0x3, 5},         {0x23, 6},       {0x4, 5},        {0x24, 6},        {0x5, 5},
    {0x25, 6},       {0x26, 6},        {0x27, 6},       {0x6, 5},        {0x74, 7},        {0x75, 7},
    {0x28, 6},       {0x29, 6},        {0x2a, 6},       {0x7, 5},        {0x2b, 6},        {0x76, 7},
    {0x2c, 6},       {0x8, 5},         {0x9, 5},        {0x2d, 6},       {0x77, 7},        {0x78, 7},
    {0x79, 7},       {0x7a, 7},        {0x7b, 7},       {0x7ffe, 15},    {0x7fc, 11},      {0x3ffd, 14},
    {0x1ffd, 13},    {0xffffffc, 28},  {0xfffe6, 20},   {0x3fffd2, 22},  {0xfffe7, 20},    {0xfffe8, 20},
    {0x3fffd3, 22},  {0x3fffd4, 22},   {0x3fffd5, 22},  {0x7fffd9, 23},  {0x3fffd6, 22},   {0x7fffda, 23},
    {0x7fffdb, 23},  {0x7fffdc, 23},   {0x7fffdd, 23},  {0x7fffde, 23},  {0xffffeb, 24},   {0x7fffdf, 23},
    {0xffffec, 24},  {0xffffed, 24},   {0x3fffd7, 22},  {0x7fffe0, 23},  {0xffffee, 24},   {0x7fffe1, 23},
    {0x7fffe2, 23},  {0x7fffe3, 23},   {0x7fffe4, 23},  {0x1fffdc, 21},  {0x3fffd8, 22},   {0x7fffe5, 23},
    {0x3fffd9, 22},  {0x7fffe6, 23},   {0x7fffe7, 23},  {0xffffef, 24},  {0x3fffda, 22},   {0x1fffdd, 21},
    {0xfffe9, 20},   {0x3fffdb, 22},   {0x3fffdc, 22},  {0x7fffe8, 23},  {0x7fffe9, 23},   {0x1fffde, 21},
    {0x7fffea, 23},  {0x3fffdd, 22},   {0x3fffde, 22},  {0xfffff0, 24},  {0x1fffdf, 21},   {0x3fffdf, 22},
    {0x7fffeb, 23},  {0x7fffec, 23},   {0x1fffe0, 21},  {0x1fffe1, 21},  {0x3fffe0, 22},   {0x1fffe2, 21},
    {0x7fffed, 23},  {0x3fffe1, 22},   {0x7fffee, 23},  {0x7fffef, 23},  {0xfffea, 20},    {0x3fffe2, 22},
    {0x3fffe3, 22},  {0x3fffe4, 22},   {0x7ffff0, 23},  {0x3fffe5, 22},  {0x3fffe6, 22},   {0x7ffff1, 23},
    {0x3ffffe0, 26}, {0x3ffffe1, 26},  {0xfffeb, 20},   {0x7fff1, 19},   {0x3fffe7, 22},   {0x7ffff2, 23},
    {0x3fffe8, 22},  {0x1ffffec, 25},  {0x3ffffe2, 26}, {0x3ffffe3, 26}, {0x3ffffe4, 26},  {0x7ffffde, 27},
    {0x7ffffdf, 27}, {0x3ffffe5, 26},  {0xfffff1, 24},  {0x1ffffed, 25}, {0x7fff2, 19},    {0x1fffe3, 21},
    {0x3ffffe6, 26}, {0x7ffffe0, 27},  {0x7ffffe1, 27}, {0x3ffffe7, 26}, {0x7ffffe2, 27},  {0xfffff2, 24},
    {0x1fffe4, 21},  {0x1fffe5, 21},   {0x3ffffe8, 26}, {0x3ffffe9, 26}, {0xffffffd, 28},  {0x7ffffe3, 27},
    {0x7ffffe4, 27}, {0x7ffffe5, 27},  {0xfffec, 20},   {0xfffff3, 24},  {0xfffed, 20},    {0x1fffe6, 21},
    {0x3fffe9, 22},  {0x1fffe7, 21},   {0x1fffe8, 21},  {0x7ffff3, 23},  {0x3fffea, 22},   {0x3fffeb, 22},
    {0x1ffffee, 25}, {0x1ffffef, 25},  {0xfffff4, 24},  {0xfffff5, 24},  {0x3ffffea, 26},  {0x7ffff4, 23},
    {0x3ffffeb, 26}, {0x7ffffe6, 27},  {0x3ffffec, 26}, {0x3ffffed, 26}, {0x7ffffe7, 27},  {0x7ffffe8, 27},
    {0x7ffffe9, 27}, {0x7ffffea, 27},  {0x7ffffeb, 27}, {0xffffffe, 28}, {0x7ffffec, 27},  {0x7ffffed, 27},
    {0x7ffffee, 27}, {0x7ffffef, 27},  {0x7fffff0, 27}, {0x3ffffee, 26}, {0x3fffffff, 30},
};
static_assert(std::size(kRfcHuffmanCodes) == 257U);

// RFC 7541 Appendix A, generated from the text of the RFC.
constexpr http::HeaderView kRfcStaticTable[]{
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};
static_assert(std::size(kRfcStaticTable) == 61U);

// A field decoded by the model, with the representation it came in.
struct ModelField {
  enum class Representation : uint8_t { Indexed, WithIndexing, WithoutIndexing, NeverIndexed };

  std::string name;
  std::string value;
  Representation representation;
  uint64_t nameIndex;  // 0 for a literal name
};

// An HPACK decoder following the text of RFC 7541 and sharing nothing with the library: Huffman strings decoded bit
// by bit from the table of the RFC, the dynamic table as a deque of strings, no HTTP validation. It also checks what
// §4.2 requires from an encoder when the maximum size of the table changes between two blocks.
class ModelHpackDecoder {
 public:
  explicit ModelHpackDecoder(std::size_t maxTableSize) : _maxTableSize(maxTableSize), _limit(maxTableSize) {
    // The code as a binary tree: a node per prefix of a code, the symbol at its leaf.
    _nodes.emplace_back();
    for (std::size_t symbol = 0; symbol < std::size(kRfcHuffmanCodes); ++symbol) {
      uint32_t node = 0;
      for (uint32_t bit = kRfcHuffmanCodes[symbol].nbBits; bit != 0; --bit) {
        const std::size_t side = (kRfcHuffmanCodes[symbol].code >> (bit - 1U)) & 1U;
        if (_nodes[node].next[side] == 0) {
          _nodes[node].next[side] = static_cast<uint32_t>(_nodes.size());
          _nodes.emplace_back();
        }
        node = _nodes[node].next[side];
      }
      _nodes[node].symbol = static_cast<int32_t>(symbol);
    }
  }

  // A new SETTINGS_HEADER_TABLE_SIZE was acknowledged.
  void setLimit(std::size_t limit) {
    _limit = limit;
    _smallestLimit = std::min(_smallestLimit, limit);
  }

  // Decode a block. Returns false on a decoding error, or if the encoder did not signal a reduced table size.
  [[nodiscard]] bool decode(std::span<const std::byte> block, vector<ModelField>& fields) {
    _data = block;
    _pos = 0;
    fields.clear();
    bool fieldSeen = false;
    std::size_t smallestUpdate = std::numeric_limits<std::size_t>::max();
    while (_pos < _data.size()) {
      const uint8_t firstByte = byteAt(_pos);
      if ((firstByte & 0xE0U) == 0x20U) {
        uint64_t size;
        if (fieldSeen || !readInteger(5, size) || size > _limit) {
          return false;
        }
        smallestUpdate = std::min(smallestUpdate, static_cast<std::size_t>(size));
        _maxTableSize = static_cast<std::size_t>(size);
        evict(0);
        continue;
      }
      if (!fieldSeen) {
        fieldSeen = true;
        // §4.2: a table size reduced since the last block must have been signaled by now, with its smallest value.
        if (_smallestLimit < _maxTableSizeAtLastBlock && smallestUpdate > _smallestLimit) {
          return false;
        }
        if (_maxTableSize > _limit) {
          return false;
        }
      }
      ModelField field;
      if ((firstByte & 0x80U) != 0) {
        uint64_t index;
        if (!readInteger(7, index) || index == 0 || !lookup(index, field.name, &field.value)) {
          return false;
        }
        field.representation = ModelField::Representation::Indexed;
        field.nameIndex = index;
      } else {
        const bool withIndexing = (firstByte & 0xC0U) == 0x40U;
        if (withIndexing) {
          field.representation = ModelField::Representation::WithIndexing;
        } else if ((firstByte & 0xF0U) == 0x10U) {
          field.representation = ModelField::Representation::NeverIndexed;
        } else {
          field.representation = ModelField::Representation::WithoutIndexing;
        }
        if (!readInteger(withIndexing ? 6 : 4, field.nameIndex)) {
          return false;
        }
        if (field.nameIndex == 0 ? !readString(field.name) : !lookup(field.nameIndex, field.name, nullptr)) {
          return false;
        }
        if (!readString(field.value)) {
          return false;
        }
        if (withIndexing) {
          // §4.4: evict until the entry fits, an entry larger than the table emptying it.
          const std::size_t entrySize = field.name.size() + field.value.size() + 32U;
          evict(entrySize);
          if (entrySize <= _maxTableSize) {
            _entries.emplace_front(field.name, field.value);
            _tableSize += entrySize;
          }
        }
      }
      fields.push_back(std::move(field));
    }
    if (fieldSeen) {
      _maxTableSizeAtLastBlock = _maxTableSize;
      _smallestLimit = _limit;
    }
    return true;
  }

  [[nodiscard]] const std::deque<std::pair<std::string, std::string>>& entries() const noexcept { return _entries; }
  [[nodiscard]] std::size_t tableSize() const noexcept { return _tableSize; }
  [[nodiscard]] std::size_t maxTableSize() const noexcept { return _maxTableSize; }

 private:
  struct HuffmanNode {
    uint32_t next[2]{};  // 0 (the root, never a child): no code with this prefix
    int32_t symbol{-1};
  };

  [[nodiscard]] uint8_t byteAt(std::size_t pos) const { return static_cast<uint8_t>(_data[pos]); }

  // §5.1, with the pseudocode of the RFC.
  bool readInteger(uint32_t prefixBits, uint64_t& value) {
    const uint64_t prefixMax = (uint64_t{1} << prefixBits) - 1U;
    value = byteAt(_pos++) & prefixMax;
    if (value < prefixMax) {
      return true;
    }
    for (uint32_t shift = 0; shift < 56U; shift += 7U) {
      if (_pos == _data.size()) {
        return false;
      }
      const uint8_t byte = byteAt(_pos++);
      value += static_cast<uint64_t>(byte & 0x7FU) << shift;
      if ((byte & 0x80U) == 0) {
        return true;
      }
    }
    return false;
  }

  // §5.2.
  bool readString(std::string& str) {
    if (_pos == _data.size()) {
      return false;
    }
    const bool huffman = (byteAt(_pos) & 0x80U) != 0;
    uint64_t length;
    if (!readInteger(7, length) || length > _data.size() - _pos) {
      return false;
    }
    const std::size_t end = _pos + static_cast<std::size_t>(length);
    if (!huffman) {
      str.assign(reinterpret_cast<const char*>(_data.data()) + _pos, static_cast<std::size_t>(length));
      _pos = end;
      return true;
    }
    uint32_t node = 0;
    uint32_t nbBits = 0;  // of the symbol being read
    bool onlyOnes = true;
    for (; _pos < end; ++_pos) {
      for (uint32_t bit = 8; bit != 0; --bit) {
        const std::size_t side = (byteAt(_pos) >> (bit - 1U)) & 1U;
        node = _nodes[node].next[side];
        if (node == 0) {
          return false;  // not a code
        }
        ++nbBits;
        onlyOnes = onlyOnes && side == 1U;
        if (_nodes[node].symbol >= 0) {
          if (_nodes[node].symbol == 256) {
            return false;  // EOS in the string
          }
          str.push_back(static_cast<char>(_nodes[node].symbol));
          node = 0;
          nbBits = 0;
          onlyOnes = true;
        }
      }
    }
    // A padding of less than 8 bits, the most significant bits of EOS (1s).
    return nbBits < 8U && onlyOnes;
  }

  // §2.3.3.
  bool lookup(uint64_t index, std::string& name, std::string* pValue) const {
    if (index <= std::size(kRfcStaticTable)) {
      name = kRfcStaticTable[index - 1U].name;
      if (pValue != nullptr) {
        *pValue = kRfcStaticTable[index - 1U].value;
      }
      return true;
    }
    index -= std::size(kRfcStaticTable) + 1U;
    if (index >= _entries.size()) {
      return false;
    }
    name = _entries[static_cast<std::size_t>(index)].first;
    if (pValue != nullptr) {
      *pValue = _entries[static_cast<std::size_t>(index)].second;
    }
    return true;
  }

  // §4.3, §4.4: evict the oldest entries until 'room' more bytes fit.
  void evict(std::size_t room) {
    while (!_entries.empty() && _tableSize + room > _maxTableSize) {
      _tableSize -= _entries.back().first.size() + _entries.back().second.size() + 32U;
      _entries.pop_back();
    }
  }

  vector<HuffmanNode> _nodes;
  std::deque<std::pair<std::string, std::string>> _entries;
  std::span<const std::byte> _data;
  std::size_t _pos{0};
  std::size_t _tableSize{0};
  std::size_t _maxTableSize;
  std::size_t _maxTableSizeAtLastBlock{_maxTableSize};
  std::size_t _limit;
  std::size_t _smallestLimit{_limit};
};

void ExpectSameTable(const HpackDynamicTable& table, const ModelHpackDecoder& model) {
  ASSERT_EQ(table.entryCount(), model.entries().size());
  ASSERT_EQ(table.currentSizeBytes(), model.tableSize());
  ASSERT_EQ(table.maxSizeBytes(), model.maxTableSize());
  for (std::size_t idx = 0; idx < model.entries().size(); ++idx) {
    ASSERT_EQ(table[static_cast<uint32_t>(idx)].name(), model.entries()[idx].first) << idx;
    ASSERT_EQ(table[static_cast<uint32_t>(idx)].value(), model.entries()[idx].second) << idx;
  }
}

}  // namespace

// The model decodes the examples of the RFC: it is a decoder of its own.
TEST(HpackAudit, ModelDecoderDecodesAppendixC) {
  ModelHpackDecoder requests(4096);
  vector<ModelField> fields;
  const std::span<const http::HeaderView> kRequests[]{kRequest1, kRequest2, kRequest3};
  for (std::size_t idx = 0; idx < std::size(kRequests); ++idx) {
    const vector<uint8_t> block = FromHex(kRequestsWithHuffmanHex[idx]);
    ASSERT_TRUE(requests.decode(AsBytes(block), fields));
    ASSERT_EQ(fields.size(), kRequests[idx].size());
    for (uint32_t fieldIdx = 0; fieldIdx < fields.size(); ++fieldIdx) {
      EXPECT_EQ(fields[fieldIdx].name, kRequests[idx][fieldIdx].name);
      EXPECT_EQ(fields[fieldIdx].value, kRequests[idx][fieldIdx].value);
    }
  }
  EXPECT_EQ(requests.tableSize(), 164U);

  ModelHpackDecoder responses(kResponseExamplesTableSize);
  const std::span<const http::HeaderView> kResponses[]{kResponse1, kResponse2, kResponse3};
  for (std::size_t idx = 0; idx < std::size(kResponses); ++idx) {
    const vector<uint8_t> block = FromHex(kResponsesWithHuffmanHex[idx]);
    ASSERT_TRUE(responses.decode(AsBytes(block), fields));
    ASSERT_EQ(fields.size(), kResponses[idx].size());
    for (uint32_t fieldIdx = 0; fieldIdx < fields.size(); ++fieldIdx) {
      EXPECT_EQ(fields[fieldIdx].name, kResponses[idx][fieldIdx].name);
      EXPECT_EQ(fields[fieldIdx].value, kResponses[idx][fieldIdx].value);
    }
  }
  EXPECT_EQ(responses.tableSize(), 215U);
  EXPECT_EQ(responses.entries().front().second, kExampleSetCookie);
}

// Random messages, with the indexing mode of each field as the connection chooses it or a random one, and table sizes
// changing between the blocks (up to three times in a row): the model decodes each block to the encoded fields, and
// the dynamic tables of the encoder, of the model and of the decoder of the library hold the same entries after each.
TEST(HpackAudit, EncoderBlocksAreDecodedByTheModelUnderTableSizeChanges) {
  static constexpr std::string_view kNames[]{
      ":status",
      ":path",
      "accept",
      "cache-control",
      "content-type",
      "content-length",
      "cookie",
      "etag",
      "set-cookie",
      "authorization",
      "date",
      "location",
      "x-a",
      "x-bb",
      "x-ccc",
      "y",
      "last-modified",
      "expires",
      "user-agent",
      "via",
      "x-request-id",
      "age",
      "x-a-custom-name-longer-than-the-others",
  };
  static constexpr std::string_view kRepeatedValues[]{
      "",
      "0",
      "200",
      "text/html",
      "max-age=3600",
      "gzip, deflate",
      "a value that repeats in most of the messages of a connection",
  };
  static constexpr uint32_t kTableSizes[]{0, 32, 33, 64, 100, 300, 1000, 4096, 9000, 30000};
  using Mode = HpackEncoder::IndexingMode;

  std::mt19937 rng(7541);  // NOLINT(bugprone-random-generator-seed)
  const auto randomValue = [&rng]() {
    std::string value;
    switch (rng() % 8U) {
      case 0:
      case 1:
      case 2:
        value = kRepeatedValues[rng() % std::size(kRepeatedValues)];
        break;
      case 3:  // text, shorter with Huffman coding
        value.resize(1U + (rng() % 60U));
        std::ranges::generate(value, [&rng] { return static_cast<char>('a' + (rng() % 26U)); });
        break;
      case 4:  // any byte allowed in a value, longer with Huffman coding
        value.resize(1U + (rng() % 40U));
        std::ranges::generate(value, [&rng] {
          char ch;
          do {
            ch = static_cast<char>(rng() % 256U);
          } while (ch == '\0' || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t');
          return ch;
        });
        break;
      case 5: {  // larger than the small tables - one draw per statement, as function arguments are unsequenced
        const std::size_t size = 200U + (rng() % 1000U);
        value.assign(size, static_cast<char>('a' + (rng() % 26U)));
        break;
      }
      default:
        value = std::to_string(rng());
        break;
    }
    return value;
  };

  uint32_t tableSize = 4096;
  HpackEncoder encoder(tableSize);
  ModelHpackDecoder model(tableSize);
  HpackDecoder decoder(tableSize, std::numeric_limits<uint32_t>::max(), true, HpackDecoder::Messages::Responses);
  vector<ModelField> decodedFields;
  std::size_t nbEvictingBlocks = 0;
  std::size_t nbSizeUpdates = 0;

  struct Field {
    std::string_view name;
    std::string value;
    Mode mode;
  };

  vector<Field> fields;
  RawBytes block;

  for (int message = 0; message < 3000; ++message) {
    if (rng() % 6U == 0) {
      for (uint32_t nbChanges = 1U + (rng() % 3U); nbChanges != 0; --nbChanges) {
        tableSize = kTableSizes[rng() % std::size(kTableSizes)];
        encoder.setMaxDynamicTableSize(tableSize);
        model.setLimit(tableSize);
        decoder.setMaxDynamicTableSize(tableSize);
      }
    }

    fields.clear();
    block.clear();
    for (const std::string_view name : kNames) {
      if (rng() % 3U == 0) {
        continue;
      }
      Field& field = fields.emplace_back(name, randomValue(), Mode::Indexed);
      field.mode = rng() % 4U == 0 ? static_cast<Mode>(rng() % 3U) : HpackEncoder::IndexingModeOf(name, field.value);
      const std::size_t sizeBefore = block.size();
      EXPECT_EQ(encoder.encode(block, field.name, field.value, field.mode), name.size() + field.value.size() + 32U);
      nbSizeUpdates += fields.size() == 1U && (static_cast<uint8_t>(block[sizeBefore]) & 0xE0U) == 0x20U ? 1U : 0U;
    }
    const std::size_t entryCountBefore = model.entries().size();

    ASSERT_TRUE(model.decode(block, decodedFields)) << message;
    ASSERT_EQ(decodedFields.size(), fields.size()) << message;
    for (uint32_t idx = 0; idx < fields.size(); ++idx) {
      ASSERT_EQ(decodedFields[idx].name, fields[idx].name) << message;
      ASSERT_EQ(decodedFields[idx].value, fields[idx].value) << message << " " << fields[idx].name;
      // The representation follows the mode: only a field found whole in a table is an index.
      const ModelField::Representation representation = decodedFields[idx].representation;
      if (representation != ModelField::Representation::Indexed) {
        ASSERT_EQ(static_cast<int>(representation), 1 + static_cast<int>(fields[idx].mode)) << fields[idx].name;
      }
    }
    ASSERT_NO_FATAL_FAILURE(ExpectSameTable(encoder.dynamicTable(), model)) << message;
    nbEvictingBlocks += model.entries().size() < entryCountBefore ? 1U : 0U;

    // The decoder of the library reads the same fields, and keeps the same table.
    const auto result = decoder.decode(block);
    ASSERT_TRUE(result.isSuccess()) << message << " " << static_cast<int>(result.error);
    ASSERT_EQ(result.decodedHeaders.headers.size() +
                  static_cast<std::size_t>(std::popcount(result.decodedHeaders.pseudoHeadersMask)),
              fields.size());
    for (const Field& field : fields) {
      if (field.name == ":status") {
        ASSERT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Status), field.value);
      } else if (field.name == ":path") {
        ASSERT_EQ(result.decodedHeaders.pseudoHeader(PseudoHeader::Path), field.value);
      } else {
        ASSERT_EQ(result.decodedHeaders.headers.at(field.name), field.value) << field.name;
      }
    }
    ASSERT_NO_FATAL_FAILURE(ExpectSameTable(decoder.dynamicTable(), model)) << message;
  }
  // The run went through evictions and size updates.
  EXPECT_GT(nbEvictingBlocks, 100U);
  EXPECT_GT(nbSizeUpdates, 50U);
}

// The indexing policy of docs/protocols/http2.md, seen by the model: credentials come as never-indexed literals and
// the fields that change in each message as literals without indexing, both with the static index of their name, and
// none of them enters the dynamic table. The other fields are indexed.
TEST(HpackAudit, IndexingPolicyAsSeenByTheModel) {
  using Representation = ModelField::Representation;
  struct Case {
    std::string_view name;
    std::string_view value;
    Representation expected;
    uint64_t staticNameIndex;
  };
  static constexpr Case kCases[]{
      {"authorization", "Bearer abcdefghijklmnopqrstuvwxyz0123456789", Representation::NeverIndexed, 23},
      {"proxy-authorization", "Basic dXNlcjpwYXNzd29yZA==", Representation::NeverIndexed, 49},
      {"cookie", "id=0123456789abcdef", Representation::NeverIndexed, 32},   // 19 bytes
      {"cookie", "id=0123456789abcdefg", Representation::WithIndexing, 32},  // 20 bytes
      {"content-length", "1234", Representation::WithoutIndexing, 28},
      {"etag", "\"abc\"", Representation::WithoutIndexing, 34},
      {"last-modified", "Mon, 21 Oct 2013 20:13:21 GMT", Representation::WithoutIndexing, 44},
      {"location", "https://www.example.com", Representation::WithoutIndexing, 46},
      {"set-cookie", "id=1; Secure", Representation::WithoutIndexing, 55},
      {"age", "12", Representation::WithoutIndexing, 21},
      {"if-modified-since", "Mon, 21 Oct 2013 20:13:21 GMT", Representation::WithoutIndexing, 40},
      {"if-none-match", "\"abc\"", Representation::WithoutIndexing, 41},
      {"content-type", "text/html", Representation::WithIndexing, 31},
      {"date", "Mon, 21 Oct 2013 20:13:21 GMT", Representation::WithIndexing, 33},
      {"x-custom", "value", Representation::WithIndexing, 0},
  };

  HpackEncoder encoder(4096);
  ModelHpackDecoder model(4096);
  vector<ModelField> fields;
  // Three messages: the fields that are not indexed are sent the same way each time.
  for (int message = 0; message < 3; ++message) {
    RawBytes block;
    for (const Case& testCase : kCases) {
      encoder.encode(block, testCase.name, testCase.value, HpackEncoder::IndexingModeOf(testCase.name, testCase.value));
    }
    ASSERT_TRUE(model.decode(block, fields));
    ASSERT_EQ(fields.size(), std::size(kCases));
    for (uint32_t idx = 0; idx < std::size(kCases); ++idx) {
      const Case& testCase = kCases[idx];
      EXPECT_EQ(fields[idx].name, testCase.name);
      EXPECT_EQ(fields[idx].value, testCase.value);
      const bool indexedOnce = testCase.expected == Representation::WithIndexing;
      EXPECT_EQ(fields[idx].representation, indexedOnce && message != 0 ? Representation::Indexed : testCase.expected)
          << testCase.name << " " << message;
      if (!indexedOnce) {
        EXPECT_EQ(fields[idx].nameIndex, testCase.staticNameIndex) << testCase.name;
      }
    }
    // The four indexed fields only.
    ASSERT_EQ(model.entries().size(), 4U);
    for (const auto& [name, value] : model.entries()) {
      EXPECT_TRUE(name == "cookie" || name == "content-type" || name == "date" || name == "x-custom") << name;
    }
    ASSERT_NO_FATAL_FAILURE(ExpectSameTable(encoder.dynamicTable(), model));
  }
}

// ---- The Huffman coding, against the model
//
// The decoder reads a string shorter than 8 bytes at once and the others 8 bytes at a time (the last ones from the
// last 8 bytes of the string), decodes up to 7 short codes after each read, and tells the end of the string from the
// padding it reads as a long code. The encoder writes 4 symbols at once, and gives up as soon as the Huffman string is
// not shorter than the raw one. The model reads and writes one bit at a time: they are compared on every small length.

namespace {

// The Huffman string of 'str', from the table of the RFC.
vector<uint8_t> RfcHuffmanString(std::string_view str) {
  HuffmanBitWriter writer;
  for (const char ch : str) {
    const RfcHuffmanCode code = kRfcHuffmanCodes[static_cast<uint8_t>(ch)];
    writer.append(code.code, code.nbBits);
  }
  return writer.finish();
}

// Decode a field whose value is the given Huffman string with the decoder and with the model: the same strings are
// refused, and the same value is decoded from the others.
void ExpectHuffmanDecodedAsByTheModel(HpackDecoder& decoder, ModelHpackDecoder& model,
                                      std::span<const uint8_t> huffmanString) {
  const auto block = LiteralFieldX(huffmanString, true);
  vector<ModelField> fields;
  const bool isHuffmanString = model.decode(AsBytes(block), fields);
  const auto result = decoder.decode(AsBytes(block));
  if (!isHuffmanString) {
    ASSERT_EQ(result.error, HpackDecoder::DecodeResult::Error::FailedToDecodeLiteralHeaderValue);
    return;
  }
  ASSERT_EQ(fields.size(), 1U);
  const std::string_view value = fields[0].value;
  // A Huffman string may still not be a field value (RFC 9113 §8.2.1).
  const bool isFieldValue =
      value.empty() || (!http::IsHeaderWhitespace(value.front()) && !http::IsHeaderWhitespace(value.back()) &&
                        value.find_first_of(std::string_view("\0\r\n", 3)) == std::string_view::npos);
  if (!isFieldValue) {
    ASSERT_EQ(result.error, HpackDecoder::DecodeResult::Error::MalformedFieldValue);
    return;
  }
  ASSERT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.headers.find("x")->second, value);
}

std::string HexOf(std::span<const uint8_t> bytes) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex;
  for (const uint8_t byte : bytes) {
    hex.push_back(kHexDigits[byte >> 4U]);
    hex.push_back(kHexDigits[byte & 0x0FU]);
  }
  return hex;
}

// Characters with a code of 5 bits, of 5 to 8 bits (header text), and of 8 bits.
constexpr std::string_view kFiveBitChars = "012aceiost";
constexpr std::string_view kShortCodeChars =
    "012aceiost %-./3456789=A_bdfghlmnpru:BCDEFGHIJKLMNOPQRSTUVWYjkqvwxyz&*,;XZ";
constexpr std::string_view kEightBitChars = "&*,;XZ";

}  // namespace

TEST(HpackHuffman, EveryStringOfOneAndTwoBytesIsDecodedAsByTheModel) {
  auto decoder = CreateHpackDecoder();
  ModelHpackDecoder model(4096);
  for (uint32_t first = 0; first < 256U; ++first) {
    const uint8_t oneByte[]{static_cast<uint8_t>(first)};
    ASSERT_NO_FATAL_FAILURE(ExpectHuffmanDecodedAsByTheModel(decoder, model, oneByte)) << HexOf(oneByte);
    for (uint32_t second = 0; second < 256U; ++second) {
      const uint8_t twoBytes[]{static_cast<uint8_t>(first), static_cast<uint8_t>(second)};
      ASSERT_NO_FATAL_FAILURE(ExpectHuffmanDecodedAsByTheModel(decoder, model, twoBytes)) << HexOf(twoBytes);
    }
  }
}

TEST(HpackHuffman, RandomBytesAreDecodedAsByTheModel) {
  std::mt19937 rng(7541);  // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
  auto decoder = CreateHpackDecoder();
  ModelHpackDecoder model(4096);
  vector<uint8_t> bytes;
  for (std::size_t len = 0; len <= 34; ++len) {
    for (int iter = 0; iter < 1500; ++iter) {
      bytes.clear();
      // Bytes with many 1s make long codes and paddings likely.
      const bool manyOnes = iter % 3 == 0;
      for (std::size_t pos = 0; pos < len; ++pos) {
        const auto random = static_cast<uint32_t>(rng());
        bytes.push_back(static_cast<uint8_t>(manyOnes ? random | (random >> 8U) : random));
      }
      ASSERT_NO_FATAL_FAILURE(ExpectHuffmanDecodedAsByTheModel(decoder, model, bytes)) << HexOf(bytes);
    }
  }
}

TEST(HpackHuffman, AlteredHuffmanStringsAreDecodedAsByTheModel) {
  std::mt19937 rng(9113);  // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
  auto decoder = CreateHpackDecoder();
  ModelHpackDecoder model(4096);
  std::size_t nbValid = 0;
  for (int iter = 0; iter < 20000; ++iter) {
    std::string value(rng() % 48U, 'a');
    // Symbols with long codes in 1 string out of 3.
    const uint32_t longCodePercent = iter % 3 == 0 ? rng() % 60U : 0U;
    for (char& ch : value) {
      ch = rng() % 100U < longCodePercent ? static_cast<char>(rng()) : kShortCodeChars[rng() % kShortCodeChars.size()];
    }
    vector<uint8_t> huffmanString = RfcHuffmanString(value);
    ASSERT_NO_FATAL_FAILURE(ExpectHuffmanDecodedAsByTheModel(decoder, model, huffmanString)) << HexOf(huffmanString);
    nbValid += decoder.decode(AsBytes(LiteralFieldX(huffmanString, true))).isSuccess() ? 1U : 0U;

    switch (rng() % 6U) {
      case 0:  // another padding, or another last symbol
        if (!huffmanString.empty()) {
          huffmanString.back() = static_cast<uint8_t>(rng());
        }
        break;
      case 1:  // a whole byte of padding
        huffmanString.push_back(0xFF);
        break;
      case 2:  // a string ending within a code
        if (!huffmanString.empty()) {
          huffmanString.pop_back();
        }
        break;
      case 3:  // other symbols from some bit on
        if (!huffmanString.empty()) {
          huffmanString[rng() % huffmanString.size()] ^= static_cast<uint8_t>(1U << (rng() % 8U));
        }
        break;
      case 4:  // EOS
        for (uint32_t nbBytes = 1U + (rng() % 4U); nbBytes != 0; --nbBytes) {
          huffmanString.push_back(0xFF);
        }
        break;
      default:
        huffmanString.push_back(static_cast<uint8_t>(rng()));
        break;
    }
    ASSERT_NO_FATAL_FAILURE(ExpectHuffmanDecodedAsByTheModel(decoder, model, huffmanString)) << HexOf(huffmanString);
  }
  // Most of the strings before alteration are field values: the comparison is not only about refused strings.
  EXPECT_GT(nbValid, 12000U);
}

// The string literal written by the encoder is exactly the shortest of the Huffman string of the model and of the raw
// string, with its length prefix: the one of a Huffman string is shorter than the one of the raw string from 127 and
// from 255 bytes on.
TEST(HpackHuffman, EncoderWritesTheShortestOfTheHuffmanAndRawStrings) {
  std::mt19937 rng(5);  // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
  enum class Content : uint8_t {
    FiveBitCodes,     // the shortest Huffman strings
    HeaderText,       // codes of 5 to 8 bits
    SomeLongCodes,    // not encoded 4 symbols at once
    EightBitCodes,    // a Huffman string as long as the raw one
    AlmostEightBits,  // a Huffman string shorter by 0 to 15 bits than the raw one
    AnyBytes,         // a Huffman string longer than the raw one
  };
  static constexpr Content kContents[]{
      Content::FiveBitCodes,  Content::HeaderText,      Content::SomeLongCodes,
      Content::EightBitCodes, Content::AlmostEightBits, Content::AnyBytes,
  };
  std::size_t nbHuffman = 0;
  std::size_t nbRaw = 0;
  for (std::size_t len = 0; len <= 400; ++len) {
    for (const Content content : kContents) {
      std::string value(len, 'a');
      // Number of the last characters with a code of 7 bits instead of 8.
      const std::size_t nbSevenBitCodes = rng() % 16U;
      for (std::size_t pos = 0; pos < len; ++pos) {
        switch (content) {
          case Content::FiveBitCodes:
            value[pos] = kFiveBitChars[rng() % kFiveBitChars.size()];
            break;
          case Content::HeaderText:
            value[pos] = kShortCodeChars[rng() % kShortCodeChars.size()];
            break;
          case Content::SomeLongCodes:
            value[pos] = rng() % 10U == 0 ? static_cast<char>(rng()) : kShortCodeChars[rng() % kShortCodeChars.size()];
            break;
          case Content::EightBitCodes:
            value[pos] = kEightBitChars[rng() % kEightBitChars.size()];
            break;
          case Content::AlmostEightBits:
            value[pos] = pos + nbSevenBitCodes >= len ? 'B' : kEightBitChars[rng() % kEightBitChars.size()];
            break;
          case Content::AnyBytes:
            value[pos] = static_cast<char>(rng());
            break;
        }
      }

      HpackEncoder encoder(4096);
      RawBytes block;
      encoder.encode(block, "x", value, HpackEncoder::IndexingMode::WithoutIndexing);

      const vector<uint8_t> huffmanString = RfcHuffmanString(value);
      const bool huffmanIsShorter = huffmanString.size() < value.size();
      (huffmanIsShorter ? nbHuffman : nbRaw) += 1U;
      const vector<uint8_t> expected = huffmanIsShorter ? LiteralFieldX(huffmanString, true) : LiteralFieldX(value);
      ASSERT_EQ(HexOf(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(block.data()), block.size())),
                HexOf(expected))
          << "length " << len << ", content " << static_cast<int>(content);
    }
  }
  EXPECT_GT(nbHuffman, 1000U);
  EXPECT_GT(nbRaw, 800U);
}

}  // namespace aeronet::http2
