-- large_body.lua
-- Scenario 2: Large body POST performance
--
-- Tests the server's ability to process large request bodies.
-- Configurable via:
--   - body_size: Size of request body in bytes (overrides body_min/body_max)
--   - pool_size: Number of pre-generated bodies
--   - chunked: Use chunked transfer encoding (default: false)

local body_min = 1024                -- 1KB minimum
local body_max = 1 * 1024 * 1024     -- 1MB maximum
local pool_size = 10                 -- number of pre-generated bodies
local use_chunked = false

local charset = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
local bodies = {}

local function build_body(size)
  local buf = {}
  for i = 1, size do
    local idx = math.random(1, #charset)
    buf[i] = charset:sub(idx, idx)
  end
  return table.concat(buf)
end

local function prepare_pool()
  if body_max < body_min then
    body_max = body_min
  end
  local buckets = pool_size
  if buckets < 1 then
    buckets = 1
  end
  local step = (body_max - body_min) / (buckets - 1)
  bodies = {}
  for i = 0, buckets - 1 do
    local size = math.floor(body_min + i * step + 0.5)
    bodies[#bodies + 1] = build_body(math.max(size, 1))
  end
  pool_size = #bodies
end

function init(args)
  for i, arg in ipairs(args) do
    if arg == "--body-size" then
      local v = tonumber(args[i + 1])
      if v then
        body_min = v
        body_max = v
      end
    elseif arg == "--pool-size" then
      local v = tonumber(args[i + 1])
      if v and v >= 1 then
        pool_size = v
      end
    elseif arg == "--chunked" then
      use_chunked = true
    end
  end
  math.randomseed(42)
  prepare_pool()
end

function request()
  local headers = {
    ["Content-Type"] = "application/octet-stream",
    ["Connection"] = "keep-alive"
  }
  local idx = math.random(1, pool_size)
  local body = bodies[idx]
  if use_chunked then
    headers["Transfer-Encoding"] = "chunked"
  else
    headers["Content-Length"] = tostring(#body)
  end
  return wrk.format("POST", "/uppercase", headers, body)
end

function done(summary, latency, requests)
  print("-------- Large Body Results --------")
  print(string.format("Total requests: %d", summary.requests))
  print(string.format("Duration: %.2fs", summary.duration / 1000000))
  print(string.format("Errors (connect): %d", summary.errors.connect))
  print(string.format("Errors (read): %d", summary.errors.read))
  print(string.format("Errors (write): %d", summary.errors.write))
  print(string.format("Errors (timeout): %d", summary.errors.timeout))
  print(string.format("Non-2xx responses: %d", summary.errors.status))
  print("-------------------------------------")
end
