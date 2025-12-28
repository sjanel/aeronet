-- mixed_workload.lua
-- Scenario 6: Mixed realistic workload
--
-- Simulates realistic microservice traffic patterns with a mix of:
-- - Fast health checks (30%)
-- - JSON API calls (25%)
-- - Medium body requests (25%)
-- - Header-heavy requests (25%)
-- - CPU-intensive operations (15%)
--
-- Configurable via:
--   - distribution: Comma-separated percentages (default: 30,25,25,25,15)
--   - close_ratio: Percentage of requests using "Connection: close"

local close_ratio = 10

local headers_keep_alive = { ["Connection"] = "keep-alive" }
local headers_close = { ["Connection"] = "close" }

-- Request type definitions
local request_types = {
  { path = "/ping", weight = 5, name = "ping" },
  { path = "/json?items=10", weight = 30, name = "json" },
  { path = "/body?size=4096", weight = 25, name = "body" },
  { path = "/headers?count=20&size=64", weight = 25, name = "headers" },
  { path = "/compute?complexity=25&hash_iters=500", weight = 15, name = "compute" }
}

-- Cumulative weights for weighted random selection
local cumulative_weights = {}
local total_weight = 0

function init(args)
  -- Parse custom distribution if provided
  for i, arg in ipairs(args) do
    if arg == "--distribution" then
      local dist = args[i + 1]
      if dist then
        local idx = 1
        for w in string.gmatch(dist, "(%d+)") do
          if request_types[idx] then
            request_types[idx].weight = tonumber(w)
          end
          idx = idx + 1
        end
      end
    elseif arg == "--close-ratio" then
      close_ratio = tonumber(args[i + 1]) or close_ratio
    end
  end
  
  -- Calculate cumulative weights
  total_weight = 0
  for i, rt in ipairs(request_types) do
    total_weight = total_weight + rt.weight
    cumulative_weights[i] = total_weight
  end
  
  print("Mixed workload distribution:")
  for i, rt in ipairs(request_types) do
    print(string.format("  %s: %d%% (%s)", rt.name, rt.weight, rt.path))
  end

  if close_ratio < 0 then close_ratio = 0 end
  if close_ratio > 100 then close_ratio = 100 end
  print(string.format("Connection close ratio: %d%%", close_ratio))
  
  -- Initialize random seed
  math.randomseed(os.time())
end

-- Select request type based on weighted random
function select_request_type()
  local r = math.random(1, total_weight)
  for i, cw in ipairs(cumulative_weights) do
    if r <= cw then
      return request_types[i]
    end
  end
  return request_types[1]  -- fallback
end

function request()
  local rt = select_request_type()
  if close_ratio > 0 and math.random(1, 100) <= close_ratio then
    return wrk.format("GET", rt.path, headers_close)
  end
  return wrk.format("GET", rt.path, headers_keep_alive)
end

-- Track per-type statistics
local type_counts = {}
local type_errors = {}
-- Note: wrk's response() callback is per-thread and stats are NOT aggregated.
-- Use summary object in done() for real data.

function done(summary, latency, requests)
  print("-------- Mixed Workload Results --------")
  print(string.format("Total requests: %d", summary.requests))
  print(string.format("Duration: %.2fs", summary.duration / 1000000))
  print(string.format("Errors (connect): %d", summary.errors.connect))
  print(string.format("Errors (read): %d", summary.errors.read))
  print(string.format("Errors (timeout): %d", summary.errors.timeout))
  print(string.format("Non-2xx responses: %d", summary.errors.status))
  print("")
  print("Latency distribution:")
  print(string.format("  Avg: %.2f us", latency.mean))
  print(string.format("  P50: %.2f us", latency:percentile(50)))
  print(string.format("  P90: %.2f us", latency:percentile(90)))
  print(string.format("  P99: %.2f us", latency:percentile(99)))
  print(string.format("  Max: %.2f us", latency.max))
  print("")
  print(string.format("Throughput: %.2f req/s", summary.requests / (summary.duration / 1000000)))
  print(string.format("Transfer rate: %.2f MB/s", summary.bytes / (summary.duration / 1000000) / (1024 * 1024)))
  print("----------------------------------------")
end
