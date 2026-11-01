#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-view.hpp"
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
  explicit HpackDynamicTable(uint32_t maxSizeBytes) noexcept : _maxSizeBytes(maxSizeBytes) {}

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
  void setMaxSize(uint32_t maxSize);

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
  uint32_t _maxSizeBytes;
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

/// The pseudo-header fields (RFC 9113 §8.3) accepted by HpackDecoder.
enum class PseudoHeader : uint8_t { Method, Scheme, Authority, Path, Status, Protocol };

/// The names of the pseudo-header fields, indexed by PseudoHeader.
inline constexpr std::string_view kPseudoHeaderNames[]{
    http::PseudoHeaderMethod, http::PseudoHeaderScheme, http::PseudoHeaderAuthority,
    http::PseudoHeaderPath,   http::PseudoHeaderStatus, http::PseudoHeaderProtocol,
};

/// A header block (headers or trailers of a message) decoded by HpackDecoder.
///
/// Its pseudo-header fields are kept apart from its regular fields, in a slot each. The names and values point either
/// to the HPACK static table or into 'storage', whose bytes never move: the owner of a message takes them over by
/// swapping 'headers' and 'storage' with its own (empty) ones, so that each decoded byte is written only once. The
/// values are writable, except the ones of the static table, which are never values of :path or of content-encoding.
struct DecodedHeaders {
  using Storage = ObjectArrayPool<char>;

  /// First block of a storage: enough for the decoded headers of most requests.
  static constexpr Storage::size_type kStorageInitialCapacity = 512U;

  /// Tell whether the block has the given pseudo-header field.
  [[nodiscard]] bool hasPseudoHeader(PseudoHeader pseudoHeader) const noexcept {
    return (pseudoHeadersMask & PseudoHeaderBit(pseudoHeader)) != 0;
  }

  /// Tell whether the block has any pseudo-header field.
  [[nodiscard]] bool hasPseudoHeaders() const noexcept { return pseudoHeadersMask != 0; }

  /// The value of the given pseudo-header field, empty if absent (see hasPseudoHeader()).
  [[nodiscard]] std::string_view pseudoHeader(PseudoHeader pseudoHeader) const noexcept {
    return pseudoHeaders[static_cast<std::size_t>(pseudoHeader)];
  }

  /// Forget the decoded fields, keeping the capacity of the map and of the storage.
  void clear() noexcept;

  [[nodiscard]] static constexpr uint8_t PseudoHeaderBit(PseudoHeader pseudoHeader) noexcept {
    return static_cast<uint8_t>(1U << static_cast<uint8_t>(pseudoHeader));
  }

  SvToSvMap headers;  // Regular fields, the duplicates merged
  Storage storage{kStorageInitialCapacity};
  std::string_view pseudoHeaders[std::size(kPseudoHeaderNames)];
  // Total size of the names and values of the fields as received (duplicates not merged), pseudo-headers included.
  std::size_t namesAndValuesSize{0};
  uint8_t pseudoHeadersMask{0};
};

/// HPACK decoder for decompressing HTTP/2 header blocks (RFC 7541).
///
/// Thread safety: NOT thread-safe. Each HTTP/2 connection should have its own decoder.
/// The decoder maintains state (dynamic table) that persists across header blocks.
class HpackDecoder {
 public:
  /// Create a decoder with the specified maximum dynamic table size, also the limit of the Dynamic Table Size Updates
  /// of the encoder (see setMaxDynamicTableSize()).
  /// The fields of a header list larger than maxHeaderListSize (sum of the sizes of the names and values of its fields,
  /// plus 32 bytes per field, RFC 7541 §4.1) are not stored: its decoding fails with HeaderListTooLarge, without taking
  /// more memory than this limit, whatever the number of references to the dynamic table in its block.
  HpackDecoder(uint32_t maxDynamicTableSize, uint32_t maxHeaderListSize,
               bool mergeAllowedForUnknownRequestHeaders) noexcept;

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
      HeaderListTooLarge,

      // Compression errors (RFC 7541 §6.1, §6.2, §6.3)
      FailedToDecodeIndexedHeaderFieldIndex,
      InvalidIndexZeroInIndexedHeaderField,
      IndexOutOfBoundsInIndexedHeaderField,
      FailedToDecodeDynamicTableSizeUpdate,
      DynamicTableSizeUpdateAboveLimit,
      DynamicTableSizeUpdateAfterField,
      FailedToDecodeLiteralHeaderIndex,
      FailedToDecodeLiteralHeaderName,
      IndexOutOfBoundsForHeaderName,
      FailedToDecodeLiteralHeaderValue,

    };

    [[nodiscard]] bool isSuccess() const noexcept { return error == Error::None; }
    [[nodiscard]] bool isCompressionError() const noexcept {
      return error >= Error::FailedToDecodeIndexedHeaderFieldIndex;
    }

    DecodedHeaders& decodedHeaders;
    uint64_t headerListSize;
    Error error{Error::None};
  };

  /// Decode a complete header block.
  /// The decoded headers are valid until the next decode() call, unless they were taken over (see DecodedHeaders).
  ///
  /// @param data The compressed header block
  /// @return DecodeResult indicating success or failure with the decoded headers
  DecodeResult decode(std::span<const std::byte> data);

  /// Set the limit of the dynamic table size (SETTINGS_HEADER_TABLE_SIZE for HTTP/2, once acknowledged by the peer):
  /// a Dynamic Table Size Update above it is a decoding error (RFC 7541 §6.3). The table is reduced to it if larger,
  /// while a larger limit is only reached by a size update of the encoder.
  void setMaxDynamicTableSize(uint32_t maxSize);

  /// Get the current dynamic table for inspection.
  [[nodiscard]] const HpackDynamicTable& dynamicTable() const noexcept { return _dynamicTable; }

 private:
  /// Where the bytes of a decoded name or value are.
  enum class Source : uint8_t {
    StaticTable,  // The static table: valid, and never freed
    Storage,      // The storage of the decoded headers (a decoded Huffman string)
    Transient,    // The input or the dynamic table: copied to the storage when stored
  };

  struct DecodedString {
    static constexpr std::size_t kInvalidConsumed = static_cast<std::size_t>(~0ULL);

    std::string_view str;
    std::size_t consumed{kInvalidConsumed};
    Source source{Source::Transient};
  };

  struct IndexedField {
    http::HeaderView header;  // Empty name if the index is out of bounds
    Source source{Source::Transient};
  };

  /// Decode a string literal (RFC 7541 §5.2).
  /// Returns the decoded string and number of bytes consumed, or kInvalidConsumed on error.
  [[nodiscard]] DecodedString decodeString(std::span<const std::byte> data);

  /// Decode a Huffman-encoded string into the storage of the decoded headers.
  /// Returns a null string_view on error.
  [[nodiscard]] std::string_view decodeHuffman(std::span<const std::byte> data);

  /// Look up a header by combined index (1-61 = static, 62+ = dynamic).
  [[nodiscard]] IndexedField lookupIndex(uint32_t index) const;

  /// The decoded string, copied to the storage of the decoded headers unless it is already there or in the static
  /// table.
  [[nodiscard]] std::string_view stableString(std::string_view str, Source source);

  // Store a decoded header field into the decoded headers.
  DecodeResult::Error storeHeader(std::string_view name, Source nameSource, std::string_view value, Source valueSource,
                                  bool& seenRegularHeader);

  // Join the values of the repeated fields recorded in _deferredJoins to their first one, once the block is decoded.
  void joinRepeatedFields();

  /// A value to append to a repeated field, after its separator.
  struct DeferredJoin {
    std::string_view name;  // The name of the field in the decoded headers (the same view for all its values)
    std::string_view value;
    uint32_t order;  // Position among the deferred joins: the order of the values of a field
    char separator;
  };

  HpackDynamicTable _dynamicTable;

  DecodedHeaders _decodedHeaders;
  vector<DeferredJoin> _deferredJoins;
  uint32_t _maxHeaderListSize;
  uint32_t _dynamicTableSizeLimit;
  bool _mergeAllowedForUnknownRequestHeaders{true};
};

/// HPACK encoder for compressing HTTP/2 header blocks (RFC 7541).
///
/// Thread safety: NOT thread-safe. Each HTTP/2 connection should have its own encoder.
/// The encoder maintains state (dynamic table) that persists across header blocks.
class HpackEncoder {
 public:
  /// Create an encoder with the specified maximum dynamic table size.
  explicit HpackEncoder(uint32_t maxDynamicTableSize = 4096) noexcept : _dynamicTable(maxDynamicTableSize) {}

  /// Encoding options for a header field.
  enum class IndexingMode : std::uint8_t {
    Indexed,          ///< Add to dynamic table (default)
    WithoutIndexing,  ///< Don't add to dynamic table
    NeverIndexed,     ///< Never index (sensitive values)
  };

  /// The indexing mode of a field of a message (request, response or trailers):
  ///  - never indexed for the credentials (authorization, proxy-authorization, and cookie values shorter than 20
  ///    bytes), which a guessing attack on the compressed size could recover (RFC 7541 §7.1.3).
  ///  - without indexing for the fields whose value most likely differs from one message to the next (content-length,
  ///    etag, last-modified, location, set-cookie...), like nghttp2 does. Each of them would otherwise be a dynamic
  ///    table insertion, evicting the fields that do repeat (content-type, the global headers...).
  ///  - indexed for the others.
  [[nodiscard]] static IndexingMode IndexingModeOf(std::string_view name, std::string_view value) noexcept;

  /// Encode a header field and append to the output buffer.
  /// @param output The buffer to append the encoded header field to.
  /// @param name The header field name.
  /// @param value The header field value (should be trimmed from OWS).
  /// @param mode The indexing mode for the header field (default is Indexed).
  /// @return The size of the encoded header field in bytes.
  uint64_t encode(RawBytes& output, std::string_view name, std::string_view value,
                  IndexingMode mode = IndexingMode::Indexed);

  /// Encode a dynamic table size update.
  void encodeDynamicTableSizeUpdate(RawBytes& output, uint32_t newSize);

  /// Set the maximum dynamic table size (at most the SETTINGS_HEADER_TABLE_SIZE of the peer for HTTP/2), signaled by a
  /// Dynamic Table Size Update at the beginning of the next header block, unless it does not change.
  void setMaxDynamicTableSize(uint32_t maxSize);

  /// Get the current dynamic table for inspection.
  [[nodiscard]] const HpackDynamicTable& dynamicTable() const noexcept { return _dynamicTable; }

  /// Find a header in the static and dynamic tables.
  [[nodiscard]] HpackLookupResult findHeader(std::string_view name, std::string_view value);

 private:
  friend struct HpackEncoderTestAccess;

  void addToDynamicIndex(std::string_view name, std::string_view value);
  [[nodiscard]] static std::size_t dynamicFullHash(std::size_t nameHash, std::string_view value) noexcept;
  [[nodiscard]] static std::size_t dynamicNameHash(std::size_t nameHash) noexcept;
  void resizeDynamicIndex();
  [[nodiscard]] HpackLookupResult findDynamicHeaderIndexed(std::string_view name, std::string_view value) const;
  [[nodiscard]] std::size_t dynamicLiveIndex(uint64_t serial) const noexcept;
  void indexDynamicHeader(std::string_view name, std::string_view value, uint64_t serial);
  void rebuildDynamicIndex();
  void resetDynamicIndex();

  HpackDynamicTable _dynamicTable;
  flat_hash_map<std::size_t, uint64_t> _dynamicIndex;
  uint32_t _pendingTableSizeUpdate{};
  bool _hasPendingTableSizeUpdate{false};
  uint64_t _newestDynamicSerial{0U};
};

}  // namespace aeronet::http2
