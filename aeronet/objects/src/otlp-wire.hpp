#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Protobuf wire encoding (https://protobuf.dev/programming-guides/encoding/) of the OTLP messages written by aeronet
// itself: the spans (see SpanRecord) and the headers of their batches (see SpanBatchHeader).
namespace aeronet::tracing::detail::otlp {

enum class WireType : uint8_t { kVarint = 0, kFixed64 = 1, kLen = 2, kFixed32 = 5 };

// The tag of a field: its number and wire type, itself written as a varint.
consteval uint32_t Tag(uint32_t fieldNumber, WireType wireType) noexcept {
  return (fieldNumber << 3U) | static_cast<uint32_t>(wireType);
}

// Fields of opentelemetry.proto.trace.v1.ResourceSpans.
inline constexpr uint32_t kResourceSpansResource = Tag(1, WireType::kLen);
inline constexpr uint32_t kResourceSpansScopeSpans = Tag(2, WireType::kLen);
inline constexpr uint32_t kResourceSpansSchemaUrl = Tag(3, WireType::kLen);

// Fields of opentelemetry.proto.trace.v1.ScopeSpans.
inline constexpr uint32_t kScopeSpansScope = Tag(1, WireType::kLen);
inline constexpr uint32_t kScopeSpansSpans = Tag(2, WireType::kLen);
inline constexpr uint32_t kScopeSpansSchemaUrl = Tag(3, WireType::kLen);

// Fields of opentelemetry.proto.trace.v1.Span.
inline constexpr uint32_t kSpanTraceId = Tag(1, WireType::kLen);
inline constexpr uint32_t kSpanSpanId = Tag(2, WireType::kLen);
inline constexpr uint32_t kSpanName = Tag(5, WireType::kLen);
inline constexpr uint32_t kSpanKind = Tag(6, WireType::kVarint);
inline constexpr uint32_t kSpanStartTimeUnixNano = Tag(7, WireType::kFixed64);
inline constexpr uint32_t kSpanEndTimeUnixNano = Tag(8, WireType::kFixed64);
inline constexpr uint32_t kSpanAttributes = Tag(9, WireType::kLen);
inline constexpr uint32_t kSpanFlags = Tag(16, WireType::kFixed32);

// Fields of opentelemetry.proto.common.v1.KeyValue.
inline constexpr uint32_t kKeyValueKey = Tag(1, WireType::kLen);
inline constexpr uint32_t kKeyValueValue = Tag(2, WireType::kLen);

// Fields of opentelemetry.proto.common.v1.AnyValue.
inline constexpr uint32_t kAnyValueStringValue = Tag(1, WireType::kLen);
inline constexpr uint32_t kAnyValueIntValue = Tag(3, WireType::kVarint);
inline constexpr uint32_t kAnyValueBytesValue = Tag(7, WireType::kLen);

// Span.SpanKind.SPAN_KIND_INTERNAL, the kind of the spans started by the SDK without explicit kind.
inline constexpr uint8_t kSpanKindInternal = 1;

// SpanFlags.SPAN_FLAGS_TRACE_FLAGS_MASK bits of Span.flags: the W3C trace flags, here the sampled one.
inline constexpr uint32_t kSpanFlagsSampled = 0x01;

inline constexpr std::size_t kTraceIdSize = 16;
inline constexpr std::size_t kSpanIdSize = 8;

// Size of the longest varint of a 32-bit value.
inline constexpr std::size_t kMaxVarint32Size = 5;

// Number of bytes of the varint encoding of value: 7 bits per byte.
constexpr std::size_t VarintSize(uint64_t value) noexcept {
  return (static_cast<std::size_t>(std::bit_width(value | 1U)) + 6U) / 7U;
}

// Writes value as a varint at pDest. Returns the end of the written bytes.
constexpr char* AppendVarint(uint64_t value, char* pDest) noexcept {
  while (value >= 0x80U) {
    *pDest++ = static_cast<char>(value | 0x80U);
    value >>= 7U;
  }
  *pDest++ = static_cast<char>(value);
  return pDest;
}

// Writes value as a varint of exactly kMaxVarint32Size bytes, with redundant continuation bytes when it is shorter, as
// Perfetto's protozero does for the length of a message written before its contents are known: a parser accepts any
// varint of at most 10 bytes. Returns the end of the written bytes.
constexpr char* AppendPaddedVarint32(uint32_t value, char* pDest) noexcept {
  for (std::size_t pos = 0; pos + 1 < kMaxVarint32Size; ++pos) {
    *pDest++ = static_cast<char>((value & 0x7FU) | 0x80U);
    value >>= 7U;
  }
  *pDest++ = static_cast<char>(value);
  return pDest;
}

// Writes the little endian bytes of value at pDest (fixed32, fixed64). Returns the end of the written bytes.
template <class T>
char* AppendLittleEndian(T value, char* pDest) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  std::memcpy(pDest, &value, sizeof(value));
  return pDest + sizeof(value);
}

}  // namespace aeronet::tracing::detail::otlp
