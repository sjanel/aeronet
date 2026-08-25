// HPACK encoder/decoder micro-benchmarks.
// Measures hot paths: decode, encode, findHeader, Huffman, dynamic table ops.
#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <deque>
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
        for (std::size_t idx = 0; idx < _entryCount; ++idx) {
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

  [[nodiscard]] const http::Header& operator[](std::size_t index) const noexcept {
    return _entries[physicalIndex(_entryCount - 1U - index)];
  }

  [[nodiscard]] std::size_t entryCount() const noexcept { return _entryCount; }

 private:
  [[nodiscard]] std::size_t physicalIndex(std::size_t logicalIndex) const noexcept {
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
  std::size_t _firstEntry{0U};
  std::size_t _entryCount{0U};
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
  for (std::size_t idx = 0; idx < table.entryCount(); ++idx) {
    const auto& entry = table[idx];
    if (entry.name() == name && entry.value() == value) {
      return static_cast<uint32_t>(idx + 1U);
    }
  }
  return 0U;
}

// Optional encoder-side index candidate. It stores hashes and insertion serials rather than string_views, so table
// eviction and slot reuse cannot leave dangling keys. Hash collisions are detected against the live Header and fall
// back to a linear lookup, preserving correctness. Periodic rebuilding bounds stale hashes left by FIFO eviction.
class IndexedDynamicTable {
 public:
  explicit IndexedDynamicTable(std::size_t maxSizeBytes) : _table(maxSizeBytes) {}

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

HpackDecoder CreateHpackDecoder() { return {4096, true}; }

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
  HpackEncoder encoder(static_cast<std::size_t>(state.range(0)));
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
  Table table(static_cast<std::size_t>(state.range(0)));
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
  Table table(static_cast<std::size_t>(state.range(0)));
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
  Table table(static_cast<std::size_t>(state.range(0)));
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
  Table table(static_cast<std::size_t>(state.range(0)));
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
  IndexedDynamicTable table(static_cast<std::size_t>(state.range(0)));
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
  IndexedDynamicTable table(static_cast<std::size_t>(state.range(0)));
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
