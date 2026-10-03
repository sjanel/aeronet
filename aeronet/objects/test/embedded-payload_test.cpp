#include "aeronet/embedded-payload.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/file-payload.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet {

static_assert(EmbeddedPayload::Offset(0) == 0);
static_assert(EmbeddedPayload::Offset(1) == EmbeddedPayload::kAlign);
static_assert(EmbeddedPayload::Offset(EmbeddedPayload::kAlign) == EmbeddedPayload::kAlign);
static_assert(EmbeddedPayload::Footprint(0) == sizeof(HttpPayload));
static_assert(EmbeddedPayload::Footprint(1) == EmbeddedPayload::kMaxFootprint);
static_assert(EmbeddedPayload::Footprint(EmbeddedPayload::kAlign) == sizeof(HttpPayload));

namespace {

const std::string& LongBody() {
  static const std::string kLongBody(200, 'L');
  return kLongBody;
}

struct PayloadCase {
  std::function<HttpPayload()> make;
  std::string_view expected;
  bool allowsBufferReallocation;
};

// Covers both the trivially relocatable alternatives (memcpy) and the others (move + destroy, e.g. SSO std::string).
std::vector<PayloadCase> PayloadCases() {
  return {
      {[] { return HttpPayload(std::string("short")); }, "short", false},
      {[] { return HttpPayload(std::string(LongBody())); }, LongBody(), false},
      {[] { return HttpPayload(std::vector<char>{'v', 'e', 'c'}); }, "vec", false},
      {[] { return HttpPayload(std::string_view("static")); }, "static", true},
      {[] { return HttpPayload(RawChars("raw chars")); }, "raw chars", true},
  };
}

RawChars MakeHead(std::size_t headSize) {
  RawChars head(headSize);
  for (std::size_t pos = 0; pos < headSize; ++pos) {
    head.push_back(static_cast<char>('a' + (pos % 26)));
  }
  return head;
}

}  // namespace

TEST(EmbeddedPayloadTest, EmplaceGetReleaseForEveryPadding) {
  for (const PayloadCase& payloadCase : PayloadCases()) {
    for (std::size_t headSize = 0; headSize <= 2 * EmbeddedPayload::kAlign; ++headSize) {
      RawChars buf = MakeHead(headSize);
      const RawChars expectedHead = buf;

      HttpPayload* pPayload = EmbeddedPayload::Emplace(buf, payloadCase.make());
      ASSERT_EQ(buf.size(), EmbeddedPayload::Offset(headSize) + sizeof(HttpPayload));
      EXPECT_EQ(buf.size() - headSize, EmbeddedPayload::Footprint(headSize));
      EXPECT_EQ(pPayload, EmbeddedPayload::Get(buf));
      EXPECT_EQ(EmbeddedPayload::Get(std::as_const(buf))->view(), payloadCase.expected);
      EXPECT_EQ(std::string_view(buf.data(), headSize), std::string_view(expectedHead));
      EXPECT_EQ(EmbeddedPayload::AllowsBufferReallocation(*pPayload), payloadCase.allowsBufferReallocation);

      HttpPayload released = EmbeddedPayload::Release(buf, headSize);
      EXPECT_EQ(released.view(), payloadCase.expected);
      EXPECT_EQ(std::string_view(buf), std::string_view(expectedHead));
    }
  }
}

TEST(EmbeddedPayloadTest, EmplaceNoReallocCopiesAndDestroy) {
  for (const PayloadCase& payloadCase : PayloadCases()) {
    const HttpPayload source = payloadCase.make();
    RawChars buf = MakeHead(3);
    buf.ensureAvailableCapacity(EmbeddedPayload::Footprint(buf.size()));
    const char* pData = buf.data();

    EmbeddedPayload::EmplaceNoRealloc(buf, source);
    EXPECT_EQ(buf.data(), pData);
    EXPECT_EQ(EmbeddedPayload::Get(buf)->view(), payloadCase.expected);
    EXPECT_EQ(source.view(), payloadCase.expected);

    // Destroy must release the resources of the payload (leak sanitizer checks the heap ones).
    EmbeddedPayload::Destroy(buf, 3);
    EXPECT_EQ(std::string_view(buf), "abc");
  }
}

TEST(EmbeddedPayloadTest, RelocateKeepsPayloadUsable) {
  for (const PayloadCase& payloadCase : PayloadCases()) {
    RawChars src;
    EmbeddedPayload::Emplace(src, payloadCase.make());

    // Relocation to a raw storage, then back at the end of another buffer for every padding.
    alignas(HttpPayload) std::byte storage[sizeof(HttpPayload)];
    EmbeddedPayload::Relocate(*EmbeddedPayload::Get(src), storage);
    src.setSize(0);  // the payload object does not live in 'src' anymore
    HttpPayload* pStored = std::launder(reinterpret_cast<HttpPayload*>(storage));
    EXPECT_EQ(pStored->view(), payloadCase.expected);

    for (std::size_t headSize = 0; headSize <= EmbeddedPayload::kAlign; ++headSize) {
      RawChars dst = MakeHead(headSize);
      dst.ensureAvailableCapacity(EmbeddedPayload::kMaxFootprint);
      EmbeddedPayload::RelocateNoRealloc(dst, *pStored);
      EXPECT_EQ(EmbeddedPayload::Get(dst)->view(), payloadCase.expected);

      EmbeddedPayload::Relocate(*EmbeddedPayload::Get(dst), storage);
      dst.setSize(headSize);
      pStored = std::launder(reinterpret_cast<HttpPayload*>(storage));
    }
    std::destroy_at(pStored);
  }
}

TEST(EmbeddedPayloadTest, FilePayloadAllowsBufferReallocation) {
  RawChars buf;
  const HttpPayload* pPayload = EmbeddedPayload::Emplace(buf, HttpPayload(FilePayload{File{}, 2, 42}));
  ASSERT_NE(pPayload->getIfFilePayload(), nullptr);
  EXPECT_EQ(pPayload->getIfFilePayload()->length, 42U);
  EXPECT_TRUE(EmbeddedPayload::AllowsBufferReallocation(*pPayload));
  EmbeddedPayload::Destroy(buf, 0);
  EXPECT_TRUE(buf.empty());
}

}  // namespace aeronet
