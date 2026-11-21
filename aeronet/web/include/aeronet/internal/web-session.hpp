#pragma once

#include <amc/type_traits.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

#include "aeronet/amc-type-traits.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/web-client-config.hpp"

namespace aeronet::internal {

// The localStorage or the sessionStorage of the origins visited by a WebClient, in memory.
class StorageAreas {
 public:
  // An item: its key followed by its value, in one buffer.
  class Item {
   public:
    using trivially_relocatable = amc::is_trivially_relocatable<RawChars32>::type;

    Item(std::string_view key, std::string_view value);

    [[nodiscard]] std::string_view key() const noexcept { return std::string_view(_chars).substr(0, _keySize); }
    [[nodiscard]] std::string_view value() const noexcept { return std::string_view(_chars).substr(_keySize); }

    void setValue(std::string_view value);

   private:
    RawChars32 _chars;
    uint32_t _keySize;
  };

  // The items of an origin, in insertion order.
  struct Area {
    using trivially_relocatable = std::bool_constant<amc::is_trivially_relocatable_v<RawChars32> &&
                                                     amc::is_trivially_relocatable_v<vector<Item>>>::type;

    RawChars32 origin;
    vector<Item> items;
    uint32_t bytes{};  // of the keys and values, bounded by kMaxBytesPerOrigin
  };

  using trivially_relocatable = amc::is_trivially_relocatable<vector<Area>>::type;

  // The quota of the browsers.
  static constexpr uint32_t kMaxBytesPerOrigin = uint32_t{5} << 20U;

  // The area of 'origin', created empty on first use.
  Area& area(std::string_view origin);

 private:
  vector<Area> _areas;
};

// The bytecode of the scripts compiled by the pages of a WebClient, read back by the next pages that run the same
// script: reading is 15 to 70 times faster than compiling. An entry is keyed by the URL of the script (that of its
// document for an inline script) and by its content, through its size and a 64-bit hash: a changed script is compiled
// again. The least recently used entries are dropped beyond a total size. The bytecode is written by the process
// itself, so it is safe to read back (QuickJS-ng does not validate it).
class ScriptCache {
  struct Entry {
    using trivially_relocatable = std::bool_constant<amc::is_trivially_relocatable_v<RawBytes> &&
                                                     amc::is_trivially_relocatable_v<RawChars32>>::type;

    RawChars32 url;
    RawBytes bytecode;
    uint64_t hash;     // of the code
    uint64_t lastUse;  // sequence number of the last find() or store()
    std::size_t size;  // of the code
  };

 public:
  using trivially_relocatable = amc::is_trivially_relocatable<vector<Entry>>::type;

  // The scripts smaller than this are compiled each time: their compilation is short, their entry not worth it.
  static constexpr std::size_t kMinScriptSize = 1024;

  // A script to look up: its URL, its code, and the hash of its code, computed once for find() and store().
  struct Script {
    Script(std::string_view url, std::string_view code) noexcept;

    std::string_view url;
    std::string_view code;
    uint64_t hash;
  };

  // A cache of at most 'maxBytes' of bytecode, none for 0.
  explicit ScriptCache(std::size_t maxBytes) noexcept : _maxBytes(maxBytes) {}

  // Whether the scripts of 'size' bytes are cached.
  [[nodiscard]] bool caches(std::size_t size) const noexcept { return size >= kMinScriptSize && _maxBytes != 0; }

  // The bytecode of 'script', empty if not cached.
  [[nodiscard]] std::span<const std::byte> find(const Script& script) noexcept;

  // Stores the bytecode 'bytecode' of 'script', replacing its entry, then drops the least recently used entries beyond
  // the size limit. A bytecode larger than the limit is not stored.
  void store(const Script& script, std::span<const std::byte> bytecode);

  // The total size of the bytecode of the entries.
  [[nodiscard]] std::size_t bytes() const noexcept { return _bytes; }

  [[nodiscard]] std::size_t size() const noexcept { return _entries.size(); }

 private:
  // The entry of 'script', null if absent. A linear scan: a few thousand entries at most, read in a few microseconds,
  // much less than the read of a bytecode.
  [[nodiscard]] Entry* entryOf(const Script& script) noexcept;

  vector<Entry> _entries;
  std::size_t _bytes{};
  std::size_t _maxBytes;
  uint64_t _sequence{};
};

// The state of a WebClient that its pages share. The pages point to it: the client cannot move while it has some.
struct WebSession {
  explicit WebSession(WebClientConfig cfg);

  WebClientConfig config;
  HttpClient http;
  // The prelude compiled by the first page (see PreludeSource()), read back by the next ones: its compilation took 3/4
  // of the setup of a page (180 us instead of 760 us for a small document, Release). Written by the process, so safe
  // to read.
  RawBytes preludeBytecode;
  // The second part of the prelude (see NetworkPreludeSource()), compiled by the first page that uses one of its
  // globals.
  RawBytes networkBytecode;
  ScriptCache scripts;
  StorageAreas localStorage;
  StorageAreas sessionStorage;
  uint32_t livePages{};  // the pages of the session not destroyed yet
};

}  // namespace aeronet::internal
