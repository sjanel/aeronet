#include "aeronet/css-selector.hpp"

#include <gtest/gtest.h>

#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/sys-test-support.hpp"

namespace aeronet {

namespace {

constexpr std::string_view kTable = R"(<table class="fees">
<tr><th>Asset</th><th>Fee</th></tr>
<tr><td class="name">0x</td><td>6.00815 ZRX</td></tr>
<tr><td class="name">Aave</td><td>0.00507 AAVE</td></tr>
</table><p class="name">not a cell</p>)";

}  // namespace

TEST(CssSelector, CompilesSelectorsOfLevel4) {
  for (std::string_view selector :
       {"div", "a[href]", "table.fees tr > td:first-child", "p:not(.x)", ":is(h1, h2)", "div:has(> p)",
        "li:nth-child(2n+1)", "a[href^=\"https\" i]", "td, .name", "*", "#id ~ span + b"}) {
    EXPECT_NO_THROW(CssSelector{selector}) << selector;
  }
}

TEST(CssSelector, InvalidSelectorThrows) {
  for (std::string_view selector : {"", "div[", "!!!", "a,,b", "td:unknown-pseudo", "> p", "a[href="}) {
    try {
      CssSelector compiled(selector);
      ADD_FAILURE() << "'" << selector << "' was accepted";
    } catch (const std::invalid_argument& ex) {
      EXPECT_TRUE(std::string_view(ex.what()).contains(std::string("'").append(selector).append("'"))) << ex.what();
    }
  }
}

TEST(CssSelector, ReusedOnSeveralDocuments) {
  const CssSelector cells("tr > td.name");
  for (int idx = 0; idx < 3; ++idx) {
    const HtmlDocument doc(kTable);
    const auto elements = doc.querySelectorAll(cells);
    ASSERT_EQ(elements.size(), 2U);
    EXPECT_EQ(elements[0].textContent(), "0x");
    EXPECT_EQ(elements[1].textContent(), "Aave");
    EXPECT_EQ(doc.querySelector(cells), elements[0]);
  }
}

TEST(CssSelector, MoveConstructionAndAssignment) {
  CssSelector first("td.name");
  CssSelector second(std::move(first));
  CssSelector third("p");
  third = std::move(second);
  const HtmlDocument doc(kTable);
  EXPECT_EQ(doc.querySelectorAll(third).size(), 2U);
  // A moved-from selector can be assigned to.
  first = CssSelector("th");
  EXPECT_EQ(doc.querySelectorAll(first).size(), 2U);
  // Self move assignment keeps the selector.
  CssSelector& alias = first;
  first = std::move(alias);
  EXPECT_EQ(doc.querySelectorAll(first).size(), 2U);
}

#if AERONET_WANT_MALLOC_OVERRIDES
TEST(CssSelector, MemoryExhaustionThrowsBadAlloc) {
  // Fails each allocation of the compilation in turn, until it needs no more.
  for (int successfulAllocs = 0;; ++successfulAllocs) {
    test::FailNextMalloc(successfulAllocs, 1);
    try {
      const CssSelector selector("table tr > td:first-child, a[href]");
      test::ResetToSysMalloc();
      break;
    } catch (const std::bad_alloc&) {
      test::ResetToSysMalloc();
    }
    ASSERT_LT(successfulAllocs, 1000);
  }
}
#endif

}  // namespace aeronet
