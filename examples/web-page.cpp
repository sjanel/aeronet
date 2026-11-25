// Reads the stock of a product of the Nintendo Store, whose pages need JavaScript to be reached: the store sends each
// new visitor to a waiting room (Queue-it), whose page runs a script that checks the cookies, then navigates back to
// the store with a signed token. WebClient follows that path as a browser does (cookie jar across both sites, the
// script and its navigation), then stops as soon as the JSON that the product page embeds (script#__NEXT_DATA__, a
// Next.js page) is parsed, before any of its scripts runs.
//
//   aeronet-web-page [url] [period in seconds]
//
// With a period, it loads the page again after each period until the product can be ordered. The same WebClient keeps
// the cookies: while the token of the waiting room is valid, the next loads go straight to the product page.

#include <aeronet/aeronet-web.hpp>
#include <aeronet/html-element.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <glaze/glaze.hpp>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

using namespace aeronet;

// The fields of __NEXT_DATA__ that this example reads: glaze skips the others (600 KB of them). Outside of an anonymous
// namespace: glaze reflects types with a linkage.
// NOLINTBEGIN(misc-use-internal-linkage)
namespace store {

struct Price {
  double value{};
  std::string currencyCode;
};

struct Availability {
  std::string type;  // "InStock", "OutOfStock"...
};

struct Inventory {
  bool orderable{};
  bool preorderable{};
};

struct Product {
  std::string name;
  Price price;
  Availability availability;
  Inventory inventory;
};

struct PageProps {
  Product product;
};

struct Props {
  PageProps pageProps;
};

struct NextData {
  Props props;
};

}  // namespace store
// NOLINTEND(misc-use-internal-linkage)

template <>
struct glz::meta<store::Product> {
  using T = store::Product;
  static constexpr auto value =
      object("name", &T::name, "price", &T::price, "c_availabilityModel", &T::availability, "inventory", &T::inventory);
};

namespace {

constexpr std::string_view kNextData = "script#__NEXT_DATA__";

// Loads 'url' and prints the stock of its product: true when it can be ordered.
bool CheckStock(WebClient& web, std::string_view url) {
  const auto start = std::chrono::steady_clock::now();
  auto page = web.navigate(url, {.untilSelector = kNextData, .timeout = std::chrono::seconds{20}});
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  if (!page) {
    std::cerr << "Navigation failed: " << ErrcToStr(page.error()) << '\n';
    return false;
  }
  if (page->stopReason() != StopReason::SelectorMatched) {
    // A waiting room with a queue running polls Queue-it until the turn of the visitor: out of the scope of WebClient.
    std::cerr << "No product data after " << elapsed.count() << " ms, the page is at " << page->url() << " (status "
              << static_cast<int>(page->status()) << ")\n";
    return false;
  }
  const std::string json = page->document().querySelector(kNextData).textContent();
  store::NextData data;
  if (const auto error = glz::read<glz::opts{.error_on_unknown_keys = false}>(data, json)) {
    std::cerr << "Unexpected __NEXT_DATA__: " << glz::format_error(error, json) << '\n';
    return false;
  }
  const store::Product& product = data.props.pageProps.product;
  std::cout << product.name << "\n  " << product.price.value << ' ' << product.price.currencyCode << " - "
            << product.availability.type << (product.inventory.orderable ? ", orderable" : ", not orderable")
            << "\n  read in " << elapsed.count() << " ms from " << page->url() << '\n'
            << std::flush;
  return product.inventory.orderable;
}

}  // namespace

int main(int argc, char** argv) {
  std::string_view url =
      "https://store.nintendo.com/fr-fr/"
      "manette-pro-nintendo-switch2-edition-40e-anniversaire-de-the-legend-of-zelda-support-dexposition-"
      "000000000010019437";
  if (argc > 1) {
    url = argv[1];
  }
  try {
    const std::chrono::seconds period{argc > 2 ? std::stoul(argv[2]) : 0UL};
    WebClient web;  // its cookies outlive navigate()
    while (!CheckStock(web, url) && period.count() > 0) {
      std::this_thread::sleep_for(period);
    }
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
