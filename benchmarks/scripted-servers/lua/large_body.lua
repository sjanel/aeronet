-- large_body.lua
-- Scenario 2: Large body POST performance
--
-- Tests server's ability to handle large request bodies.
-- Configurable via:
--   - body_size: Size of request body in bytes (default: 1MB)
--   - chunked: Use chunked transfer encoding (default: false)

local body_size = 1024 * 1024  -- 1MB default
local use_chunked = false

-- Generate random body data
local charset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
local body_data = nil

function init(args)
  for i, arg in ipairs(args) do
    if arg == "--body-size" then
      body_size = tonumber(args[i + 1]) or body_size
    elseif arg == "--chunked" then
      use_chunked = true
    end
  end
  print(string.format("Large body test: size=%d bytes, chunked=%s", body_size, tostring(use_chunked)))
  
  -- Generate body data once at init (shared by all threads in this worker)
  math.randomseed(os.time())
  local chunks = {}
  local chunk_size = 4096
  local remaining = body_size
  
  while remaining > 0 do
    local size = math.min(chunk_size, remaining)
    local chunk = {}
    for i = 1, size do
      local idx = math.random(1, #charset)
      chunk[i] = charset:sub(idx, idx)
    end
    chunks[#chunks + 1] = table.concat(chunk)
    remaining = remaining - size
  end
  
  body_data = table.concat(chunks)
end

function request()
  local headers = {
    ["Content-Type"] = "application/octet-stream",
    ["Connection"] = "keep-alive"
  }
  
  if use_chunked then
    headers["Transfer-Encoding"] = "chunked"
  else
    headers["Content-Length"] = tostring(#body_data)
  end
  
  return wrk.format("POST", "/echo", headers, body_data)
end

-- Note: wrk's response() callback is per-thread and stats are NOT aggregated.
-- Use summary object in done() for real data.

function done(summary, latency, requests)
  print("-------- Large Body Results --------")
  print(string.format("Total requests: %d", summary.requests))
  print(string.format("Duration: %.2fs", summary.duration / 1000000))
  print(string.format("Request body size: %.2f KB", body_size / 1024))
  print(string.format("Errors (connect): %d", summary.errors.connect))
  print(string.format("Errors (read): %d", summary.errors.read))
  print(string.format("Errors (write): %d", summary.errors.write))
  print(string.format("Errors (timeout): %d", summary.errors.timeout))
  print(string.format("Non-2xx responses: %d", summary.errors.status))
  -- Estimate throughput: requests * body_size * 2 (sent + received)
  local total_data = summary.requests * body_size * 2
  print(string.format("Est. throughput: %.2f MB/s", total_data / (1024 * 1024) / (summary.duration / 1000000)))
  print("-------------------------------------")
end
