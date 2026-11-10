#include "aeronet/amc-type-traits.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <utility>

#include "aeronet/small-vector.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

namespace {

struct EmptyDeleter {
  void operator()(const int* ptr) const noexcept { delete ptr; }
};

}  // namespace

static_assert(amc::is_trivially_relocatable_v<std::unique_ptr<int>>);
static_assert(amc::is_trivially_relocatable_v<std::unique_ptr<int[]>>);
static_assert(amc::is_trivially_relocatable_v<std::unique_ptr<int, EmptyDeleter>>);
static_assert(amc::is_trivially_relocatable_v<std::unique_ptr<int, void (*)(int*)>>);
static_assert(amc::is_trivially_relocatable_v<vector<std::unique_ptr<int>>>);
// A deleter which is not trivially relocatable, or a reference to one, keeps the move + destroy of the elements.
static_assert(!amc::is_trivially_relocatable_v<std::unique_ptr<int, std::function<void(int*)>>>);
static_assert(!amc::is_trivially_relocatable_v<std::unique_ptr<int, EmptyDeleter&>>);
static_assert(amc::is_trivially_relocatable_v<std::shared_ptr<int>>);
static_assert(amc::is_trivially_relocatable_v<std::shared_ptr<int[]>>);

namespace {

// Counts its live instances, to check that each relocated pointer still owns its object and frees it exactly once.
class Counted {
 public:
  Counted(int value, int& liveCount) noexcept : _pLiveCount(&liveCount), _value(value) { ++liveCount; }

  Counted(const Counted&) = delete;
  Counted(Counted&&) noexcept = delete;
  Counted& operator=(const Counted&) = delete;
  Counted& operator=(Counted&&) noexcept = delete;

  ~Counted() { --*_pLiveCount; }

  [[nodiscard]] int value() const noexcept { return _value; }

 private:
  int* _pLiveCount;
  int _value;
};

using CountedPtr = std::unique_ptr<Counted>;

void DeleteCounted(Counted* pCounted) noexcept { delete pCounted; }

}  // namespace

TEST(AmcTypeTraits, VectorOfUniquePtrRelocatesOnGrowthAndShifts) {
  static constexpr int kNbElements = 10000;
  int liveCount = 0;
  {
    vector<CountedPtr> pointers;
    for (int value = 0; value < kNbElements; ++value) {
      pointers.push_back(std::make_unique<Counted>(value, liveCount));
    }
    EXPECT_EQ(liveCount, kNbElements);

    // Shifts all the elements right, then left over the erased ones.
    pointers.insert(pointers.begin(), std::make_unique<Counted>(-1, liveCount));
    pointers.erase(pointers.begin() + 1, pointers.begin() + 101);
    EXPECT_EQ(liveCount, kNbElements + 1 - 100);
    ASSERT_EQ(pointers.size(), static_cast<vector<CountedPtr>::size_type>(kNbElements + 1 - 100));
    EXPECT_EQ(pointers[0]->value(), -1);
    EXPECT_EQ(pointers[1]->value(), 100);
    EXPECT_EQ(pointers.back()->value(), kNbElements - 1);

    CountedPtr last = pointers.pop_back_val();
    EXPECT_EQ(last->value(), kNbElements - 1);
    last.reset();
    EXPECT_EQ(liveCount, kNbElements - 100);

    for (int value = 1; value < kNbElements - 100; ++value) {
      ASSERT_EQ(pointers[static_cast<vector<CountedPtr>::size_type>(value)]->value(), value + 99);
    }

    pointers.shrink_to_fit();
    EXPECT_EQ(pointers[1]->value(), 100);
    EXPECT_EQ(liveCount, kNbElements - 100);
  }
  EXPECT_EQ(liveCount, 0);
}

TEST(AmcTypeTraits, SmallVectorOfUniquePtrRelocatesInlineElements) {
  int liveCount = 0;
  {
    SmallVector<CountedPtr, 4> inlinePointers;
    inlinePointers.push_back(std::make_unique<Counted>(1, liveCount));
    inlinePointers.push_back(std::make_unique<Counted>(2, liveCount));

    // The inline elements are copied to the new vector, the moved-from one must not free them.
    SmallVector<CountedPtr, 4> moved(std::move(inlinePointers));
    ASSERT_EQ(moved.size(), 2U);
    EXPECT_EQ(moved[1]->value(), 2);
    EXPECT_EQ(liveCount, 2);

    SmallVector<CountedPtr, 4> other;
    other.push_back(std::make_unique<Counted>(3, liveCount));
    moved.swap(other);
    ASSERT_EQ(moved.size(), 1U);
    ASSERT_EQ(other.size(), 2U);
    EXPECT_EQ(moved[0]->value(), 3);
    EXPECT_EQ(other[0]->value(), 1);

    // Leaves the inline storage for the heap.
    for (int value = 4; value < 20; ++value) {
      other.push_back(std::make_unique<Counted>(value, liveCount));
    }
    EXPECT_EQ(other[0]->value(), 1);
    EXPECT_EQ(other.back()->value(), 19);
    EXPECT_EQ(liveCount, 19);
  }
  EXPECT_EQ(liveCount, 0);
}

TEST(AmcTypeTraits, VectorOfUniquePtrWithFunctionPointerDeleter) {
  using FunctionDeleterPtr = std::unique_ptr<Counted, void (*)(Counted*)>;
  static constexpr int kNbElements = 1000;
  int liveCount = 0;
  {
    vector<FunctionDeleterPtr> pointers;
    for (int value = 0; value < kNbElements; ++value) {
      pointers.push_back(FunctionDeleterPtr(new Counted(value, liveCount), &DeleteCounted));
    }
    pointers.erase(pointers.begin() + 10);
    EXPECT_EQ(liveCount, kNbElements - 1);
    EXPECT_EQ(pointers[10]->value(), 11);
    EXPECT_EQ(pointers[10].get_deleter(), &DeleteCounted);
    EXPECT_EQ(pointers.back()->value(), kNbElements - 1);
  }
  EXPECT_EQ(liveCount, 0);
}

TEST(AmcTypeTraits, VectorOfSharedPtrKeepsReferenceCounts) {
  static constexpr int kNbElements = 1000;
  int liveCount = 0;
  {
    vector<std::shared_ptr<Counted>> owners;
    vector<std::shared_ptr<Counted>> copies;
    for (int value = 0; value < kNbElements; ++value) {
      owners.push_back(std::make_shared<Counted>(value, liveCount));
      copies.push_back(owners.back());
    }
    EXPECT_EQ(liveCount, kNbElements);
    EXPECT_EQ(owners.front().use_count(), 2);

    // Erasing from the middle shifts the following elements without touching their counts.
    copies.erase(copies.begin() + 1, copies.begin() + 11);
    EXPECT_EQ(owners[0].use_count(), 2);
    EXPECT_EQ(owners[1].use_count(), 1);
    EXPECT_EQ(owners[11].use_count(), 2);
    EXPECT_EQ(copies[1]->value(), 11);

    owners.erase(owners.begin() + 1, owners.begin() + 11);
    EXPECT_EQ(liveCount, kNbElements - 10);

    owners.clear();
    EXPECT_EQ(liveCount, kNbElements - 10);
    EXPECT_EQ(copies.back().use_count(), 1);
  }
  EXPECT_EQ(liveCount, 0);
}

}  // namespace aeronet
