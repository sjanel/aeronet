-- static_routes.lua
-- Scenario 4: High request rate on small static routes
--
-- Tests routing and response writing speed with minimal payloads.
-- Rotates through multiple routes to test router performance.

local routes = {
  "/ping",
  "/status",
  "/json?items=1",
  "/body?size=64",
  "/headers?count=1&size=8"
}

-- Default headers to ensure keep-alive for all servers
local default_headers = {
  ["Connection"] = "keep-alive"
}

local route_index = 1

function init(args)
  print(string.format("Static routes test: %d routes", #routes))
  for i, route in ipairs(routes) do
    print(string.format("  [%d] %s", i, route))
  end
  
  -- Randomize starting route
  math.randomseed(os.time())
  route_index = math.random(1, #routes)
end

function request()
  local path = routes[route_index]
  route_index = route_index + 1
  if route_index > #routes then
    route_index = 1
  end
  return wrk.format("GET", path, default_headers)
end

-- Note: wrk's response() callback is per-thread and stats are NOT aggregated.
-- Use summary object in done() for real data.

function done(summary, latency, requests)
  print("-------- Static Routes Results --------")
  print(string.format("Total requests: %d", summary.requests))
  print(string.format("Duration: %.2fs", summary.duration / 1000000))
  print(string.format("Routes tested: %d", #routes))
  print(string.format("Errors (connect): %d", summary.errors.connect))
  print(string.format("Errors (read): %d", summary.errors.read))
  print(string.format("Errors (write): %d", summary.errors.write))
  print(string.format("Errors (timeout): %d", summary.errors.timeout))
  print(string.format("Non-2xx responses: %d", summary.errors.status))
  print("---------------------------------------")
  print(string.format("Requests/sec: %.2f", summary.requests / (summary.duration / 1000000)))
  print("---------------------------------------")
end
