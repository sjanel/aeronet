-- common.lua
-- Shared configuration and utilities for benchmark scripts
--
-- This module ensures consistent HTTP headers across all tests.

local M = {}

-- Default headers that should be sent with every request to ensure
-- consistent behavior across all HTTP servers (some servers like Pistache
-- default to Connection: Close if no Connection header is sent)
M.default_headers = {
  ["Connection"] = "keep-alive"
}

-- Merge user headers with default headers
-- User headers take precedence over defaults
function M.with_defaults(headers)
  local result = {}
  
  -- Copy defaults first
  for k, v in pairs(M.default_headers) do
    result[k] = v
  end
  
  -- Override with user headers
  if headers then
    for k, v in pairs(headers) do
      result[k] = v
    end
  end
  
  return result
end

-- Format a request with default headers included
function M.format(method, path, headers, body)
  return wrk.format(method, path, M.with_defaults(headers), body)
end

return M
