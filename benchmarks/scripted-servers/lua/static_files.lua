-- static_files.lua
-- Scenario: Static file serving performance
--
-- Tests server's static file handler with various file sizes.
-- Measures sendfile()/splice() efficiency and framework overhead.
--
-- Assumes files are pre-generated in a 'static/' subdirectory with:
--   - index.html (small)
--   - style.css (medium)
--   - app.js (medium)
--   - data.json (larger)
--   - image.bin (binary, larger)

local files = {
  "/index.html",
  "/style.css",
  "/app.js",
  "/data.json",
  "/image.bin"
}

local file_index = 1

function init(args)
  print(string.format("Static files test: %d files", #files))
  for i, file in ipairs(files) do
    print(string.format("  [%d] %s", i, file))
  end
  
  -- Randomize starting file
  math.randomseed(os.time())
  file_index = math.random(1, #files)
end

function request()
  local path = files[file_index]
  file_index = file_index + 1
  if file_index > #files then
    file_index = 1
  end
  
  local headers = {
    ["Connection"] = "keep-alive"
  }
  return wrk.format("GET", path, headers)
end

-- Note: wrk's response() callback is per-thread and stats are NOT aggregated.
-- Use summary object in done() for real data.

function done(summary, latency, requests)
  print("-------- Static Files Results --------")
  print(string.format("Total requests: %d", summary.requests))
  print(string.format("Duration: %.2fs", summary.duration / 1000000))
  print(string.format("Files tested: %d", #files))
  print(string.format("Errors (connect): %d", summary.errors.connect))
  print(string.format("Errors (read): %d", summary.errors.read))
  print(string.format("Errors (write): %d", summary.errors.write))
  print(string.format("Errors (timeout): %d", summary.errors.timeout))
  print(string.format("Non-2xx responses: %d", summary.errors.status))
  print(string.format("Transfer rate: %.2f MB/s", summary.bytes / (summary.duration / 1000000) / (1024 * 1024)))
  print(string.format("Avg latency: %.2f us", latency.mean))
  print(string.format("P99 latency: %.2f us", latency:percentile(99)))
  print("--------------------------------------")
end
