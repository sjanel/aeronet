#include "aeronet/test-pipe.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace aeronet::test {

std::string TestPipe::pullFromServer(std::size_t maxBytes) {
  if (_serverToClient.empty()) {
    return {};
  }
  const std::string_view sv = _serverToClient;
  const std::size_t toPull = maxBytes == 0 ? sv.size() : std::min(maxBytes, sv.size());
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  const std::string result(sv.data(), toPull);
  _serverToClient.erase_front(toPull);
  return result;
}

std::string_view TestPipe::serverRead(std::size_t maxBytes) {
  if (_clientToServer.empty()) {
    return {};
  }
  const std::string_view sv = _clientToServer;
  if (maxBytes == 0 || maxBytes >= sv.size()) {
    return sv;
  }
  return sv.substr(0, maxBytes);
}

}  // namespace aeronet::test
