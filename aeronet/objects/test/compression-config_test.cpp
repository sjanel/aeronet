#include "aeronet/compression-config.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include "aeronet/encoding.hpp"

namespace aeronet {

TEST(CompressionConfigTest, DefaultIsValid) {
  CompressionConfig config;

  EXPECT_NO_THROW(config.validate());
}

TEST(CompressionConfigTest, MinBytesZeroThrows) {
  CompressionConfig config;
  config.minBytes = 0;

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(CompressionConfigTest, InitialCompressionBufferLimitZeroThrows) {
  CompressionConfig config;
  config.initialCompressionBufferLimit = 0;

  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(CompressionConfigTest, InvalidPreferredFormatsThrows) {
  CompressionConfig config;
  config.preferredFormats.push_back(std::bit_cast<Encoding>(static_cast<std::underlying_type_t<Encoding>>(-1)));
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

#ifdef AERONET_ENABLE_ZLIB
TEST(CompressionConfigTest, DuplicatesInPreferredFormatsThrows) {
  CompressionConfig config;
  config.preferredFormats.push_back(Encoding::gzip);
  config.preferredFormats.push_back(Encoding::gzip);
  EXPECT_THROW(config.validate(), std::invalid_argument);
}
#endif

TEST(CompressionConfigTest, ZlibOK) {
  CompressionConfig config;
  config.zlib.level = 5;

  EXPECT_NO_THROW(config.validate());
}

#ifdef AERONET_ENABLE_ZLIB
TEST(CompressionConfigTest, ZlibInvalidLevelThrows) {
  CompressionConfig config;
  config.zlib.level = 17;

  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.zlib.level = 0;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}
#endif

TEST(CompressionConfigTest, ZstdOK) {
  CompressionConfig config;
  config.zstd.compressionLevel = 3;
  config.zstd.windowLog = 20;

  EXPECT_NO_THROW(config.validate());
}

#ifdef AERONET_ENABLE_ZSTD

TEST(CompressionConfigTest, ZstdInvalidLevelThrows) {
  CompressionConfig config;

  config.zstd.compressionLevel = 29;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

#endif

TEST(CompressionConfigTest, BrotliOK) {
  CompressionConfig config;
  config.brotli.quality = 6;
  config.brotli.window = 22;

  EXPECT_NO_THROW(config.validate());
}

TEST(CompressionConfigTest, NonFiniteMaxCompressRatioThrows) {
  CompressionConfig config;
  using T = std::remove_cvref_t<decltype(config.maxCompressRatio)>;

  config.maxCompressRatio = std::numeric_limits<T>::infinity();
  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.maxCompressRatio = -std::numeric_limits<T>::infinity();
  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.maxCompressRatio = std::numeric_limits<T>::quiet_NaN();
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(CompressionConfigTest, BoundaryMaxCompressRatioThrows) {
  CompressionConfig config;
  config.maxCompressRatio = 0.0;
  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.maxCompressRatio = 1.0;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

#ifdef AERONET_ENABLE_BROTLI
TEST(CompressionConfigTest, BrotliInvalidQualityThrows) {
  CompressionConfig config;
  config.brotli.quality = 56;

  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.brotli.quality = -1;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(CompressionConfigTest, BrotliInvalidWindowThrows) {
  CompressionConfig config;
  config.brotli.window = 42;

  EXPECT_THROW(config.validate(), std::invalid_argument);

  config.brotli.window = -4;
  EXPECT_THROW(config.validate(), std::invalid_argument);
}
#endif

TEST(CompressionConfigTest, EmptyAllowListAllowsEveryContentType) {
  const CompressionConfig config;
  EXPECT_TRUE(config.isContentTypeAllowed("image/png"));
  EXPECT_TRUE(config.isContentTypeAllowed(""));
}

TEST(CompressionConfigTest, AllowListComparesMediaTypesOnly) {
  CompressionConfig config;
  config.contentTypeAllowList.append("text/html");
  config.contentTypeAllowList.append(" Application/JSON ; charset=utf-8");
  for (std::string_view contentType : {
           "text/html",
           "TEXT/HTML",
           "text/html; charset=utf-8",
           " text/html ;charset=utf-8",
           "text/html;",
           "application/json",
           "application/json; charset=latin1",
           "application/json;q=1;x=y",
       }) {
    EXPECT_TRUE(config.isContentTypeAllowed(contentType)) << contentType;
  }
  for (std::string_view contentType : {
           "text/htm",
           "text/html2",
           "text/plain; charset=utf-8",
           "application/json+x",
           "image/png",
           "",
           "; text/html",
           "text /html",
       }) {
    EXPECT_FALSE(config.isContentTypeAllowed(contentType)) << contentType;
  }
}

// The whole media type of an entry must match: what follows the characters compared is only whitespace or parameters.
TEST(CompressionConfigTest, AllowListEntriesMatchOnTheirWholeMediaType) {
  CompressionConfig config;
  config.contentTypeAllowList.append("text/htmlx");
  config.contentTypeAllowList.append("text");
  config.contentTypeAllowList.append("image/svg+xml \t; x=\"a;b\"");
  for (std::string_view contentType :
       {"text/htmlx", "TEXT", "text; charset=utf-8", "image/svg+xml", "Image/SVG+XML;"}) {
    EXPECT_TRUE(config.isContentTypeAllowed(contentType)) << contentType;
  }
  for (std::string_view contentType : {"text/html", "text/htm", "tex", "image/svg", "image/svg+xml2", "x"}) {
    EXPECT_FALSE(config.isContentTypeAllowed(contentType)) << contentType;
  }
}

}  // namespace aeronet