// beast_server.cpp - Boost.Beast benchmark server for scripted benchmark scenarios.
//
// Implements HTTP/1.1 + WebSocket endpoints compatible with run_benchmarks.py
// and run_ws_benchmarks.py. Boost.Beast does not provide HTTP/2 server transport,
// so this target is intentionally HTTP/1.1-only.

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http/message_generator.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef AERONET_LINUX
#include <sys/resource.h>
#endif

#include "scripted-servers-helpers.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

namespace {

constexpr unsigned char toupper(unsigned char ch) {
  if (ch >= 'a' && ch <= 'z') {
    ch &= 0xDF;
  }
  return ch;
}

constexpr char toupper(char ch) { return static_cast<char>(toupper(static_cast<unsigned char>(ch))); }

struct RequestTargetParts {
  std::string_view path;
  std::string_view query;
};

RequestTargetParts SplitTarget(std::string_view target) {
  std::size_t queryPos = target.find('?');
  if (queryPos == std::string_view::npos) {
    return {target, {}};
  }
  return {target.substr(0, queryPos), target.substr(queryPos + 1)};
}

std::optional<std::string_view> FindQueryParam(std::string_view query, std::string_view key) {
  std::size_t pos = 0;
  while (pos < query.size()) {
    std::size_t amp = query.find('&', pos);
    if (amp == std::string_view::npos) {
      amp = query.size();
    }
    std::string_view part = query.substr(pos, amp - pos);
    std::size_t eq = part.find('=');
    std::string_view partKey = eq == std::string_view::npos ? part : part.substr(0, eq);
    if (partKey == key) {
      if (eq == std::string_view::npos || eq + 1 >= part.size()) {
        return std::string_view{};
      }
      return part.substr(eq + 1);
    }
    pos = amp + 1;
  }
  return std::nullopt;
}

template <typename IntegerType>
std::optional<IntegerType> ParseInteger(std::string_view input) {
  IntegerType value{};
  const char* begin = input.data();
  const char* end = input.data() + input.size();
  auto [ptr, ec] = std::from_chars(begin, end, value);
  if (ec != std::errc() || ptr != end) {
    return std::nullopt;
  }
  return value;
}

template <typename IntegerType>
IntegerType QueryIntOr(std::string_view query, std::string_view key, IntegerType defaultValue) {
  auto found = FindQueryParam(query, key);
  if (!found) {
    return defaultValue;
  }
  auto parsed = ParseInteger<IntegerType>(*found);
  if (!parsed) {
    return defaultValue;
  }
  return *parsed;
}

std::string GetContentType(std::string_view path) {
  if (path.ends_with(".html")) {
    return "text/html";
  }
  if (path.ends_with(".css")) {
    return "text/css";
  }
  if (path.ends_with(".js")) {
    return "application/javascript";
  }
  if (path.ends_with(".json")) {
    return "application/json";
  }
  return "application/octet-stream";
}

std::optional<int> ParseRouteNumber(std::string_view path, int routeCount) {
  if (!path.starts_with("/r") || path.size() <= 2) {
    return std::nullopt;
  }
  auto routeNum = ParseInteger<int>(path.substr(2));
  if (!routeNum || *routeNum < 0 || *routeNum >= routeCount) {
    return std::nullopt;
  }
  return routeNum;
}

struct UserPostParams {
  std::string_view user;
  std::string_view post;
};

std::optional<UserPostParams> ParseUserPostPattern(std::string_view path) {
  if (!path.starts_with("/users/")) {
    return std::nullopt;
  }
  std::size_t postsPos = path.find("/posts/", 7);
  if (postsPos == std::string_view::npos) {
    return std::nullopt;
  }
  return UserPostParams{path.substr(7, postsPos - 7), path.substr(postsPos + 7)};
}

struct ApiPatternParams {
  std::string_view resource;
  std::string_view item;
  std::string_view action;
};

std::optional<ApiPatternParams> ParseApiPattern(std::string_view path) {
  static constexpr std::string_view kPrefix = "/api/v1/resources/";
  if (!path.starts_with(kPrefix)) {
    return std::nullopt;
  }
  std::string_view rest = path.substr(kPrefix.size());
  std::size_t itemsPos = rest.find("/items/");
  if (itemsPos == std::string_view::npos) {
    return std::nullopt;
  }
  std::size_t actionsPos = rest.find("/actions/", itemsPos);
  if (actionsPos == std::string_view::npos) {
    return std::nullopt;
  }
  return ApiPatternParams{rest.substr(0, itemsPos), rest.substr(itemsPos + 7, actionsPos - itemsPos - 7),
                          rest.substr(actionsPos + 9)};
}

struct BeastRuntimeState {
  explicit BeastRuntimeState(bench::BenchConfig config) : cfg(std::move(config)), staticDirPath(cfg.staticDir) {
    if (cfg.routeCount > 0) {
      routeBodies.reserve(static_cast<std::size_t>(cfg.routeCount));
      for (int routeIdx = 0; routeIdx < cfg.routeCount; ++routeIdx) {
        routeBodies.emplace_back(std::format("route-{}", routeIdx));
      }
    }
  }

  bench::BenchConfig cfg;
  std::filesystem::path staticDirPath;
  std::vector<std::string> routeBodies;
};

std::optional<std::filesystem::path> ResolveStaticPath(const BeastRuntimeState& state, std::string_view requestPath) {
  if (state.staticDirPath.empty() || !requestPath.starts_with('/')) {
    return std::nullopt;
  }

  std::filesystem::path relative = std::filesystem::path(std::string(requestPath.substr(1))).lexically_normal();
  if (relative.empty()) {
    return std::nullopt;
  }
  if (relative.is_absolute()) {
    return std::nullopt;
  }
  if (std::ranges::any_of(relative, [](const auto& part) { return part == ".."; })) {
    return std::nullopt;
  }
  return state.staticDirPath / relative;
}

void RaiseFileDescriptorLimit() {
#ifdef AERONET_LINUX
  struct rlimit rl{};
  if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
      std::cerr << "Warning: could not raise file descriptor limit to " << rl.rlim_max << "\n";
    }
  }
#endif
}

using HttpResponse = http::message_generator;

http::response<http::string_body> MakeStringResponse(http::status statusCode, unsigned version, bool keepAlive,
                                                     std::string body, std::string_view contentType = "text/plain") {
  http::response<http::string_body> resp{statusCode, version};
  resp.keep_alive(keepAlive);
  resp.set(http::field::server, "aeronet-beast-bench");
  resp.set(http::field::content_type, contentType);
  resp.body() = std::move(body);
  resp.prepare_payload();
  return resp;
}

HttpResponse MakeBaseResponse(http::status statusCode, unsigned version, bool keepAlive, std::string body,
                              std::string_view contentType = "text/plain") {
  return MakeStringResponse(statusCode, version, keepAlive, std::move(body), contentType);
}

HttpResponse MakeFileResponse(http::status statusCode, unsigned version, bool keepAlive,
                              const std::filesystem::path& path, std::string_view contentType) {
  beast::error_code ec;
  http::file_body::value_type body;
  body.open(path.c_str(), beast::file_mode::scan, ec);
  if (ec) {
    return MakeBaseResponse(http::status::not_found, version, keepAlive, "Not Found");
  }

  http::response<http::file_body> resp{statusCode, version};
  resp.keep_alive(keepAlive);
  resp.set(http::field::server, "aeronet-beast-bench");
  resp.set(http::field::content_type, contentType);
  resp.body() = std::move(body);
  resp.content_length(resp.body().size());
  return resp;
}

HttpResponse HandleRequest(const http::request<http::string_body>& req, const BeastRuntimeState& state) {
  RequestTargetParts target = SplitTarget(req.target());
  const bool keepAlive = req.keep_alive();

  if (req.method() == http::verb::get && target.path == "/ping") {
    return MakeBaseResponse(http::status::ok, req.version(), keepAlive, "pong");
  }

  if (req.method() == http::verb::get && target.path == "/headers") {
    std::size_t count = QueryIntOr<std::size_t>(target.query, "count", 10);
    std::size_t size = QueryIntOr<std::size_t>(target.query, "size", 64);
    auto resp =
        MakeStringResponse(http::status::ok, req.version(), keepAlive, std::format("Generated {} headers", count));
    for (std::size_t idx = 0; idx < count; ++idx) {
      resp.set(std::format("X-Bench-Header-{}", idx), bench::GenerateRandomString(size));
    }
    return resp;
  }

  if (req.method() == http::verb::post && target.path == "/uppercase") {
    std::string out;
    std::string_view body = req.body();
    out.resize_and_overwrite(body.size(), [body](char* out, std::size_t len) {
      std::ranges::transform(body, out, [](char ch) { return toupper(ch); });
      return len;
    });
    return MakeBaseResponse(http::status::ok, req.version(), keepAlive, std::move(out), "application/octet-stream");
  }

  if (req.method() == http::verb::get && target.path == "/compute") {
    int complexity = QueryIntOr<int>(target.query, "complexity", 30);
    int hashIters = QueryIntOr<int>(target.query, "hash_iters", 1000);
    uint64_t fibResult = bench::Fibonacci(complexity);
    uint64_t hashResult = bench::ComputeHash(std::format("benchmark-data-{}", complexity), hashIters);

    auto resp = MakeStringResponse(http::status::ok, req.version(), keepAlive,
                                   std::format("fib({})={}, hash={}", complexity, fibResult, hashResult));
    resp.set("X-Fib-Result", std::to_string(fibResult));
    resp.set("X-Hash-Result", std::to_string(hashResult));
    return resp;
  }

  if (req.method() == http::verb::get && target.path == "/json") {
    std::size_t items = QueryIntOr<std::size_t>(target.query, "items", 10);
    return MakeBaseResponse(http::status::ok, req.version(), keepAlive, bench::BuildJson(items), "application/json");
  }

  if (req.method() == http::verb::get && target.path == "/delay") {
    int delayMs = QueryIntOr<int>(target.query, "ms", 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    return MakeBaseResponse(http::status::ok, req.version(), keepAlive, std::format("Delayed {} ms", delayMs));
  }

  if (req.method() == http::verb::get && target.path == "/body") {
    std::size_t size = QueryIntOr<std::size_t>(target.query, "size", 1024);
    return MakeBaseResponse(http::status::ok, req.version(), keepAlive, bench::GenerateRandomString(size),
                            "application/octet-stream");
  }

  if (req.method() == http::verb::post && target.path == "/body-codec") {
    std::string decoded;
    auto encodingIt = req.find(http::field::content_encoding);
    if (encodingIt != req.end() && bench::ContainsTokenInsensitive(encodingIt->value(), "gzip")) {
      auto decompressed = bench::GzipDecompress(req.body());
      if (!decompressed) {
        return MakeBaseResponse(http::status::bad_request, req.version(), keepAlive, "Invalid gzip body");
      }
      decoded = std::move(*decompressed);
    } else {
      decoded = req.body();
    }

    for (char& ch : decoded) {
      unsigned char value = static_cast<unsigned char>(ch);
      value = static_cast<unsigned char>(value + 1U);
      ch = static_cast<char>(value);
    }

    auto acceptIt = req.find(http::field::accept_encoding);
    if (acceptIt != req.end() && bench::ContainsTokenInsensitive(acceptIt->value(), "gzip")) {
      auto compressed = bench::GzipCompress(decoded);
      if (!compressed) {
        return MakeBaseResponse(http::status::internal_server_error, req.version(), keepAlive, "Compression failed");
      }
      auto resp = MakeStringResponse(http::status::ok, req.version(), keepAlive, std::move(*compressed),
                                     "application/octet-stream");
      resp.set(http::field::content_encoding, "gzip");
      resp.set(http::field::vary, "Accept-Encoding");
      return resp;
    }

    return MakeBaseResponse(http::status::ok, req.version(), keepAlive, std::move(decoded), "application/octet-stream");
  }

  if (req.method() == http::verb::get && target.path == "/status") {
    return MakeBaseResponse(
        http::status::ok, req.version(), keepAlive,
        std::format(R"({{"server":"beast","threads":{},"h2":false,"tls":false,"status":"ok"}})", state.cfg.numThreads),
        "application/json");
  }

  if (req.method() == http::verb::get) {
    if (auto fullPath = ResolveStaticPath(state, target.path);
        fullPath && std::filesystem::is_regular_file(*fullPath)) {
      return MakeFileResponse(http::status::ok, req.version(), keepAlive, *fullPath, GetContentType(target.path));
    }
  }

  if (req.method() == http::verb::get) {
    if (auto routeNum = ParseRouteNumber(target.path, state.cfg.routeCount)) {
      return MakeBaseResponse(http::status::ok, req.version(), keepAlive,
                              state.routeBodies[static_cast<std::size_t>(*routeNum)]);
    }

    if (auto userPost = ParseUserPostPattern(target.path)) {
      return MakeBaseResponse(http::status::ok, req.version(), keepAlive,
                              std::format("user={},post={}", userPost->user, userPost->post));
    }

    if (auto api = ParseApiPattern(target.path)) {
      return MakeBaseResponse(http::status::ok, req.version(), keepAlive,
                              std::format("resource={},item={},action={}", api->resource, api->item, api->action));
    }
  }

  return MakeBaseResponse(http::status::not_found, req.version(), keepAlive, "Not Found");
}

bool IsWebSocketPath(std::string_view target) {
  const auto parts = SplitTarget(target);
  return parts.path == "/ws-uncompressed" || parts.path == "/ws-compressed";
}

class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
 public:
  explicit WebSocketSession(tcp::socket socket) : _ws(std::move(socket)) {}

  void Run(http::request<http::string_body> req) {
    _ws.auto_fragment(false);
    _ws.write_buffer_bytes(64UL << 10U);
    _ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    _upgradeRequest.emplace(std::move(req));
    auto self = shared_from_this();
    _ws.async_accept(*_upgradeRequest, [self](beast::error_code ec) { self->OnAccept(ec); });
  }

 private:
  void OnAccept(beast::error_code ec) {
    _upgradeRequest.reset();
    if (ec) {
      return;
    }
    DoRead();
  }

  void DoRead() {
    auto self = shared_from_this();
    _ws.async_read(_buffer,
                   [self](beast::error_code ec, std::size_t bytesTransferred) { self->OnRead(ec, bytesTransferred); });
  }

  void OnRead(beast::error_code ec, std::size_t bytesTransferred) {
    static_cast<void>(bytesTransferred);
    if (ec == websocket::error::closed) {
      return;
    }
    if (ec) {
      return;
    }

    _ws.text(_ws.got_text());
    auto self = shared_from_this();
    _ws.async_write(_buffer.data(), [self](beast::error_code writeEc, std::size_t bytesWritten) {
      self->OnWrite(writeEc, bytesWritten);
    });
  }

  void OnWrite(beast::error_code ec, std::size_t bytesTransferred) {
    static_cast<void>(bytesTransferred);
    if (ec) {
      return;
    }

    _buffer.consume(_buffer.size());
    DoRead();
  }

  websocket::stream<tcp::socket> _ws;
  beast::flat_buffer _buffer;
  std::optional<http::request<http::string_body>> _upgradeRequest;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket socket, std::shared_ptr<BeastRuntimeState> state)
      : _socket(std::move(socket)), _state(std::move(state)) {}

  void Run() {
    beast::error_code ec;
    _socket.set_option(tcp::no_delay(true), ec);
    DoRead();
  }

 private:
  void DoRead() {
    _parser.emplace();
    _parser->body_limit(64ULL << 20);

    auto self = shared_from_this();
    http::async_read(_socket, _buffer, *_parser, [self](beast::error_code ec, std::size_t bytesTransferred) {
      self->OnRead(ec, bytesTransferred);
    });
  }

  void OnRead(beast::error_code ec, std::size_t bytesTransferred) {
    static_cast<void>(bytesTransferred);
    if (ec == http::error::end_of_stream) {
      DoClose();
      return;
    }
    if (ec) {
      return;
    }

    http::request<http::string_body> req = _parser->release();
    _parser.reset();

    if (websocket::is_upgrade(req) && IsWebSocketPath(req.target())) {
      std::make_shared<WebSocketSession>(std::move(_socket))->Run(std::move(req));
      return;
    }

    auto resp = HandleRequest(req, *_state);
    const bool shouldClose = !resp.keep_alive();
    auto self = shared_from_this();
    beast::async_write(_socket, std::move(resp),
                       [self, shouldClose](beast::error_code writeEc, std::size_t bytesWritten) {
                         self->OnWrite(shouldClose, writeEc, bytesWritten);
                       });
  }

  void OnWrite(bool shouldClose, beast::error_code ec, std::size_t bytesTransferred) {
    static_cast<void>(bytesTransferred);
    if (ec) {
      return;
    }
    if (shouldClose) {
      DoClose();
      return;
    }
    DoRead();
  }

  void DoClose() {
    beast::error_code ec;
    _socket.shutdown(tcp::socket::shutdown_send, ec);
  }

  tcp::socket _socket;
  beast::flat_buffer _buffer;
  std::shared_ptr<BeastRuntimeState> _state;
  std::optional<http::request_parser<http::string_body>> _parser;
};

class Listener : public std::enable_shared_from_this<Listener> {
 public:
  Listener(asio::io_context& ioContext, const tcp::endpoint& endpoint, std::shared_ptr<BeastRuntimeState> state)
      : _acceptor(ioContext), _state(std::move(state)) {
    beast::error_code ec;
    _acceptor.open(endpoint.protocol(), ec);
    if (ec) {
      throw beast::system_error(ec);
    }
    _acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
      throw beast::system_error(ec);
    }
    _acceptor.bind(endpoint, ec);
    if (ec) {
      throw beast::system_error(ec);
    }
    _acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
      throw beast::system_error(ec);
    }
  }

  void Run() { DoAccept(); }

 private:
  void DoAccept() {
    auto self = shared_from_this();
    _acceptor.async_accept([self](beast::error_code ec, tcp::socket socket) { self->OnAccept(ec, std::move(socket)); });
  }

  void OnAccept(beast::error_code ec, tcp::socket socket) {
    if (!ec) {
      std::make_shared<HttpSession>(std::move(socket), _state)->Run();
    }

    DoAccept();
  }

  tcp::acceptor _acceptor;
  std::shared_ptr<BeastRuntimeState> _state;
};

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
  RaiseFileDescriptorLimit();

  auto state = std::make_shared<BeastRuntimeState>(bench::BenchConfig(8089, argc, argv));

  try {
    const uint32_t threadCount = std::max<uint32_t>(1U, state->cfg.numThreads);
    asio::io_context ioContext(static_cast<int>(threadCount));
    auto listener = std::make_shared<Listener>(ioContext, tcp::endpoint(tcp::v4(), state->cfg.port), state);

    std::cout << "Beast benchmark server starting on port " << state->cfg.port << " with " << threadCount
              << " async io_context threads\n";
    std::cout << "HTTP endpoint and WebSocket endpoints registered at /ws-uncompressed and /ws-compressed\n";
    listener->Run();

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threadCount > 0 ? threadCount - 1 : 0));
    for (uint32_t threadIdx = 1; threadIdx < threadCount; ++threadIdx) {
      workers.emplace_back([&ioContext]() { ioContext.run(); });
    }

    ioContext.run();

    for (auto& worker : workers) {
      worker.join();
    }
  } catch (const std::exception& ex) {
    std::cerr << "Fatal error: " << ex.what() << "\n";
    return 1;
  }

  return 0;
}