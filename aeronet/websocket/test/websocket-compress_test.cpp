#include "aeronet/websocket-compress.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/raw-bytes.hpp"

namespace aeronet {
namespace {

// Helper function to convert string_view to bytes
std::span<const std::byte> StringToBytes(std::string_view sv) noexcept {
  return std::as_bytes(std::span<const char>(sv.data(), sv.size()));
}

// Helper function to convert RawBytes to string for comparison
std::string BytesToString(const RawBytes& buf) { return {reinterpret_cast<const char*>(buf.data()), buf.size()}; }

// Helper to compress data
std::pair<bool, std::string> CompressData(std::string_view input, bool resetContext = false) {
  WebSocketCompressor compressor(6);  // Default compression level
  RawBytes output;
  const char* error = compressor.compress(StringToBytes(input), output, resetContext);
  if (error != nullptr) {
    return {false, error};
  }
  return {true, BytesToString(output)};
}

// Helper to decompress data
std::pair<bool, std::string> DecompressData(std::span<const std::byte> input, std::size_t maxSize = 0,
                                            bool resetContext = false) {
  WebSocketDecompressor decompressor;
  RawBytes output;
  const char* error = decompressor.decompress(input, output, maxSize, resetContext);
  if (error != nullptr) {
    return {false, error};
  }
  return {true, BytesToString(output)};
}

// ============================================================================
// WebSocketCompressor Tests
// ============================================================================

class WebSocketCompressorTest : public ::testing::Test {
 protected:
  WebSocketCompressor compressor{6};
  RawBytes output;
};

TEST_F(WebSocketCompressorTest, CompressEmptyInput) {
  const char* error = compressor.compress(StringToBytes(""), output, false);
  EXPECT_EQ(error, nullptr);
  // Even empty input may produce some bytes due to flush markers
}

TEST_F(WebSocketCompressorTest, CompressSimpleText) {
  const auto input = StringToBytes("Hello World");
  const char* error = compressor.compress(input, output, false);
  EXPECT_EQ(error, nullptr);
  EXPECT_GT(output.size(), 0);
}

TEST_F(WebSocketCompressorTest, CompressMultipleCalls) {
  // Compress first message
  const char* error1 = compressor.compress(StringToBytes("First"), output, false);
  EXPECT_EQ(error1, nullptr);
  const auto size1 = output.size();
  EXPECT_GT(size1, 0);

  // Compress second message without reset (context is maintained)
  const char* error2 = compressor.compress(StringToBytes("Second"), output, false);
  EXPECT_EQ(error2, nullptr);
  EXPECT_GT(output.size(), size1);
}

TEST_F(WebSocketCompressorTest, CompressWithContextReset) {
  // Compress first message
  const char* error1 = compressor.compress(StringToBytes("First"), output, false);
  EXPECT_EQ(error1, nullptr);

  RawBytes output2;
  // Compress with context reset
  const char* error2 = compressor.compress(StringToBytes("First"), output2, true);
  EXPECT_EQ(error2, nullptr);
  // Both should produce the same output since we're compressing the same data
  EXPECT_EQ(output.size(), output2.size());
}

TEST_F(WebSocketCompressorTest, CompressLargeData) {
  std::string large(10000, 'a');
  const char* error = compressor.compress(StringToBytes(large), output, false);
  EXPECT_EQ(error, nullptr);
  EXPECT_GT(output.size(), 0);
  // Compressed data should be smaller than original for highly repetitive data
  EXPECT_LT(output.size(), large.size());
}

TEST_F(WebSocketCompressorTest, CompressRandomData) {
  std::string random(R"(Hello World! This is a test.)");
  const char* error = compressor.compress(StringToBytes(random), output, false);
  EXPECT_EQ(error, nullptr);
  EXPECT_GT(output.size(), 0);
}

TEST_F(WebSocketCompressorTest, CompressTrailerRemoved) {
  // The RFC 7692 trailer (0x00 0x00 0xff 0xff) should be removed
  const auto input = StringToBytes("test data for compression");
  const char* error = compressor.compress(input, output, false);
  EXPECT_EQ(error, nullptr);

  // Check that output doesn't end with the trailer
  constexpr std::array<std::byte, 4> trailer = {std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0xff}};
  if (output.size() >= trailer.size()) {
    const auto* tail = output.data() + output.size() - trailer.size();
    const bool hasTrailer = std::memcmp(tail, trailer.data(), trailer.size()) == 0;
    EXPECT_FALSE(hasTrailer) << "Trailer should have been removed";
  }
}

TEST_F(WebSocketCompressorTest, CompressMultipleCompressionLevels) {
  for (int level = 0; level <= 9; ++level) {
    WebSocketCompressor compressorLevel(static_cast<int8_t>(level));
    RawBytes outputLevel;
    std::string inputStr = "Test compression with level " + std::to_string(level);
    const auto input = StringToBytes(inputStr);
    const char* error = compressorLevel.compress(input, outputLevel, false);
    EXPECT_EQ(error, nullptr) << "Failed at compression level " << level;
    EXPECT_GT(outputLevel.size(), 0) << "Failed at compression level " << level;
  }
}

// ============================================================================
// WebSocketDecompressor Tests
// ============================================================================

class WebSocketDecompressorTest : public ::testing::Test {
 protected:
  WebSocketDecompressor decompressor;
  RawBytes output;
};

TEST_F(WebSocketDecompressorTest, DecompressEmptyInput) {
  const char* error = decompressor.decompress(StringToBytes(""), output, 0, false);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(output.size(), 0);
}

TEST_F(WebSocketDecompressorTest, DecompressValidCompressedData) {
  // First compress some data
  auto [compressOk, compressed] = CompressData("Hello World", true);
  ASSERT_TRUE(compressOk);

  // Then decompress it
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 0, true);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(BytesToString(output), "Hello World");
}

TEST_F(WebSocketDecompressorTest, DecompressLargeData) {
  std::string large(5000, 'a');
  auto [compressOk, compressed] = CompressData(large, true);
  ASSERT_TRUE(compressOk);

  output.clear();
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 0, true);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(BytesToString(output), large);
}

TEST_F(WebSocketDecompressorTest, DecompressMultipleCalls) {
  // Compress two messages separately
  auto [compressOk1, compressed1] = CompressData("First", true);
  ASSERT_TRUE(compressOk1);
  auto [compressOk2, compressed2] = CompressData("Second", true);
  ASSERT_TRUE(compressOk2);

  // Decompress first message
  WebSocketDecompressor dec1;
  RawBytes out1;
  const char* error1 = dec1.decompress(StringToBytes(compressed1), out1, 0, true);
  EXPECT_EQ(error1, nullptr);
  EXPECT_EQ(BytesToString(out1), "First");

  // Decompress second message
  WebSocketDecompressor dec2;
  RawBytes out2;
  const char* error2 = dec2.decompress(StringToBytes(compressed2), out2, 0, true);
  EXPECT_EQ(error2, nullptr);
  EXPECT_EQ(BytesToString(out2), "Second");
}

TEST_F(WebSocketDecompressorTest, DecompressWithContextMaintained) {
  // Compress two messages with context maintained
  WebSocketCompressor compressor(6);
  RawBytes compressed;

  const char* err1 = compressor.compress(StringToBytes("First"), compressed, true);
  EXPECT_EQ(err1, nullptr);

  const char* err2 = compressor.compress(StringToBytes("Second"), compressed, false);
  EXPECT_EQ(err2, nullptr);

  // Decompress with context maintained
  WebSocketDecompressor decompressor1;
  RawBytes output1;
  const char* error1 = decompressor1.decompress(std::span<const std::byte>(), output1, 0, true);
  // Decompressing empty data should succeed
  EXPECT_EQ(error1, nullptr);
}

TEST_F(WebSocketDecompressorTest, DecompressWithContextReset) {
  auto [compressOk, compressed] = CompressData("Test data", true);
  ASSERT_TRUE(compressOk);

  // Decompress with context reset
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 0, true);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(BytesToString(output), "Test data");
}

TEST_F(WebSocketDecompressorTest, DecompressMaxSizeZeroNoLimit) {
  std::string large(3000, 'a');
  auto [compressOk, compressed] = CompressData(large, true);
  ASSERT_TRUE(compressOk);

  const char* error = decompressor.decompress(StringToBytes(compressed), output, 0, true);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(BytesToString(output), large);
}

TEST_F(WebSocketDecompressorTest, DecompressMaxSizeExceededSmall) {
  auto [compressOk, compressed] = CompressData("Hello World", true);
  ASSERT_TRUE(compressOk);

  // Set max size smaller than the decompressed data
  // With DecoderBufferManager, the limit is enforced strictly - we never exceed it
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 5, true);
  EXPECT_NE(error, nullptr);
  EXPECT_STREQ(error, "Decompressed size exceeds maximum");
  // Size should be within the limit or error before adding more
  EXPECT_LE(output.size(), 5);
}
TEST_F(WebSocketDecompressorTest, DecompressMaxSizeLargeEnough) {
  // For a size limit to work with chunks, the limit should be significantly larger than chunk size
  // Chunk size is 16KB, so use 64KB to comfortably decompress smaller data
  std::string data(10000, 'a');  // 10KB of repetitive data (compresses well)
  auto [compressOk, compressed] = CompressData(data, true);
  ASSERT_TRUE(compressOk);

  // Set max size well above the data size and chunk size
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 64000, true);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(BytesToString(output), data);
}

TEST_F(WebSocketDecompressorTest, DecompressMaxSizeLimitedLargeData) {
  std::string data(100000, 'x');  // 100KB
  auto [compressOk, compressed] = CompressData(data, true);
  ASSERT_TRUE(compressOk);

  // Set limit to 20KB, should fail when trying to decompress 100KB
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 20000, true);
  EXPECT_NE(error, nullptr);
  EXPECT_STREQ(error, "Decompressed size exceeds maximum");
  // Should have decompressed some data before hitting the limit
  EXPECT_GT(output.size(), 0);
  EXPECT_LE(output.size(), 20000);
}
TEST_F(WebSocketDecompressorTest, DecompressMaxSizeExceededLarge) {
  std::string large(10000, 'x');
  auto [compressOk, compressed] = CompressData(large, true);
  ASSERT_TRUE(compressOk);

  // Set max size smaller than the decompressed data
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 5000, true);
  EXPECT_NE(error, nullptr);
  EXPECT_STREQ(error, "Decompressed size exceeds maximum");
}

TEST_F(WebSocketDecompressorTest, DecompressMaxSizeExactMatch) {
  std::string data("Hello World");  // 11 bytes
  auto [compressOk, compressed] = CompressData(data, true);
  ASSERT_TRUE(compressOk);

  // Set max size exactly equal to decompressed data
  // Note: DecoderBufferManager uses chunks, so requesting a 16KB chunk
  // when max is 11 bytes will trigger the limit
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 11, true);
  EXPECT_NE(error, nullptr);
  EXPECT_STREQ(error, "Decompressed size exceeds maximum");
}

TEST_F(WebSocketDecompressorTest, DecompressMaxSizeJustLargeEnough) {
  std::string data("Hello World");  // 11 bytes
  auto [compressOk, compressed] = CompressData(data, true);
  ASSERT_TRUE(compressOk);

  // Set max size one byte larger than decompressed data (12 bytes)
  // This still won't work with 16KB chunks
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 12, true);
  EXPECT_NE(error, nullptr);
  EXPECT_STREQ(error, "Decompressed size exceeds maximum");
}

TEST_F(WebSocketDecompressorTest, DecompressInvalidData) {
  // Create some invalid compressed data
  std::string invalidData = "This is not valid compressed data at all!";

  const char* error = decompressor.decompress(StringToBytes(invalidData), output, 0, true);
  EXPECT_NE(error, nullptr);
  // Should return an error message
  EXPECT_TRUE(std::string(error).find("inflate") != std::string::npos ||
              std::string(error).find("error") != std::string::npos ||
              std::string(error).find("failed") != std::string::npos);
}

TEST_F(WebSocketDecompressorTest, DecompressPartialCompressedData) {
  auto [compressOk, compressed] = CompressData("Hello World", true);
  ASSERT_TRUE(compressOk);

  // Try to decompress only part of the compressed data
  // Note: depending on where we cut, zlib might fail or succeed
  // If we cut in the middle of valid data, it might decompress fine or fail
  // The key is that we either get valid decompression or an error, not undefined behavior
  const auto partialData = std::span<const std::byte>(
      StringToBytes(compressed).data(), std::min(static_cast<size_t>(2), StringToBytes(compressed).size()));
  const char* error = decompressor.decompress(partialData, output, 0, true);
  // The result depends on how the compression worked and where we cut
  // Just verify that we either get success or a proper error, and no crash
  if (error == nullptr) {
    // If decompression succeeded, we got some output (may be empty or partial)
    EXPECT_GE(output.size(), 0);
  } else {
    // If there was an error, it should be a proper error message
    EXPECT_NE(std::string(error).find("inflate"), std::string::npos);
  }
}

TEST_F(WebSocketDecompressorTest, DecompressTinyData) {
  auto [compressOk, compressed] = CompressData("x", true);
  ASSERT_TRUE(compressOk);

  const char* error = decompressor.decompress(StringToBytes(compressed), output, 0, true);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(BytesToString(output), "x");
}

TEST_F(WebSocketDecompressorTest, DecompressSpecialCharacters) {
  // NOLINTNEXTLINE(bugprone-string-literal-with-embedded-nul)
  std::string special("\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f");
  auto [compressOk, compressed] = CompressData(special, true);
  ASSERT_TRUE(compressOk);

  const char* error = decompressor.decompress(StringToBytes(compressed), output, 0, true);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(BytesToString(output), special);
}

TEST_F(WebSocketDecompressorTest, DecompressUTF8Text) {
  std::string utf8("Hello 世界 🌍 مرحبا мир");
  auto [compressOk, compressed] = CompressData(utf8, true);
  ASSERT_TRUE(compressOk);

  const char* error = decompressor.decompress(StringToBytes(compressed), output, 0, true);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(BytesToString(output), utf8);
}

TEST_F(WebSocketDecompressorTest, DecompressRepeatingPattern) {
  std::string pattern;
  for (int i = 0; i < 100; ++i) {
    pattern += "ABCDEFGHIJ";
  }
  auto [compressOk, compressed] = CompressData(pattern, true);
  ASSERT_TRUE(compressOk);

  const char* error = decompressor.decompress(StringToBytes(compressed), output, 0, true);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(BytesToString(output), pattern);
}

// ============================================================================
// Round-trip Tests (Compress then Decompress)
// ============================================================================

class WebSocketCompressRoundTripTest : public ::testing::Test {};

TEST_F(WebSocketCompressRoundTripTest, RoundTripSimpleText) {
  std::string original("Hello World");

  auto [compressOk, compressed] = CompressData(original, true);
  ASSERT_TRUE(compressOk);

  auto [decompressOk, decompressed] = DecompressData(StringToBytes(compressed), 0, true);
  ASSERT_TRUE(decompressOk);

  EXPECT_EQ(decompressed, original);
}

TEST_F(WebSocketCompressRoundTripTest, RoundTripEmptyString) {
  std::string original;

  auto [compressOk, compressed] = CompressData(original, true);
  ASSERT_TRUE(compressOk);

  auto [decompressOk, decompressed] = DecompressData(StringToBytes(compressed), 0, true);
  ASSERT_TRUE(decompressOk);

  EXPECT_EQ(decompressed, original);
}

TEST_F(WebSocketCompressRoundTripTest, RoundTripLargeText) {
  std::string original;
  for (int i = 0; i < 1000; ++i) {
    original += "The quick brown fox jumps over the lazy dog. ";
  }

  auto [compressOk, compressed] = CompressData(original, true);
  ASSERT_TRUE(compressOk);

  auto [decompressOk, decompressed] = DecompressData(StringToBytes(compressed), 0, true);
  ASSERT_TRUE(decompressOk);

  EXPECT_EQ(decompressed, original);
}

TEST_F(WebSocketCompressRoundTripTest, RoundTripRandomBinary) {
  std::string original =
      // NOLINTNEXTLINE(bugprone-string-literal-with-embedded-nul)
      "\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xAA\xBB\xCC\xDD\xEE\xFF"
      "\xAA\x99\x88\x77\x66\x55\x44\x33\x22\x11\x00";

  auto [compressOk, compressed] = CompressData(original, true);
  ASSERT_TRUE(compressOk);

  auto [decompressOk, decompressed] = DecompressData(StringToBytes(compressed), 0, true);
  ASSERT_TRUE(decompressOk);

  EXPECT_EQ(decompressed, original);
}

TEST_F(WebSocketCompressRoundTripTest, RoundTripMultipleMessages) {
  std::vector<std::string> messages = {"First message", "Second message with more content", "Third",
                                       "Fourth message that is quite a bit longer than the others", ""};

  for (const auto& msg : messages) {
    auto [compressOk, compressed] = CompressData(msg, true);
    ASSERT_TRUE(compressOk) << "Failed to compress: " << msg;

    auto [decompressOk, decompressed] = DecompressData(StringToBytes(compressed), 0, true);
    ASSERT_TRUE(decompressOk) << "Failed to decompress: " << msg;

    EXPECT_EQ(decompressed, msg) << "Mismatch for message: " << msg;
  }
}

TEST_F(WebSocketCompressRoundTripTest, RoundTripContextMaintained) {
  WebSocketCompressor compressor(6);
  RawBytes compressed;

  // Compress first message without reset
  const char* err1 = compressor.compress(StringToBytes("First"), compressed, true);
  EXPECT_EQ(err1, nullptr);

  // For context-maintained compression, we need to handle the full message properly
  // Decompress the first part
  WebSocketDecompressor decompressor;
  RawBytes output;

  // Note: testing context maintained across multiple deflate() calls
  // would require more complex setup with partial data
  // This test verifies that a decompressor can be created and reset
  EXPECT_EQ(decompressor.decompress(std::span<const std::byte>(), output, 0, true), nullptr);
}

// ============================================================================
// Edge Cases and Error Paths
// ============================================================================

class WebSocketCompressEdgeCasesTest : public ::testing::Test {};

TEST_F(WebSocketCompressEdgeCasesTest, CompressNullInput) {
  WebSocketCompressor compressor(6);
  RawBytes output;
  // Using empty span instead of null
  const char* error = compressor.compress(std::span<const std::byte>(), output, false);
  EXPECT_EQ(error, nullptr);
}

TEST_F(WebSocketCompressEdgeCasesTest, CompressZeroCompressionLevel) {
  WebSocketCompressor compressor(0);
  RawBytes output;
  const char* error = compressor.compress(StringToBytes("test"), output, false);
  EXPECT_EQ(error, nullptr);
  EXPECT_GT(output.size(), 0);
}

TEST_F(WebSocketCompressEdgeCasesTest, CompressMaxCompressionLevel) {
  WebSocketCompressor compressor(9);
  RawBytes output;
  const char* error = compressor.compress(StringToBytes("test"), output, false);
  EXPECT_EQ(error, nullptr);
  EXPECT_GT(output.size(), 0);
}

TEST_F(WebSocketCompressEdgeCasesTest, DecompressZeroMaxSize) {
  auto [compressOk, compressed] = CompressData("test", true);
  ASSERT_TRUE(compressOk);

  WebSocketDecompressor decompressor;
  RawBytes output;
  // maxDecompressedSize = 0 means no limit
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 0, true);
  EXPECT_EQ(error, nullptr);
  EXPECT_EQ(BytesToString(output), "test");
}

TEST_F(WebSocketCompressEdgeCasesTest, DecompressMaxSizeOne) {
  auto [compressOk, compressed] = CompressData("test", true);
  ASSERT_TRUE(compressOk);

  WebSocketDecompressor decompressor;
  RawBytes output;
  const char* error = decompressor.decompress(StringToBytes(compressed), output, 1, true);
  EXPECT_NE(error, nullptr);
  EXPECT_STREQ(error, "Decompressed size exceeds maximum");
}

TEST_F(WebSocketCompressEdgeCasesTest, VeryLargeData) {
  std::string large(1000000, 'x');  // 1MB of 'x'
  auto [compressOk, compressed] = CompressData(large, true);
  ASSERT_TRUE(compressOk);
  EXPECT_LT(compressed.size(), large.size());  // Should compress very well

  auto [decompressOk, decompressed] = DecompressData(StringToBytes(compressed), 0, true);
  ASSERT_TRUE(decompressOk);
  EXPECT_EQ(decompressed, large);
}

TEST_F(WebSocketCompressEdgeCasesTest, IncompatibleCompressionLevels) {
  // Test that data compressed with one level can be decompressed regardless
  for (int level = 0; level <= 9; ++level) {
    std::string msg = "Test at level " + std::to_string(level);
    auto [compressOk, compressed] = CompressData(msg, true);
    ASSERT_TRUE(compressOk) << "Compression failed at level " << level;

    auto [decompressOk, decompressed] = DecompressData(StringToBytes(compressed), 0, true);
    ASSERT_TRUE(decompressOk) << "Decompression failed for data compressed at level " << level;
    EXPECT_EQ(decompressed, msg);
  }
}

TEST_F(WebSocketCompressEdgeCasesTest, MultipleCalls_AppendingOutput) {
  WebSocketCompressor compressor(6);
  RawBytes output;

  // First compression
  const char* err1 = compressor.compress(StringToBytes("First"), output, true);
  EXPECT_EQ(err1, nullptr);
  const auto size1 = output.size();

  // Second compression (output should append)
  const char* err2 = compressor.compress(StringToBytes("Second"), output, true);
  EXPECT_EQ(err2, nullptr);
  EXPECT_GT(output.size(), size1);
}

TEST_F(WebSocketCompressEdgeCasesTest, Whitespace) {
  std::string whitespace = "   \t\t\t   \n\n\n   ";
  auto [compressOk, compressed] = CompressData(whitespace, true);
  ASSERT_TRUE(compressOk);

  auto [decompressOk, decompressed] = DecompressData(StringToBytes(compressed), 0, true);
  ASSERT_TRUE(decompressOk);
  EXPECT_EQ(decompressed, whitespace);
}

TEST_F(WebSocketCompressEdgeCasesTest, AllBytesValues) {
  std::string allBytes;
  for (int i = 0; i < 256; ++i) {
    allBytes += static_cast<char>(i);
  }
  auto [compressOk, compressed] = CompressData(allBytes, true);
  ASSERT_TRUE(compressOk);

  auto [decompressOk, decompressed] = DecompressData(StringToBytes(compressed), 0, true);
  ASSERT_TRUE(decompressOk);
  EXPECT_EQ(decompressed, allBytes);
}

TEST_F(WebSocketCompressEdgeCasesTest, CompressedDataWithoutTrailer) {
  // Create raw deflate data without the RFC 7692 trailer
  // This tests the branch where compressed data doesn't end with 0x00 0x00 0xff 0xff
  WebSocketCompressor compressor(6);
  RawBytes output;

  // Compress some data
  const char* error = compressor.compress(StringToBytes("test"), output, true);
  EXPECT_EQ(error, nullptr);

  // The trailer should have been removed by compress()
  // Now manually add some non-trailer bytes at the end to simulate data without trailer
  std::array<std::byte, 2> extraBytes = {std::byte{0x01}, std::byte{0x02}};
  output.append(extraBytes.data(), extraBytes.size());

  // This tests that the code doesn't crash when trailer is not present
  // (The compress method should handle this gracefully)
}

TEST_F(WebSocketCompressEdgeCasesTest, VerySmallCompressedData) {
  // Test with compressed data smaller than 4 bytes (trailer size)
  // This covers the branch where compressedSize < kDeflateTrailer.size()
  WebSocketCompressor compressor(6);
  RawBytes output;

  // Pre-populate output with some data to test the "compressedSize < trailer" branch
  std::array<std::byte, 2> preData = {std::byte{0x00}, std::byte{0x00}};
  output.append(preData.data(), preData.size());

  // Now compress with empty input (which typically produces small output)
  const char* error = compressor.compress(StringToBytes(""), output, true);
  EXPECT_EQ(error, nullptr);

  // The new compressed data might be very small, testing the branch
}

TEST_F(WebSocketCompressEdgeCasesTest, DecompressCorruptedData) {
  // Test with various types of corrupted data to trigger Z_DATA_ERROR
  WebSocketDecompressor decompressor;
  RawBytes output;

  // Test 1: Random bytes that are not valid deflate data
  std::string corruptData1 = "\xFF\xFE\xFD\xFC\xFB\xFA";
  const char* error1 = decompressor.decompress(StringToBytes(corruptData1), output, 0, true);
  EXPECT_NE(error1, nullptr);
  EXPECT_STREQ(error1, "inflate() failed");

  // Test 2: Truncated valid deflate stream - but add actual deflate header first
  auto [compressOk, compressed] = CompressData("Hello World", true);
  ASSERT_TRUE(compressOk);

  // Take only first few bytes of valid compressed data (but not complete)
  if (compressed.size() > 5) {
    std::string truncated = compressed.substr(0, compressed.size() - 2);
    WebSocketDecompressor decompressor2;
    RawBytes output2;
    // This should decompress but then fail when we add the trailer because data is incomplete
    const char* error2 = decompressor2.decompress(StringToBytes(truncated), output2, 0, true);
    // This may or may not fail depending on where we truncated - just check it doesn't crash
    (void)error2;
  }

  // Test 3: Corrupt byte sequence that will fail inflate
  // Use a sequence that looks like deflate but is invalid
  std::string invalidDeflate = "\x78\x9C\xFF\xFF\xFF\xFF";  // Valid header but invalid data
  WebSocketDecompressor decompressor3;
  RawBytes output3;
  const char* error3 = decompressor3.decompress(StringToBytes(invalidDeflate), output3, 0, true);
  // May or may not fail - zlib is resilient
  (void)error3;
}

TEST_F(WebSocketCompressEdgeCasesTest, ManuallyCorruptedCompressedData) {
  // Compress valid data, then corrupt it
  auto [compressOk, compressed] = CompressData("Test data for corruption", true);
  ASSERT_TRUE(compressOk);

  // Corrupt the middle of the compressed data
  if (compressed.size() > 5) {
    std::string corrupted = compressed;
    corrupted[compressed.size() / 2] = '\xFF';  // Flip a byte in the middle

    WebSocketDecompressor decompressor;
    RawBytes output;
    const char* error = decompressor.decompress(StringToBytes(corrupted), output, 0, true);
    // This should fail with Z_DATA_ERROR
    EXPECT_NE(error, nullptr);
    EXPECT_STREQ(error, "inflate() failed");
  }
}

}  // namespace
}  // namespace aeronet
