-- body_codec.lua
-- Scenario: Compressed request + compressed response (gzip)
--
-- Sends a gzipped payload with Content-Encoding: gzip and Accept-Encoding: gzip.
-- The server is expected to automatically decompress the request body and
-- automatically compress the response body.
-- The response body should be the request bytes +1 (mod 256).
--
-- Configurable via:
--   --encoding gzip   (future extension point; only gzip is supported now)

local request_encoding = "gzip"
local accept_encoding = "gzip"

local function read_positive_env_number(name)
  local value = os.getenv(name)
  if value == nil or value == "" then
    return nil
  end
  local parsed = tonumber(value)
  if parsed == nil or parsed <= 0 then
    error(string.format("Invalid %s='%s' (must be a positive number)", name, tostring(value)))
  end
  return parsed
end

local function load_payloads()
  local source = debug.getinfo(1, "S").source
  local candidates = {}
  if source and source:sub(1, 1) == "@" then
    local dir = source:sub(2):match("(.*/)")
    if dir then
      table.insert(candidates, dir .. "body_codec_payloads.txt")
      table.insert(candidates, dir .. "../../../../benchmarks/scripted-servers/lua/body_codec_payloads.txt")
    end
  end
  table.insert(candidates, "body_codec_payloads.txt")

  for _, path in ipairs(candidates) do
    local ok, data = pcall(dofile, path)
    if ok then
      return data
    end
  end
  error("Failed to load body_codec_payloads.txt")
end

local gzip_payloads = load_payloads()

local max_payload_kb = read_positive_env_number("BODY_CODEC_MAX_PAYLOAD_KB")
local applied_max_payload_bytes = nil
if max_payload_kb ~= nil then
  applied_max_payload_bytes = math.floor(max_payload_kb * 1024)
  local filtered = {}
  for _, payload in ipairs(gzip_payloads) do
    if payload.uncompressed <= applied_max_payload_bytes then
      table.insert(filtered, payload)
    end
  end
  gzip_payloads = filtered
end

local payload_count = #gzip_payloads
local min_uncompressed = math.huge
local max_uncompressed = 0
local min_compressed = math.huge
local max_compressed = 0

for _, payload in ipairs(gzip_payloads) do
  local uncompressed = payload.uncompressed
  if uncompressed < min_uncompressed then
    min_uncompressed = uncompressed
  end
  if uncompressed > max_uncompressed then
    max_uncompressed = uncompressed
  end
  local compressed_size = #payload.compressed
  if compressed_size < min_compressed then
    min_compressed = compressed_size
  end
  if compressed_size > max_compressed then
    max_compressed = compressed_size
  end
end

-- Uncompressed payloads range from 64 KiB to 8 MiB of iota bytes (0..255 repeated).

function init(args)
  for i, arg in ipairs(args) do
    if arg == "--encoding" then
      request_encoding = args[i + 1] or request_encoding
      accept_encoding = request_encoding
    end
  end

  if request_encoding ~= "gzip" then
    error("Only gzip is supported in body-codec for now")
  end

  if payload_count == 0 then
    error("No gzip payloads loaded for body-codec")
  end

  local thread_tag = tostring(wrk.thread)
  local thread_id = tonumber(thread_tag:match("%d+")) or 0
  math.randomseed(os.time() + thread_id)
  math.random()
end

function request()
  local payload = gzip_payloads[math.random(payload_count)]
  local headers = {
    ["Connection"] = "keep-alive",
    ["Content-Type"] = "application/octet-stream",
    ["Content-Encoding"] = request_encoding,
    ["Accept-Encoding"] = accept_encoding,
  }
  return wrk.format("POST", "/body-codec", headers, payload.compressed)
end

function done(summary, latency, requests)
  print("-------- Body Codec Results --------")
  if applied_max_payload_bytes ~= nil then
    print(string.format("Applied BODY_CODEC_MAX_PAYLOAD_KB: %d", max_payload_kb))
  end
  print(string.format("Payloads: %d", payload_count))
  print(string.format("Uncompressed size range: %d..%d", min_uncompressed, max_uncompressed))
  print(string.format("Compressed size range: %d..%d", min_compressed, max_compressed))
  print(string.format("Total requests: %d", summary.requests))
  print(string.format("Duration: %.2fs", summary.duration / 1000000))
  print(string.format("Errors (connect): %d", summary.errors.connect))
  print(string.format("Errors (read): %d", summary.errors.read))
  print(string.format("Errors (write): %d", summary.errors.write))
  print(string.format("Errors (timeout): %d", summary.errors.timeout))
  print(string.format("Non-2xx responses: %d", summary.errors.status))
  print("------------------------------------")
end
