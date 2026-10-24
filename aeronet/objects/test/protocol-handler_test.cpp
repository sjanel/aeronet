#include "aeronet/protocol-handler.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/http-message-data.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

namespace {

// Minimal handler relying on the default implementations of IProtocolHandler: its output is one contiguous buffer.
class StringOutputHandler final : public IProtocolHandler {
 public:
  [[nodiscard]] ProtocolType type() const noexcept override { return ProtocolType::WebSocket; }

  [[nodiscard]] ProtocolProcessResult processInput(std::span<const std::byte> data) override {
    output.append(reinterpret_cast<const char*>(data.data()), data.size());
    return ProtocolProcessResult{.bytesConsumed = data.size()};
  }

  [[nodiscard]] std::span<const std::byte> getPendingOutput() const noexcept override {
    return std::as_bytes(std::span<const char>(output.data() + offset, output.size() - offset));
  }

  void onOutputWritten(std::size_t bytesWritten) override { offset += bytesWritten; }

  void onTransportClosing() override {}

  std::string output;
  std::size_t offset{0};
};

}  // namespace

TEST(ProtocolHandlerTest, ProcessResultDefaults) {
  const ProtocolProcessResult result;
  EXPECT_EQ(result.bytesConsumed, 0U);
  EXPECT_EQ(result.action, ProtocolProcessResult::Action::Continue);
  EXPECT_FALSE(result.outputRefersToInput);
}

TEST(ProtocolHandlerTest, DefaultFragmentsAreThePendingOutput) {
  StringOutputHandler handler;
  handler.output = "frame data";
  handler.onOutputWritten(6);

  EXPECT_TRUE(handler.hasPendingOutput());
  EXPECT_EQ(handler.pendingOutputSize(), 4U);
  vector<std::string_view> fragments{"stale"};
  handler.getPendingOutputFragments(fragments);
  ASSERT_EQ(fragments.size(), 1U);
  EXPECT_EQ(fragments[0], "data");

  // Without output referring to its input, releasing the input changes nothing.
  handler.releaseInput();
  EXPECT_EQ(handler.pendingOutputSize(), 4U);
}

TEST(ProtocolHandlerTest, DefaultDiscardWritesOffThePendingOutput) {
  StringOutputHandler handler;
  handler.output = "to discard";
  handler.discardPendingOutput();
  EXPECT_FALSE(handler.hasPendingOutput());
  handler.discardPendingOutput();
  EXPECT_EQ(handler.offset, handler.output.size());
}

TEST(ProtocolHandlerTest, DefaultDrainCopiesThePendingOutput) {
  StringOutputHandler handler;
  HttpMessageData dest;
  EXPECT_FALSE(handler.drainOutputBuffer(dest));

  handler.output = "abcdef";
  handler.onOutputWritten(2);
  EXPECT_TRUE(handler.drainOutputBuffer(dest));
  EXPECT_EQ(dest.firstBuffer(), "cdef");
  EXPECT_FALSE(handler.hasPendingOutput());
}

}  // namespace aeronet
