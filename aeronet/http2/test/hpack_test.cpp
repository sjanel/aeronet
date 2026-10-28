#include "aeronet/hpack.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/city-hash.hpp"
#include "aeronet/http-constants.hpp"
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

  // Two Cookie headers should be merged with ';'
  static constexpr uint8_t encoded[]{
      0x40, 0x06, 'c', 'o', 'o', 'k', 'i', 'e', 0x03, 'o', 'n', 'e',
      0x40, 0x06, 'c', 'o', 'o', 'k', 'i', 'e', 0x03, 't', 'w', 'o',
  };

  auto result = decoder.decode(AsBytes(encoded));

  EXPECT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.headers.size(), 1U);
  const auto it = result.decodedHeaders.headers.find("cookie");
  ASSERT_NE(it, result.decodedHeaders.headers.end());
  EXPECT_EQ(it->second, "one;two");
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

TEST(HpackDecoder, OverrideHeaderKeepsLastValue) {
  auto decoder = CreateHpackDecoder();

  // authorization (static name index 23) is an override header: only its last value is kept, like for HTTP/1.1.
  static constexpr uint8_t encoded[]{
      0x57, 0x01, 'a',  // authorization: a
      0x57, 0x01, 'b',  // authorization: b
  };

  const auto result = decoder.decode(AsBytes(encoded));

  ASSERT_TRUE(result.isSuccess());
  ASSERT_EQ(result.decodedHeaders.headers.size(), 1U);
  EXPECT_EQ(result.decodedHeaders.headers.at("authorization"), "b");
  // The size counts the fields as received.
  EXPECT_EQ(result.decodedHeaders.namesAndValuesSize, 2U * std::string_view("authorizationa").size());
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
  EXPECT_EQ(result.decodedHeaders.headers.at("cookie"), "a=1;b=2");
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

}  // namespace aeronet::http2
