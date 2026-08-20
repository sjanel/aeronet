#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/http-header.hpp"
#include "aeronet/object-array-pool.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/sv-to-sv-map.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {

/// Get the HPACK static table (61 entries, 1-indexed in the spec but 0-indexed here).
/// Returns a span of 61 entries where index 0 corresponds to static table index 1.
[[nodiscard]] std::span<const http::HeaderView> GetHpackStaticTable() noexcept;

/// HPACK dynamic table with FIFO eviction (RFC 7541 §2.3.2).
///
/// The dynamic table is a FIFO queue where new entries are added at the front
/// and old entries are evicted from the back when the table size exceeds the limit.
///
/// Indexing follows RFC 7541 §2.3.3:
/// - Static table indices: 1-61
/// - Dynamic table indices: 62+ (62 = most recently added entry)
class HpackDynamicTable {
 public:
  /// Create a dynamic table with the specified maximum size in bytes.
  explicit HpackDynamicTable(std::size_t maxSizeBytes) noexcept : _maxSizeBytes(maxSizeBytes) {}

  /// Add a new entry to the front of the table.
  /// May trigger eviction of old entries if the new entry doesn't fit.
  /// Returns true if the entry was added, false if it's too large for the table.
  bool add(std::string_view name, std::string_view value);

  /// Get entry at the specified dynamic table index (0 = most recent).
  /// Index must be lower than entryCount().
  [[nodiscard]] const http::Header& operator[](uint32_t index) const noexcept {
    return _entries[_entries.size() - 1U - index];
  }

  /// Get the number of entries in the dynamic table.
  [[nodiscard]] std::size_t entryCount() const noexcept { return _entries.size() - _firstLive; }

  /// Get the current size of the dynamic table in bytes.
  [[nodiscard]] std::size_t currentSizeBytes() const noexcept { return _currentSizeBytes; }

  /// Get the maximum size of the dynamic table in bytes.
  [[nodiscard]] std::size_t maxSizeBytes() const noexcept { return _maxSizeBytes; }

  /// Update the maximum size of the dynamic table.
  /// May trigger eviction if the new size is smaller than the current size.
  void setMaxSize(std::size_t maxSize);

  /// Clear all entries from the dynamic table.
  void clear() noexcept;

 private:
  http::Header evict();

  // Live entries occupy [_firstLive, _entries.size()) in chronological order: _entries[_firstLive] is the oldest, so
  // the next to be evicted, and _entries.back() is the newest. RFC 7541 numbers the dynamic table newest-first, which
  // is why operator[] reverses the index.
  //
  // Storing it this way makes add() an amortised push_back and evict() a cursor bump. Keeping the RFC order in memory
  // instead would mean inserting at the front, which relocates every live entry on every single add.
  // Only the 16-byte Header handles move; the name/value buffers they point at never do, so string_views handed out by
  // the decoder stay valid across add(), evict() and compact().
  vector<http::Header> _entries;
  std::size_t _currentSizeBytes{0};
  std::size_t _maxSizeBytes;
  decltype(_entries)::size_type _firstLive{0};
};

/// Result of looking up a header in the HPACK tables.
struct HpackLookupResult {
  enum class Match : std::uint8_t {
    None,      ///< No match found
    NameOnly,  ///< Name matched but value did not
    Full,      ///< Both name and value matched
  };

  Match match{Match::None};
  uint32_t index{0};  ///< Combined index (1-61 = static, 62+ = dynamic)
};

/// HPACK decoder for decompressing HTTP/2 header blocks (RFC 7541).
///
/// Thread safety: NOT thread-safe. Each HTTP/2 connection should have its own decoder.
/// The decoder maintains state (dynamic table) that persists across header blocks.
class HpackDecoder {
 public:
  /// Create a decoder with the specified maximum dynamic table size.
  HpackDecoder(std::size_t maxDynamicTableSize, bool mergeAllowedForUnknownRequestHeaders);

  /// Decode result for a single header block.
  struct DecodeResult {
    enum class Error : uint8_t {
      None,

      // Malformed field section errors (RFC 7540 §)
      PseudoHeaderFieldAfterRegularField,
      UndefinedPseudoHeaderField,
      DuplicatePseudoHeaderField,
      MalformedFieldName,
      MalformedFieldValue,
      DuplicateHeaderForbiddenToMerge,

      // Compression errors (RFC 7541 §6.1, §6.2, §6.3)
      FailedToDecodeIndexedHeaderFieldIndex,
      InvalidIndexZeroInIndexedHeaderField,
      IndexOutOfBoundsInIndexedHeaderField,
      FailedToDecodeDynamicTableSizeUpdate,
      FailedToDecodeLiteralHeaderIndex,
      FailedToDecodeLiteralHeaderName,
      IndexOutOfBoundsForHeaderName,
      FailedToDecodeLiteralHeaderValue,

    };

    [[nodiscard]] bool isSuccess() const noexcept { return error == Error::None; }
    [[nodiscard]] bool isCompressionError() const noexcept {
      return error >= Error::FailedToDecodeIndexedHeaderFieldIndex;
    }

    const SvToSvMap& decodedHeaders;
    uint64_t headerListSize;
    Error error{Error::None};
  };

  /// Decode a complete header block fragment.
  /// Calls the callback for each decoded header field.
  /// The string_views in HeaderField are valid until the next decode() call or clear().
  ///
  /// @param data The compressed header block fragment
  /// @return DecodeResult indicating success or failure with headers and error message
  DecodeResult decode(std::span<const std::byte> data);

  /// Update the maximum dynamic table size (from SETTINGS frame).
  void setMaxDynamicTableSize(std::size_t maxSize) { _dynamicTable.setMaxSize(maxSize); }

  /// Get the current dynamic table for inspection.
  [[nodiscard]] const HpackDynamicTable& dynamicTable() const noexcept { return _dynamicTable; }

 private:
  struct DecodedString {
    static constexpr std::size_t kInvalidConsumed = static_cast<std::size_t>(~0ULL);

    std::string_view str;
    std::size_t consumed{kInvalidConsumed};
  };

  using CharStorage = ObjectArrayPool<char>;
  using CharStorageSizeType = CharStorage::size_type;

  /// Decode a string literal (RFC 7541 §5.2).
  /// Returns the decoded string and number of bytes consumed, or nullopt on error.
  [[nodiscard]] DecodedString decodeString(std::span<const std::byte> data);

  /// Decode Huffman-encoded string.
  /// Returns the start pos of the decoded string in the internal buffer (end is _decodedStrings.size()),
  /// or std::numeric_limits<std::size_t>::max() on error.
  [[nodiscard]] std::string_view decodeHuffman(std::span<const std::byte> data);

  /// Look up a header by combined index (1-61 = static, 62+ = dynamic).
  /// Returns an empty header name if index is out of bounds.
  [[nodiscard]] http::HeaderView lookupIndex(uint64_t index) const;

  // Store a decoded header into internal storage.
  // Returns an error message on failure, or nullptr on success.
  DecodeResult::Error storeHeader(std::string_view name, std::string_view value, bool& seenRegularHeader);

  HpackDynamicTable _dynamicTable;

  // Buffer for storing decoded strings that outlive the input data (Huffman decoded or dynamic table copies)
  SvToSvMap _decodedHeadersMap;
  CharStorage _decodedStrings;
  bool _mergeAllowedForUnknownRequestHeaders{true};
};

/// HPACK encoder for compressing HTTP/2 header blocks (RFC 7541).
///
/// Thread safety: NOT thread-safe. Each HTTP/2 connection should have its own encoder.
/// The encoder maintains state (dynamic table) that persists across header blocks.
class HpackEncoder {
 public:
  /// Create an encoder with the specified maximum dynamic table size.
  explicit HpackEncoder(std::size_t maxDynamicTableSize = 4096) : _dynamicTable(maxDynamicTableSize) {}

  /// Encoding options for a header field.
  enum class IndexingMode : std::uint8_t {
    Indexed,          ///< Add to dynamic table (default)
    WithoutIndexing,  ///< Don't add to dynamic table
    NeverIndexed,     ///< Never index (sensitive values)
  };

  /// Encode a header field and append to the output buffer.
  void encode(RawBytes& output, std::string_view name, std::string_view value,
              IndexingMode mode = IndexingMode::Indexed);

  /// Encode a dynamic table size update.
  void encodeDynamicTableSizeUpdate(RawBytes& output, std::size_t newSize);

  /// Update the maximum dynamic table size (from SETTINGS frame).
  void setMaxDynamicTableSize(std::size_t maxSize) { _pendingTableSizeUpdate = maxSize; }

  /// Get the current dynamic table for inspection.
  [[nodiscard]] const HpackDynamicTable& dynamicTable() const noexcept { return _dynamicTable; }

  /// Find a header in the static and dynamic tables.
  [[nodiscard]] HpackLookupResult findHeader(std::string_view name, std::string_view value);

 private:
  HpackDynamicTable _dynamicTable;
  std::size_t _pendingTableSizeUpdate = static_cast<std::size_t>(~0ULL);
};

}  // namespace aeronet::http2
