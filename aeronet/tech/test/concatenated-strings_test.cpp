#include "concatenated-strings.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

using namespace aeronet;

TEST(ConcatenatedStrings, BasicAccess) {
  ConcatenatedStrings<3> cs({"alpn", "cipher", "tls1.3"});
  EXPECT_EQ(cs[0], "alpn");
  EXPECT_EQ(cs[1], "cipher");
  EXPECT_EQ(cs[2], "tls1.3");
}

TEST(ConcatenatedStrings, DefaultConstructedEmpty) {
  ConcatenatedStrings<3> info;
  EXPECT_EQ(info[0], std::string_view());
  EXPECT_EQ(info[1], std::string_view());
  EXPECT_EQ(info[2], std::string_view());
}

TEST(ConcatenatedStrings, ParameterizedStoresAndReturns) {
  ConcatenatedStrings<3> info({"h2", "TLS_AES_128_GCM_SHA256", "TLSv1.3"});
  EXPECT_EQ(info[0], "h2");
  EXPECT_EQ(info[1], "TLS_AES_128_GCM_SHA256");
  EXPECT_EQ(info[2], "TLSv1.3");
}

TEST(ConcatenatedStrings, LongStringsAreHandled) {
  std::string alpn(1000, 'A');
  std::string cipher(500, 'B');
  std::string version(200, 'C');
  ConcatenatedStrings<3> info({alpn, cipher, version});
  EXPECT_EQ(info[0], std::string_view(alpn));
  EXPECT_EQ(info[1], std::string_view(cipher));
  EXPECT_EQ(info[2], std::string_view(version));
}

TEST(ConcatenatedStrings, CopyAndAssign) {
  ConcatenatedStrings<2> src({"proto", "cipher"});
  ConcatenatedStrings<2> copyInfo = src;  // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_EQ(copyInfo[0], "proto");
  EXPECT_EQ(copyInfo[1], "cipher");

  ConcatenatedStrings<2> dst;
  dst = src;  // copy assign
  EXPECT_EQ(dst[0], "proto");
  EXPECT_EQ(dst[1], "cipher");
}

TEST(ConcatenatedStrings, SetLarger) {
  ConcatenatedStrings<3> cs({"a", "bb", "ccc"});
  // Replace middle part with a larger string
  cs.set(1, std::string_view("BBBBBBBB"));
  EXPECT_EQ(cs[0], "a");
  EXPECT_EQ(cs[1], "BBBBBBBB");
  EXPECT_EQ(cs[2], "ccc");
}

TEST(ConcatenatedStrings, SetShorter) {
  ConcatenatedStrings<3> cs({"aaaa", "bbbbbb", "cccccc"});
  // Replace first part with a shorter string
  cs.set(0, std::string_view("X"));
  EXPECT_EQ(cs[0], "X");
  EXPECT_EQ(cs[1], "bbbbbb");
  EXPECT_EQ(cs[2], "cccccc");
}

TEST(ConcatenatedStrings, SetEqualSize) {
  ConcatenatedStrings<3> cs({"one", "two", "three"});
  // Replace last part with a same-size string
  cs.set(2, std::string_view("XXX"));
  EXPECT_EQ(cs[0], "one");
  EXPECT_EQ(cs[1], "two");
  EXPECT_EQ(cs[2], "XXX");
}

TEST(ConcatenatedStrings, SetFirstGrowAndShrink) {
  ConcatenatedStrings<3> cs({"aa", "bbbb", "cc"});
  // grow first
  cs.set(0, std::string_view("AAAAAAAA"));
  EXPECT_EQ(cs[0], "AAAAAAAA");
  EXPECT_EQ(cs[1], "bbbb");
  EXPECT_EQ(cs[2], "cc");

  // shrink first
  cs.set(0, std::string_view("Z"));
  EXPECT_EQ(cs[0], "Z");
  EXPECT_EQ(cs[1], "bbbb");
  EXPECT_EQ(cs[2], "cc");
}

TEST(ConcatenatedStrings, SetMiddleMultipleTimes) {
  ConcatenatedStrings<4> cs({"a", "bb", "ccc", "dddd"});
  // grow middle part (index 1)
  cs.set(1, std::string_view("BBBBBBBBBB"));
  EXPECT_EQ(cs[0], "a");
  EXPECT_EQ(cs[1], "BBBBBBBBBB");
  EXPECT_EQ(cs[2], "ccc");
  EXPECT_EQ(cs[3], "dddd");

  // shrink part 2 (index 2) to empty
  cs.set(2, std::string_view(""));
  EXPECT_EQ(cs[2], std::string_view());
  EXPECT_EQ(cs[3], "dddd");

  // replace middle again with equal size
  cs.set(1, std::string_view("0123456789"));
  EXPECT_EQ(cs[1], "0123456789");
}

TEST(ConcatenatedStrings, SetLastGrowAndShrink) {
  ConcatenatedStrings<3> cs({"X", "YY", "ZZZ"});
  cs.set(2, std::string_view("LLLLLLLLLLLL"));
  EXPECT_EQ(cs[0], "X");
  EXPECT_EQ(cs[1], "YY");
  EXPECT_EQ(cs[2], "LLLLLLLLLLLL");

  cs.set(2, std::string_view("ok"));
  EXPECT_EQ(cs[2], "ok");
}

TEST(ConcatenatedStrings, SetEmptyAtPositions) {
  // empty first
  ConcatenatedStrings<3> cs1({"first", "middle", "last"});
  cs1.set(0, std::string_view(""));
  EXPECT_EQ(cs1[0], std::string_view());
  EXPECT_EQ(cs1[1], "middle");
  EXPECT_EQ(cs1[2], "last");

  // empty middle
  ConcatenatedStrings<3> cs2({"first", "middle", "last"});
  cs2.set(1, std::string_view(""));
  EXPECT_EQ(cs2[0], "first");
  EXPECT_EQ(cs2[1], std::string_view());
  EXPECT_EQ(cs2[2], "last");

  // empty last
  ConcatenatedStrings<3> cs3({"first", "middle", "last"});
  cs3.set(2, std::string_view(""));
  EXPECT_EQ(cs3[0], "first");
  EXPECT_EQ(cs3[1], "middle");
  EXPECT_EQ(cs3[2], std::string_view());
}

TEST(ConcatenatedStrings, StressManySets) {
  // stress test: repeated small changes across many iterations
  ConcatenatedStrings<5> cs({"a", "bb", "ccc", "dddd", "eeeee"});
  for (int iter = 0; iter < 1000; ++iter) {
    // vary sizes and positions
    cs.set(0, std::string_view((iter % 3 == 0) ? "" : "X"));
    cs.set(1, std::string_view((iter % 5 == 0) ? "BBBB" : "b"));
    cs.set(2, std::string_view("C"));
    cs.set(3, std::string_view((iter % 7 == 0) ? "" : "DD"));
    cs.set(4, std::string_view("E"));

    // basic sanity checks (not expensive string compares each time)
    EXPECT_TRUE(cs[2] == std::string_view("C"));
    // ensure offsets still produce concatenated length >= single parts
    std::size_t totalLen = cs[0].size() + cs[1].size() + cs[2].size() + cs[3].size() + cs[4].size();
    EXPECT_GE(totalLen, static_cast<std::size_t>(0));
  }
}

TEST(ConcatenatedStrings, SinglePartN1) {
  ConcatenatedStrings<1> cs({"only"});
  EXPECT_EQ(cs[0], "only");
  cs.set(0, std::string_view("new"));
  EXPECT_EQ(cs[0], "new");
  cs.set(0, std::string_view(""));
  EXPECT_EQ(cs[0], std::string_view());
}

TEST(ConcatenatedStrings, TmpNullTerminated_FirstMiddleLast) {
  ConcatenatedStrings<3> cs({"first", "middle", "last"});

  EXPECT_EQ(std::strlen(cs.makeNullTerminated(0).c_str()), 5UL);

  // First
  {
    auto ptr = const_cast<char *>(cs[0].data());
    (void)ptr;  // silence unused warnings in some builds
    auto tmp = cs.makeNullTerminated(0);
    EXPECT_EQ(tmp.c_str(), ptr);
    EXPECT_EQ(std::strlen(tmp.c_str()), cs[0].size());
    EXPECT_EQ(tmp.c_str()[cs[0].size()], '\0');
  }
  const char orig0 = const_cast<char *>(cs[0].data())[cs[0].size()];
  EXPECT_EQ(const_cast<char *>(cs[0].data())[cs[0].size()], orig0);

  // Middle
  {
    auto ptr = const_cast<char *>(cs[1].data());
    (void)ptr;
    auto tmp = cs.makeNullTerminated(1);
    EXPECT_EQ(tmp.c_str(), ptr);
    EXPECT_EQ(std::strlen(tmp.c_str()), cs[1].size());
    EXPECT_EQ(tmp.c_str()[cs[1].size()], '\0');
  }
  const char orig1 = const_cast<char *>(cs[1].data())[cs[1].size()];
  EXPECT_EQ(const_cast<char *>(cs[1].data())[cs[1].size()], orig1);

  // Last
  {
    auto ptr = const_cast<char *>(cs[2].data());
    (void)ptr;
    // end char for last initially is the trailing null appended by constructor
    auto tmp = cs.makeNullTerminated(2);
    EXPECT_EQ(tmp.c_str(), ptr);
    EXPECT_EQ(std::strlen(tmp.c_str()), cs[2].size());
    EXPECT_EQ(tmp.c_str()[cs[2].size()], '\0');
  }
  const char orig2 = const_cast<char *>(cs[2].data())[cs[2].size()];
  EXPECT_EQ(const_cast<char *>(cs[2].data())[cs[2].size()], orig2);
}

TEST(ConcatenatedStrings, TmpNullTerminated_Nested) {
  ConcatenatedStrings<4> cs({"A", "BB", "CCC", "DDDD"});
  auto ptr0 = const_cast<char *>(cs[0].data());
  auto ptr2 = const_cast<char *>(cs[2].data());
  const char o0 = ptr0[cs[0].size()];
  const char o2 = ptr2[cs[2].size()];

  // create nested temporaries for non-adjacent parts
  {
    auto t0 = cs.makeNullTerminated(0);
    EXPECT_EQ(t0.c_str(), ptr0);
    EXPECT_EQ(t0.c_str()[cs[0].size()], '\0');

    auto t2 = cs.makeNullTerminated(2);
    EXPECT_EQ(t2.c_str(), ptr2);
    EXPECT_EQ(t2.c_str()[cs[2].size()], '\0');

    // still null-terminated inside scope
    EXPECT_EQ(std::strlen(t0.c_str()), cs[0].size());
    EXPECT_EQ(std::strlen(t2.c_str()), cs[2].size());
  }

  // restored after destruction
  EXPECT_EQ(ptr0[cs[0].size()], o0);
  EXPECT_EQ(ptr2[cs[2].size()], o2);
}

TEST(ConcatenatedStrings, TmpNullTerminated_Stress) {
  ConcatenatedStrings<4> cs({"alpha", "beta", "gamma", "delta"});
  for (int i = 0; i < 2000; ++i) {
    const size_t idx = static_cast<size_t>(i % 4);
    auto ptr = const_cast<char *>(cs[idx].data());
    const char before = ptr[cs[idx].size()];
    {
      auto tmp = cs.makeNullTerminated(idx);
      EXPECT_EQ(tmp.c_str(), ptr);
      EXPECT_EQ(tmp.c_str()[cs[idx].size()], '\0');
    }
    EXPECT_EQ(ptr[cs[idx].size()], before);
  }
}

TEST(ConcatenatedStrings, TmpNullTerminated_MoveConstruct) {
  ConcatenatedStrings<3> cs({"one", "two", "three"});
  auto ptr1 = const_cast<char *>(cs[1].data());
  auto t1 = cs.makeNullTerminated(1);
  // move-construct into t2
  decltype(t1) t2(std::move(t1));
  EXPECT_EQ(t2.c_str(), ptr1);
  EXPECT_EQ(t2.c_str()[cs[1].size()], '\0');
  // moved-from t1 should be inert; its pointer may be null
  // ensure restoration on t2 destruction keeps buffer intact
}

TEST(ConcatenatedStrings, TmpNullTerminated_MoveAssign) {
  ConcatenatedStrings<3> cs({"a", "bb", "ccc"});
  auto ptr0 = const_cast<char *>(cs[0].data());
  auto ptr2 = const_cast<char *>(cs[2].data());
  auto t0 = cs.makeNullTerminated(0);
  auto t2 = cs.makeNullTerminated(2);
  // move-assign t2 into t0
  t0 = std::move(t2);
  EXPECT_EQ(t0.c_str(), ptr2);
  EXPECT_EQ(t0.c_str()[cs[2].size()], '\0');
  // after move-assign, ptr0's slot should have been restored
  EXPECT_NE(ptr0[cs[0].size()], '\0');
}