-- routing_stress.lua
-- Scenario: Router performance with many registered routes
--
-- Tests router lookup performance with:
--   - Large number of static routes (/r0 to /r999)
--   - Pattern-based routes with path parameters
--   - Tests router efficiency with large route tables
--
-- The server must have routes registered from /r0 to /r999
-- and pattern routes like /users/{id}/posts/{post}
--
-- Configurable via:
--   - route_count: Number of static routes to test (default: 1000)
--   - pattern_ratio: Ratio of pattern route requests (default: 0.3)

local route_count = 1000
local pattern_ratio = 0.3

-- Pre-built request paths
local static_routes = {}
local pattern_routes = {}

function init(args)
  for i, arg in ipairs(args) do
    if arg == "--route-count" then
      route_count = tonumber(args[i + 1]) or route_count
    elseif arg == "--pattern-ratio" then
      pattern_ratio = tonumber(args[i + 1]) or pattern_ratio
    end
  end
  
  print(string.format("Routing stress test: routes=%d, pattern_ratio=%.1f%%", 
                      route_count, pattern_ratio * 100))
  
  -- Pre-generate static route paths (matches /r{N} pattern)
  for i = 0, route_count - 1 do
    static_routes[i + 1] = string.format("/r%d", i)
  end
  
  -- Pre-generate pattern route paths
  -- These match routes like /users/{id}/posts/{post}
  for i = 1, 100 do
    pattern_routes[i] = string.format("/users/%d/posts/%d", 
                                       math.random(1, 10000), 
                                       math.random(1, 10000))
  end
  
  math.randomseed(os.time())
end

local request_count = 0

function request()
  request_count = request_count + 1
  
  local path
  if math.random() < pattern_ratio then
    -- Use pattern route
    local idx = (request_count % #pattern_routes) + 1
    path = pattern_routes[idx]
  else
    -- Use static route
    local idx = (request_count % route_count) + 1
    path = static_routes[idx]
  end
  
  local headers = {
    ["Connection"] = "keep-alive"
  }
  return wrk.format("GET", path, headers)
end

-- Note: wrk's response() callback is per-thread and stats are NOT aggregated.
-- Use summary object in done() for real data.

function done(summary, latency, requests)
  print("-------- Routing Stress Results --------")
  print(string.format("Total requests: %d", summary.requests))
  print(string.format("Duration: %.2fs", summary.duration / 1000000))
  print(string.format("Static routes: %d", route_count))
  print(string.format("Pattern ratio: %.1f%%", pattern_ratio * 100))
  print(string.format("Errors (connect): %d", summary.errors.connect))
  print(string.format("Errors (read): %d", summary.errors.read))
  print(string.format("Errors (timeout): %d", summary.errors.timeout))
  print(string.format("Non-2xx responses: %d", summary.errors.status))
  print(string.format("Avg latency: %.2f us", latency.mean))
  print(string.format("P99 latency: %.2f us", latency:percentile(99)))
  print(string.format("Max latency: %.2f us", latency.max))
  print("----------------------------------------")
end
