#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/http-status-code.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/tunnel-bridge.hpp"
#include "aeronet/vector.hpp"
#include "tunnel-resolver.hpp"

namespace aeronet::internal {

// CONNECT tunnels of a SingleHttpServer (see docs/protocols/connect.md): their setup, with the background resolution
// of the target host names, the relay of their bytes in both directions, and their teardown. An HTTP/1.1 tunnel takes
// over its client connection, an HTTP/2 tunnel is a stream of its client connection: both relay to an upstream
// connection.
// Created for the first CONNECT request of a run of the server event loop, and destroyed when the loop stops: a
// server that does not tunnel pays nothing for them.
class TunnelManager {
 public:
  using ConnectionIt = SingleHttpServer::ConnectionIt;
  using CloseStatus = SingleHttpServer::CloseStatus;
  using LoopAction = SingleHttpServer::LoopAction;

  // Maximum number of CONNECT requests waiting for the resolution of their target: the next ones are answered 503.
  static constexpr std::size_t kMaxPendingTunnels = 256;

  explicit TunnelManager(SingleHttpServer& server);

  // The tunnel manager of 'server', created on first use.
  static TunnelManager& Of(SingleHttpServer& server);

#ifdef AERONET_ENABLE_HTTP2
  // The bridge between the HTTP/2 handler of the client connection 'clientFd' and the tunnels of its streams.
  static std::unique_ptr<ITunnelBridge> MakeH2Bridge(SingleHttpServer& server, NativeHandle clientFd);
#endif

  // Answer the HTTP/1.1 CONNECT request of the connection, ending at consumedBytes in its input buffer (the requests
  // served before it may precede it): the bytes erased from the input buffer once it is answered, when it waits for the
  // resolution of its target.
  LoopAction startHttp1Tunnel(ConnectionIt& cnxIt, std::size_t consumedBytes);

  // Tells whether target host names were resolved: completeResolvedTunnels() then completes their tunnels.
  [[nodiscard]] bool hasResolvedTargets() const noexcept { return _resolver.hasResolved(); }

  // Connect to the resolved targets, then answer their CONNECT requests.
  void completeResolvedTunnels();

  // Answer 504 to the CONNECT requests whose target is still not resolved at their deadline (keepAliveTimeout after
  // the request), forgetting their resolution.
  void expirePendingTunnels(std::chrono::steady_clock::time_point now);

  // Readable event on a tunnel endpoint (isTunneling()), or on an HTTP/1.1 client connection whose CONNECT target is
  // being resolved (tunnelResolving).
  CloseStatus handleReadable(ConnectionIt cnxIt);

  // Relay the input of an endpoint of an HTTP/1.1 tunnel (client or upstream) to its peer.
  CloseStatus relayInput(ConnectionIt cnxIt);

  // Writable event on a tunnel endpoint: completes the connection to an upstream, and writes the bytes buffered for
  // the endpoint.
  CloseStatus handleWritable(ConnectionIt cnxIt);

  // The bytes that 'cnxIt' had to write toward the other end of its tunnels (relayPausedByBacklog) are written: resume
  // the relays that stopped because of them. The HTTP/1.1 peer, or the upstreams of an HTTP/2 client connection, are
  // read again from the next event-loop iteration, and an HTTP/2 tunnel stream gets flow-control credit again.
  void resumeRelaysTowards(ConnectionIt cnxIt);

  // Close the other end of the tunnels of a closing connection (its peer endpoint, or the upstreams of its HTTP/2
  // streams), and forget the resolution of its CONNECT target.
  void closeTunnelsOf(ConnectionIt cnxIt);

 private:
#ifdef AERONET_ENABLE_HTTP2
  class H2Bridge;
#endif

  void establishHttp1Tunnel(ConnectionIt cnxIt, NativeHandle upstreamFd);
  void completeHttp1Tunnel(const PendingTunnel& pending);
  // Answer the HTTP/1.1 CONNECT request of a pending tunnel with an error, closing its connection.
  void refuseHttp1Tunnel(const PendingTunnel& pending, http::StatusCode statusCode, std::string_view reason);

  // Connect to a numeric host: getaddrinfo answers without any query (host names are resolved by resolveTarget()).
  NativeHandle connectUpstream(NativeHandle clientFd, std::string_view host, uint16_t port);
  // Connect to the resolved addresses of a target.
  NativeHandle connectUpstream(const PendingTunnel& pending);
  // Register the upstream connection of a tunnel of the client connection 'clientFd'. Inserting a connection may
  // invalidate the connection iterators. Returns kInvalidHandle on failure.
  NativeHandle registerUpstream(NativeHandle clientFd, ConnectResult&& cres);

  // Resolve the target host name of a CONNECT request (HTTP/2 stream, or HTTP/1.1 when streamId is 0) in the
  // background: getaddrinfo can block for seconds.
  void resolveTarget(NativeHandle clientFd, uint32_t streamId, std::size_t consumedBytes, std::string_view host,
                     uint16_t port);

  // Forward data to a tunnel endpoint, buffering what cannot be written yet. Returns false on a fatal transport error.
  bool forward(ConnectionIt targetIt, std::string_view data);
  // Same, moving the data out of sourceBuffer (swapped when possible, to avoid a copy).
  bool forward(ConnectionIt targetIt, RawChars& sourceBuffer);

  // Half-close the write side of a tunnel endpoint, once the bytes buffered for it are written. Returns true if the
  // endpoint was closed (with its peer).
  bool shutdownWrite(ConnectionIt peerIt);

  CloseStatus readInput(ConnectionIt cnxIt, std::size_t& bytesReadThisEvent, bool& hitEagain);

#ifdef AERONET_ENABLE_HTTP2
  ITunnelBridge::TunnelSetup startH2Tunnel(NativeHandle clientFd, uint32_t streamId, std::string_view host,
                                           uint16_t port);
  void completeH2Tunnel(const PendingTunnel& pending);
  void linkH2Upstream(NativeHandle upstreamFd, uint32_t streamId);

  // Relay the input of the upstream of an HTTP/2 tunnel to its stream, within the flow-control windows.
  CloseStatus relayToStream(ConnectionIt cnxIt);

  // Operations of the HTTP/2 handler on the upstream of one of its tunnels.
  // Returns false when bytes wait for the upstream to be writable (see ITunnelBridge::writeTunnel()).
  bool writeToUpstream(NativeHandle upstreamFd, std::span<const std::byte> data);
  void shutdownUpstreamWrite(NativeHandle upstreamFd);
  void closeUpstream(NativeHandle upstreamFd);
  void resumeRelayToStream(NativeHandle upstreamFd);
#endif

  SingleHttpServer& _server;
  TunnelResolver _resolver;
  // CONNECT tunnels whose target host name is being resolved.
  vector<std::shared_ptr<PendingTunnel>> _pendingTunnels;
};

}  // namespace aeronet::internal
