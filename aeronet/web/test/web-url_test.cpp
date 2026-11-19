// The URL helpers of the pages: DocumentUrl (the URL of a document, with its origin as prefix), SplitUrl().
#include "../src/web-url.hpp"

#include <gtest/gtest.h>

#include <string_view>

#include "../src/document-url.hpp"

namespace aeronet::internal {
namespace {

TEST(WebUrlTest, DocumentUrlOrigin) {
  DocumentUrl url("https://example.com:8443/a/b?q=1#top");
  EXPECT_EQ(url.view(), "https://example.com:8443/a/b?q=1#top");
  EXPECT_EQ(url.origin(), "https://example.com:8443");
  EXPECT_EQ(std::string_view(url.c_str()), url.view());  // null-terminated
  url.assign("https://example.com:8443/");
  EXPECT_EQ(url.view(), "https://example.com:8443/");
  EXPECT_EQ(url.origin(), "https://example.com:8443");
  EXPECT_EQ(DocumentUrl("http://[::1]:8080/x").origin(), "http://[::1]:8080");
  EXPECT_EQ(DocumentUrl("http://example.com/").origin(), "http://example.com");
}

TEST(WebUrlTest, DocumentUrlSameOrigin) {
  const DocumentUrl url("https://example.com:8443/page");
  EXPECT_TRUE(url.isSameOrigin("https://example.com:8443/"));
  EXPECT_TRUE(url.isSameOrigin("https://example.com:8443/other?x#y"));
  // Another port, host, scheme, or user information.
  EXPECT_FALSE(url.isSameOrigin("https://example.com:84430/"));
  EXPECT_FALSE(url.isSameOrigin("https://example.com/"));
  EXPECT_FALSE(url.isSameOrigin("http://example.com:8443/"));
  EXPECT_FALSE(url.isSameOrigin("https://user@example.com:8443/"));
  EXPECT_FALSE(url.isSameOrigin("https://example.com:8443"));
  EXPECT_FALSE(url.isSameOrigin(""));
  // A host that extends the one of the origin.
  EXPECT_FALSE(DocumentUrl("https://example.com/").isSameOrigin("https://example.com.evil/"));
}

}  // namespace
}  // namespace aeronet::internal
