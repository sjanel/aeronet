#include "aeronet/json-serializer.hpp"

#include <gtest/gtest.h>

#include <glaze/glaze.hpp>  // IWYU pragma: export
#include <string>

// Test structures for serialization
struct TestMessage {
  std::string message;

  bool operator==(const TestMessage& other) const { return message == other.message; }
};

struct TestWorld {
  int id;
  int randomNumber;

  bool operator==(const TestWorld& other) const { return id == other.id && randomNumber == other.randomNumber; }
};

namespace aeronet::test {

namespace {
class JsonSerializerTest : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}
};
}  // namespace

TEST_F(JsonSerializerTest, SerializeSimpleMessage) {
  TestMessage msg{"Hello, World!"};
  auto json = SerializeToJson(msg);

  // The JSON should contain the serialized message
  ASSERT_FALSE(json.empty());
  ASSERT_TRUE(json.contains("Hello, World!"));
}

TEST_F(JsonSerializerTest, SerializeWorldObject) {
  TestWorld world{42, 1234};
  auto json = SerializeToJson(world);

  ASSERT_FALSE(json.empty());
  ASSERT_TRUE(json.contains("42"));
  ASSERT_TRUE(json.contains("1234"));
}

}  // namespace aeronet::test
