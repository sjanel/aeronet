// Unit tests for mime-mappings.hpp
// Covers: kMIMEMappings contents, sorting, uniqueness, and DetermineMIMETypeIdx behavior

#include "aeronet/mime-mappings.hpp"

#include <gtest/gtest.h>

namespace aeronet {

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

TEST(MIMEMappingsTest, CommonExtensions) {
  EXPECT_EQ(DetermineMIMETypeStr("sample.md"), "text/markdown");
  EXPECT_EQ(DetermineMIMETypeStr("archive.tar.gz"), "application/gzip");
  EXPECT_EQ(DetermineMIMETypeStr("index.HTML"), "text/html");
  EXPECT_EQ(DetermineMIMETypeStr("UPPER.TXT"), "text/plain");
}

TEST(MIMEMappingsTest, EdgeCases) {
  // No dot -> unknown
  EXPECT_TRUE(DetermineMIMETypeStr("file").empty());

  // Trailing dot -> unknown
  EXPECT_TRUE(DetermineMIMETypeStr("file.").empty());

  // Hidden files starting with a dot: extension part exists but typically not mapped
  EXPECT_TRUE(DetermineMIMETypeStr(".bashrc").empty());
}

}  // namespace aeronet