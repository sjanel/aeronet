#include "aeronet/flat-hash-map.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aeronet/string-equal-ignore-case.hpp"

using Map = aeronet::flat_hash_map<std::string, int, std::hash<std::string_view>, std::equal_to<>>;

TEST(flat_hash_map, basic_insert_find) {
  Map map1;
  EXPECT_TRUE(map1.empty());
  auto [it1, inserted1] = map1.emplace("alpha", 1);
  EXPECT_TRUE(inserted1);
  EXPECT_EQ(map1.size(), 1U);
  auto [it2, inserted2] = map1.insert({"beta", 2});
  EXPECT_TRUE(inserted2);
  EXPECT_EQ(map1.size(), 2U);
  auto [it3, inserted3] = map1.insert({"alpha", 42});
  EXPECT_FALSE(inserted3);
  EXPECT_EQ(it1->second, 1);

  map1["alpha"] = 5;
  EXPECT_EQ(map1.at("alpha"), 5);

  auto it = map1.find("beta");
  EXPECT_NE(it, map1.end());
  EXPECT_EQ(it->second, 2);

  EXPECT_EQ(map1.find("gamma"), map1.end());
}

TEST(flat_hash_map, heterogeneous_lookup_string_view) {
  Map map1;
  map1["path"] = 10;
  std::string_view sv = "path";
  auto it = map1.find(sv);
  ASSERT_NE(it, map1.end());
  EXPECT_EQ(it->second, 10);
  EXPECT_NE(map1.count(sv), 0U);
  EXPECT_EQ(map1.count("missing"), 0U);
}

TEST(flat_hash_map, erase_and_clear) {
  Map map1;
  map1.emplace("one", 1);
  map1.emplace("two", 2);
  map1.emplace("three", 3);
  EXPECT_EQ(map1.size(), 3U);
  auto erased = map1.erase("two");
  EXPECT_EQ(erased, 1U);
  EXPECT_EQ(map1.count("two"), 0U);
  EXPECT_EQ(map1.size(), 2U);
  EXPECT_EQ(map1.erase("nope"), 0U);
  map1.clear();
  EXPECT_TRUE(map1.empty());
}

TEST(flat_hash_map, iteration_and_contents) {
  Map map1;
  map1["a"] = 1;
  map1["b"] = 2;
  map1["c"] = 3;
  int sum = 0;
  for (auto &kv : map1) {
    sum += kv.second;
  }
  EXPECT_EQ(sum, 6);
}

TEST(flat_hash_map, reserve_and_rehash_growth) {
  Map map1;
  map1.reserve(100);
  for (int i = 0; i < 200; ++i) {
    map1.emplace("k" + std::to_string(i), i);
  }
  EXPECT_EQ(map1.size(), 200U);
  EXPECT_EQ(map1.at("k0"), 0);
  EXPECT_EQ(map1.at("k199"), 199);
}

TEST(flat_hash_map, swap_and_move) {
  Map mapA;
  mapA["x"] = 1;
  mapA["y"] = 2;
  Map mapB;
  mapB["z"] = 3;
  using std::swap;
  swap(mapA, mapB);
  EXPECT_EQ(mapA.size(), 1U);
  EXPECT_EQ(mapB.size(), 2U);
  EXPECT_NE(mapB.find("x"), mapB.end());

  Map mapC(std::move(mapB));
  EXPECT_EQ(mapC.size(), 2U);
  Map mapD;
  mapD = std::move(mapA);
  EXPECT_EQ(mapD.size(), 1U);
}

TEST(flat_hash_map, equal_range_and_count) {
  Map map1;
  map1["one"] = 1;
  auto range = map1.equal_range("one");
  EXPECT_NE(range.first, range.second);
  EXPECT_EQ(std::distance(range.first, range.second), 1);
  auto missing = map1.equal_range("missing");
  EXPECT_EQ(missing.first, missing.second);
}

TEST(flat_hash_map, insert_or_assign_semantics) {
  Map map1;
  auto [it, inserted] = map1.insert({"val", 7});
  EXPECT_TRUE(inserted);
  auto [it2, inserted2] = map1.insert({"val", 9});
  if (!inserted2) {
    it2->second = 9;
  }
  EXPECT_EQ(map1.at("val"), 9);
}

TEST(flat_hash_map, heterogeneous_cstring_lookup) {
  Map map1;
  map1["key"] = 42;
  const char *ckey = "key";
  auto it = map1.find(ckey);
  ASSERT_NE(it, map1.end());
  EXPECT_EQ(it->second, 42);
}

TEST(flat_hash_map, empty_key_support) {
  Map map1;
  map1[""] = 123;
  EXPECT_EQ(map1.at(""), 123);
  std::string_view sv;
  EXPECT_NE(map1.find(sv), map1.end());
}

TEST(flat_hash_map, preserves_value_alignment) {
  struct alignas(64) CacheLineAlignedValue {
    std::array<std::uint8_t, 64> data{};
  };

  aeronet::flat_hash_map<int, CacheLineAlignedValue> map;
  static constexpr int expectedAlignment = alignof(CacheLineAlignedValue);

  for (int i = 0; i < 128; ++i) {
    CacheLineAlignedValue value{};
    value.data[0] = static_cast<std::uint8_t>(i);
    map.emplace(i, value);
  }

  ASSERT_EQ(map.size(), 128U);
  for (auto &entry : map) {
    auto addr = reinterpret_cast<std::uintptr_t>(&entry.second);
    EXPECT_EQ(addr % expectedAlignment, 0U) << "value storage is not properly aligned";
  }

  map.reserve(512);
  map.emplace(512, CacheLineAlignedValue{});
  auto &value = map[1024];
  auto addr = reinterpret_cast<std::uintptr_t>(&value);
  EXPECT_EQ(addr % expectedAlignment, 0U);
}

TEST(flat_hash_map, proper_iteration_during_erase) {
  std::vector<std::unique_ptr<int>> pointers;
  aeronet::flat_hash_map<const int *, int> testMap;

  for (int iteration = 0; iteration < 1000; ++iteration) {
    // Insert some items in the map
    for (int i = 0; i < 10; i++) {
      pointers.push_back(std::make_unique<int>(i));
      testMap[pointers.back().get()] = 3;
    }

    // Process
    std::vector<const int *> keys;
    for (auto it = testMap.begin(); it != testMap.end();) {
      keys.push_back(it->first);
      if (--it->second == 0) {
        it = testMap.erase(it);
      } else {
        ++it;
      }
    }

    // Test consistency: look for duplicates
    std::ranges::sort(keys);
    ASSERT_EQ(std::ranges::adjacent_find(keys), keys.end()) << "Duplicate keys found in iteration " << iteration;
  }
}

TEST(flat_hash_map, fuzz_against_unordered_map) {
  static constexpr int iterations = 30000;
  static constexpr int keyRange = 300;
  Map map1;
  std::unordered_map<std::string, int> reference;
  std::mt19937 rng(1337);
  std::uniform_int_distribution<int> keyDist(0, keyRange - 1);
  std::uniform_int_distribution<int> valueDist(-1000, 1000);
  std::uniform_int_distribution<int> actionDist(0, 50);
  std::uniform_real_distribution<float> loadFactorDist(0.3F, 1.2F);

  auto assertEqualMaps = [&]() {
    ASSERT_EQ(map1.size(), reference.size());
    for (const auto &entry : reference) {
      auto it = map1.find(entry.first);
      ASSERT_NE(it, map1.end());
      EXPECT_EQ(it->second, entry.second);
    }
  };

  for (int i = 0; i < iterations; ++i) {
    int action = actionDist(rng);
    std::string key = "k" + std::to_string(keyDist(rng));
    int value = valueDist(rng);

    switch (action) {
      case 0:
      case 1:
      case 2: {
        auto [it, inserted] = map1.emplace(key, value);
        auto refResult = reference.emplace(key, value);
        if (!inserted) {
          EXPECT_FALSE(refResult.second);
        }
        break;
      }
      case 3:
      case 4:
      case 5:
      case 6:
      case 7:
      case 8:
      case 9: {
        auto [it, inserted] = map1.insert({key, value});
        auto refResult = reference.insert({key, value});
        EXPECT_EQ(inserted, refResult.second);
        if (!inserted) {
          EXPECT_FALSE(refResult.second);
        }
        break;
      }
      case 10:
      case 11:
      case 12: {
        auto erased1 = map1.erase(key);
        auto erased2 = reference.erase(key);
        EXPECT_EQ(erased1, erased2);
        break;
      }
      case 13: {
        float lf = loadFactorDist(rng);
        map1.max_load_factor(lf);
        auto desiredBuckets = reference.size() + static_cast<std::size_t>(reference.size() / 2) + 1;
        map1.rehash(desiredBuckets);
        break;
      }
      case 14: {
        map1.rehash(reference.size() + 1);
        break;
      }
      case 15: {
        map1.clear();
        reference.clear();
        break;
      }
      default: {
        map1.insert_or_assign(key, value);
        reference.insert_or_assign(key, value);
        break;
      }
    }

    assertEqualMaps();
  }
}

TEST(flat_hash_map, insert_or_assign_changes_existing_values) {
  Map map1;
  auto [it, inserted] = map1.insert_or_assign("alpha", 10);
  EXPECT_TRUE(inserted);
  EXPECT_EQ(it->second, 10);

  auto [it2, inserted2] = map1.insert_or_assign("alpha", 25);
  EXPECT_FALSE(inserted2);
  EXPECT_EQ(it2->second, 25);
  EXPECT_EQ(map1.size(), 1U);

  std::string beta = "beta";
  map1.insert_or_assign(beta, 99);
  EXPECT_EQ(map1.at("beta"), 99);
}

TEST(flat_hash_map, contains_heterogeneous_keys) {
  Map map1;
  map1.emplace("alpha", 1);
  map1.emplace("beta", 2);

  const char *ckey = "alpha";
  std::string_view sv = "beta";
  EXPECT_TRUE(map1.contains(ckey));
  EXPECT_TRUE(map1.contains(sv));
  EXPECT_FALSE(map1.contains("gamma"));

  map1.erase("alpha");
  EXPECT_FALSE(map1.contains(ckey));
}

TEST(flat_hash_map, equality_operators_respect_contents) {
  Map mapA;
  Map mapB;
  mapA["one"] = 1;
  mapA["two"] = 2;

  mapB["two"] = 2;
  mapB["one"] = 1;

  EXPECT_EQ(mapA, mapB);

  mapB["two"] = 99;
  EXPECT_NE(mapA, mapB);

  mapB["two"] = 2;
  mapB.erase("one");
  EXPECT_NE(mapA, mapB);
}

TEST(flat_hash_map, rehash_and_shrink_to_fit_preserve_entries) {
  Map map1;
  for (int i = 0; i < 200; ++i) {
    map1.emplace("key" + std::to_string(i), i);
  }

  Map reference = map1;
  auto originalBuckets = map1.bucket_count();
  ASSERT_GT(originalBuckets, 0U);

  map1.rehash(originalBuckets * 4);
  EXPECT_GE(map1.bucket_count(), originalBuckets);
  EXPECT_EQ(map1, reference);

  map1.erase("key10");
  reference.erase("key10");
  auto expandedBuckets = map1.bucket_count();
  map1.shrink_to_fit();
  EXPECT_LE(map1.bucket_count(), expandedBuckets);
  EXPECT_EQ(map1, reference);
}

TEST(flat_hash_map, erase_returns_iterator_to_next) {
  Map map1;
  map1["a"] = 1;
  map1["b"] = 2;
  map1["c"] = 3;

  auto it = map1.find("a");
  ASSERT_NE(it, map1.end());
  auto expectedNext = std::next(it);
  Map::iterator returned = map1.erase(it);

  EXPECT_EQ(returned, expectedNext);
  EXPECT_EQ(map1.count("a"), 0U);
  EXPECT_EQ(map1.size(), 2U);
}

namespace {
struct CountingValue {
  inline static int defaultConstructionCount = 0;
  int payload = 0;

  CountingValue() { ++defaultConstructionCount; }
};
}  // namespace

TEST(flat_hash_map, bracket_operator_default_constructs_values_once) {
  CountingValue::defaultConstructionCount = 0;
  aeronet::flat_hash_map<std::string, CountingValue> map;

  map["missing"].payload = 42;
  EXPECT_EQ(CountingValue::defaultConstructionCount, 1);

  map["missing"].payload = 7;
  EXPECT_EQ(CountingValue::defaultConstructionCount, 1);

  auto &ref = map["new_key"];
  EXPECT_EQ(CountingValue::defaultConstructionCount, 2);
  EXPECT_EQ(ref.payload, 0);
}

TEST(flat_hash_map, case_insensitive_contains_variants) {
  aeronet::flat_hash_map<std::string, std::string, aeronet::CaseInsensitiveHashFunc, aeronet::CaseInsensitiveEqualFunc>
      headers;
  headers["Content-Type"] = "text/html";
  headers["ACCEPT"] = "*/*";
  headers["X-Trace-Request-ID"] = "r-123";
  headers["X-SUPER-LONG-FLAG-TEST-KEYZZ"] = "1";  // 28 characters to trigger distinct contains instantiation
  headers["HostName"] = "example.com";

  std::string_view lowerType = "content-type";
  EXPECT_EQ(headers.find(lowerType)->second, "text/html");

  const char sixLetters[] = "accept";  // length 6 literal
  EXPECT_TRUE(headers.contains(sixLetters));

  const char eightLetters[] = "hostname";  // length 8 literal
  EXPECT_TRUE(headers.contains(eightLetters));

  const char eighteenChars[] = "x-trace-request-id";  // length 18 literal
  EXPECT_TRUE(headers.contains(eighteenChars));

  const char twentyEightChars[] = "x-super-long-flag-test-keyzz";  // length 28 literal
  EXPECT_TRUE(headers.contains(twentyEightChars));

  EXPECT_FALSE(headers.contains("missing-header"));

  aeronet::flat_hash_map<std::string, std::string, aeronet::CaseInsensitiveHashFunc, aeronet::CaseInsensitiveEqualFunc>
      copy = headers;
  EXPECT_EQ(headers, copy);
}

TEST(flat_hash_map, emplace_default_and_insert_or_assign_hint) {
  Map map1;
  auto [emptyIt, emptyInserted] = map1.emplace();
  ASSERT_TRUE(emptyInserted);
  EXPECT_TRUE(emptyIt->first.empty());
  EXPECT_EQ(emptyIt->second, 0);

  auto hintedIt = map1.insert_or_assign(map1.cbegin(), "gamma", 7);
  EXPECT_EQ(hintedIt->first, "gamma");
  EXPECT_EQ(hintedIt->second, 7);

  std::string deltaKey = "delta";
  auto hintedRvalue = map1.insert_or_assign(map1.cbegin(), std::move(deltaKey), 11);
  EXPECT_EQ(hintedRvalue->first, "delta");
  EXPECT_EQ(hintedRvalue->second, 11);

  ASSERT_EQ(map1.count("gamma"), 1U);
  ASSERT_EQ(map1.count("delta"), 1U);

  auto eraseBegin = map1.begin();
  auto eraseEnd = map1.end();
  map1.erase(eraseBegin, eraseEnd);
  EXPECT_TRUE(map1.empty());
}