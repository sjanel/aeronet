#include "aeronet/flat-hash-map.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

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
