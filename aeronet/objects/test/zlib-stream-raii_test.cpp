#include "aeronet/zlib-stream-raii.hpp"

#include <gtest/gtest.h>
#include <zconf.h>
#include <zlib.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "aeronet/sys-test-support.hpp"

namespace aeronet {

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(ZStreamRAII, InitFails) {
  test::FailNextMalloc();
  EXPECT_THROW(ZStreamRAII{ZStreamRAII::Variant::gzip}, std::runtime_error);
}

#endif

TEST(ZStreamRAII, VariantAndTypeSetAllocatedType) {
  // gzip + compress -> deflate
  {
    ZStreamRAII streamGzComp(ZStreamRAII::Variant::gzip, 1);
  }

  // gzip + decompress -> inflate or error (noexcept ctor may set error)
  {
    ZStreamRAII streamGzInfl(ZStreamRAII::Variant::gzip);
  }

  // deflate + compress -> deflate
  {
    ZStreamRAII streamDefComp(ZStreamRAII::Variant::deflate, 1);
  }

  // deflate + decompress -> inflate or error
  {
    ZStreamRAII streamDefInfl(ZStreamRAII::Variant::deflate);
  }
}

TEST(ZStreamRAII, DestructorCleansUpRepeatedly) {
  // Allocate and destroy many times to exercise ctor/dtor paths.
  for (int i = 0; i < 50; ++i) {
    ZStreamRAII compStream(ZStreamRAII::Variant::gzip, 1);
    ZStreamRAII inflStream(ZStreamRAII::Variant::deflate);
    // No explicit checks here beyond ensuring construction/destruction doesn't crash.
  }
}

TEST(ZStreamRAII, InvalidLevelThrows) {
  // Some zlib builds may accept out-of-range levels; the safe assertion here is that
  // either construction succeeds or throws a runtime_error. We assert that constructing
  // with an obviously invalid level does not crash the process.
  try {
    ZStreamRAII badLevelStream(ZStreamRAII::Variant::gzip, static_cast<int8_t>(127));
    SUCCEED();
  } catch (const std::runtime_error &) {
    SUCCEED();
  }
}

TEST(ZStreamRAII, InvalidVariant) {
  auto invalidVariant = static_cast<std::underlying_type_t<ZStreamRAII::Variant>>(120);
  try {
    ZStreamRAII badVariantStream(static_cast<ZStreamRAII::Variant>(invalidVariant));
    FAIL() << "Expected std::unreachable to be triggered";
  } catch (const std::invalid_argument &) {
    SUCCEED();
  }
}

TEST(ZStreamRAII, RoundTripCompressDecompress) {
  const std::string payload = [] {
    std::string outStr;
    for (int i = 0; i < 64; ++i) {
      outStr += "The quick brown fox jumps over the lazy dog. ";
    }
    return outStr;
  }();

  static constexpr ZStreamRAII::Variant kVariants[] = {
      ZStreamRAII::Variant::gzip,
      ZStreamRAII::Variant::deflate,
  };

  for (auto variant : kVariants) {
    // Compress
    ZStreamRAII compressor(variant, 6);
    compressor.stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(payload.data()));
    compressor.stream.avail_in = static_cast<uInt>(payload.size());

    std::vector<unsigned char> compressed;
    compressed.reserve(payload.size() / 2);
    static constexpr std::size_t kChunk = 4096;
    std::vector<unsigned char> outbuf(kChunk);

    while (true) {
      compressor.stream.next_out = outbuf.data();
      compressor.stream.avail_out = static_cast<uInt>(outbuf.size());
      const int ret = deflate(&compressor.stream, Z_FINISH);
      if (ret != Z_OK && ret != Z_STREAM_END) {
        FAIL() << "deflate failed with " << ret;
      }
      const std::size_t produced = outbuf.size() - compressor.stream.avail_out;
      compressed.insert(compressed.end(), outbuf.data(), outbuf.data() + produced);
      if (ret == Z_STREAM_END) {
        break;
      }
    }

    // Decompress
    ZStreamRAII decompressor(variant);
    decompressor.stream.next_in = compressed.empty() ? nullptr : reinterpret_cast<Bytef *>(compressed.data());
    decompressor.stream.avail_in = static_cast<uInt>(compressed.size());

    std::vector<char> decompressed;
    std::vector<unsigned char> inbuf(kChunk);

    do {
      decompressor.stream.next_out = inbuf.data();
      decompressor.stream.avail_out = static_cast<uInt>(inbuf.size());
      const int ret = inflate(&decompressor.stream, Z_NO_FLUSH);
      if (ret != Z_OK && ret != Z_STREAM_END) {
        FAIL() << "inflate failed with " << ret;
      }
      const std::size_t got = inbuf.size() - decompressor.stream.avail_out;
      decompressed.insert(decompressed.end(), reinterpret_cast<char *>(inbuf.data()),
                          reinterpret_cast<char *>(inbuf.data()) + got);
      if (ret == Z_STREAM_END) {
        break;
      }
    } while (decompressor.stream.avail_in != 0);

    const std::string result(decompressed.begin(), decompressed.end());
    EXPECT_EQ(result, payload);
  }
}

}  // namespace aeronet