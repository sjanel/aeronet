#include "aeronet/output-queue.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/file-payload.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/raw-chars.hpp"
#include "message-data-test-helpers.hpp"

namespace aeronet {

namespace {

// A file response: its head, then a file payload of 'fileLength' bytes.
HttpMessageData FileResponse(std::string_view head, std::size_t fileLength) {
  return {RawChars(head), HttpPayload(FilePayload{File{}, 0, fileLength})};
}

std::string Contents(const HttpMessageData& data) {
  return std::string(data.firstBuffer()) + std::string(data.secondBuffer());
}

constexpr std::size_t kLarge = OutputQueue::kMaxMergedSize;

}  // namespace

TEST(OutputQueueTest, EmptyInitially) {
  OutputQueue queue;
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.bytes(), 0U);
  EXPECT_TRUE(queue.entries().empty());

  // Nothing to drop nor to release.
  queue.clear();
  queue.shrink_to_fit();
  EXPECT_TRUE(queue.empty());
}

TEST(OutputQueueTest, LargeEntriesAreMovedInAndPoppedInOrder) {
  OutputQueue queue;
  HttpMessageData first = test::MakeMessageData(kLarge, 'a');
  const char* firstStorage = first.firstBuffer().data();
  EXPECT_FALSE(queue.push(std::move(first)));
  EXPECT_FALSE(queue.push(test::MakeMessageData(kLarge + 1, 'b')));
  EXPECT_EQ(queue.entries().size(), 2U);
  EXPECT_EQ(queue.bytes(), (2 * kLarge) + 1);

  HttpMessageData popped = queue.pop();
  // Not copied.
  EXPECT_EQ(popped.firstBuffer().data(), firstStorage);
  EXPECT_EQ(Contents(popped), std::string(kLarge, 'a'));
  EXPECT_EQ(queue.bytes(), kLarge + 1);
  EXPECT_EQ(Contents(queue.pop()), std::string(kLarge + 1, 'b'));
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.bytes(), 0U);
}

TEST(OutputQueueTest, SmallDataIsMergedIntoTheLastEntry) {
  OutputQueue queue;
  EXPECT_FALSE(queue.push(test::MakeMessageData(100, 'a')));
  EXPECT_TRUE(queue.push(test::MakeMessageData(200, 'b')));
  // Merged up to kMaxMergedSize exactly.
  EXPECT_TRUE(queue.push(test::MakeMessageData(kLarge - 300, 'c')));
  ASSERT_EQ(queue.entries().size(), 1U);
  EXPECT_EQ(queue.bytes(), kLarge);

  // One more byte would exceed it.
  EXPECT_FALSE(queue.push(test::MakeMessageData(1, 'd')));
  ASSERT_EQ(queue.entries().size(), 2U);
  EXPECT_EQ(queue.bytes(), kLarge + 1);
  EXPECT_EQ(Contents(queue.pop()), std::string(100, 'a') + std::string(200, 'b') + std::string(kLarge - 300, 'c'));
  EXPECT_EQ(Contents(queue.pop()), "d");
}

TEST(OutputQueueTest, FirstEntryWrittenInPart) {
  OutputQueue queue;
  queue.push(test::MakeMessageData(100, 'a'));
  queue.frontWritten(30);
  EXPECT_EQ(queue.size(), 1U);
  EXPECT_EQ(queue.bytes(), 70U);
  EXPECT_EQ(Contents(queue.front()), std::string(70, 'a'));

  // Still appended to.
  EXPECT_TRUE(queue.push(test::MakeMessageData(10, 'b')));
  EXPECT_EQ(queue.bytes(), 80U);
  EXPECT_EQ(Contents(queue.front()), std::string(70, 'a') + std::string(10, 'b'));

  // Its written part is not counted anymore once popped.
  EXPECT_EQ(Contents(queue.pop()), std::string(70, 'a') + std::string(10, 'b'));
  EXPECT_EQ(queue.bytes(), 0U);
}

TEST(OutputQueueTest, FirstEntryNotAppendableIsOnlyNotMergedInto) {
  OutputQueue queue;
  queue.push(test::MakeMessageData(100, 'a'));
  const char* firstStorage = queue.front().firstBuffer().data();
  EXPECT_FALSE(queue.push(test::MakeMessageData(10, 'b'), /*frontAppendable=*/false));
  ASSERT_EQ(queue.size(), 2U);
  EXPECT_EQ(queue.front().firstBuffer().data(), firstStorage);

  // The last entry, behind it, is.
  EXPECT_TRUE(queue.push(test::MakeMessageData(10, 'c'), /*frontAppendable=*/false));
  ASSERT_EQ(queue.size(), 2U);
  EXPECT_EQ(Contents(queue.front()), std::string(100, 'a'));
  EXPECT_EQ(Contents(queue.entries()[1]), std::string(10, 'b') + std::string(10, 'c'));
  EXPECT_EQ(queue.bytes(), 120U);
}

TEST(OutputQueueTest, ManySmallPiecesAreMergedInFewEntries) {
  OutputQueue queue;
  std::string expected;
  for (int pos = 0; pos < 1000; ++pos) {
    const char ch = static_cast<char>('a' + (pos % 26));
    queue.push(test::MakeMessageData(10, ch));
    expected.append(10, ch);
  }
  EXPECT_EQ(queue.bytes(), expected.size());
  EXPECT_LE(queue.entries().size(), (expected.size() / kLarge) + 1);

  std::string received;
  while (!queue.empty()) {
    received += Contents(queue.pop());
  }
  EXPECT_EQ(received, expected);
}

TEST(OutputQueueTest, FileResponsesAreNeverMerged) {
  OutputQueue queue;
  EXPECT_FALSE(queue.push(test::MakeMessageData(10, 'a')));
  EXPECT_FALSE(queue.push(FileResponse("head", 1000)));
  EXPECT_FALSE(queue.push(test::MakeMessageData(10, 'b')));
  ASSERT_EQ(queue.entries().size(), 3U);
  // The file is not counted.
  EXPECT_EQ(queue.bytes(), 24U);

  EXPECT_FALSE(queue.entries()[0].hasFilePayload());
  EXPECT_TRUE(queue.entries()[1].hasFilePayload());
  EXPECT_EQ(queue.entries()[1].firstBuffer(), "head");
  EXPECT_FALSE(queue.entries()[2].hasFilePayload());

  EXPECT_FALSE(OutputQueue::CanMerge(queue.entries()[1], test::MakeMessageData(1, 'c')));
  EXPECT_FALSE(OutputQueue::CanMerge(test::MakeMessageData(1, 'c'), queue.entries()[1]));
}

TEST(OutputQueueTest, PoppedRoomIsReusedInOrder) {
  OutputQueue queue;
  std::size_t nextPushed = 0;
  std::size_t nextPopped = 0;
  const auto push = [&queue, &nextPushed] {
    queue.push(test::MakeMessageData(kLarge, static_cast<char>('a' + (nextPushed++ % 26))));
  };
  const auto pop = [&queue, &nextPopped] {
    EXPECT_EQ(Contents(queue.pop()), std::string(kLarge, static_cast<char>('a' + (nextPopped++ % 26))));
  };
  // Pops part of the entries between pushes, so that pushes find the storage full with popped room at its start.
  for (int round = 0; round < 50; ++round) {
    for (int pos = 0; pos < 7; ++pos) {
      push();
    }
    for (int pos = 0; pos < 5; ++pos) {
      pop();
    }
  }
  EXPECT_EQ(queue.entries().size(), nextPushed - nextPopped);
  EXPECT_EQ(queue.bytes(), (nextPushed - nextPopped) * kLarge);
  while (!queue.empty()) {
    pop();
  }
  EXPECT_EQ(nextPopped, nextPushed);
}

TEST(OutputQueueTest, ShrinkKeepsTheQueuedEntries) {
  OutputQueue queue;
  for (int pos = 0; pos < 8; ++pos) {
    queue.push(test::MakeMessageData(kLarge, static_cast<char>('a' + pos)));
  }
  for (int pos = 0; pos < 5; ++pos) {
    (void)queue.pop();
  }
  queue.shrink_to_fit();
  ASSERT_EQ(queue.entries().size(), 3U);
  EXPECT_EQ(queue.bytes(), 3 * kLarge);
  EXPECT_EQ(Contents(queue.pop()), std::string(kLarge, 'f'));

  queue.clear();
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(queue.bytes(), 0U);
  // Released, then allocated again on the next push.
  queue.shrink_to_fit();
  queue.push(test::MakeMessageData(1, 'z'));
  EXPECT_EQ(Contents(queue.pop()), "z");
}

}  // namespace aeronet
