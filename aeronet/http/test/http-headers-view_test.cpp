#include "aeronet/http-headers-view.hpp"

#include <gtest/gtest.h>

#include <iterator>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-helpers.hpp"

namespace aeronet {

TEST(HttpHeadersView, DefaultConstructor) {
  HeadersView view;

  EXPECT_EQ(view.begin(), view.end());
  EXPECT_EQ(HeadersView::iterator(), HeadersView::iterator());
}

TEST(HttpHeadersView, SingleHeader) {
  const auto rawHeaders = MakeHttp1HeaderLine(http::ContentType, "text/plain");
  HeadersView view(rawHeaders);

  auto it = view.begin();
  EXPECT_NE(it, view.end());
  http::HeaderView header = *it;
  EXPECT_EQ(header.name, http::ContentType);
  EXPECT_EQ(header.value, "text/plain");

  ++it;
  EXPECT_EQ(it, view.end());
}

TEST(HttpHeadersView, LoopOnHeaders) {
  auto rawHeaders = MakeHttp1HeaderLine("Header-1", "Value1");
  rawHeaders.append(MakeHttp1HeaderLine("Header-2", "Value2"));
  rawHeaders.append(MakeHttp1HeaderLine("Header-3", "Value3"));

  HeadersView headers(rawHeaders);
  EXPECT_EQ(std::distance(headers.begin(), headers.end()), 3);

  auto it = headers.begin();
  EXPECT_EQ((*it).name, "Header-1");
  EXPECT_EQ((*it).value, "Value1");

  EXPECT_EQ((*it).name, "Header-1");
  EXPECT_EQ((*it++).value, "Value1");  // test post-increment
  EXPECT_EQ((*it).name, "Header-2");
  EXPECT_EQ((*it).value, "Value2");
  ++it;
  EXPECT_EQ((*it).name, "Header-3");
  EXPECT_EQ((*it).value, "Value3");
  EXPECT_EQ(++it, headers.end());
}

}  // namespace aeronet