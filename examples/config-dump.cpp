#include <aeronet/aeronet-server.hpp>
#ifdef AERONET_ENABLE_HTTP_CLIENT
#include <aeronet/aeronet-client.hpp>
#endif
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

using namespace aeronet;

namespace {

void PrintUsage(std::string_view programName) {
  std::cerr << "Usage: " << programName << " [--client] [--format json|yaml] [--output <path>]\n"
            << "\n"
            << "Dumps the default aeronet top-level configuration (server + router).\n"
            << "\n"
            << "Options:\n"
            << "  -c, --client   Dump the default HttpClient configuration instead\n"
            << "  -f, --format   Output format (json or yaml). Default: yaml\n"
            << "  -o, --output   Output file path. If omitted, writes to stdout\n"
            << "  -h, --help     Show this help message\n";
}

std::optional<ConfigFormat> ParseFormat(std::string_view rawFormat) {
  if (rawFormat == "json") {
    return ConfigFormat::json;
  }
  if (rawFormat == "yaml" || rawFormat == "yml") {
    return ConfigFormat::yaml;
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  ConfigFormat format = ConfigFormat::yaml;
  std::optional<std::filesystem::path> outputPath;
  bool dumpClient = false;

  try {
    for (int argIndex = 1; argIndex < argc; ++argIndex) {
      const std::string_view currentArg{argv[argIndex]};

      if (currentArg == "-h" || currentArg == "--help") {
        PrintUsage(argv[0]);
        return EXIT_SUCCESS;
      }

      if (currentArg == "-c" || currentArg == "--client") {
        dumpClient = true;
        continue;
      }

      if (currentArg == "-f" || currentArg == "--format") {
        if (argIndex + 1 >= argc) {
          std::cerr << "Missing value for " << currentArg << "\n";
          PrintUsage(argv[0]);
          return EXIT_FAILURE;
        }
        const auto parsedFormat = ParseFormat(argv[++argIndex]);
        if (!parsedFormat.has_value()) {
          std::cerr << "Unsupported format: '" << argv[argIndex] << "'\n";
          PrintUsage(argv[0]);
          return EXIT_FAILURE;
        }
        format = *parsedFormat;
        continue;
      }

      if (currentArg == "-o" || currentArg == "--output") {
        if (argIndex + 1 >= argc) {
          std::cerr << "Missing value for " << currentArg << "\n";
          PrintUsage(argv[0]);
          return EXIT_FAILURE;
        }
        outputPath = std::filesystem::path(argv[++argIndex]);
        continue;
      }

      std::cerr << "Unknown option: '" << currentArg << "'\n";
      PrintUsage(argv[0]);
      return EXIT_FAILURE;
    }

    std::string serialized;
    if (dumpClient) {
#ifdef AERONET_ENABLE_HTTP_CLIENT
      serialized = detail::SerializeClientConfig(HttpClientConfig{}, format);
#else
      std::cerr << "This build has no HTTP client (AERONET_ENABLE_HTTP_CLIENT=OFF)\n";
      return EXIT_FAILURE;
#endif
    } else {
      serialized = detail::SerializeConfig(TopLevelConfig{}, format);
    }
    if (serialized.empty()) {
      std::cerr << "Failed to serialize default configuration\n";
      return EXIT_FAILURE;
    }

    if (!outputPath.has_value()) {
      std::cout << serialized;
      if (!serialized.empty() && serialized.back() != '\n') {
        std::cout << '\n';
      }
      return EXIT_SUCCESS;
    }

    std::ofstream outFile(*outputPath, std::ios::binary | std::ios::trunc);
    if (!outFile) {
      std::cerr << "Failed to open output file: " << outputPath->string() << "\n";
      return EXIT_FAILURE;
    }
    outFile << serialized;
    if (!outFile) {
      std::cerr << "Failed to write output file: " << outputPath->string() << "\n";
      return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    std::cerr << "Server encountered error: " << e.what() << '\n';
    return EXIT_FAILURE;
  }
}
