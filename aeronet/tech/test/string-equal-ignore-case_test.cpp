#include "aeronet/string-equal-ignore-case.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iterator>
#include <random>
#include <string>
#include <string_view>

namespace aeronet {

TEST(StringEqualIgnoreCase, EqualStrings) {
  EXPECT_TRUE(CaseInsensitiveEqual("hello", "HELLO"));
  EXPECT_TRUE(CaseInsensitiveEqual("Hello", "hello"));
  EXPECT_TRUE(CaseInsensitiveEqual("HELLO", "Hello"));
  EXPECT_TRUE(CaseInsensitiveEqual("", ""));
}

TEST(StringEqualIgnoreCase, UnequalStrings) {
  EXPECT_FALSE(CaseInsensitiveEqual("hello", "world"));
  EXPECT_FALSE(CaseInsensitiveEqual("Hello", "world"));
  EXPECT_FALSE(CaseInsensitiveEqual("HELLO", "world"));
  EXPECT_FALSE(CaseInsensitiveEqual("HELLO", "hell"));
}

TEST(StringLessIgnoreCase, LessStrings) {
  EXPECT_FALSE(CaseInsensitiveLess("abc", "ABC"));
  EXPECT_TRUE(CaseInsensitiveLess("abc", "ABcD"));
  EXPECT_FALSE(CaseInsensitiveLess("abc", "AB"));
  EXPECT_FALSE(CaseInsensitiveLess("abcd", "abc"));
}

TEST(StringEqualIgnoreCase, StringViewVariants) {
  std::string_view lhs = "FooBar";
  std::string_view rhs = "foobar";
  EXPECT_TRUE(CaseInsensitiveEqual(lhs, rhs));
  EXPECT_TRUE(CaseInsensitiveEqual("Foobar", rhs));
  EXPECT_TRUE(CaseInsensitiveEqual(lhs, "fOOBAR"));
  EXPECT_FALSE(CaseInsensitiveEqual(lhs, "foo"));
  EXPECT_FALSE(CaseInsensitiveEqual("foo", "fooo"));
}

TEST(StringEqualIgnoreCase, SwarBoundariesAndOverlap) {
  // Exercise the 16-byte SSE/NEON fast path, the 8-byte SWAR path for the [8,16) remainder, and their
  // overlapping tails across the chunk boundaries, with the difference placed at the first byte, the last
  // byte, and inside the overlap region. Lengths cover single- and multi-iteration 16-byte blocks (48/64/96)
  // and every off-by-one around the 8- and 16-byte boundaries where the overlapping tail re-reads bytes.
  for (std::size_t len :
       {std::size_t{7},  std::size_t{8},  std::size_t{9},  std::size_t{15}, std::size_t{16}, std::size_t{17},
        std::size_t{23}, std::size_t{24}, std::size_t{31}, std::size_t{32}, std::size_t{33}, std::size_t{40},
        std::size_t{47}, std::size_t{48}, std::size_t{49}, std::size_t{63}, std::size_t{64}, std::size_t{65},
        std::size_t{95}, std::size_t{96}, std::size_t{97}}) {
    std::string lhs(len, 'a');
    for (std::size_t i = 0; i < len; ++i) {
      lhs[i] = static_cast<char>('a' + (i % 26));
    }
    // Equal regardless of case.
    std::string upper = lhs;
    for (char& ch : upper) {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    EXPECT_TRUE(CaseInsensitiveEqual(lhs, upper)) << "len=" << len;

    // A real (non-case) difference at first / last / overlap-region positions must be detected.
    for (std::size_t pos : {std::size_t{0}, len - 1, len / 2}) {
      std::string diff = upper;
      diff[pos] = static_cast<char>(diff[pos] ^ 0x01);  // perturb without staying a case-only change
      EXPECT_FALSE(CaseInsensitiveEqual(lhs, diff)) << "len=" << len << " pos=" << pos;
    }
  }
}

TEST(StringEqualIgnoreCase, NonAsciiBytesComparedRaw) {
  // High-bit bytes (>= 0x80) must be compared as-is (never lowercased), matching scalar tolower. This is the
  // path the printable-ASCII fuzz never reaches, and where a SWAR/SSE lowercase bug would surface.
  const std::string str1 = "caf\xC3\xA9-Header";  // contains 0xC3 0xA9
  const std::string str2 = "CAF\xC3\xA9-HEADER";  // same non-ASCII bytes, letters case-flipped
  EXPECT_TRUE(CaseInsensitiveEqual(str1, str2));

  std::string str3 = str2;
  str3[3] = static_cast<char>(0xC4);  // change a non-ASCII byte -> must differ
  EXPECT_FALSE(CaseInsensitiveEqual(str1, str3));

  // Bytes 0x80..0xFF are not letters: two strings differing only in such a byte are unequal even though they
  // would be "equal" under a buggy unsigned-range lowercase.
  const std::string hi1(10, static_cast<char>(0xE0));
  std::string hi2 = hi1;
  hi2[9] = static_cast<char>(0xC0);
  EXPECT_FALSE(CaseInsensitiveEqual(hi1, hi2));
  EXPECT_TRUE(CaseInsensitiveEqual(hi1, std::string(10, static_cast<char>(0xE0))));

  // Same checks at >= 16 bytes so the 16-byte SSE/NEON path drives the high-bit handling: the signed
  // _mm_cmpgt_epi8 / vcgeq_u8 range test must treat bytes >= 0x80 as non-letters (never lowercased). Mixing
  // ASCII letters (case-flipped, must compare equal) with high bytes (compared raw) in one wide block.
  const std::string wideA = "X-Caf\xC3\xA9-Custom-Header-Name";  // 24 bytes, spans two 16-byte blocks
  std::string wideB = wideA;
  for (char& ch : wideB) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
      ch = static_cast<char>(ch ^ 0x20);  // flip case of ASCII letters only; high bytes untouched
    }
  }
  EXPECT_TRUE(CaseInsensitiveEqual(wideA, wideB));
  // Perturbing a high byte (a non-letter) in the second 16-byte block must be detected, not masked away.
  std::string wideC = wideB;
  wideC[4] = static_cast<char>(0xC4);  // was 0xC3
  EXPECT_FALSE(CaseInsensitiveEqual(wideA, wideC));
  // A run of identical high bytes longer than one 16-byte block stays equal; a single differing high byte in
  // the overlapping tail region is still caught.
  const std::string hiWide(20, static_cast<char>(0xE0));
  EXPECT_TRUE(CaseInsensitiveEqual(hiWide, std::string(20, static_cast<char>(0xE0))));
  std::string hiWide2 = hiWide;
  hiWide2[19] = static_cast<char>(0xC0);
  EXPECT_FALSE(CaseInsensitiveEqual(hiWide, hiWide2));
}

TEST(StringEqualIgnoreCase, HashConsistency) {
  CaseInsensitiveHashFunc hashFunc;
  const std::string_view str1 = "MiXeDCase";
  const std::string_view str2 = "mixedcase";
  EXPECT_EQ(hashFunc(str1), hashFunc(str2));
  EXPECT_NE(hashFunc(str1), hashFunc("different"));
}

TEST(StringEqualIgnoreCase, EqualFuncWrapper) {
  CaseInsensitiveEqualFunc eqFunc;
  EXPECT_TRUE(eqFunc("Sample", "sample"));
  EXPECT_FALSE(eqFunc("Sample", "samples"));
}

namespace {

std::string LowerCase(std::string_view sv) {
  std::string result;
  result.reserve(sv.size());
  std::ranges::transform(sv, std::back_inserter(result),
                         [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); });
  return result;
}

// Helper reference implementations using tolower for ASCII only
bool ReferenceCaseInsensitiveEqual(std::string_view lhs, std::string_view rhs) {
  return LowerCase(lhs) == LowerCase(rhs);
}

bool ReferenceCaseInsensitiveLess(std::string_view lhs, std::string_view rhs) {
  return LowerCase(lhs) < LowerCase(rhs);
}

}  // namespace

TEST(StringEqualIgnoreCase, FuzzRandomAsciiEqual) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937_64 rng(123456789);
  // Up to 100 bytes so the 16-byte SSE/NEON path (multiple iterations + overlapping tail) is fuzzed against
  // the scalar reference, not just the <= 32-byte sizes.
  std::uniform_int_distribution<std::size_t> lenDist(0, 100);
  std::uniform_int_distribution<int> charDist(0x20, 0x7E);  // printable ASCII
  std::uniform_int_distribution<int> caseDist(0, 1);
  std::string s1;
  std::string s2;

  CaseInsensitiveHashFunc hashFunc;

  for (int iteration = 0; iteration < 5000; ++iteration) {
    std::size_t sz = lenDist(rng);
    std::uniform_int_distribution<std::size_t> nbCharsToChange(0, sz / 4);
    s1.clear();
    s2.clear();
    for (std::size_t i = 0; i < sz; ++i) {
      char ch = static_cast<char>(charDist(rng));
      // randomly change case for letters
      if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
        if (caseDist(rng) != 0) {
          ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        } else {
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
      }
      s1.push_back(ch);
      // produce s2 by randomizing case per character
      char ch2 = ch;
      if ((ch2 >= 'A' && ch2 <= 'Z') || (ch2 >= 'a' && ch2 <= 'z')) {
        if (caseDist(rng) != 0) {
          ch2 = static_cast<char>(std::toupper(static_cast<unsigned char>(ch2)));
        } else {
          ch2 = static_cast<char>(std::tolower(static_cast<unsigned char>(ch2)));
        }
      }

      s2.push_back(ch2);
    }

    if (sz != 0) {
      // to introduce some differences, occasionally change a random number of characters
      std::uniform_int_distribution<std::size_t> changePos(0, sz - 1);
      std::size_t nbChanges = nbCharsToChange(rng);
      for (std::size_t changeIdx = 0; changeIdx < nbChanges; ++changeIdx) {
        std::size_t pos = changePos(rng);
        s2[pos] = static_cast<char>(s2[pos] ^ 0x20);  // flip case
      }
    }

    EXPECT_EQ(CaseInsensitiveEqual(s1, s2), ReferenceCaseInsensitiveEqual(s1, s2));
    // also cross-compare with different lengths occasionally
    if (iteration % 10 == 0) {
      std::string s3 = s1 + static_cast<char>(charDist(rng));
      EXPECT_EQ(CaseInsensitiveEqual(s1, s3), ReferenceCaseInsensitiveEqual(s1, s3));
    }
    // The hash is not guaranteed to be different for unequal strings, but it's the case for this randomly (but
    // deterministic) set of data, and it's a good sanity check. However, we expect case insensitive equal strings to
    // always have the same hash.
    EXPECT_EQ(hashFunc(s1) == hashFunc(s2), CaseInsensitiveEqual(s1, s2));
  }
}

TEST(StringLessIgnoreCase, FuzzRandomAsciiLess) {
  // NOLINTNEXTLINE(bugprone-random-generator-seed)
  std::mt19937_64 rng(987654321);
  std::uniform_int_distribution<std::size_t> lenDist(0, 32);
  std::uniform_int_distribution<int> charDist(0x20, 0x7E);  // printable ASCII
  std::string lhs;
  std::string rhs;

  for (int iteration = 0; iteration < 2000; ++iteration) {
    std::size_t na = lenDist(rng);
    std::size_t nb = lenDist(rng);
    lhs.clear();
    rhs.clear();
    for (std::size_t i = 0; i < na; ++i) {
      char ch = static_cast<char>(charDist(rng));
      if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
        if (std::uniform_int_distribution<int>(0, 1)(rng) != 0) {
          ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        } else {
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
      }
      lhs.push_back(ch);
    }
    for (std::size_t i = 0; i < nb; ++i) {
      char ch = static_cast<char>(charDist(rng));
      if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
        if (std::uniform_int_distribution<int>(0, 1)(rng) != 0) {
          ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        } else {
          ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
      }
      rhs.push_back(ch);
    }
    EXPECT_EQ(CaseInsensitiveLess(lhs, rhs), ReferenceCaseInsensitiveLess(lhs, rhs));
  }
}

}  // namespace aeronet