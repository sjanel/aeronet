#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

#include "aeronet/http-header.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {

/// Get the HPACK static table (61 entries, 1-indexed in the spec but 0-indexed here).
/// Returns a span of 61 entries where index 0 corresponds to static table index 1.
[[nodiscard]] std::span<const http::HeaderView> GetHpackStaticTable() noexcept;

/// HPACK dynamic table entry.
/// Each entry has an overhead of 32 bytes as per RFC 7541 §4.1.
class HpackDynamicEntry {
 public:
  static constexpr std::size_t kOverhead = 32;

  // Create a dynamic table entry with the specified name and value.
  // Internally, the name is stored in lowercase for string searching optimization.
  HpackDynamicEntry(std::string_view name, std::string_view value);

  /// Calculate the size of this entry as defined by RFC 7541 §4.1.
  [[nodiscard]] std::size_t size() const noexcept { return _nameLength + _valueLength + kOverhead; }

  [[nodiscard]] std::string_view name() const noexcept { return {_data.get(), _nameLength}; }

  [[nodiscard]] std::string_view value() const noexcept { return {_data.get() + _nameLength, _valueLength}; }

  using trivially_relocatable = std::true_type;

 private:
  std::unique_ptr<char[]> _data;
  std::size_t _nameLength;
  std::size_t _valueLength;
};

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
  explicit HpackDynamicTable(std::size_t maxSize = 4096) noexcept : _maxSize(maxSize) {}

  /// Add a new entry to the front of the table.
  /// May trigger eviction of old entries if the new entry doesn't fit.
  /// Returns true if the entry was added, false if it's too large for the table.
  bool add(std::string_view name, std::string_view value);

  /// Get entry at the specified dynamic table index (0 = most recent).
  /// Returns nullptr if index is out of bounds.
  [[nodiscard]] const HpackDynamicEntry& operator[](uint32_t index) const noexcept { return _entries[index]; }

  /// Get the number of entries in the dynamic table.
  [[nodiscard]] std::size_t entryCount() const noexcept { return _entries.size(); }

  /// Get the current size of the dynamic table in bytes.
  [[nodiscard]] std::size_t currentSize() const noexcept { return _currentSize; }

  /// Get the maximum size of the dynamic table in bytes.
  [[nodiscard]] std::size_t maxSize() const noexcept { return _maxSize; }

  /// Update the maximum size of the dynamic table.
  /// May trigger eviction if the new size is smaller than the current size.
  void setMaxSize(std::size_t maxSize);

  /// Clear all entries from the dynamic table.
  void clear() noexcept;

 private:
  void evict();

  vector<HpackDynamicEntry> _entries;
  std::size_t _currentSize{0};
  std::size_t _maxSize;
};

/// Result of looking up a header in the HPACK tables.
struct HpackLookupResult {
  enum class Match : std::uint8_t {
    None,      ///< No match found
    NameOnly,  ///< Name matched but value did not
    Full       ///< Both name and value matched
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
  explicit HpackDecoder(std::size_t maxDynamicTableSize = 4096) : _dynamicTable(maxDynamicTableSize) {}

  /// Decode result for a single header block.
  struct DecodeResult {
    [[nodiscard]] bool isSuccess() const noexcept { return errorMessage == nullptr; }

    const char* errorMessage;
  };

  using DecodeCallback = std::function<void(std::string_view name, std::string_view value)>;

  /// Decode a complete header block fragment.
  /// Calls the callback for each decoded header field.
  /// The string_views in HeaderField are valid until the next decode() call or clear().
  ///
  /// @param data The compressed header block fragment
  /// @param callback Called for each decoded header (name, value)
  /// @return DecodeResult indicating success or failure with error message
  DecodeResult decode(std::span<const std::byte> data, const DecodeCallback& callback);

  /// Update the maximum dynamic table size (from SETTINGS frame).
  void setMaxDynamicTableSize(std::size_t maxSize) { _dynamicTable.setMaxSize(maxSize); }

  /// Get the current dynamic table for inspection.
  [[nodiscard]] const HpackDynamicTable& dynamicTable() const noexcept { return _dynamicTable; }

  /// Clear decoded strings buffer (for memory management between requests).
  void clearDecodedStrings() noexcept { _decodedStrings.clear(); }

 private:
  struct DecodedString {
    static constexpr std::size_t kInvalidConsumed = static_cast<std::size_t>(~0ULL);

    std::string_view str;
    std::size_t consumed{kInvalidConsumed};
  };

  /// Decode a string literal (RFC 7541 §5.2).
  /// Returns the decoded string and number of bytes consumed, or nullopt on error.
  [[nodiscard]] DecodedString decodeString(std::span<const std::byte> data);

  /// Decode Huffman-encoded string.
  /// Returns the start pos of the decoded string in the internal buffer (end is _decodedStrings.size()),
  /// or std::numeric_limits<std::size_t>::max() on error.
  [[nodiscard]] std::size_t decodeHuffman(std::span<const std::byte> data);

  /// Look up a header by combined index (1-61 = static, 62+ = dynamic).
  /// Returns an empty header name if index is out of bounds.
  [[nodiscard]] http::HeaderView lookupIndex(std::size_t index) const;

  HpackDynamicTable _dynamicTable;

  // Buffer for storing decoded strings that outlive the input data (Huffman decoded or dynamic table copies)
  RawChars _decodedStrings;
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
    NeverIndexed      ///< Never index (sensitive values)
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

  using trivially_relocatable = std::true_type;

 private:
  HpackDynamicTable _dynamicTable;
  RawChars32 _lowerCaseBuffer;
  std::size_t _pendingTableSizeUpdate = static_cast<std::size_t>(~0ULL);
};

}  // namespace aeronet::http2
