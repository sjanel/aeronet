-- tls_handshake.lua
-- Scenario: TLS handshake and encrypted communication performance
--
-- Tests server's TLS implementation overhead.
-- Note: wrk must be invoked with -H "Connection: close" to force new connections
-- or use the default keep-alive to test sustained TLS throughput.
--
-- Configurable via:
--   - body_size: Size of response body to request (default: 1024)

local body_size = 1024

function init(args)
  for i, arg in ipairs(args) do
    if arg == "--body-size" then
      body_size = tonumber(args[i + 1]) or body_size
    end
  end
  print(string.format("TLS handshake test: body_size=%d", body_size))
end

function request()
  local path = string.format("/body?size=%d", body_size)
  local headers = {
    ["Connection"] = "keep-alive"
  }
  return wrk.format("GET", path, headers)
end

-- Note: wrk's response() callback is per-thread and stats are NOT aggregated.
-- Use summary object in done() for real data.

function done(summary, latency, requests)
  print("-------- TLS Benchmark Results --------")
  print(string.format("Total requests: %d", summary.requests))
  print(string.format("Duration: %.2fs", summary.duration / 1000000))
  print(string.format("Body size: %d bytes", body_size))
  print(string.format("Errors (connect): %d", summary.errors.connect))
  print(string.format("Errors (read): %d", summary.errors.read))
  print(string.format("Errors (write): %d", summary.errors.write))
  print(string.format("Errors (timeout): %d", summary.errors.timeout))
  print(string.format("Non-2xx responses: %d", summary.errors.status))
  print(string.format("Avg latency: %.2f ms", latency.mean / 1000))
  print(string.format("P99 latency: %.2f ms", latency:percentile(99) / 1000))
  print("---------------------------------------")
end
