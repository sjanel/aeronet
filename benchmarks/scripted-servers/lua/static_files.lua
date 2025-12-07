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

-- Discover static directory relative to this Lua script file, not the current working directory.
local files = {}

local function get_script_dir()
  -- Walk a few stack frames to find the chunk source that refers to a file (starts with '@')
  for lvl = 1, 8 do
    local info = debug.getinfo(lvl, "S")
    if info and info.source and type(info.source) == 'string' then
      local src = info.source
      if src:sub(1,1) == '@' then
        local path = src:sub(2)
        local dir = path:match('^(.*)[/\\]')
        return dir or "."
      end
    end
  end
  return "."
end

local script_dir = get_script_dir()
local static_dir = script_dir .. "/../static"

-- Debugging help: print resolved paths so it's clear what the script thinks the static dir is.
print(string.format("static_files.lua: script_dir='%s' static_dir='%s'", script_dir, static_dir))

local function discover_static_files()
  local list = {}
  -- Use find to list regular files only, print relative paths without leading ./
  -- Quote the path to handle spaces and ensure it's absolute-relative to the script
  local safe_path = static_dir:gsub('"', '\\"')
  local cmd = string.format('find "%s" -maxdepth 1 -type f -printf "%%f\n"', safe_path)
  local fh = io.popen(cmd)
  if fh then
    for line in fh:lines() do
      line = line:match("^%s*(.-)%s*$")
      if line ~= "" then
        table.insert(list, "/" .. line)
      end
    end
    fh:close()
  end
  return list
end

-- Populate files; fall back to a small default set if discovery fails
do
  files = discover_static_files()
end

local file_index = 1

function init(args)
  if #files == 0 then
    error(string.format("static_files.lua: no files discovered in %s; please create test files (e.g. index.html, large.bin) in that directory", static_dir))
  end
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
