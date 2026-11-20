// A command-line WebSocket client, in the spirit of wscat or websocat: it connects to a ws:// or wss:// URL, sends the
// messages given on the command line once the connection is open, and prints each message of the server on its own
// line of the standard output. The status of the connection goes to the standard error, and the logs of aeronet,
// written to the standard output, are off unless --verbose: the messages can be piped to a tool like jq. Examples:
//
//   # 5 messages of the Kraken ticker of BTC/USD (status, subscription acknowledgment, snapshot, updates...)
//   SUBSCRIBE='{"method":"subscribe","params":{"channel":"ticker","symbol":["BTC/USD"]}}'
//   aeronet-websocket-client --max-messages 5 wss://ws.kraken.com/v2 "$SUBSCRIBE"
//
//   # Two messages echoed by the aeronet-websocket-echo example
//   aeronet-websocket-client -n 2 ws://localhost:8080/ws hello world

#include <aeronet/aeronet-client.hpp>
#include <aeronet/log.hpp>
#include <aeronet/lower-ascii-key.hpp>
#include <aeronet/signal-handler.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <ios>
#include <iostream>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace aeronet;

namespace {

void PrintUsage(std::string_view programName) {
  std::cerr << "Usage: " << programName << " [options] <url> [message...]\n"
            << "\n"
            << "Connects to a WebSocket server (ws://, wss://), sends each message as a text message once the\n"
            << "connection is open, and prints the messages of the server, one per line (binary ones in hexadecimal),\n"
            << "until the server closes the connection, a limit below is reached, or Ctrl+C.\n"
            << "\n"
            << "Options:\n"
            << "  -n, --max-messages <count>  Close the connection after <count> received messages\n"
            << "  -t, --duration <seconds>    Close the connection after <seconds>\n"
            << "  -H, --header <line>         Add a header to the opening handshake ('Name: value'), repeatable\n"
            << "  -p, --protocol <list>       Offer subprotocols (comma-separated)\n"
            << "      --no-compression        Do not offer permessage-deflate\n"
            << "  -v, --verbose               Print the debug logs of aeronet (on the standard output)\n"
            << "  -h, --help                  Show this help message\n"
            << "      --                      End of the options: the next arguments are messages\n"
            << "\n"
            << "Exits with 0 when the connection ended with a closing handshake of code 1000 or 1001.\n";
}

struct Options {
  std::string_view url;
  std::vector<std::string_view> messages;  // sent once the connection is open
  std::vector<std::string_view> headers;   // 'Name: value' lines of the upgrade request
  std::string_view protocols;              // offered subprotocols, comma-separated
  std::chrono::seconds duration{0};        // 0: no limit
  uint64_t maxMessages{0};                 // 0: no limit
  bool compression{true};
  bool verbose{false};
  bool help{false};
};

// Parses 'text', a whole non-negative integer.
template <class Unsigned>
bool ParseCount(std::string_view text, Unsigned& count) {
  const auto [ptr, errc] = std::from_chars(text.data(), text.data() + text.size(), count);
  return errc == std::errc{} && ptr == text.data() + text.size();
}

// Sets 'value' to the argument following the option at 'argIndex', which then designates it. Returns false, after
// printing why, if there is none.
bool NextValue(int argc, char** argv, int& argIndex, std::string_view& value) {
  if (argIndex + 1 >= argc) {
    std::cerr << "Missing value for " << argv[argIndex] << '\n';
    return false;
  }
  value = argv[++argIndex];
  return true;
}

// Returns false, after printing why, if the command line is invalid.
bool ParseOptions(int argc, char** argv, Options& options) {
  bool optionsEnded = false;  // after '--', the messages may start with '-'
  std::string_view value;
  for (int argIndex = 1; argIndex < argc; ++argIndex) {
    const std::string_view arg(argv[argIndex]);
    if (optionsEnded || !arg.starts_with('-') || arg == "-") {
      if (options.url.empty()) {
        options.url = arg;
      } else {
        options.messages.push_back(arg);
      }
    } else if (arg == "--") {
      optionsEnded = true;
    } else if (arg == "-h" || arg == "--help") {
      options.help = true;
      return true;
    } else if (arg == "--no-compression") {
      options.compression = false;
    } else if (arg == "-v" || arg == "--verbose") {
      options.verbose = true;
    } else if (arg == "-n" || arg == "--max-messages") {
      if (!NextValue(argc, argv, argIndex, value)) {
        return false;
      }
      if (!ParseCount(value, options.maxMessages)) {
        std::cerr << "Invalid message count: '" << value << "'\n";
        return false;
      }
    } else if (arg == "-t" || arg == "--duration") {
      if (!NextValue(argc, argv, argIndex, value)) {
        return false;
      }
      uint32_t seconds = 0;
      if (!ParseCount(value, seconds)) {
        std::cerr << "Invalid duration: '" << value << "'\n";
        return false;
      }
      options.duration = std::chrono::seconds{seconds};
    } else if (arg == "-H" || arg == "--header") {
      if (!NextValue(argc, argv, argIndex, value)) {
        return false;
      }
      options.headers.push_back(value);
    } else if (arg == "-p" || arg == "--protocol") {
      if (!NextValue(argc, argv, argIndex, value)) {
        return false;
      }
      options.protocols = value;
    } else {
      std::cerr << "Unknown option: " << arg << '\n';
      return false;
    }
  }
  if (options.url.empty()) {
    std::cerr << "Missing url\n";
    return false;
  }
  return true;
}

// Adds the 'Name: value' header lines to the upgrade request. Throws std::invalid_argument on an invalid header.
void AddHeaders(std::span<const std::string_view> headers, HttpRequest& request) {
  std::string name;
  for (const std::string_view line : headers) {
    const std::size_t colonPos = line.find(':');
    if (colonPos == std::string_view::npos) {
      throw std::invalid_argument("A header line has no ':' (expected 'Name: value')");
    }
    // Header names are case-insensitive, and stored in lower case by aeronet.
    name.assign(line.substr(0, colonPos));
    std::ranges::transform(name, name.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    request.headerAddLine(LowerAsciiKey{name}, line.substr(colonPos + 1));  // the value is trimmed
  }
}

// The state of the callbacks, given to connect() and returned by WebSocketConnection::userData().
struct Session {
  std::span<const std::string_view> messages;  // sent by onOpen
  WebSocketConnection* pConnection{nullptr};   // set by onOpen, null again once the connection ended
  uint64_t maxMessages{0};
  uint64_t nbReceived{0};
  websocket::CloseCode closeCode{websocket::CloseCode::AbnormalClosure};
  bool closeRequested{false};
};

Session& SessionOf(WebSocketConnection& cnx) { return *static_cast<Session*>(cnx.userData()); }

// Starts the closing handshake, once: the connection ends when the server answers it, or after closeTimeout.
void RequestClose(Session& session, std::string_view reason) {
  if (session.pConnection != nullptr && !session.closeRequested) {
    session.closeRequested = true;
    // False when the server already started the closing handshake.
    if (session.pConnection->close(websocket::CloseCode::Normal, reason)) {
      std::cerr << "Closing: " << reason << '\n';
    }
  }
}

void PrintHex(std::span<const std::byte> payload) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string line;
  line.reserve((2U * payload.size()) + 1U);
  for (const std::byte byte : payload) {
    line.push_back(kDigits[std::to_integer<unsigned>(byte) >> 4U]);
    line.push_back(kDigits[std::to_integer<unsigned>(byte) & 0xFU]);
  }
  line.push_back('\n');
  std::cout << line;
}

void OnOpen(WebSocketConnection& cnx) {
  Session& session = SessionOf(cnx);
  session.pConnection = &cnx;
  // Sent before anything the server may send next: the subscriptions of a market data feed, for instance.
  for (const std::string_view message : session.messages) {
    if (!cnx.sendText(message)) {
      std::cerr << "Could not send '" << message << "': the connection is closing\n";
      break;
    }
  }
}

void OnMessage(WebSocketConnection& cnx, std::span<const std::byte> payload, bool isBinary) {
  Session& session = SessionOf(cnx);
  if (session.maxMessages != 0 && session.nbReceived == session.maxMessages) {
    return;  // in flight when the closing handshake started, at the limit: not printed
  }
  // The payload is a view of the receive buffers of the client, only valid during this call.
  if (isBinary) {
    PrintHex(payload);
  } else {
    std::cout.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    std::cout << '\n';
  }
  std::cout << std::flush;  // each message as soon as it is received, also when piped

  if (++session.nbReceived == session.maxMessages) {
    RequestClose(session, "message limit reached");
  }
}

void OnClose(WebSocketConnection& cnx, websocket::CloseCode code, std::string_view reason) {
  Session& session = SessionOf(cnx);
  session.pConnection = nullptr;  // destroyed after this call
  session.closeCode = code;
  std::cerr << "Closed with code " << static_cast<uint16_t>(code);
  if (!reason.empty()) {
    std::cerr << ": " << reason;
  }
  std::cerr << " (received messages: " << session.nbReceived << ")\n";
}

// Prints the response of a server refusing the upgrade: its status, and the start of its body, which often tells why.
void PrintRefusal(const HttpResponse& response) {
  static constexpr std::size_t kMaxBodyBytes = 512;
  std::cerr << "Response status: " << static_cast<int>(response.status());
  if (!response.reason().empty()) {
    std::cerr << ' ' << response.reason();
  }
  std::cerr << '\n';
  const std::string_view body = response.bodyInMemory();
  if (!body.empty()) {
    std::cerr << body.substr(0, kMaxBodyBytes) << (body.size() > kMaxBodyBytes ? "...\n" : "\n");
  }
}

int Run(const Options& options) {
  // The standard output is for the messages, and the outcome of the connection is printed on the standard error.
  log::set_level(options.verbose ? log::level::debug : log::level::off);

  websocket::WebSocketConfig config;
#ifdef AERONET_ENABLE_ZLIB
  config.deflateConfig.enabled = options.compression;
#endif
  WebSocketClient client(config);

  HttpRequest request = client.makeUpgradeRequest(options.url);
  AddHeaders(options.headers, request);
  if (!options.protocols.empty()) {
    request.headerAddLine(websocket::SecWebSocketProtocol, options.protocols);
  }

  Session session;
  session.messages = options.messages;
  session.maxMessages = options.maxMessages;
  const WebSocketConnectResult result = client.connect(
      std::move(request),
      WebSocketClientCallbacks{.onOpen = OnOpen, .onMessage = OnMessage, .onPong = nullptr, .onClose = OnClose},
      &session);
  if (!result) {
    std::cerr << "Connection to " << options.url << " failed: " << result.pErrorMessage << '\n';
    if (result.httpError == HttpClientErrc::noError) {
      // The server answered, but refused the upgrade (a 401, a 429...) or sent an invalid 101 response.
      PrintRefusal(result.response);
    }
    return EXIT_FAILURE;
  }

  std::cerr << "Connected to " << options.url;
  if (const std::string_view protocol = result.response.headerValueOrEmpty(websocket::SecWebSocketProtocol);
      !protocol.empty()) {
    std::cerr << ", subprotocol " << protocol;
  }
  if (const std::string_view extensions = result.response.headerValueOrEmpty(websocket::SecWebSocketExtensions);
      !extensions.empty()) {
    std::cerr << ", extensions " << extensions;
  }
  std::cerr << '\n';

  // Ctrl+C starts the closing handshake, which the server answers within closeTimeout.
  SignalHandler::Enable(config.closeTimeout);
  const auto deadline = options.duration.count() == 0 ? std::chrono::steady_clock::time_point::max()
                                                      : std::chrono::steady_clock::now() + options.duration;
  // poll() returns at the latest after its timeout: the loop checks the signal and the duration in between.
  while (client.poll(std::chrono::milliseconds{100}) != 0) {
    if (SignalHandler::IsStopRequested()) {
      RequestClose(session, "interrupted");
    } else if (std::chrono::steady_clock::now() >= deadline) {
      RequestClose(session, "duration elapsed");
    }
  }

  return session.closeCode == websocket::CloseCode::Normal || session.closeCode == websocket::CloseCode::GoingAway
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
      PrintUsage(argv[0]);
      return EXIT_FAILURE;
    }
    if (options.help) {
      PrintUsage(argv[0]);
      return EXIT_SUCCESS;
    }
    return Run(options);
  } catch (const std::exception& ex) {
    std::cerr << "Error: " << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
