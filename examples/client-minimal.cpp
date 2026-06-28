#include <aeronet/aeronet-client.hpp>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string_view>

using namespace aeronet;

int main(int argc, char** argv) {
  std::string_view url = "https://example.com";
  if (argc > 1) {
    url = argv[1];
  }

  std::cout << "Sending GET request to: " << url << '\n';

  try {
    HttpClient client;
    auto result = client.get(url);
    if (result) {
      const HttpResponse& resp = *result;

      std::cout << "Response status: " << static_cast<int>(resp.status()) << '\n';
      std::cout << "Response reason: " << resp.reason() << '\n';
      std::cout << "Headers:\n";
      for (const auto& [headerKey, headerValue] : resp.headers()) {
        std::cout << " - " << headerKey << ": " << headerValue << '\n';
      }
      std::cout << "Body:\n" << resp.bodyInMemory() << '\n';
    } else {
      auto reason = ErrcToStr(result.error());
      std::cerr << "Request failed: " << reason << '\n';
    }
  } catch (const std::exception& e) {
    std::cerr << "Server encountered error: " << e.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
