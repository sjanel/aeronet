#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aeronet/city-hash.hpp"
#include "aeronet/flat-hash-map.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/toupperlower.hpp"

using namespace aeronet;

namespace {
struct CaseInsensitiveHashBoostStyle {
  std::size_t operator()(std::string_view str) const noexcept {
    std::size_t hash = 0;
    for (unsigned char c : str) {
      hash ^= static_cast<std::size_t>(tolower(c)) + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (hash << 6) +
              (hash >> 2);
    }
    return hash;
  }
};

struct CaseInsensitiveHashFnv1Style {
  std::size_t operator()(std::string_view str) const noexcept {
    std::size_t hash = 14695981039346656037ULL;
    for (char c : str) {
      hash ^= tolower(c);
      hash *= 1099511628211ULL;
    }
    return hash;
  }
};

std::vector<std::string> GenerateTestStrings() {
  constexpr std::size_t kCount = 100'000;
  constexpr std::size_t kMinLen = 4;
  constexpr std::size_t kMaxLen = 96;

  std::vector<std::string> strings;
  strings.reserve(kCount);

  std::mt19937_64 rng(0xC0FFEE);
  std::normal_distribution<double> length_dist(16.0, 8.0);
  std::uniform_int_distribution<int> char_dist('a', 'z');
  std::bernoulli_distribution upper_dist(0.3);  // mix upper/lowercase

  for (std::size_t i = 0; i < kCount; ++i) {
    std::size_t len;
    do {
      len = static_cast<std::size_t>(length_dist(rng));
    } while (len < kMinLen || len > kMaxLen);

    std::string s;
    s.resize(len);

    for (std::size_t j = 0; j < len; ++j) {
      char c = static_cast<char>(char_dist(rng));
      if (upper_dist(rng)) {
        c = static_cast<char>(c - 'a' + 'A');
      }
      s[j] = c;
    }

    strings.emplace_back(std::move(s));
  }

  return strings;
}

const auto kStorage = GenerateTestStrings();

template <typename Hash>
void BuildMap(const std::vector<std::string>& storage, auto& map) {
  map.reserve(storage.size());
  // map.max_load_factor(0.7f);

  for (const auto& s : storage) {
    map.emplace(s, s);
  }
}

}  // namespace

// ------------------------------------------------------------
// Benchmarks
// ------------------------------------------------------------

static void BM_Hash_CI_Boost(benchmark::State& state) {
  CaseInsensitiveHashBoostStyle hasher;

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(hasher(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_Hash_CI_FNV1a(benchmark::State& state) {
  CaseInsensitiveHashFnv1Style hasher;

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(hasher(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_Hash_City(benchmark::State& state) {
  CityHash hasher;

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(hasher(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_UnorderedMap_Find_CI_Boost(benchmark::State& state) {
  std::unordered_map<std::string_view, std::string_view, CaseInsensitiveHashBoostStyle, CaseInsensitiveEqualFunc> map;
  BuildMap<CaseInsensitiveHashBoostStyle>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_UnorderedMap_Find_CI_FNV1a(benchmark::State& state) {
  std::unordered_map<std::string_view, std::string_view, CaseInsensitiveHashFnv1Style, CaseInsensitiveEqualFunc> map;
  BuildMap<CaseInsensitiveHashFnv1Style>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}
static void BM_UnorderedMap_Find_City(benchmark::State& state) {
  std::unordered_map<std::string_view, std::string_view, CityHash, CaseInsensitiveEqualFunc> map;
  BuildMap<CityHash>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_FlatHashMap_Find_CI_Boost(benchmark::State& state) {
  flat_hash_map<std::string_view, std::string_view, CaseInsensitiveHashBoostStyle, CaseInsensitiveEqualFunc> map;
  BuildMap<CaseInsensitiveHashBoostStyle>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_FlatHashMap_Find_CI_FNV1a(benchmark::State& state) {
  flat_hash_map<std::string_view, std::string_view, CaseInsensitiveHashFnv1Style, CaseInsensitiveEqualFunc> map;
  BuildMap<CaseInsensitiveHashFnv1Style>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_FlatHashMap_Find_City(benchmark::State& state) {
  flat_hash_map<std::string_view, std::string_view, CityHash> map;
  BuildMap<CityHash>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

static void BM_FlatHashMap_Find_Sv(benchmark::State& state) {
  flat_hash_map<std::string_view, std::string_view, std::hash<std::string_view>> map;
  BuildMap<std::hash<std::string_view>>(kStorage, map);

  for (auto _ : state) {
    for (std::string_view s : kStorage) {
      benchmark::DoNotOptimize(map.find(s));
    }
  }

  state.SetItemsProcessed(state.iterations() * kStorage.size());
}

// ------------------------------------------------------------

BENCHMARK(BM_Hash_CI_Boost);
BENCHMARK(BM_Hash_CI_FNV1a);
BENCHMARK(BM_Hash_City);

BENCHMARK(BM_UnorderedMap_Find_CI_Boost);
BENCHMARK(BM_UnorderedMap_Find_CI_FNV1a);
BENCHMARK(BM_UnorderedMap_Find_City);

BENCHMARK(BM_FlatHashMap_Find_CI_Boost);
BENCHMARK(BM_FlatHashMap_Find_CI_FNV1a);
BENCHMARK(BM_FlatHashMap_Find_City);
BENCHMARK(BM_FlatHashMap_Find_Sv);

BENCHMARK_MAIN();
