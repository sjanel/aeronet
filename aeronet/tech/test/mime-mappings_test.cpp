// Unit tests for mime-mappings.hpp
// Covers: kMIMEMappings contents, sorting, uniqueness, and DetermineMIMETypeIdx behavior

#include "aeronet/mime-mappings.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace aeronet;

TEST(MIMEMappings, ContainsKnownExtension) {
  // Spot-check several known mappings
  auto idx = DetermineMIMETypeIdx("file.html");
  ASSERT_NE(idx, kUnknownMIMEMappingIdx);
  if (idx != kUnknownMIMEMappingIdx) {
    EXPECT_EQ(kMIMEMappings[idx].mimeType, "text/html");
  }

  idx = DetermineMIMETypeIdx("image.jpeg");
  ASSERT_NE(idx, kUnknownMIMEMappingIdx);
  if (idx != kUnknownMIMEMappingIdx) {
    EXPECT_EQ(kMIMEMappings[idx].mimeType, "image/jpeg");
  }

  idx = DetermineMIMETypeIdx("script.js");
  ASSERT_NE(idx, kUnknownMIMEMappingIdx);
  if (idx != kUnknownMIMEMappingIdx) {
    EXPECT_EQ(kMIMEMappings[idx].mimeType, "text/javascript");
  }
}

TEST(MIMEMappings, UnknownExtension) {
  EXPECT_EQ(DetermineMIMETypeIdx("file.unknownext"), kUnknownMIMEMappingIdx);
  EXPECT_EQ(DetermineMIMETypeIdx("file.00a"), kUnknownMIMEMappingIdx);
  EXPECT_EQ(DetermineMIMETypeIdx("file.zzz"), kUnknownMIMEMappingIdx);
}

TEST(MIMEMappings, CaseInsensitiveExtensions) {
  // DetermineMIMETypeIdx should handle mixed-case extensions
  auto idx1 = DetermineMIMETypeIdx("UPPER.HTML");
  auto idx2 = DetermineMIMETypeIdx("upper.html");
  EXPECT_EQ(idx1, idx2);
}

TEST(MIMEMappings, MultiDotFilenames) {
  auto idx = DetermineMIMETypeIdx("archive.tar.gz");
  ASSERT_NE(idx, kUnknownMIMEMappingIdx);
  if (idx != kUnknownMIMEMappingIdx) {
    EXPECT_EQ(kMIMEMappings[idx].mimeType, "application/gzip");
  }
}

TEST(MIMEMappings, SortedAndUnique) {
  // Ensure kMIMEMappings is sorted by extension and contains no duplicates
  std::size_t nbMappings = std::size(kMIMEMappings);
  for (std::size_t i = 1; i < nbMappings; ++i) {
    const auto &prev = kMIMEMappings[i - 1].extension;
    const auto &cur = kMIMEMappings[i].extension;
    EXPECT_LT(prev, cur) << "Mappings not strictly increasing at index " << (i - 1);
  }
}

TEST(MIMEMappingsTest, CommonExtensions) {
  EXPECT_EQ(std::string(DetermineMIMETypeStr("sample.md")), "text/markdown");
  EXPECT_EQ(std::string(DetermineMIMETypeStr("archive.tar.gz")), "application/gzip");
  EXPECT_EQ(std::string(DetermineMIMETypeStr("index.HTML")), "text/html");
  EXPECT_EQ(std::string(DetermineMIMETypeStr("UPPER.TXT")), "text/plain");
}

TEST(MIMEMappingsTest, EdgeCases) {
  // No dot -> unknown
  EXPECT_TRUE(DetermineMIMETypeStr("file").empty());

  // Trailing dot -> unknown
  EXPECT_TRUE(DetermineMIMETypeStr("file.").empty());

  // Hidden files starting with a dot: extension part exists but typically not mapped
  EXPECT_TRUE(DetermineMIMETypeStr(".bashrc").empty());
}

TEST(MIMEMappingsTest, MaxExtensionLengthBehavior) {
  // Determine the maximum extension length from the mapping table
  std::size_t maxLen = 0;
  std::size_t maxIdx = 0;
  for (std::size_t i = 0; i < std::size(kMIMEMappings); ++i) {
    if (kMIMEMappings[i].extension.size() > maxLen) {
      maxLen = kMIMEMappings[i].extension.size();
      maxIdx = i;
    }
  }

  // A path that uses the longest known extension should map to that MIME type
  const std::string pathWithMaxExt = std::string("test.") + std::string(kMIMEMappings[maxIdx].extension);
  EXPECT_EQ(std::string(DetermineMIMETypeStr(pathWithMaxExt)), std::string(kMIMEMappings[maxIdx].mimeType));

  // An extension longer than the maximum known size must be ignored and return empty
  const std::string longExt(maxLen + 1, 'x');
  EXPECT_TRUE(DetermineMIMETypeStr(std::string("file.") + longExt).empty());
}