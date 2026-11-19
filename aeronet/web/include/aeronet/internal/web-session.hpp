#pragma once

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "aeronet/amc-type-traits.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/web-client-config.hpp"
#include "amc/type_traits.hpp"

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

// The state of a WebClient that its pages share. The pages point to it: the client cannot move while it has some.
struct WebSession {
  explicit WebSession(WebClientConfig cfg);

  WebClientConfig config;
  HttpClient http;
  // The prelude compiled by the first page (see PreludeSource()), read back by the next ones: its compilation took 3/4
  // of the setup of a page (180 us instead of 760 us for a small document, Release). Written by the process, so safe
  // to read.
  RawBytes preludeBytecode;
  StorageAreas localStorage;
  StorageAreas sessionStorage;
  uint32_t livePages{};  // the pages of the session not destroyed yet
};

}  // namespace aeronet::internal
