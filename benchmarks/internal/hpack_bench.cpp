// HPACK encoder/decoder micro-benchmarks.
// Measures hot paths: decode, encode, findHeader, Huffman, dynamic table ops.
#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/hpack.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/raw-bytes.hpp"

namespace aeronet::http2 {
namespace {

// ---------------------------------------------------------------------------
// Helpers: build synthetic HPACK-encoded header blocks at various sizes
// ---------------------------------------------------------------------------

// Encode a set of headers into a raw HPACK block using the encoder.
RawBytes EncodeHeaderBlock(std::span<const http::HeaderView> headers) {
  HpackEncoder encoder;
  RawBytes out;
  for (const auto& hv : headers) {
    encoder.encode(out, hv.name, hv.value);
  }
  return out;
}

// Small: 5 typical request pseudo-headers + a few regular headers
constexpr std::array<http::HeaderView, 5> kSmallHeaders{{
    {":method", "GET"},
    {":path", "/api/users/123"},
    {":scheme", "https"},
    {":authority", "example.com"},
    {"accept", "application/json"},
}};

// Medium: 20 headers mixing indexed and literal
constexpr std::array<http::HeaderView, 20> kMediumHeaders{{
    {":method", "POST"},
    {":path", "/api/v2/resources"},
    {":scheme", "https"},
    {":authority", "bench.example.com"},
    {"content-type", "application/json"},
    {"accept", "application/json"},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", "en-US"},
    {"authorization", "Bearer dummy-token-value"},
    {"cache-control", "no-cache"},
    {"user-agent", "aeronet-bench/1.0"},
    {"x-request-id", "aaaabbbb-cccc-dddd-eeee-ffff00001111"},
    {"x-correlation-id", "11112222-3333-4444-5555-666677778888"},
    {"content-length", "256"},
    {"cookie", "session=dummy_cookie; theme=dark"},
    {"referer", "https://bench.example.com/dashboard"},
    {"origin", "https://bench.example.com"},
    {"x-forwarded-for", "192.168.1.100"},
    {"x-real-ip", "10.0.0.42"},
    {"if-none-match", "W/\"abc123\""},
}};

// Large: 50 headers — simulates header-heavy workloads (proxies, CDN)
auto BuildLargeHeaders() {
  // We need storage that outlives the function — use static buffers.
  static std::array<std::string, 50> names;
  static std::array<std::string, 50> values;
  std::array<http::HeaderView, 50> hdrs{};

  // First 4 are pseudo-headers
  names[0] = ":method";
  values[0] = "GET";
  names[1] = ":path";
  values[1] = "/api/benchmark/large-header-stress";
  names[2] = ":scheme";
  values[2] = "https";
  names[3] = ":authority";
  values[3] = "large-header.bench.example.com";

  for (std::size_t ii = 4; ii < 50; ++ii) {
    names[ii] = "x-bench-hdr-" + std::to_string(ii);
    values[ii] = std::string(128, static_cast<char>('a' + (ii % 26)));
  }
  for (std::size_t ii = 0; ii < 50; ++ii) {
    hdrs[ii] = {names[ii], values[ii]};
  }
  return hdrs;
}

const auto kLargeHeaders = BuildLargeHeaders();

constexpr std::size_t kDynamicTableEntryOverhead = 32U;
constexpr std::size_t kDynamicTableWorkloadSize = 2048U;

auto BuildDynamicTableWorkload() {
  struct Workload {
    std::array<std::string, kDynamicTableWorkloadSize> names;
    std::array<std::string, kDynamicTableWorkloadSize> values;
  };

  Workload workload;
  for (std::size_t idx = 0; idx < kDynamicTableWorkloadSize; ++idx) {
    workload.names[idx] = "x-hpack-" + std::to_string(idx);
    // Alternate realistic short and long values. The varying entry sizes exercise single and multi-entry eviction.
    const std::size_t valueSize = idx % 8U == 0U ? 128U : (idx % 3U == 0U ? 64U : 24U);
    workload.values[idx].assign(valueSize, static_cast<char>('a' + (idx % 26U)));
  }
  return workload;
}

const auto kDynamicTableWorkload = BuildDynamicTableWorkload();

// Candidate contiguous circular table. Slots are reused after eviction and logical HPACK indices are translated to
// ring positions, so neither insertion nor eviction relocates live Header handles once the table reaches steady state.
class CircularDynamicTable {
  using size_type = vector<http::Header>::size_type;

 public:
  explicit CircularDynamicTable(std::size_t maxSizeBytes) noexcept : _maxSizeBytes(maxSizeBytes) {}

  bool add(std::string_view name, std::string_view value) {
    const std::size_t entrySize = name.size() + value.size() + kDynamicTableEntryOverhead;
    if (_maxSizeBytes < entrySize) {
      clear();
      return false;
    }

    while (_maxSizeBytes < _currentSizeBytes + entrySize) {
      evict();
    }

    if (_entryCount == _entries.size()) {
      if (_firstEntry != 0U) {
        vector<http::Header> linearized;
        linearized.reserve(_entries.size() + 1U);
        for (size_type idx = 0; idx < _entryCount; ++idx) {
          linearized.push_back(std::move(_entries[physicalIndex(idx)]));
        }
        _entries = std::move(linearized);
        _firstEntry = 0U;
      }
      _entries.push_back(http::Header(name, value));
    } else {
      const auto insertIdx = physicalIndex(_entryCount);
      _entries[insertIdx] = http::Header(std::move(_entries[insertIdx]), name, value);
    }

    ++_entryCount;
    _currentSizeBytes += entrySize;
    return true;
  }

  [[nodiscard]] const http::Header& operator[](uint32_t index) const noexcept {
    return _entries[physicalIndex(_entryCount - 1U - index)];
  }

  [[nodiscard]] std::size_t entryCount() const noexcept { return _entryCount; }

 private:
  [[nodiscard]] size_type physicalIndex(size_type logicalIndex) const noexcept {
    const auto index = _firstEntry + logicalIndex;
    return index < _entries.size() ? index : index - _entries.size();
  }

  void evict() {
    const auto entrySize = _entries[_firstEntry].size() + kDynamicTableEntryOverhead;
    _currentSizeBytes -= entrySize;
    _firstEntry = _firstEntry + 1U == _entries.size() ? 0U : _firstEntry + 1U;
    --_entryCount;
  }

  void clear() noexcept {
    _entries.clear();
    _currentSizeBytes = 0U;
    _firstEntry = 0U;
    _entryCount = 0U;
  }

  vector<http::Header> _entries;
  std::size_t _currentSizeBytes{0U};
  std::size_t _maxSizeBytes;
  size_type _firstEntry{0U};
  size_type _entryCount{0U};
};

// Segmented-queue candidate. This deliberately keeps deque's native block layout in the comparison.
class DequeDynamicTable {
 public:
  explicit DequeDynamicTable(std::size_t maxSizeBytes) noexcept : _maxSizeBytes(maxSizeBytes) {}

  bool add(std::string_view name, std::string_view value) {
    const std::size_t entrySize = name.size() + value.size() + kDynamicTableEntryOverhead;
    if (_maxSizeBytes < entrySize) {
      clear();
      return false;
    }

    http::Header reusable;
    while (_maxSizeBytes < _currentSizeBytes + entrySize) {
      auto evicted = std::move(_entries.back());
      _entries.pop_back();
      _currentSizeBytes -= evicted.size() + kDynamicTableEntryOverhead;
      if (reusable.empty()) {
        reusable = std::move(evicted);
      }
    }

    if (reusable.empty()) {
      _entries.emplace_front(name, value);
    } else {
      _entries.emplace_front(std::move(reusable), name, value);
    }
    _currentSizeBytes += entrySize;
    return true;
  }

  [[nodiscard]] const http::Header& operator[](std::size_t index) const noexcept { return _entries[index]; }

  [[nodiscard]] std::size_t entryCount() const noexcept { return _entries.size(); }

 private:
  void clear() noexcept {
    _entries.clear();
    _currentSizeBytes = 0U;
  }

  std::deque<http::Header> _entries;
  std::size_t _currentSizeBytes{0U};
  std::size_t _maxSizeBytes;
};

template <typename Table>
void PopulateDynamicTable(Table& table) {
  for (std::size_t idx = 0; idx < kDynamicTableWorkloadSize; ++idx) {
    table.add(kDynamicTableWorkload.names[idx], kDynamicTableWorkload.values[idx]);
  }
}

template <typename Table>
uint32_t FindDynamicHeader(const Table& table, std::string_view name, std::string_view value) {
  for (uint32_t idx = 0; idx < table.entryCount(); ++idx) {
    const auto& entry = table[idx];
    if (entry.name() == name && entry.value() == value) {
      return idx + 1U;
    }
  }
  return 0U;
}

// Optional encoder-side index candidate. It stores hashes and insertion serials rather than string_views, so table
// eviction and slot reuse cannot leave dangling keys. Hash collisions are detected against the live Header and fall
// back to a linear lookup, preserving correctness. Periodic rebuilding bounds stale hashes left by FIFO eviction.
class IndexedDynamicTable {
 public:
  explicit IndexedDynamicTable(uint32_t maxSizeBytes) : _table(maxSizeBytes) {}

  bool add(std::string_view name, std::string_view value) {
    if (!_table.add(name, value)) {
      if (_table.entryCount() == 0U) {
        _index.clear();
      }
      return false;
    }

    ++_newestSerial;
    index(name, value, _newestSerial);
    if (_table.entryCount() * 4U + 32U < _index.size()) {
      rebuild();
    }
    return true;
  }

  [[nodiscard]] const http::Header& operator[](std::size_t index) const noexcept {
    return _table[static_cast<uint32_t>(index)];
  }

  [[nodiscard]] std::size_t entryCount() const noexcept { return _table.entryCount(); }

  [[nodiscard]] uint32_t find(std::string_view name, std::string_view value) const {
    const auto nameHash = CityHash{}(name);
    if (const auto iter = _index.find(FullHash(nameHash, value)); iter != _index.end()) {
      const auto index = liveIndex(iter->second);
      if (index < _table.entryCount()) {
        const auto& entry = _table[static_cast<uint32_t>(index)];
        if (entry.name() == name && entry.value() == value) {
          return static_cast<uint32_t>(index + 1U);
        }
      }
      // The slot can only disagree for a hash collision or a stale serial. Preserve correctness for collisions.
      return FindDynamicHeader(_table, name, value);
    }

    if (const auto iter = _index.find(NameHash(nameHash)); iter != _index.end()) {
      const auto index = liveIndex(iter->second);
      if (index < _table.entryCount() && _table[static_cast<uint32_t>(index)].name() == name) {
        return static_cast<uint32_t>(index + 1U);
      }
      return FindDynamicHeader(_table, name, value);
    }
    return 0U;
  }

 private:
  [[nodiscard]] static std::size_t FullHash(std::size_t nameHash, std::string_view value) {
    const auto valueHash = CityHash{}(value);
    return nameHash ^
           (valueHash + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (nameHash << 6U) + (nameHash >> 2U));
  }

  [[nodiscard]] static std::size_t NameHash(std::size_t nameHash) noexcept {
    return nameHash ^ static_cast<std::size_t>(0xd6e8feb86659fd93ULL);
  }

  [[nodiscard]] std::size_t liveIndex(uint64_t serial) const noexcept {
    return serial <= _newestSerial ? static_cast<std::size_t>(_newestSerial - serial) : _table.entryCount();
  }

  void index(std::string_view name, std::string_view value, uint64_t serial) {
    const auto nameHash = CityHash{}(name);
    _index[NameHash(nameHash)] = serial;
    _index[FullHash(nameHash, value)] = serial;
  }

  void rebuild() {
    _index.clear();
    _index.reserve(_table.entryCount() * 2U);
    for (std::size_t idx = _table.entryCount(); idx != 0U; --idx) {
      const auto tableIdx = idx - 1U;
      const auto& entry = _table[static_cast<uint32_t>(tableIdx)];
      index(entry.name(), entry.value(), _newestSerial - tableIdx);
    }
  }

  HpackDynamicTable _table;
  flat_hash_map<std::size_t, uint64_t> _index;
  uint64_t _newestSerial{0U};
};

// Pre-encoded blocks (built once, reused across iterations)
const auto kSmallBlock = EncodeHeaderBlock(kSmallHeaders);
const auto kMediumBlock = EncodeHeaderBlock(kMediumHeaders);
const auto kLargeBlock = EncodeHeaderBlock(kLargeHeaders);

std::span<const std::byte> AsBytes(const RawBytes& rb) { return {rb.begin(), rb.size()}; }

// ---------------------------------------------------------------------------
// Decode benchmarks
// ---------------------------------------------------------------------------

namespace {

HpackDecoder CreateHpackDecoder() { return {4096, std::numeric_limits<uint32_t>::max(), true}; }

}  // namespace

void BM_HpackDecodeSmall(benchmark::State& state) {
  auto block = AsBytes(kSmallBlock);
  for ([[maybe_unused]] auto iter : state) {
    auto decoder = CreateHpackDecoder();
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeSmall);

void BM_HpackDecodeMedium(benchmark::State& state) {
  auto block = AsBytes(kMediumBlock);
  for ([[maybe_unused]] auto iter : state) {
    auto decoder = CreateHpackDecoder();
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeMedium);

void BM_HpackDecodeLarge(benchmark::State& state) {
  auto block = AsBytes(kLargeBlock);
  for ([[maybe_unused]] auto iter : state) {
    auto decoder = CreateHpackDecoder();
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeLarge);

// Stateful decode: decoder persists across iterations (dynamic table builds up)
void BM_HpackDecodeSmallStateful(benchmark::State& state) {
  auto block = AsBytes(kSmallBlock);
  auto decoder = CreateHpackDecoder();
  for ([[maybe_unused]] auto iter : state) {
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeSmallStateful);

void BM_HpackDecodeMediumStateful(benchmark::State& state) {
  auto block = AsBytes(kMediumBlock);
  auto decoder = CreateHpackDecoder();
  for ([[maybe_unused]] auto iter : state) {
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeMediumStateful);

void BM_HpackDecodeLargeStateful(benchmark::State& state) {
  auto block = AsBytes(kLargeBlock);
  auto decoder = CreateHpackDecoder();
  for ([[maybe_unused]] auto iter : state) {
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeLargeStateful);

// ---------------------------------------------------------------------------
// Encode benchmarks
// ---------------------------------------------------------------------------

void BM_HpackEncodeSmall(benchmark::State& state) {
  for ([[maybe_unused]] auto iter : state) {
    HpackEncoder encoder;
    RawBytes out;
    for (const auto& hv : kSmallHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeSmall);

void BM_HpackEncodeMedium(benchmark::State& state) {
  for ([[maybe_unused]] auto iter : state) {
    HpackEncoder encoder;
    RawBytes out;
    for (const auto& hv : kMediumHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeMedium);

void BM_HpackEncodeLarge(benchmark::State& state) {
  for ([[maybe_unused]] auto iter : state) {
    HpackEncoder encoder;
    RawBytes out;
    for (const auto& hv : kLargeHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeLarge);

// Stateful encode: encoder persists across iterations (dynamic table builds up)
void BM_HpackEncodeSmallStateful(benchmark::State& state) {
  HpackEncoder encoder;
  for ([[maybe_unused]] auto iter : state) {
    RawBytes out;
    for (const auto& hv : kSmallHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeSmallStateful);

void BM_HpackEncodeMediumStateful(benchmark::State& state) {
  HpackEncoder encoder;
  for ([[maybe_unused]] auto iter : state) {
    RawBytes out;
    for (const auto& hv : kMediumHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeMediumStateful);

void BM_HpackEncodeLargeStateful(benchmark::State& state) {
  HpackEncoder encoder;
  for ([[maybe_unused]] auto iter : state) {
    RawBytes out;
    for (const auto& hv : kLargeHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeLargeStateful);

// Stateful encode of successive responses: fields that repeat (content-type, server, the global headers...) and fields
// whose value differs in each of them (content-length, etag, last-modified).
// Arg 0: all fields indexed, each new value being inserted in the dynamic table (and evicting the oldest entries).
// Arg 1: indexing mode of each field from HpackEncoder::IndexingModeOf().
void BM_HpackEncodeResponsesWithVaryingFields(benchmark::State& state) {
  const bool perFieldIndexingMode = state.range(0) != 0;
  static constexpr std::size_t kNbResponses = 1024;
  std::deque<std::string> values;  // stable addresses
  std::array<std::array<http::HeaderView, 9>, kNbResponses> responses;
  for (std::size_t responseIdx = 0; responseIdx < kNbResponses; ++responseIdx) {
    const std::string_view length = values.emplace_back(std::to_string(1000 + (responseIdx * 37U)));
    const std::string_view etag = values.emplace_back("\"" + std::to_string(responseIdx * 7919U) + "-5f3c9a2b\"");
    const std::string_view lastModified =
        values.emplace_back("Mon, 1" + std::to_string(responseIdx % 10U) + " Oct 2026 08:4" +
                            std::to_string(responseIdx % 6U) + ":" + std::to_string(10 + (responseIdx % 50U)) + " GMT");
    responses[responseIdx] = {{
        {":status", "200"},
        {"content-type", "application/json"},
        {"content-length", length},
        {"etag", etag},
        {"last-modified", lastModified},
        {"cache-control", "public, max-age=3600"},
        {"server", "aeronet"},
        {"x-frame-options", "DENY"},
        {"strict-transport-security", "max-age=31536000; includeSubDomains"},
    }};
  }

  HpackEncoder encoder;
  RawBytes out;
  std::size_t responseIdx = 0;
  std::size_t totalBytes = 0;
  for ([[maybe_unused]] auto iter : state) {
    out.clear();
    for (const auto& [name, value] : responses[responseIdx++ % kNbResponses]) {
      encoder.encode(
          out, name, value,
          perFieldIndexingMode ? HpackEncoder::IndexingModeOf(name, value) : HpackEncoder::IndexingMode::Indexed);
    }
    totalBytes += out.size();
    benchmark::DoNotOptimize(out.data());
  }
  state.counters["bytes_per_response"] =
      benchmark::Counter(static_cast<double>(totalBytes), benchmark::Counter::kAvgIterations);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HpackEncodeResponsesWithVaryingFields)->Arg(0)->Arg(1);

// Same responses with a date field, encoded with the indexing mode of HpackEncoder::IndexingModeOf(), by an encoder
// whose dynamic table holds the date values of the previous seconds: the steady state of a long-lived connection, as
// date is indexed and changes each second (4096 bytes hold about 60 of them).
// Arg: number of seconds since the fields that repeat were last added to the table, that is the number of date values
// to go through before finding them (they are added again once evicted, about once a minute).
void BM_HpackEncodeResponsesFullDynamicTable(benchmark::State& state) {
  static constexpr std::size_t kNbResponses = 1024;
  static constexpr int kNbFillerDates = 100;
  const int nbNewerDates = static_cast<int>(state.range(0));
  const auto dateOf = [](int second) {
    std::string date = "Mon, 12 Oct 2026 08:10:00 GMT";
    date[20] = static_cast<char>('1' + (second / 60));
    date[23] = static_cast<char>('0' + ((second % 60) / 10));
    date[24] = static_cast<char>('0' + (second % 10));
    return date;
  };
  // The date of the responses is the last one added to the table.
  const std::string currentDate = dateOf(kNbFillerDates + nbNewerDates - 1);

  std::deque<std::string> values;  // stable addresses
  std::array<std::array<http::HeaderView, 10>, kNbResponses> responses;
  for (std::size_t responseIdx = 0; responseIdx < kNbResponses; ++responseIdx) {
    const std::string_view length = values.emplace_back(std::to_string(1000 + (responseIdx * 37U)));
    const std::string_view etag = values.emplace_back("\"" + std::to_string(responseIdx * 7919U) + "-5f3c9a2b\"");
    responses[responseIdx] = {{
        {":status", "200"},
        {"date", currentDate},
        {"content-type", "application/json"},
        {"content-length", length},
        {"etag", etag},
        {"last-modified", "Mon, 12 Oct 2026 07:00:00 GMT"},
        {"cache-control", "public, max-age=3600"},
        {"server", "aeronet"},
        {"x-frame-options", "DENY"},
        {"strict-transport-security", "max-age=31536000; includeSubDomains"},
    }};
  }

  HpackEncoder encoder;
  RawBytes out;
  int second = 0;
  for (; second < kNbFillerDates; ++second) {
    encoder.encode(out, "date", dateOf(second));
  }
  for (const auto& [name, value] : responses[0]) {
    if (name != "date") {
      encoder.encode(out, name, value, HpackEncoder::IndexingModeOf(name, value));
    }
  }
  for (; second < kNbFillerDates + nbNewerDates; ++second) {
    encoder.encode(out, "date", dateOf(second));
  }
  state.counters["dyn_entries"] = static_cast<double>(encoder.dynamicTable().entryCount());

  std::size_t responseIdx = 0;
  std::size_t totalBytes = 0;
  for ([[maybe_unused]] auto iter : state) {
    out.clear();
    for (const auto& [name, value] : responses[responseIdx++ % kNbResponses]) {
      encoder.encode(out, name, value, HpackEncoder::IndexingModeOf(name, value));
    }
    totalBytes += out.size();
    benchmark::DoNotOptimize(out.data());
  }
  state.counters["bytes_per_response"] =
      benchmark::Counter(static_cast<double>(totalBytes), benchmark::Counter::kAvgIterations);
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HpackEncodeResponsesFullDynamicTable)->Arg(0)->Arg(25)->Arg(50)->ArgName("newer_dates");

// Stateful encode of successive responses carrying many indexed fields whose value differs in each of them, as the
// 'headers' scenario of the scripted-servers benchmarks (/headers?count=50&size=128), or a request identifier
// (1 field of 36 bytes): the dynamic table only holds the last fields added, so that each field of a response is looked
// up in vain in the whole table, then added to it.
// Args: number of such fields per response, size of their values.
void BM_HpackEncodeResponsesOverflowingDynamicTable(benchmark::State& state) {
  const auto nbFields = static_cast<std::size_t>(state.range(0));
  const auto valueSize = static_cast<std::size_t>(state.range(1));
  // Enough values for the fields of many responses: the table never holds a value again when it is encoded.
  static constexpr std::size_t kNbValues = 4096;

  std::deque<std::string> names;
  for (std::size_t fieldIdx = 0; fieldIdx < nbFields; ++fieldIdx) {
    names.push_back("x-bench-header-" + std::to_string(fieldIdx));
  }
  static constexpr std::string_view kAlphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  std::string values(kNbValues * valueSize, '\0');
  uint64_t rng = 0x9E3779B97F4A7C15ULL;
  for (char& ch : values) {
    rng ^= rng << 13U;
    rng ^= rng >> 7U;
    rng ^= rng << 17U;
    ch = kAlphabet[rng % kAlphabet.size()];
  }

  HpackEncoder encoder;
  RawBytes out;
  std::size_t valueIdx = 0;
  const auto encodeField = [&](std::string_view name, std::string_view value) {
    encoder.encode(out, name, value, HpackEncoder::IndexingModeOf(name, value));
  };
  for ([[maybe_unused]] auto iter : state) {
    out.clear();
    encodeField(":status", "200");
    encodeField("date", "Sat, 14 Nov 2026 18:28:42 GMT");
    for (const std::string& name : names) {
      encodeField(name, std::string_view(values.data() + (valueIdx * valueSize), valueSize));
      valueIdx = (valueIdx + 1U) % kNbValues;
    }
    encodeField("content-type", "text/plain");
    encodeField("content-length", "19");
    benchmark::DoNotOptimize(out.data());
  }
  state.counters["dyn_entries"] = static_cast<double>(encoder.dynamicTable().entryCount());
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HpackEncodeResponsesOverflowingDynamicTable)
    ->Args({1, 36})
    ->ArgsProduct({{10, 50}, {16, 128}})
    ->ArgNames({"fields", "value_size"});

// ---------------------------------------------------------------------------
// String literals: Huffman coding throughput, against raw strings
// ---------------------------------------------------------------------------

namespace {

// 'size' bytes of header text (letters, digits and punctuation, as in a user-agent or a cookie).
std::string HeaderText(std::size_t size) {
  static constexpr std::string_view kText =
      "Mozilla/5.0 (X11; Linux x86_64; rv:131.0) Gecko/20100101 Firefox/131.0; session=7f3a9c2e5b1d4f6a8e0c; "
      "theme=dark; lang=en-US,en;q=0.5 ";
  std::string text;
  while (text.size() < size) {
    text.append(kText.substr(0, size - text.size()));
  }
  return text;
}

// A literal field without indexing, with a new name: its value as a Huffman string (what HpackEncoder emits for text)
// or as a raw one.
RawBytes EncodeLiteralField(std::string_view name, std::string_view value, bool huffman) {
  RawBytes block;
  if (huffman) {
    HpackEncoder encoder;
    encoder.encode(block, name, value, HpackEncoder::IndexingMode::WithoutIndexing);
    return block;
  }
  const auto appendRawString = [&block](std::string_view str) {
    // Integer with a 7-bit prefix (RFC 7541 §5.1), the Huffman bit unset.
    std::size_t length = str.size();
    if (length < 127U) {
      block.push_back(static_cast<std::byte>(length));
    } else {
      block.push_back(std::byte{127});
      for (length -= 127U; length >= 128U; length >>= 7U) {
        block.push_back(static_cast<std::byte>((length & 0x7FU) | 0x80U));
      }
      block.push_back(static_cast<std::byte>(length));
    }
    block.append(reinterpret_cast<const std::byte*>(str.data()), str.size());
  };
  block.push_back(std::byte{0});  // literal without indexing, new name
  appendRawString(name);
  appendRawString(value);
  return block;
}

}  // namespace

// Encode a field whose value is 'size' bytes of text, not indexed: the length of the Huffman string, then its bits.
void BM_HpackHuffmanEncode(benchmark::State& state) {
  const std::string value = HeaderText(static_cast<std::size_t>(state.range(0)));
  HpackEncoder encoder;
  RawBytes out;
  for ([[maybe_unused]] auto iter : state) {
    out.clear();
    encoder.encode(out, "x-bench", value, HpackEncoder::IndexingMode::WithoutIndexing);
    benchmark::DoNotOptimize(out.data());
  }
  state.counters["encoded_size"] = static_cast<double>(out.size());
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(value.size()));
}
BENCHMARK(BM_HpackHuffmanEncode)->Arg(16)->Arg(64)->Arg(256)->Arg(4096);

// Decode a field whose value is 'size' bytes of text, as a Huffman string or as a raw one: the difference is the cost
// of the Huffman decoding, the rest (validation, storage, header map) being common.
void BM_HpackDecodeLiteral(benchmark::State& state) {
  const std::string value = HeaderText(static_cast<std::size_t>(state.range(0)));
  const RawBytes block = EncodeLiteralField("x-bench", value, state.range(1) != 0);
  auto decoder = CreateHpackDecoder();
  for ([[maybe_unused]] auto iter : state) {
    auto result = decoder.decode(AsBytes(block));
    benchmark::DoNotOptimize(result);
  }
  if (decoder.decode(AsBytes(block)).decodedHeaders.headers.find("x-bench")->second != value) {
    state.SkipWithError("the decoded value differs");
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(value.size()));
}
BENCHMARK(BM_HpackDecodeLiteral)->ArgsProduct({{16, 64, 256, 4096}, {0, 1}})->ArgNames({"size", "huffman"});

namespace {

// Number of values of MixedLengthValues(), a power of two: too many for the pattern of their lengths to be learnt.
constexpr std::size_t kNbMixedLengthValues = 16384;

// Header text of the lengths found in real header blocks, in random order: of the Huffman strings of the requests and
// responses of github.com/http2jp/hpack-test-case, a third are at most 8 bytes long, half at most 16 and nine tenths
// at most 32. The fixed sizes above hide what a string costs once decoded or encoded: the branch misprediction at its
// end, which depends on its length.
const std::deque<std::string>& MixedLengthValues() {
  static const std::deque<std::string> kValues = [] {
    const std::string text = HeaderText(4096);
    std::mt19937 rng(7541);  // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::deque<std::string> values;
    for (std::size_t idx = 0; idx < kNbMixedLengthValues; ++idx) {
      const uint32_t pick = rng() % 100U;
      std::size_t size;
      if (pick < 33U) {
        size = 1U + (rng() % 10U);
      } else if (pick < 50U) {
        size = 11U + (rng() % 10U);
      } else if (pick < 90U) {
        size = 21U + (rng() % 20U);
      } else if (pick < 97U) {
        size = 41U + (rng() % 40U);
      } else {
        size = 81U + (rng() % 120U);
      }
      std::string& value = values.emplace_back(text.substr(rng() % (text.size() - size), size));
      // A field value neither starts nor ends with a space.
      if (value.front() == ' ') {
        value.front() = 'a';
      }
      if (value.back() == ' ') {
        value.back() = 'a';
      }
    }
    return values;
  }();
  return kValues;
}

}  // namespace

// Encode fields whose values are header text of mixed lengths, not indexed.
void BM_HpackHuffmanEncodeMixedLengths(benchmark::State& state) {
  const std::deque<std::string>& values = MixedLengthValues();
  HpackEncoder encoder;
  RawBytes out;
  std::size_t idx = 0;
  int64_t nbBytes = 0;
  for ([[maybe_unused]] auto iter : state) {
    const std::string& value = values[idx++ % kNbMixedLengthValues];
    out.clear();
    encoder.encode(out, "x-bench", value, HpackEncoder::IndexingMode::WithoutIndexing);
    benchmark::DoNotOptimize(out.data());
    nbBytes += static_cast<int64_t>(value.size());
  }
  state.SetBytesProcessed(nbBytes);
}
BENCHMARK(BM_HpackHuffmanEncodeMixedLengths);

// Decode fields whose values are header text of mixed lengths, as Huffman strings or as raw ones (see
// BM_HpackDecodeLiteral).
void BM_HpackDecodeLiteralMixedLengths(benchmark::State& state) {
  const std::deque<std::string>& values = MixedLengthValues();
  // The blocks one after the other, as the fields of header blocks are.
  RawBytes blocks;
  std::array<uint32_t, kNbMixedLengthValues + 1U> offsets;
  for (std::size_t idx = 0; idx < kNbMixedLengthValues; ++idx) {
    offsets[idx] = static_cast<uint32_t>(blocks.size());
    const RawBytes block = EncodeLiteralField("x-bench", values[idx], state.range(0) != 0);
    blocks.append(block.data(), block.size());
  }
  offsets[kNbMixedLengthValues] = static_cast<uint32_t>(blocks.size());

  auto decoder = CreateHpackDecoder();
  std::size_t idx = 0;
  int64_t nbBytes = 0;
  for ([[maybe_unused]] auto iter : state) {
    const std::size_t valueIdx = idx++ % kNbMixedLengthValues;
    auto result =
        decoder.decode(AsBytes(blocks).subspan(offsets[valueIdx], offsets[valueIdx + 1U] - offsets[valueIdx]));
    benchmark::DoNotOptimize(result);
    nbBytes += static_cast<int64_t>(values[valueIdx].size());
  }
  state.SetBytesProcessed(nbBytes);
}
BENCHMARK(BM_HpackDecodeLiteralMixedLengths)->Arg(0)->Arg(1)->ArgName("huffman");

// ---------------------------------------------------------------------------
// findHeader benchmark (the identified bottleneck)
// ---------------------------------------------------------------------------

// Parameterized: dynamic table size 0, 10, 50, 100
void BM_HpackFindHeader(benchmark::State& state) {
  const int dynTableEntries = static_cast<int>(state.range(0));

  HpackEncoder encoder;
  // Populate dynamic table
  RawBytes dummy;
  for (int ii = 0; ii < dynTableEntries; ++ii) {
    std::string nm = "x-dyn-" + std::to_string(ii);
    std::string vl = "value-" + std::to_string(ii);
    encoder.encode(dummy, nm, vl);
  }

  // Search for headers that are: in static table, in dynamic table, and not found
  static constexpr std::array<http::HeaderView, 4> kQueries{{
      {":method", "GET"},                        // static full match
      {"content-type", "application/json"},      // static name-only
      {"x-dyn-0", "value-0"},                    // dynamic full match (if populated)
      {"x-not-found", "no-match-anywhere-val"},  // miss
  }};

  std::size_t qi = 0;
  for ([[maybe_unused]] auto iter : state) {
    const auto& qh = kQueries[qi % kQueries.size()];
    auto result = encoder.findHeader(qh.name, qh.value);
    benchmark::DoNotOptimize(result);
    ++qi;
  }
  state.counters["dyn_entries"] = static_cast<double>(dynTableEntries);
}
BENCHMARK(BM_HpackFindHeader)->Arg(0)->Arg(10)->Arg(50)->Arg(100);

// Production lookup policy across byte-sized tables. At 4 KiB this exercises the selected contiguous linear scan;
// enlarged tables cross the lazy encoder-index threshold and exercise the selected hash/serial lookup.
void BM_HpackFindHeaderByTableSize(benchmark::State& state) {
  HpackEncoder encoder(static_cast<uint32_t>(state.range(0)));
  RawBytes dummy;
  for (std::size_t idx = 0; idx < kDynamicTableWorkloadSize; ++idx) {
    encoder.encode(dummy, kDynamicTableWorkload.names[idx], kDynamicTableWorkload.values[idx]);
  }

  const auto& table = encoder.dynamicTable();
  const auto middleIdx = table.entryCount() / 2U;
  const auto oldestIdx = table.entryCount() - 1U;
  const std::array<std::pair<std::string, std::string>, 4> queries{{
      {std::string(table[0U].name()), std::string(table[0U].value())},
      {std::string(table[static_cast<uint32_t>(middleIdx)].name()),
       std::string(table[static_cast<uint32_t>(middleIdx)].value())},
      {std::string(table[static_cast<uint32_t>(oldestIdx)].name()),
       std::string(table[static_cast<uint32_t>(oldestIdx)].value())},
      {"x-hpack-not-present", "not-present"},
  }};

  std::size_t idx = 0U;
  for ([[maybe_unused]] auto iter : state) {
    const auto& [name, value] = queries[idx % queries.size()];
    benchmark::DoNotOptimize(encoder.findHeader(name, value));
    ++idx;
  }
  state.counters["dyn_entries"] = static_cast<double>(table.entryCount());
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_HpackFindHeaderByTableSize)->Arg(4096)->Arg(16384)->Arg(65536);

// ---------------------------------------------------------------------------
// Encode-decode round-trip
// ---------------------------------------------------------------------------

void BM_HpackRoundTrip(benchmark::State& state) {
  const auto headerCount = static_cast<std::size_t>(state.range(0));
  const auto headers =
      headerCount <= kSmallHeaders.size()    ? std::span<const http::HeaderView>(kSmallHeaders.data(), headerCount)
      : headerCount <= kMediumHeaders.size() ? std::span<const http::HeaderView>(kMediumHeaders.data(), headerCount)
                                             : std::span<const http::HeaderView>(kLargeHeaders.data(), headerCount);

  for ([[maybe_unused]] auto iter : state) {
    HpackEncoder encoder;
    RawBytes encoded;
    for (const auto& hv : headers) {
      encoder.encode(encoded, hv.name, hv.value);
    }
    auto decoder = CreateHpackDecoder();
    auto result = decoder.decode(AsBytes(encoded));
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackRoundTrip)->Arg(5)->Arg(20)->Arg(50);

// ---------------------------------------------------------------------------
// Dynamic-table container comparison
// ---------------------------------------------------------------------------

template <typename Table>
void BM_HpackDynamicTableChurn(benchmark::State& state) {
  Table table(static_cast<uint32_t>(state.range(0)));
  PopulateDynamicTable(table);
  std::size_t idx = 0U;
  for ([[maybe_unused]] auto iter : state) {
    const auto workloadIdx = idx % kDynamicTableWorkloadSize;
    benchmark::DoNotOptimize(
        table.add(kDynamicTableWorkload.names[workloadIdx], kDynamicTableWorkload.values[workloadIdx]));
    ++idx;
  }
  state.SetItemsProcessed(state.iterations());
}

template <typename Table>
void BM_HpackDynamicTableIndexedAccess(benchmark::State& state) {
  Table table(static_cast<uint32_t>(state.range(0)));
  PopulateDynamicTable(table);
  std::size_t idx = 0U;
  for ([[maybe_unused]] auto iter : state) {
    const auto tableIdx = (idx * 37U) % table.entryCount();
    benchmark::DoNotOptimize(table[static_cast<uint32_t>(tableIdx)].name());
    ++idx;
  }
  state.SetItemsProcessed(state.iterations());
}

template <typename Table>
void BM_HpackDynamicTableLinearLookup(benchmark::State& state) {
  Table table(static_cast<uint32_t>(state.range(0)));
  PopulateDynamicTable(table);
  const auto middleIdx = table.entryCount() / 2U;
  const auto oldestIdx = table.entryCount() - 1U;
  const std::array<std::pair<std::string, std::string>, 4> queries{{
      {std::string(table[0U].name()), std::string(table[0U].value())},
      {std::string(table[static_cast<uint32_t>(middleIdx)].name()),
       std::string(table[static_cast<uint32_t>(middleIdx)].value())},
      {std::string(table[static_cast<uint32_t>(oldestIdx)].name()),
       std::string(table[static_cast<uint32_t>(oldestIdx)].value())},
      {"x-hpack-not-present", "not-present"},
  }};

  std::size_t idx = 0U;
  for ([[maybe_unused]] auto iter : state) {
    const auto& [name, value] = queries[idx % queries.size()];
    benchmark::DoNotOptimize(FindDynamicHeader(table, name, value));
    ++idx;
  }
  state.SetItemsProcessed(state.iterations());
}

template <typename Table>
void BM_HpackDynamicTableCombined(benchmark::State& state) {
  Table table(static_cast<uint32_t>(state.range(0)));
  PopulateDynamicTable(table);
  std::size_t idx = 0U;
  for ([[maybe_unused]] auto iter : state) {
    const auto workloadIdx = idx % kDynamicTableWorkloadSize;
    table.add(kDynamicTableWorkload.names[workloadIdx], kDynamicTableWorkload.values[workloadIdx]);

    const auto middleIdx = table.entryCount() / 2U;
    const auto oldestIdx = table.entryCount() - 1U;
    const auto& newest = table[0U];
    const auto& middle = table[static_cast<uint32_t>(middleIdx)];
    benchmark::DoNotOptimize(table[static_cast<uint32_t>(oldestIdx)].name());
    benchmark::DoNotOptimize(FindDynamicHeader(table, newest.name(), newest.value()));
    benchmark::DoNotOptimize(FindDynamicHeader(table, middle.name(), middle.value()));
    benchmark::DoNotOptimize(FindDynamicHeader(table, "x-hpack-not-present", "not-present"));
    ++idx;
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_HpackDynamicTableIndexedLookup(benchmark::State& state) {
  IndexedDynamicTable table(static_cast<uint32_t>(state.range(0)));
  PopulateDynamicTable(table);
  const auto middleIdx = table.entryCount() / 2U;
  const auto oldestIdx = table.entryCount() - 1U;
  const std::array<std::pair<std::string, std::string>, 4> queries{{
      {std::string(table[0U].name()), std::string(table[0U].value())},
      {std::string(table[middleIdx].name()), std::string(table[middleIdx].value())},
      {std::string(table[oldestIdx].name()), std::string(table[oldestIdx].value())},
      {"x-hpack-not-present", "not-present"},
  }};

  std::size_t idx = 0U;
  for ([[maybe_unused]] auto iter : state) {
    const auto& [name, value] = queries[idx % queries.size()];
    benchmark::DoNotOptimize(table.find(name, value));
    ++idx;
  }
  state.SetItemsProcessed(state.iterations());
}

void BM_HpackDynamicTableIndexedCombined(benchmark::State& state) {
  IndexedDynamicTable table(static_cast<uint32_t>(state.range(0)));
  PopulateDynamicTable(table);
  std::size_t idx = 0U;
  for ([[maybe_unused]] auto iter : state) {
    const auto workloadIdx = idx % kDynamicTableWorkloadSize;
    table.add(kDynamicTableWorkload.names[workloadIdx], kDynamicTableWorkload.values[workloadIdx]);

    const auto middleIdx = table.entryCount() / 2U;
    const auto oldestIdx = table.entryCount() - 1U;
    const auto& newest = table[0U];
    const auto& middle = table[middleIdx];
    benchmark::DoNotOptimize(table[oldestIdx].name());
    benchmark::DoNotOptimize(table.find(newest.name(), newest.value()));
    benchmark::DoNotOptimize(table.find(middle.name(), middle.value()));
    benchmark::DoNotOptimize(table.find("x-hpack-not-present", "not-present"));
    ++idx;
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK_TEMPLATE(BM_HpackDynamicTableChurn, HpackDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);
BENCHMARK_TEMPLATE(BM_HpackDynamicTableChurn, CircularDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);
BENCHMARK_TEMPLATE(BM_HpackDynamicTableChurn, DequeDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);

BENCHMARK_TEMPLATE(BM_HpackDynamicTableIndexedAccess, HpackDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);
BENCHMARK_TEMPLATE(BM_HpackDynamicTableIndexedAccess, CircularDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);
BENCHMARK_TEMPLATE(BM_HpackDynamicTableIndexedAccess, DequeDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);

BENCHMARK_TEMPLATE(BM_HpackDynamicTableLinearLookup, HpackDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);
BENCHMARK_TEMPLATE(BM_HpackDynamicTableLinearLookup, CircularDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);
BENCHMARK_TEMPLATE(BM_HpackDynamicTableLinearLookup, DequeDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);

BENCHMARK_TEMPLATE(BM_HpackDynamicTableCombined, HpackDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);
BENCHMARK_TEMPLATE(BM_HpackDynamicTableCombined, CircularDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);
BENCHMARK_TEMPLATE(BM_HpackDynamicTableCombined, DequeDynamicTable)->Arg(4096)->Arg(16384)->Arg(65536);

BENCHMARK(BM_HpackDynamicTableIndexedLookup)->Arg(4096)->Arg(16384)->Arg(65536);
BENCHMARK(BM_HpackDynamicTableIndexedCombined)->Arg(4096)->Arg(16384)->Arg(65536);

}  // namespace
}  // namespace aeronet::http2

BENCHMARK_MAIN();
