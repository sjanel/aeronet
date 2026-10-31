#include "aeronet/transfer-coding.hpp"

#include <gtest/gtest.h>

namespace aeronet::http {

TEST(TransferCodingTest, ChunkedAlone) {
  EXPECT_TRUE(LastTransferCodingIsChunked("chunked"));
  EXPECT_TRUE(LastTransferCodingIsChunked("CHUNKED"));
  EXPECT_TRUE(LastTransferCodingIsChunked("Chunked"));
}

TEST(TransferCodingTest, ChunkedLastOfSeveral) {
  EXPECT_TRUE(LastTransferCodingIsChunked("gzip, chunked"));
  EXPECT_TRUE(LastTransferCodingIsChunked("gzip,chunked"));
  EXPECT_TRUE(LastTransferCodingIsChunked("gzip ,\tchunked"));
  EXPECT_TRUE(LastTransferCodingIsChunked("chunked, chunked"));
  EXPECT_TRUE(LastTransferCodingIsChunked("gzip, deflate, ChUnKeD"));
}

TEST(TransferCodingTest, ChunkedNotLast) {
  EXPECT_FALSE(LastTransferCodingIsChunked("gzip"));
  EXPECT_FALSE(LastTransferCodingIsChunked("identity"));
  EXPECT_FALSE(LastTransferCodingIsChunked("chunked, gzip"));
  EXPECT_FALSE(LastTransferCodingIsChunked("chunked,"));
  EXPECT_FALSE(LastTransferCodingIsChunked("chunked, "));
  EXPECT_FALSE(LastTransferCodingIsChunked(""));
  EXPECT_FALSE(LastTransferCodingIsChunked(","));
}

TEST(TransferCodingTest, ChunkedLookalikes) {
  EXPECT_FALSE(LastTransferCodingIsChunked("chunke"));
  EXPECT_FALSE(LastTransferCodingIsChunked("chunkedx"));
  EXPECT_FALSE(LastTransferCodingIsChunked("xchunked"));
  EXPECT_FALSE(LastTransferCodingIsChunked("chunked;q=1"));
  EXPECT_FALSE(LastTransferCodingIsChunked("gzip, chunked x"));
  // Only SP and HTAB are optional whitespace.
  EXPECT_FALSE(LastTransferCodingIsChunked("gzip,\vchunked"));
}

}  // namespace aeronet::http
