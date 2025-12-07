-- headers_stress.lua
-- Scenario 1: Pure header parsing performance
--
-- Tests server's ability to handle requests with many large headers.
-- Configurable via:
--   - header_count: Number of headers per request (default: 50)
--   - header_size: Size of each header value in bytes (default: 128)

local header_count = 50
local header_size = 128

-- Generate random alphanumeric string
local charset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
function random_string(length)
  local result = {}
  for i = 1, length do
    local idx = math.random(1, #charset)
    result[i] = charset:sub(idx, idx)
  end
  return table.concat(result)
end

-- Pre-generate headers to avoid per-request allocation
local request_headers = {}

-- Parse command line arguments and initialize headers
function init(args)
  for i, arg in ipairs(args) do
    if arg == "--header-count" then
      header_count = tonumber(args[i + 1]) or header_count
    elseif arg == "--header-size" then
      header_size = tonumber(args[i + 1]) or header_size
    end
  end
  print(string.format("Headers stress test: count=%d, size=%d", header_count, header_size))
  
  -- Pre-generate headers at init
  math.randomseed(os.time())
  request_headers["Connection"] = "keep-alive"  -- Ensure keep-alive for fair comparison
  for i = 1, header_count do
    local name = string.format("X-Stress-Header-%d", i)
    local value = random_string(header_size)
    request_headers[name] = value
  end
end

function request()
  local path = string.format("/headers?count=%d&size=%d", header_count, header_size)
  return wrk.format("GET", path, request_headers)
end

-- Note: wrk's response() callback is per-thread and stats are NOT aggregated
-- across threads. The summary object in done() provides the real aggregated data.

function done(summary, latency, requests)
  print("-------- Headers Stress Results --------")
  print(string.format("Total requests: %d", summary.requests))
  print(string.format("Duration: %.2fs", summary.duration / 1000000))
  print(string.format("Errors (connect): %d", summary.errors.connect))
  print(string.format("Errors (read): %d", summary.errors.read))
  print(string.format("Errors (write): %d", summary.errors.write))
  print(string.format("Errors (timeout): %d", summary.errors.timeout))
  print(string.format("Non-2xx responses: %d", summary.errors.status))
  print(string.format("Config: count=%d, size=%d", header_count, header_size))
  print("-----------------------------------------")
end
