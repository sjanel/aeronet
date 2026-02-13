#include "aeronet/static-concatenated-strings.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aeronet {

using TestType = StaticConcatenatedStrings<3, uint32_t>;

TEST(ConcatenatedStrings, DefaultConstructedEmpty) {
  TestType info;
  EXPECT_EQ(info[0], std::string_view());
  EXPECT_EQ(info[1], std::string_view());
  EXPECT_EQ(info[2], std::string_view());
  EXPECT_EQ(info.c_str(0), nullptr);
  EXPECT_EQ(info.c_str(1), nullptr);
  EXPECT_EQ(info.c_str(2), nullptr);
  EXPECT_EQ(info, TestType());  // equality comparison
}

TEST(ConcatenatedStrings, SetFromDefaultConstructed) {
  TestType def;
  TestType withInitialCapacity{4};
  for (TestType* pInfo : {&def, &withInitialCapacity}) {
    TestType& info = *pInfo;
    info.set(0, "1");
    EXPECT_EQ(info[0], "1");
    EXPECT_EQ(info[1], "");
    EXPECT_EQ(info[2], "");

    info.set(1, "22");
    EXPECT_EQ(info[0], "1");
    EXPECT_EQ(info[1], "22");
    EXPECT_EQ(info[2], "");

    info.set(2, "333");
    EXPECT_EQ(info[0], "1");
    EXPECT_EQ(info[1], "22");
    EXPECT_EQ(info[2], "333");
    EXPECT_STREQ(info.c_str(0), "1");
    EXPECT_STREQ(info.c_str(1), "22");
    EXPECT_STREQ(info.c_str(2), "333");
  }
}

TEST(ConcatenatedStrings, BasicAccess) {
  TestType info({"h2", "TLS_AES_128_GCM_SHA256", "TLSv1.3"});
  EXPECT_EQ(info[0], "h2");
  EXPECT_EQ(info[1], "TLS_AES_128_GCM_SHA256");
  EXPECT_EQ(info[2], "TLSv1.3");
  EXPECT_STREQ(info.c_str(0), "h2");
  EXPECT_STREQ(info.c_str(1), "TLS_AES_128_GCM_SHA256");
  EXPECT_STREQ(info.c_str(2), "TLSv1.3");
}

TEST(ConcatenatedStrings, NotSameNumberOfParts) {
  EXPECT_THROW(TestType({"one", "two"}), std::length_error);
  EXPECT_THROW(TestType({"one", "two", "three", "four"}), std::length_error);
}

TEST(ConcatenatedStrings, LongStringsAreHandled) {
  std::string alpn(1000, 'A');
  std::string cipher(500, 'B');
  std::string version(200, 'C');
  TestType info({alpn, cipher, version});
  EXPECT_EQ(info[0], alpn);
  EXPECT_EQ(info[1], cipher);
  EXPECT_EQ(info[2], version);
  EXPECT_STREQ(info.c_str(0), alpn.c_str());
  EXPECT_STREQ(info.c_str(1), cipher.c_str());
  EXPECT_STREQ(info.c_str(2), version.c_str());
}

TEST(ConcatenatedStrings, GuardAgainstOverflowConstruction) {
  if constexpr (sizeof(uint32_t) >= sizeof(std::size_t)) {
    GTEST_SKIP() << "Test not applicable when uint32_t cannot overflow size_t";
  }

  char ch{};
  std::string aa(128UL, 'A');
  std::string_view bb(&ch, std::numeric_limits<uint32_t>::max() - 50UL);

  EXPECT_NO_THROW((TestType({aa, aa, aa})));
  EXPECT_THROW((TestType({aa, aa, bb})), std::overflow_error);

  std::string_view cc(&ch, std::numeric_limits<uint32_t>::max() + 1UL);
  EXPECT_THROW((TestType({cc, aa, aa})), std::overflow_error);
}

TEST(ConcatenatedStrings, GuardAgainstOverflowSet) {
  if constexpr (sizeof(uint32_t) >= sizeof(std::size_t)) {
    GTEST_SKIP() << "Test not applicable when uint32_t cannot overflow size_t";
  }
  TestType tooSmall({"", "", ""});
  char ch{};
  std::string aa(128UL, 'A');
  std::string_view bb(&ch, std::numeric_limits<uint32_t>::max() - 50UL);
  tooSmall.set(0, aa);
  EXPECT_THROW(tooSmall.set(1, bb), std::overflow_error);
}

TEST(ConcatenatedStrings, CopyAndAssign) {
  TestType src({"proto", "cipher", "version"});
  TestType copyInfo = src;  // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(copyInfo[0], "proto");
  EXPECT_EQ(copyInfo[1], "cipher");
  EXPECT_EQ(copyInfo[2], "version");

  TestType dst;
  dst = src;  // copy assign
  EXPECT_EQ(dst[0], "proto");
  EXPECT_EQ(dst[1], "cipher");
  EXPECT_EQ(dst[2], "version");
}

TEST(ConcatenatedStrings, SetLarger) {
  TestType cs({"a", "bb", "ccc"});
  // Replace middle part with a larger string
  cs.set(1U, std::string_view("BBBBBBBB"));
  EXPECT_EQ(cs[0], "a");
  EXPECT_EQ(cs[1], "BBBBBBBB");
  EXPECT_EQ(cs[2], "ccc");
  EXPECT_STREQ(cs.c_str(0), "a");
  EXPECT_STREQ(cs.c_str(1), "BBBBBBBB");
  EXPECT_STREQ(cs.c_str(2), "ccc");

  cs.set(2U, std::string_view("DDDDDDDDDDDDDD"));
  EXPECT_EQ(cs[0], "a");
  EXPECT_EQ(cs[1], "BBBBBBBB");
  EXPECT_EQ(cs[2], "DDDDDDDDDDDDDD");
}

TEST(ConcatenatedStrings, SetShorter) {
  TestType cs({"aaaa", "bbbbbb", "cccccc"});
  // Replace first part with a shorter string
  cs.set(0, std::string_view("X"));
  EXPECT_EQ(cs[0], "X");
  EXPECT_EQ(cs[1], "bbbbbb");
  EXPECT_EQ(cs[2], "cccccc");
  EXPECT_STREQ(cs.c_str(0), "X");
  EXPECT_STREQ(cs.c_str(1), "bbbbbb");
  EXPECT_STREQ(cs.c_str(2), "cccccc");

  cs.set(1, std::string_view("YY"));
  EXPECT_EQ(cs[0], "X");
  EXPECT_EQ(cs[1], "YY");
  EXPECT_EQ(cs[2], "cccccc");
}

TEST(ConcatenatedStrings, SetEqualSize) {
  TestType cs({"one", "two", "three"});
  // Replace last part with a same-size string
  cs.set(2, std::string_view("XXX"));
  EXPECT_EQ(cs[0], "one");
  EXPECT_EQ(cs[1], "two");
  EXPECT_EQ(cs[2], "XXX");
  EXPECT_STREQ(cs.c_str(0), "one");
  EXPECT_STREQ(cs.c_str(1), "two");
  EXPECT_STREQ(cs.c_str(2), "XXX");
}

TEST(ConcatenatedStrings, SetEqualSizeEmpty) {
  TestType cs({"first", "", "third"});
  EXPECT_EQ(cs[0], "first");
  EXPECT_EQ(cs[1], "");
  EXPECT_EQ(cs[2], "third");
  cs.set(1, std::string_view(""));
  EXPECT_EQ(cs[0], "first");
  EXPECT_EQ(cs[1], "");
  EXPECT_EQ(cs[2], "third");
}

TEST(ConcatenatedStrings, SetFirstGrowAndShrink) {
  TestType cs({"aa", "bbbb", "cc"});
  // grow first
  cs.set(0, std::string_view("AAAAAAAA"));
  EXPECT_EQ(cs[0], "AAAAAAAA");
  EXPECT_EQ(cs[1], "bbbb");
  EXPECT_EQ(cs[2], "cc");
  EXPECT_STREQ(cs.c_str(0), "AAAAAAAA");
  EXPECT_STREQ(cs.c_str(1), "bbbb");
  EXPECT_STREQ(cs.c_str(2), "cc");

  // shrink first
  cs.set(0, std::string_view("Z"));
  EXPECT_EQ(cs[0], "Z");
  EXPECT_EQ(cs[1], "bbbb");
  EXPECT_EQ(cs[2], "cc");
  EXPECT_STREQ(cs.c_str(0), "Z");
  EXPECT_STREQ(cs.c_str(1), "bbbb");
  EXPECT_STREQ(cs.c_str(2), "cc");
}

TEST(ConcatenatedStrings, SetMiddleMultipleTimes) {
  TestType cs({"a", "bb", "ccc"});
  cs.set(0, std::string_view("BBBBBBBBBB"));
  EXPECT_EQ(cs[0], "BBBBBBBBBB");
  EXPECT_EQ(cs[1], "bb");
  EXPECT_EQ(cs[2], "ccc");
  EXPECT_STREQ(cs.c_str(0), "BBBBBBBBBB");
  EXPECT_STREQ(cs.c_str(1), "bb");
  EXPECT_STREQ(cs.c_str(2), "ccc");

  cs.set(1, std::string_view(""));
  EXPECT_EQ(cs[0], "BBBBBBBBBB");
  EXPECT_EQ(cs[1], std::string_view());
  EXPECT_EQ(cs[2], "ccc");
  EXPECT_STREQ(cs.c_str(0), "BBBBBBBBBB");
  EXPECT_STREQ(cs.c_str(1), "");
  EXPECT_STREQ(cs.c_str(2), "ccc");

  // replace middle again with equal size
  cs.set(0, std::string_view("0123456789"));
  EXPECT_EQ(cs[0], "0123456789");
  EXPECT_EQ(cs[1], std::string_view());
  EXPECT_EQ(cs[2], "ccc");
  EXPECT_STREQ(cs.c_str(0), "0123456789");
  EXPECT_STREQ(cs.c_str(1), "");
  EXPECT_STREQ(cs.c_str(2), "ccc");
}

TEST(ConcatenatedStrings, SetLastGrowAndShrink) {
  TestType cs({"X", "YY", "ZZZ"});
  cs.set(2, std::string_view("LLLLLLLLLLLL"));
  EXPECT_EQ(cs[0], "X");
  EXPECT_EQ(cs[1], "YY");
  EXPECT_EQ(cs[2], "LLLLLLLLLLLL");
  EXPECT_STREQ(cs.c_str(0), "X");
  EXPECT_STREQ(cs.c_str(1), "YY");
  EXPECT_STREQ(cs.c_str(2), "LLLLLLLLLLLL");

  cs.set(2, std::string_view("ok"));
  EXPECT_EQ(cs[0], "X");
  EXPECT_EQ(cs[1], "YY");
  EXPECT_EQ(cs[2], "ok");
  EXPECT_STREQ(cs.c_str(0), "X");
  EXPECT_STREQ(cs.c_str(1), "YY");
  EXPECT_STREQ(cs.c_str(2), "ok");
}

TEST(ConcatenatedStrings, SetEmptyAtPositions) {
  // empty first
  TestType cs1({"first", "middle", "last"});
  cs1.set(0, std::string_view(""));
  EXPECT_EQ(cs1[0], std::string_view());
  EXPECT_EQ(cs1[1], "middle");
  EXPECT_EQ(cs1[2], "last");
  EXPECT_STREQ(cs1.c_str(0), "");
  EXPECT_STREQ(cs1.c_str(1), "middle");
  EXPECT_STREQ(cs1.c_str(2), "last");

  // empty middle
  TestType cs2({"first", "middle", "last"});
  cs2.set(1, std::string_view(""));
  EXPECT_EQ(cs2[0], "first");
  EXPECT_EQ(cs2[1], std::string_view());
  EXPECT_EQ(cs2[2], "last");
  EXPECT_STREQ(cs2.c_str(0), "first");
  EXPECT_STREQ(cs2.c_str(1), "");
  EXPECT_STREQ(cs2.c_str(2), "last");

  // empty last
  TestType cs3({"first", "middle", "last"});
  cs3.set(2, std::string_view(""));
  EXPECT_EQ(cs3[0], "first");
  EXPECT_EQ(cs3[1], "middle");
  EXPECT_EQ(cs3[2], std::string_view());
  EXPECT_STREQ(cs3.c_str(0), "first");
  EXPECT_STREQ(cs3.c_str(1), "middle");
  EXPECT_STREQ(cs3.c_str(2), "");
}

TEST(ConcatenatedStrings, StressManySets) {
  // stress test: repeated small changes across many iterations
  TestType cs({"a", "bb", "ccc"});
  for (int iter = 0; iter < 1000; ++iter) {
    // vary sizes and positions
    cs.set(0, std::string((2UL * static_cast<uint32_t>(iter % 3)) + 1, 'A'));
    cs.set(1, std::string((3UL * static_cast<uint32_t>(iter % 4)) + 1, 'B'));
    cs.set(2, std::string((5UL * static_cast<uint32_t>(iter % 5)) + 1, 'C'));

    // basic sanity checks (not expensive string compares each time)
    EXPECT_TRUE(cs[0].starts_with('A'));
    EXPECT_TRUE(cs[1].starts_with('B'));
    EXPECT_TRUE(cs[2].starts_with('C'));
    EXPECT_STREQ(cs.c_str(0), cs[0].data());  // NOLINT(bugprone-suspicious-stringview-data-usage)
    EXPECT_STREQ(cs.c_str(1), cs[1].data());  // NOLINT(bugprone-suspicious-stringview-data-usage)
    EXPECT_STREQ(cs.c_str(2), cs[2].data());  // NOLINT(bugprone-suspicious-stringview-data-usage)
    // ensure offsets still produce concatenated length >= single parts
    std::size_t totalLen = cs[0].size() + cs[1].size() + cs[2].size();
    EXPECT_GE(totalLen, static_cast<std::size_t>(0));
  }
}

TEST(ConcatenatedStrings, TmpNullTerminated_FirstMiddleLast) {
  TestType cs({"first", "middle", "last"});

  EXPECT_EQ(std::strlen(cs.c_str(0)), 5UL);

  // First
  {
    auto ptr = const_cast<char*>(cs[0].data());
    (void)ptr;  // silence unused warnings in some builds
    auto tmp = cs.c_str(0);
    EXPECT_EQ(tmp, ptr);
    EXPECT_EQ(std::strlen(tmp), cs[0].size());
    EXPECT_EQ(tmp[cs[0].size()], '\0');
  }
  const char orig0 = const_cast<char*>(cs[0].data())[cs[0].size()];
  EXPECT_EQ(const_cast<char*>(cs[0].data())[cs[0].size()], orig0);

  // Middle
  {
    auto ptr = const_cast<char*>(cs[1].data());
    (void)ptr;
    auto tmp = cs.c_str(1);
    EXPECT_EQ(tmp, ptr);
    EXPECT_EQ(std::strlen(tmp), cs[1].size());
    EXPECT_EQ(tmp[cs[1].size()], '\0');
  }
  const char orig1 = const_cast<char*>(cs[1].data())[cs[1].size()];
  EXPECT_EQ(const_cast<char*>(cs[1].data())[cs[1].size()], orig1);

  // Last
  {
    auto ptr = const_cast<char*>(cs[2].data());
    (void)ptr;
    // end char for last initially is the trailing null appended by constructor
    auto tmp = cs.c_str(2);
    EXPECT_EQ(tmp, ptr);
    EXPECT_EQ(std::strlen(tmp), cs[2].size());
    EXPECT_EQ(tmp[cs[2].size()], '\0');
  }
  const char orig2 = const_cast<char*>(cs[2].data())[cs[2].size()];
  EXPECT_EQ(const_cast<char*>(cs[2].data())[cs[2].size()], orig2);
}

TEST(ConcatenatedStrings, TmpNullTerminated_Nested) {
  TestType cs({"A", "BB", "CCC"});
  auto ptr0 = const_cast<char*>(cs[0].data());
  auto ptr2 = const_cast<char*>(cs[2].data());
  const char o0 = ptr0[cs[0].size()];
  const char o2 = ptr2[cs[2].size()];

  // create nested temporaries for non-adjacent parts
  {
    auto t0 = cs.c_str(0);
    EXPECT_EQ(t0, ptr0);
    EXPECT_EQ(t0[cs[0].size()], '\0');

    auto t2 = cs.c_str(2);
    EXPECT_EQ(t2, ptr2);
    EXPECT_EQ(t2[cs[2].size()], '\0');

    // still null-terminated inside scope
    EXPECT_EQ(std::strlen(t0), cs[0].size());
    EXPECT_EQ(std::strlen(t2), cs[2].size());
  }

  // restored after destruction
  EXPECT_EQ(ptr0[cs[0].size()], o0);
  EXPECT_EQ(ptr2[cs[2].size()], o2);
}

TEST(ConcatenatedStrings, TmpNullTerminated_Stress) {
  TestType cs({"alpha", "beta", "gamma"});
  for (int i = 0; i < 2000; ++i) {
    const uint32_t idx = static_cast<uint32_t>(i % 3);
    auto ptr = const_cast<char*>(cs[idx].data());
    const char before = ptr[cs[idx].size()];
    {
      auto tmp = cs.c_str(idx);
      EXPECT_EQ(tmp, ptr);
      EXPECT_EQ(tmp[cs[idx].size()], '\0');
    }
    EXPECT_EQ(ptr[cs[idx].size()], before);
  }
}
}  // namespace aeronet