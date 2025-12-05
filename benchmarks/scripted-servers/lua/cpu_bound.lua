-- cpu_bound.lua
-- Scenario 5: CPU-bound handler test
--
-- Tests scheduling overhead and event loop efficiency with
-- computationally expensive handlers.
-- Configurable via:
--   - complexity: Fibonacci complexity (default: 35)
--   - hash_iters: Hash iterations (default: 5000)

local complexity = 35
local hash_iters = 5000

function init(args)
  for i, arg in ipairs(args) do
    if arg == "--complexity" then
      complexity = tonumber(args[i + 1]) or complexity
    elseif arg == "--hash-iters" then
      hash_iters = tonumber(args[i + 1]) or hash_iters
    end
  end
  print(string.format("CPU-bound test: complexity=%d, hash_iters=%d", complexity, hash_iters))
end

function request()
  local path = string.format("/compute?complexity=%d&hash_iters=%d", complexity, hash_iters)
  local headers = { ["Connection"] = "keep-alive" }
  return wrk.format("GET", path, headers)
end

-- Note: wrk's response() callback is per-thread and stats are NOT aggregated.
-- Use summary object in done() for real data.

function done(summary, latency, requests)
  print("-------- CPU-Bound Results --------")
  print(string.format("Total requests: %d", summary.requests))
  print(string.format("Duration: %.2fs", summary.duration / 1000000))
  print(string.format("Fibonacci complexity: %d", complexity))
  print(string.format("Hash iterations: %d", hash_iters))
  print(string.format("Errors (connect): %d", summary.errors.connect))
  print(string.format("Errors (read): %d", summary.errors.read))
  print(string.format("Errors (timeout): %d", summary.errors.timeout))
  print(string.format("Non-2xx responses: %d", summary.errors.status))
  print(string.format("Avg latency: %.2f ms", latency.mean / 1000))
  print(string.format("P99 latency: %.2f ms", latency:percentile(99) / 1000))
  print(string.format("Max latency: %.2f ms", latency.max / 1000))
  print("-----------------------------------")
end
